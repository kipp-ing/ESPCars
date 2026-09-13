#!/usr/bin/env python3
"""Turn DBC signal definitions into `can_gateway` sensor YAML.

Hand-transcribing bit offsets out of a DBC is how a decode goes quietly wrong:
an off-by-one in a start bit still produces plausible numbers, and nothing on
the bench will tell you. This does the transcription mechanically, and refuses
the cases the component cannot actually decode instead of emitting config that
looks fine and reads garbage.

    script/dbc2yaml.py <file.dbc> --id 0x100 --id 0x101 --port seg2

DBC files in the wild are ISO-8859-1 with CRLF, not UTF-8 — we decode as
latin-1 explicitly rather than letting a stray 0xB0 ('°') abort the parse.

## Why some signals are refused

`can_gateway`'s bit-level form models the payload as one 64-bit little-endian
word and takes bits `[bit_offset, bit_offset + bit_length)` — which is exactly
the DBC Intel (`@1`) start-bit convention, so those map across unchanged.

Motorola (`@0`) signals do not: their bits run the other way through the frame.
The component offers no bit-level big-endian form (see `validate_signal_position`
in components/can_gateway/__init__.py), only a *byte-aligned* one via
`byte_order: big`. So a Motorola signal is emitted only when it is byte-aligned
and a whole number of bytes wide; anything else is reported as unsupported
rather than silently mistranslated. A 10-bit Motorola SOC signal is a real
example seen in a vehicle DBC in the wild — it needs a component change to
decode, not a cleverer generator.
"""

import argparse
import re
import sys

# SG_ <name> : <start>|<len>@<order><sign> (<factor>,<offset>) [<min>|<max>] "<unit>" <receivers>
SG_RE = re.compile(
    r'^\s*SG_\s+(?P<name>\w+)\s*:\s*'
    r'(?P<start>\d+)\|(?P<len>\d+)@(?P<order>[01])(?P<sign>[+-])\s*'
    r'\((?P<factor>[^,]+),(?P<offset>[^)]+)\)\s*'
    r'\[(?P<min>[^|]*)\|(?P<max>[^\]]*)\]\s*"(?P<unit>[^"]*)"'
)
BO_RE = re.compile(r'^BO_\s+(?P<id>\d+)\s+(?P<name>\w+)\s*:\s*(?P<dlc>\d+)')
# BA_ "GenSigSNA" SG_ <msg id> <signal> "7h";  -- the per-signal not-available raw
SNA_RE = re.compile(
    r'^BA_\s+"GenSigSNA"\s+SG_\s+(?P<id>\d+)\s+(?P<sig>\w+)\s+"(?P<val>[0-9A-Fa-f]+)h"',
    re.M,
)
# VAL_ <msg id> <signal> <raw> "<label>" ... ;
VAL_RE = re.compile(r'^VAL_\s+(?P<id>\d+)\s+(?P<sig>\w+)\s+(?P<body>.*?);', re.M)
VAL_PAIR_RE = re.compile(r'(\d+)\s+"([^"]*)"')


def parse_sna(text):
    """(can_id, signal) -> raw value meaning not-available.

    The DBC states this per signal in a `GenSigSNA` attribute rather than by a
    file-wide rule, and it is not always all-ones — so we read it rather than
    infer it. A value of 0 means the signal has no SNA at all (that is how every
    1-bit flag is marked), which is why 0 is dropped here instead of being
    treated as a raw value that could match real data.
    """
    out = {}
    for m in SNA_RE.finditer(text):
        raw = int(m["val"], 16)
        if raw:
            out[(int(m["id"]) & 0x1FFFFFFF, m["sig"])] = raw
    return out


def parse_val(text):
    """(can_id, signal) -> {raw: label} from the VAL_ enumeration tables."""
    out = {}
    for m in VAL_RE.finditer(text):
        key = (int(m["id"]) & 0x1FFFFFFF, m["sig"])
        out[key] = {int(r): lab for r, lab in VAL_PAIR_RE.findall(m["body"])}
    return out


def parse(text):
    """DBC text -> {can_id: {"name": str, "dlc": int, "signals": [dict]}}."""
    messages = {}
    current = None
    for line in text.splitlines():
        bo = BO_RE.match(line)
        if bo:
            # DBC ids carry the extended-frame flag in bit 31.
            raw = int(bo["id"])
            current = {
                "id": raw & 0x1FFFFFFF,
                "extended": bool(raw & (1 << 31)),
                "name": bo["name"],
                "dlc": int(bo["dlc"]),
                "signals": [],
            }
            messages[current["id"]] = current
            continue
        sg = SG_RE.match(line)
        if sg and current is not None:
            current["signals"].append(
                {
                    "name": sg["name"],
                    "start": int(sg["start"]),
                    "length": int(sg["len"]),
                    "little_endian": sg["order"] == "1",
                    "signed": sg["sign"] == "-",
                    "factor": float(sg["factor"]),
                    "offset": float(sg["offset"]),
                    "unit": sg["unit"],
                }
            )
    return messages


def position(sig):
    """Decode position for a signal, or a reason it cannot be decoded.

    Returns (kind, kwargs, reason). `kind` is "bit", "byte" or None; when None,
    `reason` says why — the caller reports it rather than emitting config.
    """
    if sig["length"] > 32:
        return None, {}, f"{sig['length']} bits — wider than the 32-bit decode limit"
    if sig["little_endian"]:
        if sig["start"] + sig["length"] > 64:
            return None, {}, "runs past the 64-bit frame"
        return "bit", {"bit_offset": sig["start"], "bit_length": sig["length"]}, ""
    # Motorola: only the byte-aligned form exists.
    #
    # A DBC Motorola start bit names the field's MOST significant bit in the
    # "sawtooth" numbering where bit 7 of byte 0 is 7 and bit 0 of byte 0 is 0.
    # So a byte-aligned big-endian field starts at the top of a byte — start % 8
    # == 7, not 0. Every byte-multiple Motorola signal in a real vehicle DBC
    # this was checked against obeys this; testing for 0 instead silently
    # refuses all of them, which looks like conservatism and is really a bug.
    if sig["length"] % 8 or sig["start"] % 8 != 7:
        return (
            None,
            {},
            f"Motorola and not byte-aligned (start {sig['start']}, "
            f"{sig['length']} bits) — no bit-level big-endian decode exists",
        )
    byte_offset = sig["start"] // 8
    n = sig["length"] // 8
    if byte_offset + n > 8:
        return None, {}, "runs past the 8-byte frame"
    return "byte", {"offset": byte_offset, "length": n, "byte_order": "big"}, ""


def sna_for(msg, sig, sna_map, val_map, enabled):
    """The `sna:` value for one signal, or None to leave SNA handling off.

    Two ways this must not fire, both of which produce a silently wrong bench:

    - The signal has no SNA (`GenSigSNA` absent or 0). Every 1-bit flag is in
      this class; mapping its "1" to NaN would erase real state.
    - All-ones is an enumerated *meaning* rather than "not available". Several
      value tables in a real vehicle DBC this was checked against do this,
      including a contactor-fault flag whose all-ones raw value is a genuine
      fault state, not a missing reading. Turning that into NaN would hide
      exactly the event worth seeing, so we refuse and say why rather than
      emitting a plausible-looking sensor.
    """
    if not enabled:
        return None
    raw = sna_map.get((msg["id"], sig["name"]))
    if raw is None:
        return None
    label = val_map.get((msg["id"], sig["name"]), {}).get(raw)
    if label and "SNA" not in label.upper():
        print(
            f"  # NO SNA for {sig['name']}: raw {raw} is enumerated {label!r}, "
            "not not-available",
            file=sys.stderr,
        )
        return None
    return "all_ones" if raw == (1 << sig["length"]) - 1 else raw


def emit(msg, sig, port, sna, throttle):
    """One `- platform: can_gateway` sensor block, or None if undecodable."""
    kind, kw, reason = position(sig)
    if kind is None:
        print(f"  # SKIPPED {sig['name']}: {reason}", file=sys.stderr)
        return None

    lines = [
        f"  # {msg['name']} :: {sig['name']}",
        "  - platform: can_gateway",
        f"    name: \"{msg['name']} {sig['name']}\"",
        f"    port_id: {port}",
        f"    can_id: 0x{msg['id']:03X}",
    ]
    if msg["extended"]:
        lines.append("    use_extended_id: true")
    for key, value in kw.items():
        lines.append(f"    {key}: {value}")
    if sig["signed"]:
        lines.append("    signed: true")
    if sna is not None:
        lines.append(f"    sna: {sna}")
    if throttle:
        lines.append(f"    throttle: {throttle}")
    if sig["unit"]:
        lines.append(f'    unit_of_measurement: "{sig["unit"]}"')

    # Scaling is ESPHome's job, not the component's: factor/offset become
    # standard sensor filters so NaN from an SNA raw value passes through
    # untouched (multiply/offset propagate NaN).
    filters = []
    if sig["factor"] != 1:
        filters.append(f"      - multiply: {sig['factor']:g}")
    if sig["offset"] != 0:
        filters.append(f"      - offset: {sig['offset']:g}")
    if filters:
        lines.append("    filters:")
        lines.extend(filters)
    # Decimals that suit the factor: 0.1 -> 1 place, 1 -> 0 places.
    if sig["factor"] < 1:
        lines.append(f"    accuracy_decimals: {len(str(sig['factor']).split('.')[-1])}")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dbc")
    ap.add_argument("--id", action="append", default=[], metavar="0x100",
                    help="message id to emit; repeatable. Default: all.")
    ap.add_argument("--signal", action="append", default=[], metavar="NAME",
                    help="emit only these signal names; repeatable.")
    ap.add_argument("--port", default="seg2", help="can_gateway port id")
    ap.add_argument("--no-sna", action="store_true",
                    help="omit the sna key even where the DBC declares one")
    ap.add_argument("--throttle", default="", help="e.g. 1s")
    args = ap.parse_args(argv)

    text = open(args.dbc, encoding="latin-1").read()
    messages = parse(text)
    sna_map = parse_sna(text)
    val_map = parse_val(text)
    wanted = [int(x, 0) for x in args.id] or sorted(messages)
    blocks = []
    for mid in wanted:
        msg = messages.get(mid)
        if msg is None:
            print(f"  # 0x{mid:03X} not in {args.dbc}", file=sys.stderr)
            continue
        for sig in msg["signals"]:
            if args.signal and sig["name"] not in args.signal:
                continue
            sna = sna_for(msg, sig, sna_map, val_map, not args.no_sna)
            block = emit(msg, sig, args.port, sna, args.throttle)
            if block:
                blocks.append(block)
    if blocks:
        print("sensor:")
        print("\n\n".join(blocks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
