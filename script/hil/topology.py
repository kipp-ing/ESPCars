#!/usr/bin/env python3
"""Read the bench's CAN topology off the boards themselves, and judge it.

Every bench config carries the topology beacon (`tests/hil/topology_rx.yaml` +
`topology_tx.yaml`), so each board already prints, every 10 s, which boards it
hears on each of its CAN ports. This script collects those lines from every
attached board at once and cross-references them, which is the part a human
reading three consoles side by side gets wrong.

WHY CROSS-REFERENCING IS THE WHOLE POINT. A single board's line answers "who do
I hear?". It cannot answer "does anyone hear me?" — and those two failing
*separately* is what a broken bench actually looks like:

    hears nobody + nobody hears it   the port is electrically absent. Unpowered
                                     transceiver (on the C6 adapter: +5 V, i.e.
                                     GPIO7 high AND JP14 bridged), unplugged, or
                                     wrong bit rate. NOT a cabling question yet.
    hears somebody, nobody hears it  it can receive but not drive — a half-dead
                                     transceiver or a TX pin fault.
    two ports hear each other that
    belong to the same board         SAME-WIRE: one physical wire. Under a config
                                     with routes in both directions that is a
                                     forwarding loop.

The connected components of the "can hear" graph are the physical segments, so
this prints the measured wiring rather than the wiring table someone last
edited. `tests/hil/HIL.md` has cost several sessions to exactly
this confusion.

Usage:
    script/hil/topology.py                # 25 s capture, table + verdict
    script/hil/topology.py --seconds 12   # shorter (needs >= ~11 s for one line)
    script/hil/topology.py --json
    script/hil/topology.py --expect green.bus0=orange.bus0 --expect orange.bus1=purple.bus0

Exit codes: 0 all beaconing ports are heard by someone and hear someone (and
every --expect held), 1 otherwise, 2 nothing attached.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ports import enumerate_boards  # noqa: E402  — sibling script, not a package

# Which bus index each board's port ids map to, matching the fixed slot map in
# tests/hil/topology_rx.yaml (slot = node*2 + bus, beacon id = 0x0C0 + slot).
# Ports absent from a given config simply never show up in its output.
PORT_BUS = {
    ("green", "can1"): 0,
    ("green", "can2"): 1,
    ("blue", "can1"): 0,
    ("blue", "can2"): 1,
    ("orange", "seg1"): 0,
    ("orange", "seg2"): 1,
    ("purple", "can0"): 0,
    ("purple", "mcp2515"): 1,
}

# "[I][topo:259]: seg1 <- green.bus0 (912 ms) purple.bus0 (500 ms)"
# "[W][topo:251]: seg1 <- nothing (no beacon in 5000 ms) - alone on this wire, ..."
TOPO_LINE = re.compile(r"\[topo:\d+\]:\s+(\S+)\s+<-\s+(.*)")
HEARD = re.compile(r"(\w+)\.bus(\d)(?:\s+\((\d+) ms\))?(\s+SAME-WIRE|\s+ECHO)?")


def capture(port: str, seconds: float, out: dict, key: str) -> None:
    """Read a console without rebooting it.

    DTR/RTS are deasserted *before* open(), not after: on the C6's
    USB-Serial-JTAG they are the boot-mode controls and macOS asserts both on
    open, which can park the chip in the ROM download stub. Same pattern as
    verify.py — copy it into anything new that opens a bench port.
    """
    try:
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
    except Exception as exc:  # port busy/missing — an empty capture, not a crash
        out[key] = f"__CAPTURE_ERROR__ {exc}"


def parse(board: str, text: str) -> dict[str, dict]:
    """Last [topo] line per port on this board -> what it heard."""
    ports: dict[str, dict] = {}
    for line in text.splitlines():
        match = TOPO_LINE.search(line)
        if match is None:
            continue
        port_id, rest = match.group(1), match.group(2)
        entry = {"heard": [], "flags": set()}
        if not rest.startswith("nothing"):
            for node, bus, age, flag in HEARD.findall(rest):
                entry["heard"].append({"slot": f"{node}.bus{bus}", "age_ms": int(age or 0)})
                if flag and flag.strip():
                    entry["flags"].add(flag.strip())
        # Overwrite: the newest line in the window is the current truth.
        ports[port_id] = entry
    for port_id, entry in ports.items():
        entry["slot"] = slot_of(board, port_id)
    return ports


def slot_of(board: str, port_id: str) -> str | None:
    bus = PORT_BUS.get((board, port_id))
    return None if bus is None else f"{board}.bus{bus}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=25)
    ap.add_argument("--json", action="store_true")
    ap.add_argument(
        "--expect",
        action="append",
        default=[],
        metavar="A=B",
        help="assert two slots share a wire, e.g. green.bus0=orange.bus0",
    )
    args = ap.parse_args()

    boards = [b for b in enumerate_boards() if b["name"]]
    if not boards:
        print("no bench boards attached — run script/hil/ports.py", file=sys.stderr)
        return 2

    logs: dict[str, str] = {}
    threads = [
        threading.Thread(target=capture, args=(b["port"], args.seconds, logs, b["name"]))
        for b in boards
    ]
    if not args.json:
        names = ", ".join(sorted(b["name"] for b in boards))
        print(f"listening {args.seconds:.0f}s to {names} (beacons are 1 Hz, lines every 10 s)\n")
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # slot -> what that port heard; and the inverse, who heard that slot.
    ports: dict[str, dict] = {}
    for board in sorted(logs):
        if logs[board].startswith("__CAPTURE_ERROR__"):
            print(f"WARNING {board}: {logs[board]}", file=sys.stderr)
            continue
        for port_id, entry in parse(board, logs[board]).items():
            entry["board"], entry["port_id"] = board, port_id
            ports[entry["slot"] or f"{board}.{port_id}"] = entry

    # No parsed report at all is NOT a clean bench. Every check below is a loop
    # over `ports`, so an empty dict produces an empty problem list and the
    # final "OK" — a green gate derived from zero evidence, which is the one
    # answer this tool must never give. It happens for a mundane reason: this
    # parser reads the 10-second `topology_rx.yaml` report ("seg1 <- green.bus0"),
    # while boards flashed with the LIVE catcher (tests/hil/topo-live*.yaml)
    # print the MAC-based format instead. Same question, different beacon —
    # read those with script/hil/topo-watch.py.
    if not ports:
        message = (
            f"no [topo] report parsed from any board in {args.seconds:.0f}s — "
            "this is NOT a pass. Either the boards are silent, or they run the "
            "live catcher (tests/hil/topo-live*.yaml), whose beacon format this "
            "parser does not read — use script/hil/topo-watch.py for those."
        )
        if args.json:
            print(json.dumps({"ports": {}, "segments": [], "problems": [message]}, indent=2))
        else:
            print(message, file=sys.stderr)
        return 2

    heard_by: dict[str, list[str]] = {slot: [] for slot in ports}
    for slot, entry in ports.items():
        for item in entry["heard"]:
            heard_by.setdefault(item["slot"], []).append(slot)

    # Physical segments = connected components of the "can hear" graph.
    adjacency: dict[str, set[str]] = {slot: set() for slot in ports}
    for slot, entry in ports.items():
        for item in entry["heard"]:
            if item["slot"] in adjacency:
                adjacency[slot].add(item["slot"])
                adjacency[item["slot"]].add(slot)
    segments, unseen = [], set(ports)
    while unseen:
        stack, group = [unseen.pop()], []
        while stack:
            node = stack.pop()
            group.append(node)
            for peer in adjacency.get(node, ()):
                if peer in unseen:
                    unseen.discard(peer)
                    stack.append(peer)
        segments.append(sorted(group))

    # A board with one healthy port has *proved* its shared supply: the +5 V
    # domain, GPIO7 and JP14 are common to both CAN channels, so a sibling that
    # links rules all three out. Saying "check the rail" anyway is not merely
    # noise, it points at the one thing already known good and costs bench time.
    healthy_boards = {
        entry["board"]
        for slot, entry in ports.items()
        if entry["heard"] and set(heard_by.get(slot, [])) - {slot}
    }

    problems = []
    for slot, entry in sorted(ports.items()):
        listeners = [s for s in heard_by.get(slot, []) if s != slot]
        if not entry["heard"] and not listeners:
            if entry["board"] in healthy_boards:
                problems.append(
                    f"{slot} is electrically absent, but {entry['board']} has another port "
                    f"that links — so its +5 V/GPIO7/JP14 are PROVEN GOOD. Suspect this "
                    f"channel alone: its jumpers (JP20/JP22 arm the CAN2 channel on "
                    f"GPIO10/11), its pair, or its termination"
                )
            else:
                problems.append(
                    f"{slot} is electrically absent — hears nobody and nobody hears it, "
                    f"and no port on {entry['board']} links, so the whole board is suspect"
                )
        elif not listeners:
            problems.append(f"{slot} can receive but nobody hears it — TX side dead")
        elif not entry["heard"]:
            problems.append(f"{slot} is heard by {', '.join(listeners)} but hears nobody — RX side dead")
        if entry["flags"]:
            problems.append(f"{slot} reports {'/'.join(sorted(entry['flags']))} — two ports on one wire")

    for spec in args.expect:
        left, _, right = spec.partition("=")
        if right not in [i["slot"] for i in ports.get(left, {}).get("heard", [])]:
            problems.append(f"expected {left} to hear {right}, it does not")

    if args.json:
        print(
            json.dumps(
                {
                    "ports": {
                        s: {
                            "board": e["board"],
                            "port_id": e["port_id"],
                            "hears": [i["slot"] for i in e["heard"]],
                            "heard_by": sorted(set(heard_by.get(s, [])) - {s}),
                            "flags": sorted(e["flags"]),
                        }
                        for s, e in sorted(ports.items())
                    },
                    "segments": segments,
                    "problems": problems,
                },
                indent=2,
            )
        )
        return 0 if not problems else 1

    print(f"{'port':<22}{'hears':<28}{'heard by':<28}")
    print("-" * 78)
    for slot, entry in sorted(ports.items()):
        hears = ", ".join(i["slot"] for i in entry["heard"]) or "— nothing —"
        listeners = sorted(set(heard_by.get(slot, [])) - {slot})
        label = f"{slot} ({entry['port_id']})"
        print(f"{label:<22}{hears:<28}{', '.join(listeners) or '— nobody —':<28}")

    print("\nsegments measured (connected components of 'can hear'):")
    for index, group in enumerate(segments, 1):
        note = "  <- isolated" if len(group) == 1 else ""
        print(f"  {index}. {' + '.join(group)}{note}")

    if problems:
        print("\nPROBLEMS")
        for problem in problems:
            print(f"  ! {problem}")
        if any("PROVEN GOOD" not in p for p in problems) and healthy_boards != set(
            e["board"] for e in ports.values()
        ):
            print(
                "\nWhere every CAN port on a board is absent, it is a board/power question\n"
                "before a cabling one: the transceivers need +5 V, which needs GPIO7 high\n"
                "AND JP14 bridged. LIN keeps working without it (U9 runs off 12 V), so\n"
                "'LIN fine, every CAN port absent' means the board, not the wire."
            )
        return 1

    print("\nOK — every port hears someone and is heard by someone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
