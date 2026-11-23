"""P9 scope gates; missing observations never become zero-valued measurements."""

from __future__ import annotations

import math

from p9_scope import distribution
from p9_scope_format import VERSION


def scope_rows(ref: dict, now: dict, name: str) -> list:
    rows = []

    def add(metric, a, b, status, reason):
        rows.append((name, metric, a, b, status, reason))

    def review(metric, reason):
        add(metric, None, None, "REVIEW", reason)

    valid = True
    for label, data in (("ref", ref), ("now", now)):
        required = (
            "kind",
            "capture_sha256",
            "dt_s",
            "channel",
            "threshold_v",
            "scope_serial",
            "scope_variant",
            "tool_version",
            "errors",
            "duration_s",
            "baud",
            "what",
        )
        missing = [key for key in required if key not in data or data[key] is None]
        if (
            missing
            or data.get("schema") != VERSION
            or data.get("tool_version") != VERSION
        ):
            review(
                f"provenance.{label}", f"missing/unsupported scope evidence: {missing}"
            )
            valid = False
        errors = data.get("errors")
        if isinstance(errors, list):
            add(
                f"errors.{label}",
                0,
                len(errors),
                "FAIL" if errors else "PASS",
                "; ".join(errors) or "zero decode/CRC/stuff/overflow errors",
            )
        else:
            review(f"errors.{label}", "decode error evidence absent")
            valid = False
        if data.get("kind") == "lin":
            frames = data.get("frames", [])
            violations = [
                f"frame {n}"
                for n, f in enumerate(frames)
                if f.get("break_bits", 0) < 13 - 1e-6
                or f.get("delimiter_bits", 0) < 1 - 1e-6
                or f.get("parity_ok") is False
                or f.get("checksum_ok") is False
                or f.get("frame_ratio", 0) > 1.4 + 1e-12
            ]
            add(
                f"LIN.limits.{label}",
                0,
                len(violations),
                "FAIL" if violations else "PASS",
                "break >=13 bits, delimiter >=1, good parity/checksum, frame <=1.4x nominal",
            )
            tx = data.get("listener_tx_dominant_samples")
            if tx is not None:
                add(
                    f"listener.TX.{label}",
                    0,
                    tx,
                    "PASS" if tx == 0 else "FAIL",
                    "TXD must remain recessive for the entire capture",
                )
    for field in (
        "kind",
        "what",
        "baud",
        "duration_s",
        "channel",
        "threshold_v",
        "scope_serial",
        "scope_variant",
        "tool_version",
        "checksum_mode",
        "lengths",
        "periods",
        "output_channel",
        "output_threshold_v",
        "tx_channel",
        "tx_threshold_v",
    ):
        a, b = ref.get(field), now.get(field)
        # A midpoint derived independently can move slightly; require an explicit common
        # threshold for gating, so an analog level change cannot alter decode semantics.
        if a != b:
            review(
                field,
                "capture/analysis settings differ; use matched settings and explicit thresholds",
            )
            valid = False
    for data in (ref, now):
        if (
            not isinstance(data.get("dt_s"), (int, float))
            or not math.isfinite(data["dt_s"])
            or data["dt_s"] <= 0
        ):
            review("dt_s", "sample interval must be finite and positive")
            valid = False
    if not valid:
        return rows
    uncertainty = 2 * max(ref["dt_s"], now["dt_s"])

    def timing(metric, a, b, scale=1):
        if a is None or b is None or not math.isfinite(a) or not math.isfinite(b):
            review(metric, "missing timing samples")
            return
        limit = max(uncertainty * scale, abs(a) * 0.05)
        add(
            metric,
            a,
            b,
            "PASS" if abs(b - a) <= limit + 1e-12 else "FAIL",
            f"|delta| <= max(2 sample intervals, 5% reference) = {limit:.9g}",
        )

    def dist(metric, a, b, scale=1):
        for stat in ("min", "median", "p95", "max"):
            timing(f"{metric}.{stat}", (a or {}).get(stat), (b or {}).get(stat), scale)

    kind = ref["kind"]
    if kind == "lin":
        for label, data in (("ref", ref), ("now", now)):
            frames = data.get("frames", [])
            if not frames:
                review(label, "no LIN frames")
            if any(f.get("response") is False for f in frames):
                review(
                    f"responses.{label}",
                    "headers without response; recovery/timeout evidence requires review",
                )
        ids = {
            f["id"] for data in (ref, now) for f in data.get("frames", []) if "id" in f
        }
        for identifier in sorted(ids):
            a = [f for f in ref.get("frames", []) if f.get("id") == identifier]
            b = [f for f in now.get("frames", []) if f.get("id") == identifier]
            if not a or not b:
                review(hex(identifier), "PID missing on one side")
                continue
            for field in (
                "break_us",
                "delimiter_us",
                "header_us",
                "response_space_us",
                "frame_us",
            ):
                dist(
                    f"{identifier:#x}.{field}",
                    distribution([f[field] for f in a if field in f]),
                    distribution([f[field] for f in b if field in f]),
                    1e6,
                )
            dist(
                f"{identifier:#x}.inter_byte_us",
                distribution([v for f in a for v in f.get("inter_byte_us", [])]),
                distribution([v for f in b for v in f.get("inter_byte_us", [])]),
                1e6,
            )
            dist(
                f"{identifier:#x}.cadence_s",
                ref.get("cadence", {}).get(hex(identifier), {}).get("interval_s"),
                now.get("cadence", {}).get(hex(identifier), {}).get("interval_s"),
            )
        if "listener" in name and any(
            d.get("listener_tx_dominant_samples") is None for d in (ref, now)
        ):
            review(
                "listener.TX",
                "shared LIN bus cannot prove listener zero TX; observe its TXD on channel B",
            )
        review(
            "P4.recovery_and_payload",
            "scope timing does not establish payload semantics, finalization, stall/reboot/error recovery; attach runbook exercises",
        )
    elif kind == "can":
        for identifier in sorted(ref.get("ids", {}).keys() | now.get("ids", {}).keys()):
            a, b = (
                ref.get("ids", {}).get(identifier, {}),
                now.get("ids", {}).get(identifier, {}),
            )
            dist(f"{identifier}.interval_s", a.get("interval_s"), b.get("interval_s"))
            if "period_s" in a or "period_s" in b:
                timing(
                    f"{identifier}.max_abs_deviation_s",
                    a.get("max_abs_deviation_s"),
                    b.get("max_abs_deviation_s"),
                )
        for label, data in (("ref", ref), ("now", now)):
            if data.get("missing_periods"):
                review(
                    f"periods.{label}",
                    f"no frames for masks: {data['missing_periods']}",
                )
            if not data.get("frames"):
                review(f"frames.{label}", "no CAN frame observations")
    elif kind == "forward":
        dist("wire_latency_s", ref.get("latency_s"), now.get("latency_s"))
        for label, data in (("ref", ref), ("now", now)):
            for field in ("unmatched_input", "unmatched_output"):
                if data.get(field) != 0:
                    review(
                        f"{field}.{label}",
                        "unmatched frames (including capture edges/unrouted traffic); inspect before delivery claims",
                    )
        review(
            "gateway.rx_isr_to_forward_us",
            "wire latency (includes arbitration/queueing); IRAM callback timing unavailable in current esp_timing",
        )
    else:
        review("kind", "unsupported scope result kind")
    return rows
