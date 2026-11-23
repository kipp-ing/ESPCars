#!/usr/bin/env python3
"""Capture P9 evidence by MAC. Flash immutable images only with --flash."""

from __future__ import annotations

import argparse
from datetime import UTC, datetime
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import threading

# CLI scripts also need to work when loaded by pytest via importlib.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from p9_build import REPO, sha256
from ports import KNOWN_MACS, enumerate_boards
from verify import CRITERIA, capture

ANSI = re.compile(r"\x1b\[[0-9;]*m")
HEAP = re.compile(r"free (\d+) B, largest block (\d+) B, min since boot (\d+) B")
LOOP = re.compile(r"max (\d+) us, (\d+) gaps > 50 ms, (\d+) loops")
MISSING = {
    "gateway.rx_isr_to_forward_us": "not printed; scope/CAN trace required; callback timestamps are not ISR latency",
    "lin.break_us": "not printed; scope LIN wire in commander/responder/listener runs",
    "lin.sync_us": "not printed; scope sync/PID order",
    "lin.first_response_us": "not printed; scope PID-to-first-byte latency",
    "lin.response_window_us": "not printed; scope response/inter-byte windows",
    "cyclic.jitter_us": "not printed; timestamp wire frames; per-ID mean intervals do not measure jitter",
    "slots.in_use": "not printed in this capture; do not infer zero",
    "loop.max_iteration_us": "no runtime_stats active_max; use separate profile images (perturbs timing)",
}


def select_board(board: str, attached: list[dict]) -> dict:
    """Use ports.py's MAC mapping, never a cached path or the reported name alone."""
    macs = {mac for mac, name in KNOWN_MACS.items() if name == board}
    matches = [item for item in attached if item.get("mac", "").upper() in macs]
    if len(matches) != 1:
        raise ValueError(
            f"{board}: expected one board with a known MAC, found {len(matches)}"
        )
    return matches[0]


def parse_log(text: str, seconds: float, board: str, *, ring: bool = False) -> dict:
    """Parse actual bench formats; keep missing evidence distinct from zero."""
    text = ANSI.sub("", text)
    samples: dict[str, list[float]] = {}

    def add(key, value):
        samples.setdefault(key, []).append(float(value))

    port = "unknown"
    runtime_period = True
    for line in text.splitlines():
        if "Period stats (last" in line:
            runtime_period = True
        elif "Total stats (since boot)" in line:
            runtime_period = False
        if match := HEAP.search(line):
            phase = "setup" if "after_setup" in line else "load"
            for name, value in zip(
                ("free", "largest", "min"), match.groups(), strict=True
            ):
                add(f"heap.{phase}.{name}_bytes", value)
        if match := LOOP.search(line):
            gap, slow, count = map(int, match.groups())
            add("loop.max_period_us", gap)
            add("loop.slow_gaps", slow)
            if count:
                # Existing sampler's 10s window, not a LIN/task jitter proxy.
                add("loop.period_mean_us", 10_000_000 / count)
        if match := re.search(r"took a long time for an operation \((\d+) ms\)", line):
            add("loop.max_blocking_operation_us", int(match[1]) * 1000)
        if runtime_period and (
            match := re.search(r"main_loop:.*active_max=([\d.]+)ms", line)
        ):
            add("loop.max_iteration_us", float(match[1]) * 1000)
        if match := re.search(r"CPU ([\d.]+)%", line):
            add("cpu.busy_percent", match[1])
        if match := re.search(
            r"\[task(?::\d+)?\]:\s+(\S+)\s+([\d.]+)%\s+prio\s+\d+\s+stack_hw\s+(\d+) B",
            line,
        ):
            add(f"task.{match[1]}.cpu_percent", match[2])
            add(f"task.{match[1]}.stack_hw_bytes", match[3])
        if match := re.search(r"Port (\d+) stats:", line):
            port = match[1]
        for key, value in re.findall(
            r"\b(bus_err|err_events|tx_fail|TEC|REC) (\d+)", line
        ):
            add(f"errors.can{port}.{key}", value)
        if match := re.search(
            r"(Checksum Errors|Send Failures): (\d+)(?: \| (\d+))?", line
        ):
            add(
                f"errors.lin.{match[1].lower().replace(' ', '_')}",
                max(int(v) for v in match.groups()[1:] if v is not None),
            )
        if match := re.search(r"Error Rate: ([\d.]+)%", line):
            add("errors.lin.rate_percent", match[1])
        for key, value in re.findall(
            r"\b(dropped|tap_dropped|text_dropped|card_dropped|index_refused)=(\d+)",
            line,
        ):
            add(f"errors.sd.{key}", value)
        if match := re.search(r"Slots in use: (\d+)/(\d+)", line):
            add("slots.in_use", match[1])
            add("slots.capacity", match[2])
        # Preserve optional diagnostic counters verbatim when a firmware prints them.
        if "tracker" in line.lower():
            for key, value in re.findall(r"(\w+)=(\d+)", line):
                add(f"slots.tracker.{key}", value)
        if match := re.search(
            r"Port (\d+) ID (0x[0-9A-Fa-f]+)(?: X)?: avg (\d+) us, n (\d+)", line
        ):
            add(f"can{match[1]}.{match[2]}.period_mean_us", match[3])
        if match := re.search(
            r"ID (0x[0-9A-Fa-f]+) upd=(\d+) miss=(\d+) min=([\d.]+)ms max=([\d.]+)ms exp=([\d.]+)ms",
            line,
        ):
            add(f"lin.selftest.{match[1]}.min_ms", match[4])
            add(f"lin.selftest.{match[1]}.max_ms", match[5])
            add(f"errors.lin.selftest.{match[1]}.miss", match[3])

    metrics = {}
    for key, values in samples.items():
        # Keep descriptive means separate from headroom minima and error maxima.
        if key.endswith(("period_mean_us", "cpu_percent", "busy_percent")):
            metrics[key] = sum(values) / len(values)
        elif key.startswith("heap.") or key.endswith(("stack_hw_bytes", ".min_ms")):
            metrics[key] = min(values)
        else:
            metrics[key] = max(values)
    missing = {key: reason for key, reason in MISSING.items() if key not in metrics}
    for phase in ("setup", "load"):
        for name in ("free", "largest", "min"):
            key = f"heap.{phase}.{name}_bytes"
            if key not in metrics:
                missing[key] = "no heap sample for this phase"
    for key in ("loop.period_mean_us", "loop.max_period_us"):
        if key not in metrics:
            missing[key] = "no loop sampler window"
    errors = [
        pattern
        for pattern in ("TOKEN LOST", "bus-off", "LIN error:", "VCC sag:")
        if pattern in text
    ]
    if text.startswith("__CAPTURE_ERROR__"):
        errors.append(text.strip())
    verification = None
    if ring:
        criteria = CRITERIA[f"mr-{board}"]
        need = [pattern for pattern in criteria["need"] if pattern not in text]
        forbidden = [pattern for pattern in criteria["forbid"] if pattern in text]
        errors = sorted(set(errors + forbidden))
        verification = {
            "missing": need,
            "forbidden": forbidden,
            "pass": not need and not errors,
        }
        if board == "green":
            laps = [int(n) for n in re.findall(r"RING lap (\d+) closed", text)]
            if len(laps) >= 2 and laps == sorted(laps):
                progress = max(laps) - min(laps)
                metrics["ring.laps_per_s"] = progress / seconds
                metrics["ring.laps"] = progress
                verification["pass"] &= progress >= 75
            else:
                missing["ring.laps_per_s"] = (
                    "need two increasing lap reports; reboot/counter reset invalidates capture"
                )
                verification["pass"] = False
    metrics["errors.events"] = len(errors)
    return {
        "metrics": metrics,
        "samples": samples,
        "missing": missing,
        "errors": errors,
        "verify": verification,
    }


def flash_image(entry: dict, manifest: Path) -> None:
    """Validate both artifacts before resolving a live port and flashing at 0x0."""
    for key, hash_key in (("image", "sha256"), ("flash_image", "flash_sha256")):
        path = Path(entry[key])
        if not path.is_absolute():
            path = manifest.parent / path
        if sha256(path) != entry[hash_key]:
            raise ValueError(f"{entry['role']}: {key} checksum mismatch")
    board = select_board(entry["board"], enumerate_boards())
    factory = Path(entry["flash_image"])
    if not factory.is_absolute():
        factory = manifest.parent / factory
    if entry.get("flash_offset") != "0x0":
        raise ValueError("expected a factory image with bootloader/partitions at 0x0")
    subprocess.run(
        [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            "esp32c6",
            "--port",
            board["port"],
            "--after",
            "hard-reset",
            "write-flash",
            "0x0",
            str(factory),
        ],
        check=True,
        stdout=sys.stderr,
    )


def measure(
    manifest: Path,
    roles: list[str],
    seconds: float,
    *,
    flash: bool = False,
    flash_only: bool = False,
) -> dict:
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("config_only"):
        raise ValueError("config-only manifest has no flashable images")
    requested = (
        [f"{roles[0]}-{b}" for b in ("green", "blue", "orange")]
        if roles in (["ring"], ["profile"])
        else roles
    )
    if len(set(requested)) != len(requested):
        raise ValueError("duplicate roles")
    images = []
    for role in requested:
        entries = [entry for entry in data["images"] if entry["role"] == role]
        if len(entries) != 1:
            raise ValueError(f"expected one manifest entry for {role}")
        images.append(entries[0])
    if len({entry["board"] for entry in images}) != len(images):
        raise ValueError("cannot measure two images on one board simultaneously")
    # Validate all target identities before the first upload.
    for entry in images:
        select_board(entry["board"], enumerate_boards())
    if flash:
        for entry in images:
            flash_image(entry, manifest)
    result = {
        "schema": 1,
        "ref": data["ref"],
        "sha": data["sha"],
        "yaml_sha256": data["yaml_sha256"],
        "tool_version": data["tool_version"],
        "seconds": seconds,
        "timestamp": datetime.now(UTC).isoformat(),
        "flashed": flash,
        "runs": {},
    }
    if flash_only:
        return result
    logs = {}
    identities = {
        entry["role"]: select_board(entry["board"], enumerate_boards())
        for entry in images
    }
    threads = [
        threading.Thread(
            target=capture,
            args=(identities[entry["role"]]["port"], seconds, logs, entry["role"]),
        )
        for entry in images
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for entry in images:
        role = entry["role"]
        result["runs"][role] = {
            **parse_log(
                logs[role],
                seconds,
                entry["board"],
                ring=role.startswith("ring-"),
            ),
            "board_mac": identities[role]["mac"],
            "image_sha256": entry["sha256"],
            "log": logs[role],
        }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument(
        "--role",
        action="append",
        required=True,
        help="manifest role; repeat for concurrent capture, or ring/profile for all three",
    )
    parser.add_argument("--seconds", required=True, type=float)
    parser.add_argument("--flash", action="store_true")
    parser.add_argument(
        "--flash-only",
        action="store_true",
        help="requires --flash; stage a whole rig before capture",
    )
    parser.add_argument("--out", type=Path, help="external JSON path (default stdout)")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("--seconds must be finite and positive")
    if args.flash_only and not args.flash:
        parser.error("--flash-only requires --flash")
    if args.out and args.out.resolve().is_relative_to(REPO):
        parser.error("store bench evidence outside this repository")
    try:
        result = measure(
            args.manifest.resolve(),
            args.role,
            args.seconds,
            flash=args.flash,
            flash_only=args.flash_only,
        )
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        parser.exit(2, f"{exc}\n")
    rendered = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return int(
        any(
            run["errors"]
            or any(v for k, v in run["metrics"].items() if k.startswith("errors."))
            or (run["verify"] is not None and not run["verify"]["pass"])
            for run in result["runs"].values()
        )
    )


if __name__ == "__main__":
    sys.exit(main())
