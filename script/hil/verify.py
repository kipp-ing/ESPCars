#!/usr/bin/env python3
"""HIL bench verifier: capture all boards' serial logs, assert the token ring.

Usage: .venv/bin/python script/hil/verify.py [--seconds 30] [--min-laps 50] [--outdir DIR]

The ring (see tests/hil/HIL.md): Green owns lap N on LIN, Blue relays it to
CAN, Orange gateways it between the CAN segments, Green closes the lap and
advances — under high-priority cyclic load on both CAN segments.

Green criteria per board over the capture window:
  mr-green   "RING lap N closed" progressing by >= --min-laps; no "TOKEN LOST"
  mr-blue    "RING relay" and "RING ack" present; no "BLUE degraded"
  mr-orange  "RING gw" and "LIN sniff 48" present; no "ORANGE degraded"
  all        no "bus-off" anywhere

Exit 0 iff every criterion on every board passes.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
import threading
import time

import serial
import yaml

REPO = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ports import enumerate_boards  # noqa: E402  — sibling script, not a package

CRITERIA = {
    "mr-green": {"need": ["RING lap"], "forbid": ["TOKEN LOST", "bus-off"]},
    "mr-blue": {"need": ["RING relay", "RING ack"], "forbid": ["BLUE degraded", "bus-off"]},
    "mr-orange": {"need": ["RING gw", "LIN sniff 48"], "forbid": ["ORANGE degraded", "bus-off"]},
}


def capture(port: str, seconds: float, out: dict, key: str) -> None:
    try:
        # Deassert DTR/RTS *before* the port is opened, not after. On the C6's
        # USB-Serial-JTAG these two lines are the boot-mode controls (DTR drives
        # GPIO9, RTS drives reset), and macOS asserts both on open. Opening with
        # them asserted and clearing them afterwards can leave the chip in the
        # ROM download stub — it then sits at "waiting for download" with no
        # firmware running, which reads on the bench as a dead board or a
        # silent bus. Observed on Mr. Orange 2026-07-27; costs a full
        # investigation every time it happens.
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 1
        s.dtr = False
        s.rts = False
        s.open()
        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            buf += s.read(4096)
        s.close()
        out[key] = re.sub(r"\x1b\[[0-9;]*m", "", buf.decode("utf-8", "replace"))
    except Exception as e:  # port busy/missing — report as empty capture
        out[key] = f"__CAPTURE_ERROR__ {e}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--min-laps", type=int, default=50)
    ap.add_argument("--outdir", type=Path, default=None)
    args = ap.parse_args()

    bench = yaml.safe_load((REPO / "tests/hil/bench.yaml").read_text())["boards"]

    # Resolve ports by board identity now — the hub renumbers on every reset,
    # so a path recorded anywhere is already suspect. A missing board is not a
    # ring failure to be reported per-criterion; it is "the bench is not here".
    live = {b["name"]: b["port"] for b in enumerate_boards() if b["name"]}
    missing = [b["name"] for b in bench if b["name"].removeprefix("mr-") not in live]
    if missing:
        print(f"NOT ATTACHED: {', '.join(missing)} — the ring needs all three", file=sys.stderr)
        print(f"attached and identified: {', '.join(sorted(live)) or 'none'}", file=sys.stderr)
        print("run script/hil/ports.py to see everything on the hub", file=sys.stderr)
        return 2
    for b in bench:
        b["port"] = live[b["name"].removeprefix("mr-")]

    logs: dict[str, str] = {}
    threads = [
        threading.Thread(target=capture, args=(b["port"], args.seconds, logs, b["name"]))
        for b in bench
    ]
    print(f"Capturing {args.seconds:.0f}s from {len(bench)} boards in parallel...")
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    if args.outdir:
        args.outdir.mkdir(parents=True, exist_ok=True)
        for name, text in logs.items():
            (args.outdir / f"{name}.log").write_text(text)

    all_ok = True
    for b in bench:
        name, text = b["name"], logs[b["name"]]
        crit = CRITERIA[name]
        err = text.startswith("__CAPTURE_ERROR__")

        need_hits = {p: (p in text) for p in crit["need"]}
        forbid_hits = {p: (p in text) for p in crit["forbid"]}
        ok = all(need_hits.values()) and not any(forbid_hits.values()) and not err

        lap_note = ""
        if name == "mr-green" and not err:
            laps = [int(m) for m in re.findall(r"RING lap (\d+) closed", text)]
            progress = (max(laps) - min(laps)) if len(laps) >= 2 else 0
            rate = progress / args.seconds
            lap_note = f"  laps: progress={progress} (~{rate:.1f}/s, logged every 25th)"
            if progress < args.min_laps:
                ok = False
                lap_note += f"  << below --min-laps {args.min_laps}"

        all_ok &= ok
        print(f"\n== {name} — {'GREEN' if ok else 'NOT GREEN'} ==")
        if err:
            print(f"  capture failed: {text}")
            continue
        for p, hit in need_hits.items():
            print(f"  {'ok  ' if hit else 'MISS'} {p!r}")
        for p, hit in forbid_hits.items():
            if hit:
                print(f"  HIT  forbidden {p!r}")
        if lap_note:
            print(lap_note)
        if not ok:
            tail = "\n    ".join(text.splitlines()[-12:])
            print(f"  log tail:\n    {tail}")

    print(f"\nBENCH: {'ALL GREEN' if all_ok else 'NOT GREEN'}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
