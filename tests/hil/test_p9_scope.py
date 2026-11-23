"""Fictional waveforms only; no scope SDK, numpy, USB, or network required."""

from array import array
from io import StringIO
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "script/hil"))
import p9_scope as scope
from p9_scope_format import BUFFER_SAMPLES, read_block, validate_request, write_block
import p9_scope_helper as helper


def block(tmp_path, channels, baud, spb=20, probe=1):
    meta = {
        "what": "fictional",
        "dt_s": 1 / baud / spb,
        "range_v": 2 if probe == 10 else 5,
        "max_adc": 30000,
        "probe": probe,
        "trigger": {"channel": "A", "direction": "falling", "threshold_v": 0.6},
        "scope_serial": "fictional",
        "scope_variant": "3203D",
        "overflow": dict.fromkeys(channels, False),
    }
    samples = {
        ch: array("h", (int(v * 18000) for v in values))
        for ch, values in channels.items()
    }
    path = write_block(tmp_path / "block", meta, samples)
    meta, samples = read_block(path)
    return meta, samples, path


def uart(value):
    return [0] + [(value >> i) & 1 for i in range(8)] + [1]


def lin_bits(
    data=(1, 2, 3, 4),
    *,
    identifier=0x30,
    parity_bad=False,
    check_bad=False,
    break_bits=13,
    delimiter=1,
    space=2,
    gap=1,
    missing=False,
    classic=False,
):
    pid = scope.lin_pid(identifier) ^ (64 if parity_bad else 0)
    bits = [1] * 20 + [0] * break_bits + [1] * delimiter + uart(0x55) + uart(pid)
    if not missing:
        bits += [1] * space
        checksum = scope.lin_checksum(data, None if classic else pid) ^ int(check_bad)
        for i, value in enumerate([*data, checksum]):
            if i:
                bits += [1] * gap
            bits += uart(value)
    return bits + [1] * 30


def sampled(bits, spb=20):
    return [b for b in bits for _ in range(spb)]


def binary(value, width):
    return [(value >> i) & 1 for i in range(width - 1, -1, -1)]


def can_bits(
    identifier=0x150,
    data=(0, 0xFF, 0x55),
    *,
    extended=False,
    crc_bad=False,
    stuff_bad=False,
    rtr=False,
):
    bits = [0]
    if extended:
        bits += (
            binary(identifier >> 18, 11)
            + [1, 1]
            + binary(identifier & ((1 << 18) - 1), 18)
            + [int(rtr), 0, 0]
        )
    else:
        bits += binary(identifier, 11) + [int(rtr), 0, 0]
    bits += binary(len(data), 4)
    if not rtr:
        for value in data:
            bits += binary(value, 8)
    # Independent polynomial division implementation (not the decoder algorithm).
    message = 0
    for bit in bits:
        message = (message << 1) | bit
    remainder = message << 15
    polynomial = (1 << 15) | 0x4599
    for shift in range(remainder.bit_length() - 16, -1, -1):
        if remainder & (1 << (shift + 15)):
            remainder ^= polynomial << shift
    bits += binary(remainder ^ int(crc_bad), 15)
    stuffed, last, run = [], None, 0
    for bit in bits:
        stuffed.append(bit)
        run = run + 1 if bit == last else 1
        last = bit
        if run == 5:
            stuffed.append(bit if stuff_bad else 1 - bit)
            stuff_bad = False
            last, run = 1 - bit, 1
    # CRC delimiter, dominant ACK, ACK delimiter, seven EOF, three intermission.
    return stuffed + [1, 0, 1] + [1] * 10


def test_lin_values_and_timings_roundtrip(tmp_path):
    bits = lin_bits() + lin_bits()
    meta, samples, _ = block(tmp_path, {"A": sampled(bits)}, 19200, probe=10)
    result = scope.lin(meta, samples, lengths={0x30: 4})
    assert result["errors"] == []
    f = result["frames"][0]
    assert f["sync"] == 0x55 and f["pid"] == 0xF0
    assert f["data"] == [1, 2, 3, 4] and f["parity_ok"] and f["enhanced_ok"]
    for key, expected in {
        "break_us": 13 / 19200 * 1e6,
        "delimiter_us": 1 / 19200 * 1e6,
        "header_us": 34 / 19200 * 1e6,
        "response_space_us": 2 / 19200 * 1e6,
        "frame_us": 90 / 19200 * 1e6,
    }.items():
        assert f[key] == pytest.approx(expected, abs=meta["dt_s"] * 1e6)
    assert f["inter_byte_us"] == pytest.approx([1 / 19200 * 1e6] * 4)
    assert result["cadence"]["0x30"]["interval_s"]["median"] == pytest.approx(
        len(lin_bits()) / 19200
    )
    assert result["capture_sha256"] == meta["capture_sha256"]


@pytest.mark.parametrize(
    ("options", "error"),
    [
        ({"check_bad": True}, "checksum"),
        ({"parity_bad": True}, "parity"),
        ({"break_bits": 12}, "break <13"),
        ({"space": 50}, ">1.4x"),
        ({"delimiter": 0}, "delimiter <1"),
    ],
)
def test_lin_hard_errors(tmp_path, options, error):
    # Zero delimiter merges into sync start; it cannot be a valid sync byte.
    meta, samples, _ = block(tmp_path, {"A": sampled(lin_bits(**options))}, 19200)
    result = scope.lin(meta, samples)
    assert any(error in e for e in result["errors"]) or (
        options.get("delimiter") == 0 and result["errors"]
    )


def test_lin_missing_response_is_explicit(tmp_path):
    meta, samples, _ = block(tmp_path, {"A": sampled(lin_bits(missing=True))}, 19200)
    result = scope.lin(meta, samples, lengths={0x30: 4})
    assert result["errors"] == []
    assert result["missing_responses"] == 1
    assert result["frames"][0]["response"] is False
    assert "response_space_us" not in result["frames"][0]


@pytest.mark.parametrize("identifier", [0x30, 0x3C])
def test_classic_checksum(tmp_path, identifier):
    meta, samples, _ = block(
        tmp_path, {"A": sampled(lin_bits(identifier=identifier, classic=True))}, 19200
    )
    result = scope.lin(
        meta, samples, checksum="classic" if identifier == 0x30 else "enhanced"
    )
    assert result["errors"] == []
    assert result["frames"][0]["classic_ok"]


@pytest.mark.parametrize("baud", [125000, 500000])
@pytest.mark.parametrize(
    ("identifier", "extended"), [(0x150, False), (0x1234567, True)]
)
def test_stuffed_can_values_cadence_and_frame_end(tmp_path, baud, identifier, extended):
    frame = can_bits(identifier, extended=extended)
    bits = [1] * 20 + frame + [1] * 40 + frame
    meta, samples, _ = block(tmp_path, {"A": sampled(bits)}, baud)
    result = scope.can(
        meta,
        samples,
        baud=baud,
        periods={"0x15x": (len(frame) + 40) / baud} if not extended else {},
    )
    assert result["errors"] == []
    assert len(result["frames"]) == 2
    f = result["frames"][0]
    assert f["id"] == identifier and f["extended"] == extended
    assert f["data"] == [0, 255, 85] and f["crc_ok"]
    assert f["start_s"] == pytest.approx(20 / baud, abs=meta["dt_s"])
    assert f["end_s"] == pytest.approx((20 + len(frame) - 3) / baud, abs=meta["dt_s"])
    if not extended:
        assert result["ids"]["0x150"]["max_abs_deviation_s"] == pytest.approx(
            0, abs=meta["dt_s"]
        )


@pytest.mark.parametrize(
    ("option", "error"), [("crc_bad", "CRC-15"), ("stuff_bad", "stuff error")]
)
def test_can_errors(tmp_path, option, error):
    meta, samples, _ = block(
        tmp_path, {"A": sampled([1] * 20 + can_bits(**{option: True}))}, 500000
    )
    result = scope.can(meta, samples, baud=500000)
    assert any(error in e for e in result["errors"])


def test_can_resync_on_slow_clock(tmp_path):
    bits = [1] * 20 + can_bits(data=[0x55] * 8)
    # One extra sample every 4 bits: 1.25% slower sender than nominal at 20 S/bit.
    samples = [bit for i, bit in enumerate(bits) for _ in range(20 + (i % 4 == 0))]
    meta, raw, _ = block(tmp_path, {"A": samples}, 125000)
    result = scope.can(meta, raw)
    assert result["errors"] == []
    assert result["frames"][0]["data"] == [0x55] * 8


def test_forward_pair_delay_and_unmatched(tmp_path):
    f = can_bits()
    # EOF is 3 bits before the returned frame's end (intermission).
    delay = 30
    a = [1] * 20 + f + [1] * 200
    b = [1] * (20 + len(f) - 3 + delay) + f
    n = max(len(a), len(b))
    a += [1] * (n - len(a))
    b += [1] * (n - len(b))
    meta, samples, _ = block(tmp_path, {"A": sampled(a), "B": sampled(b)}, 500000)
    result = scope.forward(meta, samples)
    assert result["errors"] == []
    assert result["latency_s"]["median"] == pytest.approx(
        delay / 500000, abs=meta["dt_s"]
    )
    assert result["unmatched_input"] == result["unmatched_output"] == 0
    assert result["label"] == "wire latency (includes arbitration/queueing)"
    result = scope.forward(meta, samples, input_channel="B", output_channel="A")
    assert result["latency_s"] is None
    assert result["unmatched_input"] == result["unmatched_output"] == 1


def test_listener_tx_is_observed_separately(tmp_path):
    a = sampled(lin_bits())
    meta, samples, _ = block(tmp_path, {"A": a, "B": [1] * len(a)}, 19200)
    assert scope.lin(meta, samples)["listener_tx_dominant_samples"] is None
    assert scope.lin(meta, samples, tx_channel="B")["listener_tx_dominant_samples"] == 0
    samples["B"][100] = 0
    assert scope.lin(meta, samples, tx_channel="B")["listener_tx_dominant_samples"] == 1


def request(tmp_path):
    return {
        "cmd": "capture",
        "out": str(tmp_path / "helper"),
        "what": "fictional",
        "channels": ["A"],
        "rate": 100,
        "seconds": 1,
        "range_v": 5,
        "probe": 1,
        "trigger": {"channel": "A", "direction": "falling", "threshold_v": 1.65},
    }


def test_helper_json_line_protocol_without_picobench(tmp_path):
    events = []

    class FakeScope:
        _max_adc = SimpleNamespace(value=30000)

        def __enter__(self):
            return self

        def __exit__(self, *args):
            events.append("close")

        def info(self):
            return SimpleNamespace(variant="3203D", serial="fictional", driver="fake")

        def set_channel(self, ch, _range, *, enabled):
            events.append((ch, enabled))

        def set_simple_trigger(self, *_args, **kwargs):
            assert kwargs["auto_ms"] == 0

        def capture_block(self, channels, rate, n, *, timeout_s):
            assert channels == ["A"] and rate == n == 100 and timeout_s == 16
            return SimpleNamespace(
                volts={"A": [0, 3] * 50}, dt_s=0.01, overflow={"A": False}
            )

    sink = StringIO()
    helper.serve(
        StringIO(json.dumps(request(tmp_path)) + '\n{"cmd":"unknown"}\n'),
        sink,
        FakeScope,
    )
    replies = [json.loads(line) for line in sink.getvalue().splitlines()]
    meta, raw = read_block(Path(replies[0]["saved"]))
    assert list(raw["A"][:4]) == [0, 18000, 0, 18000]
    assert meta["scope_variant"] == "3203D"
    assert ("B", False) in events and events[-1] == "close"
    assert "expected capture" in replies[1]["error"]


def test_buffer_rejected_before_scope_creation(tmp_path):
    req = request(tmp_path)
    req.update(channels=["A", "B"], rate=BUFFER_SAMPLES, seconds=1)
    sink = StringIO()
    helper.serve(
        StringIO(json.dumps(req) + "\n"),
        sink,
        lambda: pytest.fail("must not create scope"),
    )
    assert "64 MS" in json.loads(sink.getvalue())["error"]
    req["rate"] = BUFFER_SAMPLES / 2
    assert validate_request(req) == BUFFER_SAMPLES / 2


def test_tampering_and_undersampling_rejected(tmp_path):
    meta, raw, path = block(tmp_path, {"A": sampled(lin_bits())}, 19200)
    with pytest.raises(ValueError, match="20 samples"):
        scope.lin(dict(meta, dt_s=1 / 19200 / 19), raw)
    (path.parent / "A.bin").write_bytes(b"bad")
    with pytest.raises(ValueError, match="hash/length"):
        read_block(path)
    meta["dt_s"] *= 2
    path.write_text(json.dumps(meta), encoding="utf-8")
    with pytest.raises(ValueError, match="metadata hash"):
        read_block(path)


def test_cli_stdlib_only(tmp_path):
    _, _, path = block(tmp_path, {"A": sampled(lin_bits())}, 19200)
    out = tmp_path / "result.json"
    proc = subprocess.run(
        [
            sys.executable,
            "-S",
            "script/hil/p9_scope.py",
            "lin",
            str(path),
            "--out",
            str(out),
            "--length",
            "0x30=4",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    assert json.loads(proc.stdout)["frames"][0]["data"] == [1, 2, 3, 4]
    assert json.loads(out.read_text(encoding="utf-8"))["errors"] == []


def test_capture_process_uses_x86_sibling_and_validates_reply(tmp_path, monkeypatch):
    meta, _, path = block(tmp_path, {"A": sampled(lin_bits())}, 19200)
    args = SimpleNamespace(
        picobench_dir=str(tmp_path / "pico"),
        what="fictional",
        channels="A",
        rate=100,
        seconds=1,
        range_v=5,
        probe=1,
        trigger=request(tmp_path)["trigger"],
        out=tmp_path / "new",
    )

    def fake_run(command, **kwargs):
        assert command[:2] == ["arch", "-x86_64"]
        assert command[2] == str(tmp_path / "pico/.venv-x86/bin/python")
        assert command[-1] == str(tmp_path / "pico")
        req = json.loads(kwargs["input"])
        assert req["cmd"] == "capture" and req["out"] == str(args.out)
        return SimpleNamespace(
            stdout=json.dumps(
                {"saved": str(path), "capture_sha256": meta["capture_sha256"]}
            )
        )

    monkeypatch.setattr(scope.subprocess, "run", fake_run)
    assert scope.capture(args)["saved"] == str(path)


@pytest.mark.parametrize("kind", ["lin", "can", "forward"])
def test_scope_comparison_and_missing_evidence(tmp_path, kind):
    from p9_scope_compare import scope_rows

    if kind == "lin":
        bits = sampled(lin_bits() + lin_bits())
        meta, raw, _ = block(tmp_path, {"A": bits}, 19200)
        result = scope.lin(meta, raw, threshold=1.5)
    else:
        f = can_bits()
        a = [1] * 20 + f + [1] * 200 + f + [1] * 200
        if kind == "can":
            meta, raw, _ = block(tmp_path, {"A": sampled(a)}, 500000)
            result = scope.can(
                meta, raw, baud=500000, periods={"0x15x": 0.01}, threshold=1.5
            )
        else:
            b = [1] * 200 + a[:-200]
            meta, raw, _ = block(tmp_path, {"A": sampled(a), "B": sampled(b)}, 500000)
            result = scope.forward(meta, raw, threshold=1.5)
    rows = scope_rows(result, result, "fictional")
    assert not any(row[4] == "FAIL" for row in rows)
    assert any(row[4] == "PASS" for row in rows)
    assert any(row[4] == "REVIEW" for row in scope_rows(result, {}, "fictional"))
    broken = dict(result, errors=["synthetic decode error"])
    assert any(row[4] == "FAIL" for row in scope_rows(result, broken, "fictional"))


def test_compare_tolerance_listener_tx_and_hard_limits(tmp_path):
    from copy import deepcopy

    from p9_scope_compare import scope_rows

    meta, raw, _ = block(tmp_path, {"A": sampled(lin_bits() + lin_bits())}, 19200)
    result = scope.lin(meta, raw, threshold=1.5)
    now = deepcopy(result)
    # 2 samples dominates a 2-bit response space's 5% tolerance.
    for f in now["frames"]:
        f["response_space_us"] += 2 * meta["dt_s"] * 1e6
    rows = scope_rows(result, now, "fictional")
    assert all(r[4] == "PASS" for r in rows if "response_space" in r[1])
    for f in now["frames"]:
        f["response_space_us"] += meta["dt_s"] * 1e6
    assert any(
        r[4] == "FAIL"
        for r in scope_rows(result, now, "fictional")
        if "response_space" in r[1]
    )
    for key, value in (
        ("break_bits", 12),
        ("delimiter_bits", 0.5),
        ("parity_ok", False),
        ("checksum_ok", False),
        ("frame_ratio", 1.41),
    ):
        now = deepcopy(result)
        now["frames"][0][key] = value
        assert any(
            r[4] == "FAIL"
            for r in scope_rows(result, now, "fictional")
            if "LIN.limits" in r[1]
        )
    now = dict(result, listener_tx_dominant_samples=1)
    assert any(r[4] == "FAIL" for r in scope_rows(result, now, "listener"))
    assert any(
        r[4] == "REVIEW" and "listener.TX" in r[1]
        for r in scope_rows(result, result, "listener")
    )


def test_can_truncation_and_remote_frame(tmp_path):
    meta, raw, _ = block(
        tmp_path, {"A": sampled([1] * 20 + can_bits(rtr=True))}, 125000
    )
    result = scope.can(meta, raw)
    assert result["errors"] == []
    assert result["frames"][0]["rtr"] and result["frames"][0]["data"] == []
    cut = {"A": raw["A"][:-150]}
    assert any("truncated" in e for e in scope.can(meta, cut)["errors"])


def test_scope_integrates_with_existing_p9_records(tmp_path):
    import p9_compare

    meta, raw, _ = block(tmp_path, {"A": sampled(lin_bits() + lin_bits())}, 19200)
    evidence = scope.lin(meta, raw)
    record = {
        "sha": "fictional",
        "yaml_sha256": "fictional",
        "tool_version": "fictional",
        "seconds": 1,
        "runs": {
            "profile-green": {
                "board_mac": "fictional",
                "metrics": {"errors.events": 0},
                "missing": {
                    "lin.break_us": "scope needed",
                    "gateway.rx_isr_to_forward_us": "not available",
                },
            }
        },
    }
    markdown, code = p9_compare.compare(
        record,
        record,
        scope_ref={"profile-green-lin": evidence},
        scope_now={"profile-green-lin": evidence},
    )
    assert "scope needed" not in markdown
    assert "break_us.median" in markdown
    assert "rx_isr_to_forward" in markdown and code == 2
    broken = dict(evidence, errors=["CAN stuff error"])
    _, code = p9_compare.compare(
        record,
        record,
        scope_ref={"profile-green-lin": evidence},
        scope_now={"profile-green-lin": broken},
    )
    assert code == 1


def test_half_bit_delimiter_and_truncated_break(tmp_path):
    bits = sampled(lin_bits())
    # Remove half the delimiter's samples while preserving the following sync.
    delimiter = (20 + 13) * 20
    del bits[delimiter : delimiter + 10]
    meta, raw, _ = block(tmp_path, {"A": bits}, 19200)
    result = scope.lin(meta, raw)
    assert result["frames"][0]["delimiter_bits"] == 0.5
    assert any("delimiter <1" in e for e in result["errors"])
    raw["A"].extend([0] * 13 * 20)
    assert any("truncated LIN break" in e for e in scope.lin(meta, raw)["errors"])


def test_hard_fail_survives_scope_settings_mismatch(tmp_path):
    from p9_scope_compare import scope_rows

    meta, raw, _ = block(tmp_path, {"A": sampled(lin_bits())}, 19200)
    result = scope.lin(meta, raw)
    now = dict(
        result, scope_serial="different-fictional", listener_tx_dominant_samples=1
    )
    rows = scope_rows(result, now, "listener")
    assert any(r[4] == "REVIEW" and r[1] == "scope_serial" for r in rows)
    assert any(r[4] == "FAIL" and "listener.TX" in r[1] for r in rows)
