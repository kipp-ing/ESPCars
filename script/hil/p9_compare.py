#!/usr/bin/env python3
"""Compare P9 run records and emit Markdown evidence for the ledger."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p9_scope_compare import scope_rows

REPO = Path(__file__).resolve().parents[2]


def verdict(metric: str, ref: float | None, now: float | None) -> tuple[str, str]:
    if ref is None or now is None or not math.isfinite(ref) or not math.isfinite(now):
        return "REVIEW", "missing/non-finite evidence"
    if metric == "ring.laps_per_s":
        return (
            ("PASS", ">= reference and >= 19.4/s")
            if now >= ref and now >= 19.4
            else ("FAIL", "requires >= reference and >= 19.4/s")
        )
    if metric == "ring.laps":
        return ("PASS", ">= 75 laps") if now >= 75 else ("FAIL", "requires >= 75 laps")
    if metric.startswith("errors."):
        return (
            ("PASS", "zero errors in both runs")
            if ref == now == 0
            else ("FAIL", "requires zero errors in both runs")
        )
    delta = abs(now - ref) / abs(ref) if ref else (0 if now == 0 else math.inf)
    return (
        ("REVIEW", "delta > 5%; plan gives no hard limit")
        if delta > 0.05 + 1e-12
        else ("PASS", "delta <= 5%; descriptive comparison")
    )


def compare(ref: dict, now: dict, *, scope_ref=None, scope_now=None) -> tuple[str, int]:
    if ref.get("kind") or now.get("kind"):
        return render(ref, now, scope_rows(ref, now, ref.get("what", "scope")))
    scope_ref, scope_now = scope_ref or {}, scope_now or {}
    rows = [
        (
            "inputs",
            field,
            None,
            None,
            "REVIEW",
            "inputs differ or are missing; repeat with identical YAML/tool/window",
        )
        for field in ("yaml_sha256", "tool_version", "seconds")
        if not ref.get(field) or ref.get(field) != now.get(field)
    ]
    if not ref.get("runs") or not now.get("runs"):
        rows.append(
            ("inputs", "runs", None, None, "REVIEW", "empty capture is not evidence")
        )
    for role in sorted(ref.get("runs", {}).keys() | now.get("runs", {}).keys()):
        left, right = (
            ref.get("runs", {}).get(role, {}),
            now.get("runs", {}).get(role, {}),
        )
        if not left.get("board_mac") or left.get("board_mac") != right.get("board_mac"):
            rows.append(
                (
                    role,
                    "board_mac",
                    None,
                    None,
                    "REVIEW",
                    "missing/different physical board",
                )
            )
        if role.startswith("ring-"):
            for label, run in (("reference", left), ("now", right)):
                verify = run.get("verify")
                status = (
                    "REVIEW"
                    if verify is None
                    else ("PASS" if verify.get("pass") else "FAIL")
                )
                rows.append(
                    (
                        role,
                        f"verify.{label}",
                        None,
                        None,
                        status,
                        "verify.py criteria, including 75-lap progress on green",
                    )
                )
        metrics = (
            left.get("metrics", {}).keys()
            | right.get("metrics", {}).keys()
            | left.get("missing", {}).keys()
            | right.get("missing", {}).keys()
        )
        if role == "ring-green":
            metrics |= {"ring.laps_per_s", "ring.laps"}
        metrics |= {"errors.events"}
        for metric in sorted(metrics):
            scope_kind = {
                "lin.break_us": "lin",
                "lin.sync_us": "lin",
                "lin.first_response_us": "lin",
                "lin.response_window_us": "lin",
                "cyclic.jitter_us": "can",
            }.get(metric)
            if scope_kind and any(
                name.startswith(f"{role}-")
                and data.get("kind") == scope_kind
                and scope_now.get(name, {}).get("kind") == scope_kind
                for name, data in scope_ref.items()
            ):
                # Detailed scope rows below replace the unprinted-metric placeholder.
                # ISR latency is deliberately never covered by wire evidence.
                continue
            a, b = (
                left.get("metrics", {}).get(metric),
                right.get("metrics", {}).get(metric),
            )
            status, reason = verdict(metric, a, b)
            if a is None or b is None:
                reason = (
                    right.get("missing", {}).get(metric)
                    or left.get("missing", {}).get(metric)
                    or reason
                )
            rows.append((role, metric, a, b, status, reason))
    for name in sorted(scope_ref.keys() | scope_now.keys()):
        rows.extend(scope_rows(scope_ref.get(name, {}), scope_now.get(name, {}), name))
    return render(ref, now, rows)


def render(ref, now, rows):
    statuses = {row[4] for row in rows}
    overall = (
        "FAIL" if "FAIL" in statuses else ("REVIEW" if "REVIEW" in statuses else "PASS")
    )
    lines = [
        f"P9 comparison: **{overall}**",
        "",
        f"Reference `{ref.get('sha', ref.get('capture_sha256', '?'))}` → now `{now.get('sha', now.get('capture_sha256', '?'))}`.",
        "",
        "| Role | Metric | Ref | Now | Delta | Verdict | Evidence / limit |",
        "|---|---|---:|---:|---:|---|---|",
    ]
    for role, metric, a, b, status, reason in rows:
        delta = (
            "—"
            if a is None or b is None
            else f"{b - a:+.6g}" + (f" ({100 * (b - a) / abs(a):+.2f}%)" if a else "")
        )
        cells = (
            role,
            metric,
            a if a is not None else "—",
            b if b is not None else "—",
            delta,
            status,
            reason,
        )
        lines.append(
            "| "
            + " | ".join(str(c).replace("|", r"\|").replace("\n", " ") for c in cells)
            + " |"
        )
    lines += [
        "",
        "Do not close a ledger row until its scope/trace and SD decode/rotation/collection evidence is also attached. REVIEW is not a bench pass.",
        "",
    ]
    return "\n".join(lines), (
        1 if overall == "FAIL" else 2 if overall == "REVIEW" else 0
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("now", type=Path)
    parser.add_argument(
        "--out",
        type=Path,
        help="Markdown evidence (default: NOW.p9.md, outside repository)",
    )
    parser.add_argument(
        "--scope-ref",
        action="append",
        default=[],
        metavar="NAME=JSON",
        help="reference scope results; repeat per capture",
    )
    parser.add_argument(
        "--scope-now",
        action="append",
        default=[],
        metavar="NAME=JSON",
        help="current scope results with matching names",
    )
    args = parser.parse_args()
    output = args.out or args.now.with_suffix(".p9.md")
    if output.resolve().is_relative_to(REPO):
        parser.error("keep real bench evidence outside this repository")

    def scope_inputs(items):
        result = {}
        for item in items:
            name, path = item.split("=", 1)
            if name in result:
                parser.error(f"duplicate scope name {name}")
            result[name] = json.loads(Path(path).read_text(encoding="utf-8"))
        return result

    markdown, code = compare(
        json.loads(args.reference.read_text(encoding="utf-8")),
        json.loads(args.now.read_text(encoding="utf-8")),
        scope_ref=scope_inputs(args.scope_ref),
        scope_now=scope_inputs(args.scope_now),
    )
    output.write_text(markdown, encoding="utf-8")
    print(markdown, end="")
    return code


if __name__ == "__main__":
    sys.exit(main())
