#!/usr/bin/env python3
"""One command that answers "is this bench sane, and what is it wired to?".

Flashes `tests/hil/selftest.yaml` onto every attached board, reads all the
consoles at once, cross-references what each board heard against what every
other board heard, and writes the whole picture to `tests/hil/BENCH-STATE.md`
for the next person — or the next model — to read before touching anything.

WHY THIS EXISTS
---------------
The bench has cost three sessions to the same misdiagnosis: "no CAN traffic"
read as a wiring fault when the real cause was that the +5 V transceiver domain
was never switched on. The PCB's own docs are blunt about why that keeps
happening (PCB-ESP32C6-Adapter-CAN-Modbus-LIN/docs/MANDATORY-PREREQUISITES.md):

    "An unpowered CAN transceiver reproduces EVERY symptom of broken wiring."

So the fix cannot be a better bus trace — the two are indistinguishable at that
level. It has to be an ordering rule, enforced by a tool that refuses to
speculate:

    a wiring verdict is only ever printed for a port whose transceiver gate
    passed, and a transceiver verdict only for a board whose rail gate passed.

Everything below is in service of that one rule. When a gate fails, this prints
the gate that failed and *stops* — it does not go on to describe the symptoms
downstream of it as though they were independent findings.

WHAT IT MERGES
--------------
Three things that used to be separate, plus one that lived in the other repo:

  script/hil/ports.py        board identity by MAC, never by port path
  script/hil/topology.py     the `[topo]` beacon cross-reference
  tests/hil/probe-*.yaml     the rail / transceiver / SD probes
  the PCB repo's selftest    its "what does this gate transitively prove"
                             framing, and its LED-is-the-whole-UI idea

USAGE
-----
    script/hil/selftest.py                        # the whole bench
    script/hil/selftest.py --board orange         # by bench name
    script/hil/selftest.py --mac 20:6E:F1:0A:D1:E0
    script/hil/selftest.py --port /dev/cu.usbmodem2111201
    script/hil/selftest.py --color orange=ff0000  # override the identity hue
    script/hil/selftest.py --no-flash             # just read what is running
    script/hil/selftest.py --seconds 40

Exit 0 = every gate on every board passed and the topology has no problems.
Exit 1 = something failed; read the report. Exit 2 = nothing to talk to.

A board that will not accept a flash fails the `reach` gate, which sits above
`rail`: it is reported as a row like any other failure, the remaining boards are
still measured, and the report is still written. It used to abort the sweep and
print an esphome traceback instead — which meant the one bench-wide failure this
tool exists to make legible was the one it could not describe.

⚠ Do NOT run this against a live stress rig. The transceiver echo gate pulls
each segment dominant for ~200 us at boot, which is ~100 bit times of
corruption at 500 kbit/s — enough to walk a saturated partner to bus-off. It is
safe during a self-test because every board ends up on the same quiet firmware
and is then reset, so the echo pass sees nothing but 1 Hz beacons; it is not
safe against anything else.

The LIN gate needs a master AND a slave — a master alone puts only headers on
the wire and nothing counts as a received frame. See PROFILES below: Green
drives the schedule, Blue answers it, everyone else listens.
"""

import argparse
import re
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ports import enumerate_boards  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / "tests" / "hil" / "selftest.yaml"
# The LIN master needs a `schedule:`, which linbus's schema rejects for any
# other mode — so it is a separate wrapper around the same file rather than one
# more substitution. Without it nobody drives the bus and every board's LIN gate
# is unmeasurable.
CONFIG_MASTER = REPO / "tests" / "hil" / "selftest-master.yaml"
# ...and one board has to answer it, or the master is only sending headers and
# `frames_received` never moves anywhere on the bus. `responses:` is likewise
# slave-only in the schema.
CONFIG_SLAVE = REPO / "tests" / "hil" / "selftest-slave.yaml"
CONFIG_FOR_MODE = {"master": CONFIG_MASTER, "slave": CONFIG_SLAVE}
REPORT = REPO / "tests" / "hil" / "BENCH-STATE.md"
ESPHOME = REPO / ".venv" / "bin" / "esphome"

# Per-board flashing profile. Identity hue is "hue is identity, brightness is
# state" from hil_common.yaml — the same colour the board wears in every other
# bench config, so a board keeps its identity across firmwares.
#
# `beacons` are the fixed topology slots: slot = node*2 + bus, id = 0x0C0 + slot
# (tests/hil/topology_rx.yaml). They are NOT free to change — topology.py's
# PORT_BUS table decodes them.
PROFILES = {
    # Exactly one master, or the LIN gate is unmeasurable everywhere: listeners
    # can only report the schedule somebody else drives. Green is the bench's
    # LIN master in every other config, so it keeps the role here.
    "green": {
        "color": "00ff00",
        "ports": [("can1", "GPIO2", "GPIO3", "0x0C0"), ("can2", "GPIO10", "GPIO11", "0x0C1")],
        "lin_mode": "master",
        "sd": False,
        # Deliberately not on any segment — see HIL.md ("loose: Blue can2,
        # Green can2"). Declared so the report files them as expected rather
        # than as a problem: a Problems list that always has two entries in it
        # is a Problems list nobody reads.
        "loose": ["can2"],
    },
    "blue": {
        "color": "0033ff",
        "ports": [("can1", "GPIO2", "GPIO3", "0x0C2"), ("can2", "GPIO10", "GPIO11", "0x0C3")],
        "lin_mode": "slave",
        "sd": False,
        "loose": ["can2"],
    },
    "orange": {
        "color": "ff5900",
        "ports": [("seg1", "GPIO2", "GPIO3", "0x0C4"), ("seg2", "GPIO10", "GPIO11", "0x0C5")],
        "lin_mode": "listener",
        "sd": True,
        "loose": [],
    },
    # Mr. Purple is a classic ESP32 on a different carrier: no +5 V sleep
    # domain, no GPIO7 rail, SN65HVD230 transceivers, and GPIO6-11 are its SPI
    # flash pins. selftest.yaml is written for the C6 adapter and would brick
    # the boot on him, so he is skipped rather than guessed at.
    "purple": None,
}

BENCH_V = re.compile(r"\[bench:\d+\]:\s+v1 node=(\S+)")
BENCH_GATE = re.compile(r"\[bench:\d+\]:\s+gate=(\S+)\s+state=(\S+)\s+detail=(\S*)")
# `first_broken=` on a FAIL, `first_unproven=` on an INCONCLUSIVE — both name
# the gate to go and look at, so both parse into the same slot.
BENCH_VERDICT = re.compile(r"\[bench:\d+\]:\s+verdict=(\S+)\s+first_(?:broken|unproven)=(\S+)")
TOPO_LINE = re.compile(r"\[topo:\d+\]:\s+(\S+)\s+<-\s+(.*)")
HEARD = re.compile(r"(\w+)\.bus(\d)(?:\s+\((\d+) ms\))?(\s+SAME-WIRE|\s+ECHO)?")
ANSI = re.compile(r"\x1b\[[0-9;]*m")

# Which (board, port_id) is which topology slot. Mirrors topology.py's table.
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


def slot_of(board, port_id):
    bus = PORT_BUS.get((board, port_id))
    return None if bus is None else f"{board}.bus{bus}"


def hexcolor(s):
    """'ff5900' or '#ff5900' -> ('100.0%', '34.9%', '0.0%') for ESPHome."""
    s = s.lstrip("#")
    if len(s) != 6:
        raise ValueError(f"colour must be RRGGBB, got {s!r}")
    r, g, b = (int(s[i : i + 2], 16) for i in (0, 2, 4))
    return tuple(f"{v / 255 * 100:.1f}%" for v in (r, g, b))


def flash(board, profile, color):
    """Build and upload selftest.yaml for one board. Returns True on success."""
    (pa, pa_tx, pa_rx, ba), (pb, pb_tx, pb_rx, bb) = profile["ports"]
    r, g, b = hexcolor(color)
    # selftest.yaml needs each pin twice — as an ESPHome pin name for
    # can_gateway, and as a bare number for the pre-can_gateway echo lambda,
    # where the IDF wants a gpio_num_t. Deriving the second from the first here
    # is what stops the two forms drifting apart.
    num = lambda p: p.removeprefix("GPIO")  # noqa: E731
    cmd = [
        str(ESPHOME),
        "-s", "node_name", f"mr-{board}",
        "-s", "id_r", r, "-s", "id_g", g, "-s", "id_b", b,
        "-s", "port_a_id", pa, "-s", "pin_a_tx", pa_tx, "-s", "pin_a_rx", pa_rx, "-s", "beacon_a", ba,
        "-s", "port_b_id", pb, "-s", "pin_b_tx", pb_tx, "-s", "pin_b_rx", pb_rx, "-s", "beacon_b", bb,
        "-s", "pin_a_tx_num", num(pa_tx), "-s", "pin_a_rx_num", num(pa_rx),
        "-s", "pin_b_tx_num", num(pb_tx), "-s", "pin_b_rx_num", num(pb_rx),
        "-s", "lin_mode", profile["lin_mode"],
        "-s", "sd_expected", "true" if profile["sd"] else "false",
        "run", "--no-logs",
        str(CONFIG_FOR_MODE.get(profile["lin_mode"], CONFIG)),
        "--device", board_port(board),
    ]
    print(f"  flashing {board} ...", flush=True)
    res = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"  ! {board}: flash failed\n{res.stdout[-1500:]}{res.stderr[-1500:]}", file=sys.stderr)
        return False
    return True


def diagnose_unreachable(board):
    """Why would this board not take a flash? Returns (state, what to do).

    This is the gate ABOVE `rail`, and it exists because the first thing that
    goes wrong on this bench is not a gate at all — it is a board that cannot be
    talked to, which used to abort the whole sweep and print an esphome
    traceback instead of a verdict.

    There are two dead states and they need opposite responses, so guessing
    between them is expensive:

      esptool CAN sync  -> the ROM download stub. No firmware is running, but the
                           chip is fine and a hard reset recovers it in software.
      esptool CANNOT    -> not the stub. esptool speaks to mask ROM, so failing
                           to sync on every reset mode means neither firmware nor
                           a half-written flash explains it. Only a physical
                           power cycle has ever recovered this.

    Both look identical from a bus trace, and in both `ports.py` still lists the
    board — it reads the MAC off the USB descriptor, which the USB-serial
    peripheral answers whether or not the core executes.
    """
    port = board_port(board)
    probe = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "--port", port,
             "--before", "no-reset", "--after", "no-reset", "chip-id"]
    try:
        res = subprocess.run(probe, cwd=REPO, capture_output=True, text=True, timeout=90)
    except subprocess.TimeoutExpired:
        return ("unreachable", "esptool probe timed out")
    if res.returncode == 0:
        return ("ROM download stub",
                f"recoverable in software: python -m esptool --port {port} --after hard-reset chip-id")
    return ("no esptool sync on any reset mode",
            "physical power cycle required — 12 V and re-plug the board; see tests/hil/HIL.md")


def reset_all(names):
    """Reboot every board once the last one has been flashed.

    The transceiver echo gate runs once, early in boot, and is only valid on an
    idle wire. Flashing is sequential, so the first board boots while the others
    are still running whatever they had before — on 2026-07-29 that was a 92 %
    load generator, and all six echo gates came back INDETERMINATE because no
    segment was ever quiet. Resetting everything after the last upload means the
    whole bench re-runs that gate with nothing on the wire but 1 Hz beacons.

    esptool rather than a DTR/RTS poke: on the USB-Serial-JTAG those lines are
    the boot-mode controls, and driving them by hand is how a board ends up
    parked in the ROM download stub looking dead.
    """
    print("resetting all boards so the echo gate runs on a quiet bench ...")
    for name in names:
        res = subprocess.run(
            [str(REPO / ".venv" / "bin" / "python"), "-m", "esptool",
             "--chip", "esp32c6", "--port", board_port(name), "--after", "hard-reset", "chip-id"],
            cwd=REPO, capture_output=True, text=True,
        )
        if res.returncode != 0:
            print(f"  ! {name}: reset failed, its echo gate may read INDETERMINATE", file=sys.stderr)


def board_port(name):
    for b in enumerate_boards():
        if b["name"] == name:
            return b["port"]
    raise SystemExit(f"{name} is not attached")


def capture(port, seconds, sink):
    """Read one console for `seconds`, appending de-ANSI'd lines to `sink`.

    DTR/RTS are cleared BEFORE open on purpose: on the C6's USB-Serial-JTAG they
    are the boot-mode controls and macOS asserts both on open, which can park
    the chip in the ROM download stub — a board that then looks dead on a bench
    that is in fact fine. Same guard as topology.py and verify.py.
    """
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 1
        s.dtr = False
        s.rts = False
        s.open()
    except Exception as e:  # noqa: BLE001 - a dead port must not kill the run
        sink.append(f"!! could not open {port}: {e}")
        return
    end = time.time() + seconds
    try:
        while time.time() < end:
            line = s.readline()
            if line:
                sink.append(ANSI.sub("", line.decode("utf8", "replace")).rstrip())
    finally:
        s.close()


def parse(lines):
    """Pull the gate table and the last `[topo]` line per port out of a console."""
    gates, topo, verdict = {}, {}, None
    for ln in lines:
        m = BENCH_GATE.search(ln)
        if m:
            gates[m.group(1)] = (m.group(2), m.group(3))
            continue
        m = BENCH_VERDICT.search(ln)
        if m:
            verdict = (m.group(1), m.group(2))
            continue
        m = TOPO_LINE.search(ln)
        if m:
            port, rest = m.group(1), m.group(2)
            # Keep the LAST line per port: the newest window is current truth.
            heard, flags = [], []
            if not rest.startswith("nothing"):
                for h in HEARD.finditer(rest):
                    heard.append(f"{h.group(1)}.bus{h.group(2)}")
                    if h.group(4):
                        flags.append(h.group(4).strip())
            topo[port] = {"heard": heard, "flags": flags}
    return gates, topo, verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", action="append", help="bench name; repeatable. Default: all known")
    ap.add_argument("--mac", action="append", help="select by MAC instead of name")
    ap.add_argument("--port", action="append", help="select by USB device path")
    ap.add_argument("--color", action="append", metavar="BOARD=RRGGBB",
                    help="override a board's identity hue")
    ap.add_argument("--seconds", type=float, default=35.0,
                    help="console capture window (default 35; the report is on a 10 s cadence)")
    ap.add_argument("--no-flash", action="store_true", help="read what is already running")
    ap.add_argument("--report", type=Path, default=REPORT)
    args = ap.parse_args()

    attached = {b["name"]: b for b in enumerate_boards() if b["name"]}
    if not attached:
        print("no bench boards attached — run script/hil/ports.py", file=sys.stderr)
        return 2

    # Selection: name, MAC or port all resolve to the same bench names.
    want = set(args.board or [])
    for mac in args.mac or []:
        want |= {n for n, b in attached.items() if b["mac"].upper() == mac.upper()}
    for port in args.port or []:
        want |= {n for n, b in attached.items() if b["port"] == port}
    if not want:
        want = set(attached)

    colors = {}
    for spec in args.color or []:
        name, _, val = spec.partition("=")
        colors[name] = val

    targets, skipped = [], []
    for name in sorted(want):
        if name not in attached:
            skipped.append((name, "not attached"))
        elif PROFILES.get(name) is None:
            skipped.append((name, "no selftest profile — see PROFILES in this script"))
        else:
            targets.append(name)

    if not targets:
        print("nothing to test", file=sys.stderr)
        for n, why in skipped:
            print(f"  {n}: {why}", file=sys.stderr)
        return 2

    unreachable = {}
    if not args.no_flash:
        print(f"flashing {', '.join(targets)} (sequential — shared hub bandwidth)")
        flashed = []
        for name in targets:
            prof = PROFILES[name]
            if flash(name, prof, colors.get(name, prof["color"])):
                flashed.append(name)
                continue
            # One board that cannot be flashed is a finding about that board, not
            # a reason to stop measuring the others. Until 2026-07-29 this
            # returned 1 here and wrote no report at all, so the bench-wide
            # failure it was meant to catch surfaced as a raw esphome traceback.
            state, fix = diagnose_unreachable(name)
            unreachable[name] = (state, fix)
            print(f"  ! {name}: UNREACHABLE — {state}\n      {fix}", file=sys.stderr)
        targets = flashed
        if targets:
            reset_all(targets)
        else:
            print("no board took a flash — every gate below is unmeasurable", file=sys.stderr)

    if targets:
        print(f"listening {args.seconds:.0f}s to {', '.join(targets)} ...")
    sinks, threads = {}, []
    for name in targets:
        sinks[name] = []
        # Re-resolve the port: flashing resets the board and the hub renumbers.
        t = threading.Thread(target=capture, args=(board_port(name), args.seconds, sinks[name]))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()

    results = {n: parse(sinks[n]) for n in targets}
    loose = {
        slot_of(n, port)
        for n in targets
        for port in (PROFILES[n].get("loose") or [])
        if slot_of(n, port)
    }
    text = render(results, skipped, loose, unreachable, did_flash=not args.no_flash,
                  invocation=sys.argv[1:])
    print(text)
    args.report.write_text(text)
    try:
        shown = args.report.relative_to(REPO)
    except ValueError:
        shown = args.report  # --report may point anywhere
    print(f"\nwritten to {shown}")

    failed = any(v and v[0] != "PASS" for _, _, v in results.values())
    silent = any(not g for g, _, _ in results.values())
    return 1 if (failed or silent or unreachable) else 0


def render(results, skipped, loose=frozenset(), unreachable=None, did_flash=True, invocation=None):
    """Build the report. Ordering is the product here, not decoration."""
    now = datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")
    # The header carries the exact invocation, because this file is tracked and a later,
    # narrower run overwrites an earlier, fuller one — which is fine exactly as long as the
    # report says so itself. A --no-flash overwrite cost a good report once (restored from
    # git); the header is what stops a stale or partial report from implying authority.
    argv = " ".join(invocation or [])
    cmd = "script/hil/selftest.py" + (f" {argv}" if argv else "")
    out = [
        "# Bench state",
        "",
        f"Measured {now} by `{cmd}`. Regenerate rather than edit.",
        "",
    ]
    caveats = []
    if invocation and any(a in ("--board", "--mac", "--port") for a in invocation):
        caveats.append("**Partial run** — boards not selected were not measured. Their state "
                       "here is absent, not passing.")
    if not did_flash:
        caveats.append("**`--no-flash`** — this read whatever firmware was already running, "
                       "which may not be the selftest firmware; a gate can FAIL or read "
                       "INDETERMINATE for that reason alone, and this report may say less "
                       "than the one it overwrote (`git diff` before trusting it).")
    for c in caveats:
        out += [c, ""]
    out += [
        "**Read the gates before the wiring.** Each gate is a precondition for the",
        "one below it. An unpowered CAN transceiver reproduces every symptom of",
        "broken wiring — no echo, no ACK, TEC to bus-off, nothing received — so a",
        "topology line from a board whose `xcvr` gate failed is not evidence about",
        "cabling and is reported as INDETERMINATE here on purpose.",
        "",
        "| gate | proves |",
        "|---|---|",
        "| `reach` | the board can be talked to at all — it accepted a flash. Above every gate below it: an unreachable board has no measurements, only a recovery. |",
        "| `rail` | 12 V on J8 (P1) **and** GPIO7 high (P2). The GPIO6 sense is itself gated by GPIO7, so 0 V means *GPIO7 low **or** no 12 V*, never \"no 12 V\" alone. |",
        "| `xcvr.<port>` | the +5 V domain (U18, **JP14**), the TJA1044, and the logic-side jumpers (JP20/JP22 arm the GPIO10/11 channel). One TXD→RXD echo, no fixture needed. |",
        "| `bus` | that traffic **survives** on the wire — the controller's own `bus_err` / `tx_fail` / TEC / REC, judged on movement between windows. The echo above proves the transceiver; this proves the segment. A missing terminator or a long stub echoes perfectly and still shreds frames. |",
        "| `lin` | the LIN transceiver — which runs off 12 V and does **not** need +5 V. LIN healthy with both CAN ports dead is the signature of JP14 open. |",
        "| `lin.selftest` | master only — the LIN schedule is answered **on time**, not merely answered. A wire that works but jitters passes `lin` and fails this. |",
        "| `sd` | the card answering CMD0, bit-banged, with no component in the way. |",
        "",
    ]

    if skipped:
        out += ["## Not tested", ""]
        out += [f"- **{n}** — {why}" for n, why in skipped]
        out.append("")

    if unreachable:
        out += [
            "## Unreachable — no gate below this was measured", "",
            "These boards would not accept a flash, so nothing further about them",
            "is known. `ports.py` still lists them: it reads the MAC off the USB",
            "descriptor, which the serial peripheral answers whether or not the",
            "core is executing. Recover them, then re-run.", "",
            "| board | state | recovery |",
            "|---|---|---|",
        ]
        out += [f"| **{n}** | {state} | {fix} |" for n, (state, fix) in sorted(unreachable.items())]
        out.append("")

    out += ["## Gates", "",
            "| board | reach | rail | xcvr | bus | lin | sd | verdict |",
            "|---|---|---|---|---|---|---|---|"]
    for name, (state, _fix) in sorted((unreachable or {}).items()):
        out.append(
            f"| **{name}** | FAIL ({state}) | — | — | — | — | — | "
            f"**FAIL** (first broken: `reach`) |"
        )
    for name, (gates, _topo, verdict) in sorted(results.items()):
        if not gates:
            why = ("took the flash, then said nothing" if did_flash
                   else "no `[bench]` line — is selftest.yaml the running firmware?")
            out.append(
                f"| **{name}** | {'PASS' if did_flash else 'n/a'} | — | — | — | — | — | "
                f"**SILENT — {why}** |"
            )
            continue
        xc = " / ".join(
            f"{k.split('.', 1)[1]}:{v[0]}" for k, v in sorted(gates.items()) if k.startswith("xcvr.")
        )
        v = f"**{verdict[0]}**" if verdict else "**—**"
        if verdict and verdict[1] != "none":
            word = "first broken" if verdict[0] == "FAIL" else "unproven"
            v += f" ({word}: `{verdict[1]}`)"
        sd = gates.get("sd", ("—", ""))
        sd_cell = sd[0] if sd[0] != "SKIP" else "n/a"
        if sd[1]:
            sd_cell += f" ({sd[1]})"
        # lin.selftest rides in the lin cell: it is the same subsystem, and a
        # column that is empty on every board but one is wasted width.
        lin_cell = gates.get("lin", ("—",))[0]
        if "lin.selftest" in gates:
            lin_cell += f" (+selftest {gates['lin.selftest'][0]})"
        out.append(
            f"| **{name}** | {'PASS' if did_flash else 'n/a'} | {gates.get('rail', ('—',))[0]} "
            f"({gates.get('rail', ('', '—'))[1]}) | {xc or '—'} | "
            f"{gates.get('bus', ('—',))[0]} | {lin_cell} | {sd_cell} | {v} |"
        )
    out.append("")

    # Topology, but only from ports that earned the right to be believed.
    hears, muted = {}, []
    for name, (gates, topo, _v) in sorted(results.items()):
        for port_id, info in sorted(topo.items()):
            slot = slot_of(name, port_id)
            if slot is None:
                continue
            gate = gates.get(f"xcvr.{port_id}", ("SKIP", ""))[0]
            if gate != "PASS":
                muted.append((slot, port_id, gate))
                continue
            hears[slot] = info

    out += ["## Wiring", ""]
    if muted:
        out += [
            "These ports are **excluded** from the wiring picture — their transceiver",
            "gate did not pass, so what they did or did not hear says nothing about",
            "the cable:",
            "",
        ]
        out += [f"- `{s}` (`{p}`) — xcvr gate {g}" for s, p, g in muted]
        out.append("")

    if not hears:
        out += ["No port qualified. Fix the gates above before looking at any wire.", ""]
    else:
        heard_by = {s: [] for s in hears}
        for s, info in hears.items():
            for peer in info["heard"]:
                heard_by.setdefault(peer, []).append(s)
        out += ["| port | hears | heard by |", "|---|---|---|"]
        for s in sorted(hears):
            h = ", ".join(f"`{x}`" for x in hears[s]["heard"]) or "— nothing —"
            hb = ", ".join(f"`{x}`" for x in sorted(heard_by.get(s, []))) or "— nobody —"
            flags = " ".join(hears[s]["flags"])
            out.append(f"| `{s}`{' **' + flags + '**' if flags else ''} | {h} | {hb} |")
        out.append("")

        # Segments = connected components of "can hear".
        seen, segments = set(), []
        for s in sorted(hears):
            if s in seen:
                continue
            comp, stack = set(), [s]
            while stack:
                cur = stack.pop()
                if cur in comp:
                    continue
                comp.add(cur)
                stack += [p for p in hears.get(cur, {}).get("heard", []) if p not in comp]
                stack += [p for p in heard_by.get(cur, []) if p not in comp]
            seen |= comp
            segments.append(sorted(comp))
        out += ["Segments measured (connected components of \"can hear\"):", ""]
        for i, comp in enumerate(segments, 1):
            out.append(f"{i}. " + (" ↔ ".join(f"`{c}`" for c in comp) if len(comp) > 1 else f"`{comp[0]}` — isolated"))
        out.append("")

        problems, expected = [], []
        for s in sorted(hears):
            h, hb = hears[s]["heard"], heard_by.get(s, [])
            if not h and not hb:
                if s in loose:
                    expected.append(f"`{s}` is isolated — declared loose in PROFILES, so this is the expected state.")
                else:
                    problems.append(f"`{s}` hears nobody and is heard by nobody. Its xcvr gate PASSED, so the board is fine — this is a cable, a jumper or a termination.")
            elif not hb:
                problems.append(f"`{s}` hears others but nobody hears it — TX side open.")
            elif not h:
                problems.append(f"`{s}` is heard but hears nothing — RX side open.")
            if hears[s]["flags"]:
                problems.append(f"`{s}` reports {' '.join(hears[s]['flags'])} — two ports on one physical wire.")
        if problems:
            out += ["### Problems", ""] + [f"- {p}" for p in problems] + [""]
        else:
            out += ["No unexpected wiring problems: every qualified port either "
                    "hears someone and is heard by someone, or is declared loose.", ""]
        if expected:
            out += ["### Expected loose", ""] + [f"- {e}" for e in expected] + [""]

    return "\n".join(out)


if __name__ == "__main__":
    sys.exit(main())
