#!/usr/bin/env python3
"""Enumerate the HIL bench by identity, not by port path.

Port paths are not stable. They change when a board resets, when esptool drops
it into the bootloader, and when anything upstream on the hub re-enumerates —
`tests/hil/HIL.md` has already drifted twice. Never cache them; run this instead.

Two stable identifiers are available without touching the board:

  MAC       ESP32 native-USB (USB-JTAG/serial) devices report their MAC as the
            USB serial number, so the identity is readable straight from the
            descriptor. No reset, no bootloader, no flash.
  LOCATION  the physical hub-port path (e.g. 2-1.4.3). Stable until the cable
            is physically moved to a different socket.

Mr. Purple is a CP2102 bridge, not native USB, so he has no MAC in his
descriptor — he reports serial "0001" and is matched on that plus his VID:PID.

Usage:
    script/hil/ports.py                 # table of everything attached
    script/hil/ports.py purple          # print just that board's port, or exit 1
    script/hil/ports.py --json          # machine-readable
"""

import json
import sys

from serial.tools import list_ports

# MAC -> bench name. Taken from tests/hil/HIL.md plus what has since appeared.
# 20:6E:F1:0A:D5:AC was the original Mr. Green; retired 2026-07-26 and replaced
# by the board below. 20:6E:F1:0A:D0:F0 (blue) and 20:6E:F1:0A:D1:E0 (orange)
# were retired 2026-08-04 and replaced by the two boards below, confirmed by
# the user against the physical bench. Left out deliberately — two MACs
# mapping to one name would report two of that board on the hub.
KNOWN_MACS = {
    "F0:F5:BD:0E:DD:50": "green",
    "AC:EB:E6:C1:47:08": "blue",
    "20:6E:F1:15:68:0C": "orange",
    # Purple is a CP2102 bridge, so this MAC is NOT readable over USB — it is
    # matched on VID:PID + serial below. The MAC is here anyway because the CAN
    # topology beacon (tests/hil/topology_live.h) identifies boards by MAC, and
    # script/hil/topo-watch.py resolves those to names through this same table.
    # Learned from the beacon on 2026-07-27; harmless as a USB entry because no
    # descriptor will ever report it.
    "94:B9:7E:E4:80:C0": "purple",
}

# Non-native-USB boards, matched on VID:PID + USB serial instead of MAC.
KNOWN_BRIDGES = {
    (0x10C4, 0xEA60, "0001"): "purple",
}

# Roles as of the 2026-07-27 topology (tests/hil/HIL.md): Green and
# Purple sit on DIFFERENT ports of Orange, so the DUT is the only path between
# them. Blue is out of the ring and off both segments.
#
# 2026-08-04: blue and orange are new physical boards (see KNOWN_MACS above).
# Per the user, orange now carries the real BMS on CAN2, blue is on CAN1 —
# not yet re-verified against selftest.py, so treat this as unconfirmed intent
# rather than a proven wiring fact.
ROLES = {
    "green": "Seg1 load generator (CAN1) + LIN master",
    "blue": "CAN1 (role otherwise unconfirmed since 2026-08-04 board swap)",
    "orange": "DUT: gateway seg1<->seg2 + BMS on CAN2 + SD logger (unconfirmed since swap)",
    "purple": "Seg2 ACK partner + witness (native TWAI), classic ESP32",
}


def enumerate_boards():
    """Return one dict per attached board, best-effort identified."""
    out = []
    for p in sorted(list_ports.comports(), key=lambda x: x.device):
        vid, pid = p.vid or 0, p.pid or 0
        serial = p.serial_number or ""
        # Espressif native USB-JTAG/serial: VID 0x303a, serial *is* the MAC.
        if vid == 0x303A:
            mac = serial.upper()
            name = KNOWN_MACS.get(mac)
        else:
            mac = ""
            name = KNOWN_BRIDGES.get((vid, pid, serial))
            if name is None:
                # Not a board we know and not an ESP - skip Bluetooth etc.
                if vid == 0 or "Bluetooth" in p.device or "debug-console" in p.device:
                    continue
                if vid != 0x10C4:  # CP210x is the only bridge on this bench
                    continue
        out.append(
            {
                "name": name,
                "port": p.device,
                "mac": mac,
                "location": p.location or "",
                "vid_pid": f"{vid:#06x}:{pid:#06x}",
                "product": p.product or "",
            }
        )
    return out


def main():
    args = [a for a in sys.argv[1:]]
    as_json = "--json" in args
    args = [a for a in args if a != "--json"]

    boards = enumerate_boards()

    if args:
        want = args[0].lower()
        for b in boards:
            if b["name"] == want:
                print(b["port"])
                return 0
        print(f"'{want}' is not attached", file=sys.stderr)
        known = ", ".join(sorted(b["name"] for b in boards if b["name"])) or "none"
        print(f"attached and identified: {known}", file=sys.stderr)
        return 1

    if as_json:
        print(json.dumps(boards, indent=2))
        return 0

    if not boards:
        print("no bench boards attached")
        return 1

    name_w = max(len(b["name"] or "UNKNOWN") for b in boards)
    port_w = max(len(b["port"]) for b in boards)
    for b in boards:
        name = b["name"] or "UNKNOWN"
        ident = b["mac"] or b["vid_pid"]
        role = ROLES.get(b["name"], "not in HIL.md - identify before flashing")
        print(f"{name:<{name_w}}  {b['port']:<{port_w}}  {ident:<17}  @{b['location']:<10}  {role}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
