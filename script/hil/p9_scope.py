#!/usr/bin/env python3
"""Observe one scope block, or decode portable P9 LIN/CAN blocks without numpy."""

from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
from collections import defaultdict, deque
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p9_scope_format import VERSION, external, read_block, validate_request


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {
        "min": values[0],
        "median": statistics.median(values),
        "p95": values[math.ceil(0.95 * len(values)) - 1],
        "max": values[-1],
    }


class Wave:
    def __init__(self, raw, meta, threshold=None):
        if len(raw) < 2:
            raise ValueError("capture has fewer than two samples")
        self.dt = meta["dt_s"]
        self.scale = meta["range_v"] * meta["probe"] / meta["max_adc"]
        self.threshold = (
            threshold
            if threshold is not None
            else (min(raw) + max(raw)) * self.scale / 2
        )
        self.raw = raw
        cutoff = self.threshold / self.scale
        self.edges = [0]
        self.levels = [int(raw[0] >= cutoff)]
        for i, value in enumerate(raw):
            level = int(value >= cutoff)
            if level != self.levels[-1]:
                self.edges.append(i)
                self.levels.append(level)
        self.falls = [
            i
            for i, level in zip(self.edges[1:], self.levels[1:], strict=True)
            if not level
        ]

    def level(self, sample):
        if sample < 0 or sample >= len(self.raw):
            raise ValueError("truncated frame at capture boundary")
        return self.levels[bisect_right(self.edges, sample) - 1]

    def next_fall(self, sample, limit):
        i = bisect_left(self.falls, sample - 1e-7)
        return self.falls[i] if i < len(self.falls) and self.falls[i] < limit else None

    def bit_samples(self, baud):
        bit = 1 / baud / self.dt
        if bit < 20 - 1e-6:
            raise ValueError("actual sample rate must be >=20 samples/bit")
        return bit


def provenance(meta, wave, channel, kind):
    return {
        "schema": VERSION,
        "tool_version": VERSION,
        "kind": kind,
        "what": meta["what"],
        "capture_sha256": meta["capture_sha256"],
        "dt_s": meta["dt_s"],
        "channel": channel,
        "threshold_v": wave.threshold,
        "scope_serial": meta["scope_serial"],
        "scope_variant": meta["scope_variant"],
        "duration_s": len(wave.raw) * wave.dt,
        "errors": [
            f"overflow {ch}" for ch, clipped in meta["overflow"].items() if clipped
        ],
    }


def lin_pid(identifier):
    b = [(identifier >> i) & 1 for i in range(6)]
    return (
        identifier
        | ((b[0] ^ b[1] ^ b[2] ^ b[4]) << 6)
        | ((1 ^ b[1] ^ b[3] ^ b[4] ^ b[5]) << 7)
    )


def lin_checksum(data, pid=None):
    total = sum(data) + (pid if pid is not None else 0)
    while total > 255:
        total = (total & 255) + (total >> 8)
    return total ^ 255


def uart_byte(wave, start, bit):
    if start + 10 * bit > len(wave.raw) + 1e-6:
        raise ValueError("truncated UART byte at capture boundary")
    if wave.level(start + bit / 2):
        raise ValueError("UART start bit")
    value = sum(wave.level(start + (i + 1.5) * bit) << i for i in range(8))
    if not wave.level(start + 9.5 * bit):
        raise ValueError("UART stop bit")
    return value, start + 10 * bit


def cadence(frames, key, periods=None):
    starts = defaultdict(list)
    for frame in frames:
        starts[frame[key]].append(frame["start_s"])
    result = {}
    for identifier, times in starts.items():
        intervals = [b - a for a, b in zip(times, times[1:], strict=False)]
        entry = {"count": len(times), "interval_s": distribution(intervals)}
        targets = [
            p for mask, p in (periods or {}).items() if id_match(identifier, mask)
        ]
        if len(set(targets)) > 1:
            raise ValueError(f"conflicting periods for {identifier:#x}")
        if targets:
            entry["period_s"] = targets[0]
            entry["max_abs_deviation_s"] = max(
                (abs(v - targets[0]) for v in intervals), default=None
            )
        result[hex(identifier)] = entry
    return result


def lin(
    meta,
    samples,
    channel="A",
    *,
    baud=19200,
    threshold=None,
    lengths=None,
    checksum="enhanced",
    tx_channel=None,
):
    if tx_channel == channel:
        raise ValueError("LIN bus and listener TXD must use different channels")
    wave = Wave(samples[channel], meta, threshold)
    bit = wave.bit_samples(baud)
    result = provenance(meta, wave, channel, "lin")
    result.update(
        baud=baud,
        checksum_mode=checksum,
        lengths={hex(k): v for k, v in (lengths or {}).items()},
    )
    # A UART byte cannot be low longer than 9 bits. Include malformed 10..12 bit breaks.
    breaks = [
        (s, wave.edges[i + 1])
        for i, s in enumerate(wave.edges[:-1])
        if not wave.levels[i] and wave.edges[i + 1] - s >= 10 * bit - 1e-6
    ]
    frames = []
    for n, (start, rise) in enumerate(breaks):
        if start == 0:
            continue  # pretrigger may start inside a break
        limit = breaks[n + 1][0] if n + 1 < len(breaks) else len(wave.raw)
        frame = {
            "start_s": start * wave.dt,
            "break_us": (rise - start) * wave.dt * 1e6,
            "break_bits": (rise - start) / bit,
            "errors": [],
        }
        frames.append(frame)
        errors = frame["errors"]
        if frame["break_bits"] < 13 - 1e-6:
            errors.append("LIN break <13 bit times")
        try:
            sync_start = wave.next_fall(rise, limit)
            if sync_start is None:
                raise ValueError("missing sync")
            frame["delimiter_us"] = (sync_start - rise) * wave.dt * 1e6
            frame["delimiter_bits"] = (sync_start - rise) / bit
            if frame["delimiter_bits"] < 1 - 1e-6:
                errors.append("LIN delimiter <1 bit")
            sync, end = uart_byte(wave, sync_start, bit)
            frame["sync"] = sync
            if sync != 0x55:
                errors.append("LIN sync !=0x55")
            pid_start = wave.next_fall(end, limit)
            if pid_start is None:
                raise ValueError("missing PID")
            pid, end = uart_byte(wave, pid_start, bit)
            frame.update(
                pid=pid,
                id=pid & 63,
                parity_ok=pid == lin_pid(pid & 63),
                header_us=(end - start) * wave.dt * 1e6,
            )
            if not frame["parity_ok"]:
                errors.append("LIN bad PID parity")
            response_start = wave.next_fall(end, limit)
            if response_start is None:
                frame.update(
                    response=False,
                    data=[],
                    end_s=end * wave.dt,
                    frame_us=(end - start) * wave.dt * 1e6,
                )
                continue
            frame.update(
                response=True, response_space_us=(response_start - end) * wave.dt * 1e6
            )
            values, spaces = [], []
            cursor = response_start
            expected = (lengths or {}).get(pid & 63)
            while cursor is not None and len(values) < (
                expected + 1 if expected is not None else 10
            ):
                value, end = uart_byte(wave, cursor, bit)
                values.append(value)
                cursor = wave.next_fall(end, limit)
                if cursor is not None and (
                    expected is None or len(values) < expected + 1
                ):
                    spaces.append((cursor - end) * wave.dt * 1e6)
            if expected is not None and len(values) != expected + 1:
                raise ValueError("incomplete LIN response")
            if not 2 <= len(values) <= 9:
                raise ValueError(
                    "LIN response must contain 1..8 data bytes and checksum"
                )
            if cursor is not None:
                errors.append("extra LIN response bytes")
            data, check = values[:-1], values[-1]
            classic = check == lin_checksum(data)
            enhanced = check == lin_checksum(data, pid)
            # Diagnostic frame IDs always use the classic checksum.
            required = "classic" if pid & 63 in (60, 61) else checksum
            okay = (
                classic
                if required == "classic"
                else enhanced
                if required == "enhanced"
                else classic or enhanced
            )
            nominal = (34 + 10 * (len(data) + 1)) / baud
            duration = (end - start) * wave.dt
            frame.update(
                data=data,
                checksum=check,
                classic_ok=classic,
                enhanced_ok=enhanced,
                checksum_ok=okay,
                inter_byte_us=spaces,
                end_s=end * wave.dt,
                frame_us=duration * 1e6,
                nominal_us=nominal * 1e6,
                frame_ratio=duration / nominal,
            )
            if not okay:
                errors.append("LIN bad checksum")
            if duration > 1.4 * nominal + 1e-12:
                errors.append("LIN frame >1.4x nominal")
        except ValueError as exc:
            errors.append(str(exc))
    if wave.levels[-1] == 0 and len(wave.raw) - wave.edges[-1] >= 10 * bit:
        result["errors"].append("truncated LIN break at capture boundary")
    result["frames"] = frames
    result["errors"] += [
        f"frame {n}: {error}" for n, f in enumerate(frames) for error in f["errors"]
    ]
    if not frames:
        result["errors"].append("no LIN headers decoded")
    result["cadence"] = cadence([f for f in frames if "id" in f], "id")
    result["summary"] = {
        k: distribution([f[k] for f in frames if k in f])
        for k in (
            "break_us",
            "delimiter_us",
            "header_us",
            "response_space_us",
            "frame_us",
        )
    }
    result["summary"]["inter_byte_us"] = distribution(
        [v for f in frames for v in f.get("inter_byte_us", [])]
    )
    result["missing_responses"] = sum(f.get("response") is False for f in frames)
    result["listener_tx_dominant_samples"] = None
    if tx_channel:
        tx = Wave(samples[tx_channel], meta, 1.65)
        result["tx_channel"] = tx_channel
        result["tx_threshold_v"] = tx.threshold
        # TX pin must remain recessive; fixed logic threshold avoids a flat-low midpoint.
        cutoff = 1.65 / tx.scale
        result["listener_tx_dominant_samples"] = sum(v < cutoff for v in tx.raw)
    return result


def crc15(bits):
    crc = 0
    for bit in bits:
        feedback = (crc >> 14) ^ bit
        crc = (crc << 1) & 0x7FFF
        if feedback:
            crc ^= 0x4599
    return crc


class CanBits:
    """Bit-centre sampler; falling edges resync at boundaries within a 0.2-bit SJW."""

    def __init__(self, wave, start, bit):
        self.wave, self.pos, self.bit = wave, float(start), bit
        self.run, self.last, self.bits = 0, None, []

    def physical(self):
        candidates = self.wave.falls
        i = bisect_left(candidates, self.pos - 0.2 * self.bit)
        if i < len(candidates) and abs(candidates[i] - self.pos) <= 0.2 * self.bit:
            self.pos = float(candidates[i])
        value = self.wave.level(self.pos + self.bit / 2)
        self.pos += self.bit
        return value

    def stuff(self):
        if self.run == 5:
            value = self.physical()
            if value == self.last:
                raise ValueError("CAN stuff error")
            self.last, self.run = value, 1

    def read(self, count):
        value = 0
        for _ in range(count):
            self.stuff()
            bit = self.physical()
            self.run = self.run + 1 if bit == self.last else 1
            self.last = bit
            self.bits.append(bit)
            value = (value << 1) | bit
        return value


def id_match(identifier, mask):
    text = mask.lower().removeprefix("0x")
    if any(c not in "0123456789abcdefx" for c in text) or not text:
        raise ValueError(f"invalid ID mask {mask}")
    actual = f"{identifier:0{len(text)}x}"
    return len(actual) == len(text) and all(
        b in (a, "x") for a, b in zip(actual, text, strict=True)
    )


def can(meta, samples, channel="A", *, baud=125000, threshold=None, periods=None):
    wave = Wave(samples[channel], meta, threshold)
    bit = wave.bit_samples(baud)
    result = provenance(meta, wave, channel, "can")
    result.update(baud=baud, periods=periods or {}, frames=[])
    skip = 0
    for start in wave.falls:
        if start < skip:
            continue
        edge = bisect_left(wave.edges, start)
        if start - wave.edges[edge - 1] < 11 * bit - 1e-6:
            continue
        reader = CanBits(wave, start, bit)
        try:
            if reader.read(1):
                raise ValueError("CAN SOF")
            identifier = reader.read(11)
            rtr_srr = reader.read(1)
            extended = bool(reader.read(1))
            if extended:
                if not rtr_srr:
                    raise ValueError("CAN extended SRR")
                identifier = (identifier << 18) | reader.read(18)
                rtr = reader.read(1)
                reserved = reader.read(2)
            else:
                rtr = rtr_srr
                reserved = reader.read(1)
            if reserved:
                raise ValueError("CAN reserved bit")
            dlc = reader.read(4)
            if dlc > 8:
                raise ValueError("CAN DLC >8 (CAN FD unsupported)")
            data = [reader.read(8) for _ in range(0 if rtr else dlc)]
            expected = crc15(reader.bits)
            crc = reader.read(15)
            reader.stuff()  # stuffing includes the last CRC sequence bit
            errors = []
            if crc != expected:
                errors.append("CAN CRC-15 error")
            if reader.physical() != 1:
                raise ValueError("CAN CRC delimiter")
            ack = reader.physical() == 0
            if reader.physical() != 1 or any(reader.physical() != 1 for _ in range(7)):
                raise ValueError("CAN frame end/form error")
            if reader.pos > len(wave.raw) + 1e-6:
                raise ValueError("truncated CAN EOF at capture boundary")
            frame = {
                "id": identifier,
                "extended": extended,
                "rtr": bool(rtr),
                "dlc": dlc,
                "data": data,
                "crc": crc,
                "crc_ok": crc == expected,
                "ack": ack,
                "start_s": start * wave.dt,
                "end_s": reader.pos * wave.dt,
                "errors": errors,
            }
            result["frames"].append(frame)
            result["errors"] += [f"{start * wave.dt:.9f}s: {e}" for e in errors]
            skip = reader.pos
        except ValueError as exc:
            result["errors"].append(f"{start * wave.dt:.9f}s: {exc}")
            skip = reader.pos
    if not result["frames"]:
        result["errors"].append("no CAN frames decoded")
    result["ids"] = cadence(result["frames"], "id", periods)
    # Period masks with no observed frames must not silently disappear.
    result["missing_periods"] = [
        mask
        for mask in (periods or {})
        if not any(id_match(f["id"], mask) for f in result["frames"])
    ]
    return result


def forward(
    meta, samples, *, baud=500000, threshold=None, input_channel="A", output_channel="B"
):
    left = can(meta, samples, input_channel, baud=baud, threshold=threshold)
    right = can(meta, samples, output_channel, baud=baud, threshold=threshold)
    result = provenance(
        meta, Wave(samples[input_channel], meta, threshold), input_channel, "forward"
    )
    result.update(
        baud=baud,
        output_channel=output_channel,
        output_threshold_v=right["threshold_v"],
        label="wire latency (includes arbitration/queueing)",
        errors=left["errors"] + right["errors"],
    )
    available = defaultdict(deque)
    for frame in right["frames"]:
        available[
            (
                frame["id"],
                frame["extended"],
                frame["rtr"],
                frame["dlc"],
                tuple(frame["data"]),
            )
        ].append(frame)
    matches = []
    used = 0
    for frame in left["frames"]:
        key = (
            frame["id"],
            frame["extended"],
            frame["rtr"],
            frame["dlc"],
            tuple(frame["data"]),
        )
        queue = available[key]
        while queue and queue[0]["start_s"] < frame["end_s"] - meta["dt_s"] / 2:
            queue.popleft()
        if queue:
            other = queue.popleft()
            used += 1
            matches.append(
                {
                    "id": frame["id"],
                    "input_end_s": frame["end_s"],
                    "output_start_s": other["start_s"],
                    "latency_s": other["start_s"] - frame["end_s"],
                }
            )
    result.update(
        matches=matches,
        latency_s=distribution([m["latency_s"] for m in matches]),
        unmatched_input=len(left["frames"]) - used,
        unmatched_output=len(right["frames"]) - used,
    )
    return result


def trigger(value):
    try:
        channel, direction, volts = value.split(":")
        return {"channel": channel, "direction": direction, "threshold_v": float(volts)}
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use A:falling:0.6 (volts at BNC)") from exc


def assignment(value):
    key, value = value.split("=")
    return key, value


def period(value):
    key, text = assignment(value)
    factor = 0.001 if text.endswith("ms") else 1
    seconds = float(text.removesuffix("ms").removesuffix("s")) * factor
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("period must be positive (s or ms)")
    id_match(0, key)
    return key, seconds


def capture(args):
    sibling = args.picobench_dir or os.environ.get("PICOBENCH_DIR")
    if not sibling:
        raise ValueError(
            "set PICOBENCH_DIR or --picobench-dir; see pico-bench docs/macos-sdk-setup.md"
        )
    sibling = Path(sibling).expanduser().resolve()
    req = {
        "cmd": "capture",
        "what": args.what,
        "channels": args.channels.split(","),
        "rate": args.rate,
        "seconds": args.seconds,
        "range_v": args.range_v,
        "probe": args.probe,
        "trigger": args.trigger,
        "out": str(external(args.out)),
    }
    validate_request(req)
    command = [
        "arch",
        "-x86_64",
        str(sibling / ".venv-x86/bin/python"),
        str(Path(__file__).with_name("p9_scope_helper.py")),
        str(sibling),
    ]
    proc = subprocess.run(
        command,
        input=json.dumps(req) + "\n",
        text=True,
        capture_output=True,
        timeout=args.seconds + 60,
        check=True,
    )
    reply = json.loads(proc.stdout)
    if "error" in reply:
        raise ValueError(reply["error"])
    meta, _ = read_block(Path(reply["saved"]))
    if meta["capture_sha256"] != reply["capture_sha256"]:
        raise ValueError("helper returned inconsistent capture hash")
    return reply


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    cap = commands.add_parser("capture")
    cap.add_argument("--picobench-dir")
    cap.add_argument("--what", required=True)
    cap.add_argument("--channels", required=True)
    cap.add_argument("--rate", type=float, required=True)
    cap.add_argument("--seconds", type=float, required=True)
    cap.add_argument(
        "--range-v", type=float, required=True, help="BNC full scale +/- V"
    )
    cap.add_argument(
        "--probe", type=float, default=1, help="probe attenuation (10 for LIN)"
    )
    cap.add_argument("--trigger", type=trigger, required=True)
    cap.add_argument("--out", type=Path, required=True)
    for kind in ("lin", "can", "forward"):
        sub = commands.add_parser(kind)
        sub.add_argument("capture", type=Path)
        sub.add_argument("--out", type=Path, required=True)
        sub.add_argument(
            "--baud",
            type=float,
            default=19200 if kind == "lin" else 500000 if kind == "forward" else 125000,
        )
        sub.add_argument(
            "--threshold", type=float, help="volts at probe tip; default midpoint"
        )
        if kind != "forward":
            sub.add_argument("--channel", choices=("A", "B"), default="A")
        else:
            sub.add_argument("--input-channel", choices=("A", "B"), default="A")
            sub.add_argument("--output-channel", choices=("A", "B"), default="B")
        if kind == "lin":
            sub.add_argument(
                "--length",
                action="append",
                default=[],
                help="unprotected ID=data length, e.g. 0x30=4",
            )
            sub.add_argument(
                "--checksum",
                choices=("classic", "enhanced", "auto"),
                default="enhanced",
            )
            sub.add_argument(
                "--tx-channel",
                choices=("A", "B"),
                help="listener TXD (3.3 V logic); use same attenuation on both probes",
            )
        if kind == "can":
            sub.add_argument(
                "--period",
                action="append",
                default=[],
                help="ID/mask=period, e.g. 0x15x=10ms",
            )
    args = parser.parse_args()
    try:
        if args.command == "capture":
            result = capture(args)
        else:
            output = external(args.out)
            meta, samples = read_block(args.capture)
            options = {"baud": args.baud, "threshold": args.threshold}
            if (
                not math.isfinite(args.baud)
                or args.baud <= 0
                or (args.threshold is not None and not math.isfinite(args.threshold))
            ):
                raise ValueError("baud/threshold must be finite; baud must be positive")
            if args.command == "lin":
                lengths = {int(k, 0): int(v) for k, v in map(assignment, args.length)}
                if any(not 0 <= k <= 63 or not 1 <= v <= 8 for k, v in lengths.items()):
                    raise ValueError("LIN length requires ID 0..63, length 1..8")
                result = lin(
                    meta,
                    samples,
                    args.channel,
                    lengths=lengths,
                    checksum=args.checksum,
                    tx_channel=args.tx_channel,
                    **options,
                )
            elif args.command == "can":
                result = can(
                    meta,
                    samples,
                    args.channel,
                    periods=dict(map(period, args.period)),
                    **options,
                )
            else:
                if args.input_channel == args.output_channel:
                    raise ValueError("forward requires two different channels")
                result = forward(
                    meta,
                    samples,
                    input_channel=args.input_channel,
                    output_channel=args.output_channel,
                    **options,
                )
            output.write_text(
                json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8"
            )
        print(json.dumps(result, allow_nan=False))
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"{exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
