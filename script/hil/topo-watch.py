#!/usr/bin/env python3
"""Watch the bench's CAN topology live, and print only when it changes.

Attaches to every board on the hub at once, follows the `[topo]` lines that
tests/hil/topo-live.yaml emits, and maintains a model of the wiring. It prints
the full picture once at startup and then **nothing at all until something
moves** — so you can leave it running, re-plug a tap, and read what happened
instead of correlating three scrolling consoles by eye.

    port A + peer     a tap was plugged in            (~1 s after the event)
    port A - peer     a tap was pulled                (~3 s, the beacon TTL)

SELF-DETERMINING — there is no board table anywhere in here. Boards identify
themselves by MAC in every report line, peers are named by the MAC they beacon,
and the segments are computed as the connected components of "can hear". A board
nobody has ever configured shows up correctly the first time it speaks; a board
that is renamed, reflashed or swapped needs no edit here. Friendly names are a
courtesy looked up from ports.py when the MAC happens to be known, never a
requirement — anything unknown is shown by MAC and works identically.

Usage:
    script/hil/topo-watch.py                 # follow until Ctrl-C
    script/hil/topo-watch.py --once          # one settled picture, then exit
    script/hil/topo-watch.py --seconds 120   # follow for a bounded time

Exit codes: 0 normal, 2 nothing attached.
"""

from __future__ import annotations

import argparse
import re
import sys
import threading
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ports import KNOWN_MACS, enumerate_boards  # noqa: E402 — sibling script

# "[I][topo:208]: seg1 [20:6E:F1:0A:D1:E0.bus0] = 94:B9:7E:E4:80:C0.bus0(0x300)"
# "[W][topo:205]: seg2 [20:6E:F1:0A:D1:E0.bus1] = nothing"
REPORT = re.compile(r"\[topo:\d+\]:\s+(\S+)\s+\[([0-9A-F:]{17})\.bus(\d)\]\s+=\s+(.*)")
PEER = re.compile(r"([0-9A-F:]{17})\.bus(\d)")
# "[I][topo:196]: seg1 + 94:B9:7E:E4:80:C0.bus0 id=0x300 (NEW)"
EVENT = re.compile(r"\[topo:\d+\]:\s+(\S+)\s+([+-])\s+([0-9A-F:]{17})\.bus(\d)")

RESET = "\x1b[0m"
DIM = "\x1b[2m"
GREEN = "\x1b[32m"
RED = "\x1b[31m"
BOLD = "\x1b[1m"


def nice(mac: str) -> str:
    """Friendly name if we happen to know the MAC, else the MAC itself."""
    name = KNOWN_MACS.get(mac)
    return f"{name}" if name else mac


def reader(port: str, board: str, sink: list, lock: threading.Lock, stop: threading.Event) -> None:
    """Follow one console forever, appending parsed lines to `sink`.

    DTR/RTS are deasserted *before* open(), never after: on the C6's
    USB-Serial-JTAG they are the boot-mode controls and macOS asserts both on
    open, which can park the chip in the ROM download stub — a board that then
    runs no firmware and reads on the bench as dead. Same pattern as verify.py.
    """
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.5
        s.dtr = False
        s.rts = False
        s.open()
    except Exception as exc:
        with lock:
            sink.append(("error", board, str(exc)))
        return
    buf = b""
    while not stop.is_set():
        try:
            buf += s.read(4096)
        except Exception as exc:
            with lock:
                sink.append(("error", board, str(exc)))
            break
        while b"\n" in buf:
            raw, _, buf = buf.partition(b"\n")
            line = re.sub(r"\x1b\[[0-9;]*m", "", raw.decode("utf-8", "replace")).rstrip()
            with lock:
                sink.append(("line", board, line))
    try:
        s.close()
    except Exception:
        pass


def segments_of(model: dict) -> list[list[str]]:
    """Connected components of 'can hear' — the physical wires, measured."""
    adjacency: dict[str, set[str]] = {}
    for node, peers in model.items():
        adjacency.setdefault(node, set())
        for peer in peers:
            adjacency.setdefault(peer, set())
            adjacency[node].add(peer)
            adjacency[peer].add(node)
    out, unseen = [], set(adjacency)
    while unseen:
        stack, group = [unseen.pop()], []
        while stack:
            cur = stack.pop()
            group.append(cur)
            for nxt in adjacency[cur]:
                if nxt in unseen:
                    unseen.discard(nxt)
                    stack.append(nxt)
        out.append(sorted(group))
    return sorted(out, key=lambda g: (-len(g), g))


def render(model: dict, names: dict) -> str:
    lines = []
    for group in segments_of(model):
        live = [n for n in group if model.get(n)]
        if len(group) == 1 and not live:
            lines.append(f"  {DIM}loose{RESET}  {label(group[0], names)}")
        else:
            lines.append(f"  {BOLD}wire{RESET}   " + "  ↔  ".join(label(n, names) for n in group))
    return "\n".join(lines) if lines else "  (nothing reporting yet)"


def label(node: str, names: dict) -> str:
    mac, _, bus = node.partition("/")
    port = names.get(node)
    friendly = nice(mac)
    return f"{friendly}.bus{bus}" + (f"({port})" if port else "")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=None, help="stop after N seconds")
    ap.add_argument("--once", action="store_true", help="print one settled picture and exit")
    args = ap.parse_args()

    boards = [b for b in enumerate_boards() if b["name"]]
    if not boards:
        print("no bench boards attached — run script/hil/ports.py", file=sys.stderr)
        return 2

    sink: list = []
    lock = threading.Lock()
    stop = threading.Event()
    threads = [
        threading.Thread(target=reader, args=(b["port"], b["name"], sink, lock, stop), daemon=True)
        for b in boards
    ]
    for t in threads:
        t.start()

    print(f"watching {', '.join(sorted(b['name'] for b in boards))} — Ctrl-C to stop\n")

    # model: "MAC/bus" -> set of "MAC/bus" it currently hears.
    # names: "MAC/bus" -> the local port label that board prints (seg1, can1...).
    model: dict[str, set[str]] = {}
    names: dict[str, str] = {}
    console_mac: dict[str, str] = {}
    last_render = None
    settled_at = None
    warmup_until = time.time() + 6.5  # one 5 s report cycle, plus slack
    deadline = time.time() + args.seconds if args.seconds else None

    try:
        while True:
            time.sleep(0.25)
            if deadline and time.time() > deadline:
                break
            with lock:
                batch, sink[:] = list(sink), []
            changes = []
            for kind, board, payload in batch:
                if kind == "error":
                    print(f"{RED}!{RESET} {board}: {payload}")
                    continue
                match = REPORT.search(payload)
                if match:
                    port_name, own_mac, own_bus, rest = match.groups()
                    console_mac[board] = own_mac
                    peers = {f"{m}/{b}" for m, b in PEER.findall(rest)}
                    node = f"{own_mac}/{own_bus}"
                    names[node] = port_name
                    if model.get(node) != peers:
                        model[node] = peers
                    continue
                event = EVENT.search(payload)
                if event:
                    port_name, sign, peer_mac, peer_bus = event.groups()
                    own = console_mac.get(board)
                    if own is None:
                        continue  # not yet bound to a MAC; the 5 s report will fix it
                    verb = f"{GREEN}+{RESET}" if sign == "+" else f"{RED}-{RESET}"
                    changes.append(
                        f"{verb} {nice(own)}.{port_name} "
                        f"{'gained' if sign == '+' else 'lost'} {nice(peer_mac)}.bus{peer_bus}"
                    )

            # Boards report every 5 s and each port reports separately, so the
            # first few seconds are a trickle of partial pictures. Printing them
            # would look like the topology thrashing on startup, which is the
            # opposite of this tool's job. Hold until one full report cycle has
            # passed, then print the settled picture as the baseline.
            if time.time() < warmup_until:
                continue

            current = render(model, names)
            if current != last_render:
                stamp = time.strftime("%H:%M:%S")
                if last_render is None:
                    print(f"{DIM}{stamp}{RESET}  initial topology")
                else:
                    for change in changes:
                        print(f"{DIM}{stamp}{RESET}  {change}")
                    print(f"{DIM}{stamp}{RESET}  topology changed")
                print(current + "\n", flush=True)
                last_render = current
                settled_at = time.time()
            elif changes:
                stamp = time.strftime("%H:%M:%S")
                for change in changes:
                    print(f"{DIM}{stamp}{RESET}  {change}", flush=True)

            if args.once and settled_at and time.time() - settled_at > 6:
                break
    except KeyboardInterrupt:
        print()
    finally:
        stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
