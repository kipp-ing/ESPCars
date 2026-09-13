#!/usr/bin/env python3
"""Compile, inspect and flash `.dcat` diagnostic catalogs (design doc §7).

Hand-transcribing DIDs, bit offsets and scale factors out of a diagnostic
database is how a decode goes quietly wrong — an off-by-one byte still
produces plausible numbers. This tool does the transcription mechanically from
a JSON export produced by a vendor-specific conversion tool for the factory
diagnostic database, and refuses what the format cannot represent instead of
emitting a catalog that looks fine and reads garbage.

    script/uds_catalog.py compile my-ecu.json --profile catalogs/my-ecu.profile.yaml \
        -o catalogs/my-ecu.dcat
    script/uds_catalog.py dump catalogs/my-ecu.dcat
    script/uds_catalog.py verify catalogs/my-ecu.dcat
    script/uds_catalog.py flash catalogs/my-ecu.dcat --table partition-table.bin

The factory database (several megabytes of JSON) stays outside this repo; what
is checked in is the compiled `.dcat`, the profile that selected it, the
manifest that indexes it, and the command in the profile's header — the same
precedent as the DBC in `script/dbc2yaml.py`. `tests/uds/fixtures/mini-source.json`
is a small, hand-authored stand-in shaped the same way, for a worked example
that needs no factory export.

## What the compiler decides (and where to review it)

- **Grouping.** Services sharing request bytes merge into one group (format
  doc §4); the canonical name is their longest common `_`-prefix.
- **Field names, unique within a group** (§5.1a). A name is invented only when
  nothing factual is left: the presentation qualifier, else the originating
  service qualifier, else the presentation qualifier plus `@<byte>.<bit>`. The
  manifest records each field's final name, the rule that produced it, and what
  it would otherwise have been.
- **Arrays are collapsed** (§5.1b) into one record with `repeat_count` /
  `repeat_stride`, so cell 42 is `element: 42` rather than a name differing
  from cell 41 by an offset. See `_find_array_runs` for the three guards that
  keep sibling fields (max/min/mean at constant stride) out of arrays — the
  database is full of runs that look like arrays and are not.
- **INVALID sentinels.** A scale row whose text matches the documented
  pattern list (`not available` incl. the factory's `not asvailable` typo,
  `nicht verfügbar`, `invalid`, `SNA`, `no signal`) is marked INVALID; the
  profile can override any row by text, because an all-ones raw is *usually*
  "not available" but sometimes a real fault state (private/notes/HANDOVER-guest-bms.md
  §5). Every decision lands in the manifest for review.
- **Unbounded linear rows.** The CBF stores bounds bitflag-encoded; a linear
  row with bounds (0, 0) and a real factor means "bounds not present", not
  "only raw 0 is valid" — those rows widen to the full i32 range, with a
  warning. Without this, a real unbounded linear DID decodes to NaN for every
  nonzero raw.
- **Unsigned bounds.** The factory writes `[1 .. -1]` for a full-range
  unsigned 32-bit row (`-1` is `0xFFFFFFFF`); negative bounds on an unsigned
  field are reinterpreted unsigned and clamped to `i32` max, with a warning.
  A *sentinel* row that `i32` cannot express is **dropped instead of clamped**
  (§7a): clamping would hide it behind the linear row, while dropping is
  lossless because §6's no-row-matches rule already yields not-available for
  a raw outside every row — which is exactly what the sentinel meant.
- **Oversized dumps are dropped, loudly.** ASCII/HEXDUMP fields wider than
  248 bits (31 bytes — the u8 `bit_size` ceiling) cannot be represented; a
  real factory database can carry whole-payload hexdump conveniences
  thousands of bytes long that duplicate other fields byte for byte. They
  are skipped, warned about, and listed in the manifest — never silently.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_catalog_module():
    """Import components/uds/catalog.py by path.

    `components/uds/` deliberately has no `__init__.py` yet (the ESPHome
    codegen entry point lands with the component), so the shared module is
    loaded by file location — the same code ESPHome will import as
    `esphome.components.uds.catalog` once the package exists.
    """
    path = REPO_ROOT / "components" / "uds" / "catalog.py"
    spec = importlib.util.spec_from_file_location("uds_catalog_lib", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses needs the module registered
    spec.loader.exec_module(module)
    return module


catalog = _load_catalog_module()

# Sentinel pattern list (format doc §6). Word-bounded, case-insensitive: this
# must catch 'Signal Not Available' and the factory's 'not asvailable' typo
# without ever matching a real state name by accident.
INVALID_TEXT_PATTERNS = (
    "not available",
    "not asvailable",
    "nicht verfügbar",
    "invalid",
    "sna",
    "no signal",
)
_INVALID_RES = [
    re.compile(r"\b" + re.escape(p) + r"\b") for p in INVALID_TEXT_PATTERNS
]


def is_invalid_text(text: str) -> bool:
    folded = text.casefold()
    return any(rx.search(folded) for rx in _INVALID_RES)


def canonical_group_name(qualifiers: list[str]) -> str:
    """Longest common `_`-separated prefix, else the first qualifier (§4)."""
    parts = [q.split("_") for q in qualifiers]
    common: list[str] = []
    for column in zip(*parts):
        if all(tok == column[0] for tok in column[1:]):
            common.append(column[0])
        else:
            break
    return "_".join(common).rstrip("_") or qualifiers[0]


# --- compile ------------------------------------------------------------------


class CompileError(Exception):
    pass


def _presentation_of(prep: dict, ecu_json: dict) -> dict | None:
    if prep["field_type"] == "Presentation":
        return ecu_json["global_presentations"][prep["pres_pool_index"]]
    if prep["field_type"] == "InternalPresentation":
        return ecu_json["global_internal_presentations"][prep["info_pool_index"]]
    return None


def _build_scales(
    pres: dict,
    field_name: str,
    signed: bool,
    bit_size: int,
    overrides: list[dict],
    warnings: list[str],
    dropped: list[dict],
) -> tuple:
    rows = []
    for sc in pres.get("scales") or []:
        low, high = sc["enum_low_bound"], sc["enum_up_bound"]
        factor, offset = sc["multiply_factor"], sc["add_const_offset"]
        text = sc.get("enum_description") or ""
        if not text and (low, high) == (0, 0) and (factor, offset) != (0.0, 0.0):
            # Bitflag-encoded CBF: absent bounds read as zero. A linear row
            # "valid only for raw 0" would be nonsense; widen to the field's
            # full representable range.
            low, high = (0 if not signed else catalog.I32_MIN), catalog.I32_MAX
            warnings.append(
                f"{field_name}: linear row had bounds (0,0); widened to full range"
            )
        elif not signed:
            unrepresentable = [b & 0xFFFFFFFF for b in (low, high) if b < 0]
            if unrepresentable and text:
                # §7a: a sentinel an i32 bound cannot express is DROPPED, not
                # clamped. Clamping would park it on the linear row's end and
                # shadow that row; dropping is lossless because §6's final rule
                # already makes a raw outside every row not-available, which is
                # what the sentinel said.
                dropped.append(
                    {"field": field_name, "text": text,
                     "reason": f"sentinel bound {unrepresentable[0]} exceeds i32"}
                )
                warnings.append(
                    f"{field_name}: sentinel row {text!r} at raw "
                    f"{unrepresentable[0]} does not fit i32; dropped — the "
                    f"no-row-matches rule still yields not-available"
                )
                continue
            fixed = []
            for bound in (low, high):
                if bound < 0:
                    unsigned = bound & 0xFFFFFFFF
                    bound = min(unsigned, catalog.I32_MAX)
                    warnings.append(
                        f"{field_name}: negative bound on unsigned field read as "
                        f"{unsigned}, clamped to i32 max"
                    )
                fixed.append(bound)
            low, high = fixed
        invalid = bool(text) and is_invalid_text(text)
        for ov in overrides:
            if ov.get("text") == text and ov.get("field", field_name) == field_name:
                invalid = bool(ov["invalid"])
        rows.append(
            catalog.Scale(
                low=low, high=high, factor=factor, offset=offset,
                text=text, invalid=invalid,
            )
        )
    return tuple(rows)


def _build_field(
    svc: dict,
    prep: dict,
    ecu_json: dict,
    overrides: list[dict],
    warnings: list[str],
    skipped: list[dict],
    dropped: list[dict],
) -> "catalog.Field | None":
    name = prep["qualifier"] or svc["qualifier"]
    size = prep["size_in_bits"]
    if not size:
        skipped.append(
            {"service": svc["qualifier"], "field": name,
             "reason": f"no width ({prep['field_type']}, {prep.get('size_error')})"}
        )
        warnings.append(f"{svc['qualifier']}/{name}: no width, skipped")
        return None

    pres = _presentation_of(prep, ecu_json)
    idt = pres["internal_data_type"] if pres else None
    is_ascii = idt == 6
    is_hexdump = idt == 2 or (
        pres is None
        and prep["field_type"] in ("BitDump", "ExtendedBitDump")
        and size % 8 == 0
    )
    if (is_ascii or is_hexdump) and size > catalog.MAX_BLOCK_BITS:
        skipped.append(
            {"service": svc["qualifier"], "field": name,
             "reason": f"{size}-bit dump exceeds the {catalog.MAX_BLOCK_BITS}-bit "
                       f"(u8 bit_size) ceiling"}
        )
        warnings.append(
            f"{svc['qualifier']}/{name}: {size}-bit dump cannot be represented, skipped"
        )
        return None
    if not (is_ascii or is_hexdump) and size > catalog.MAX_NUMERIC_BITS:
        raise CompileError(
            f"{svc['qualifier']}/{name}: numeric field of {size} bits exceeds the "
            f"64-bit extraction accumulator — the format cannot hold it"
        )

    signed = bool(pres and pres.get("sign_bit")) and not (is_ascii or is_hexdump)
    scales = ()
    unit = ""
    if pres and not (is_ascii or is_hexdump):
        unit = pres.get("displayed_unit") or ""
        scales = _build_scales(
            pres, name, signed, size, overrides, warnings, dropped
        )
    return catalog.Field(
        name=name,
        bit_pos=prep["bit_position"],
        bit_size=size,
        unit=unit,
        signed=signed,
        byteswap=bool(pres and pres.get("byte_order")),
        ascii=is_ascii,
        hexdump=is_hexdump,
        scales=scales,
    )


def _strip_numbers(qualifier: str) -> str:
    """A service qualifier with every numeric segment (and its separator) gone.

    `DT_Mini_Cells_Voltage_Cell_1` and a same-shaped `..._Cell_200` would
    normalise to the same string; `DT_Mini_Temperature_Maximum` and
    `_Minimum` do not. That difference is what tells an array from a row of
    siblings.
    """
    return re.sub(r"_?\d+", "", qualifier)


def _trailing_index(qualifier: str) -> int | None:
    """The last number in a qualifier, ignoring trailing non-digits."""
    m = re.search(r"(\d+)\D*$", qualifier)
    return int(m.group(1)) if m else None


def _reads_as_indices(services: list[str]) -> bool:
    """Do these qualifiers' trailing numbers read as element indices?

    The guard that keeps *numbered siblings* out of arrays. A factory service
    name can end in a number that is part of the value it names rather than
    a position — an amperage threshold such as `..._Class_2A`, say — and
    collapsing a run of those would trade several self-describing factory
    names for a bare `element: 0..N` on a name that no longer says what any
    element means. Cell (or similarly indexed) arrays, by contrast, number
    1..N in step with bit position.

    A majority must match rather than all of them, because a real factory
    database can break its own naming pattern once inside a long array — one
    member's trailing number can fail to match its position — and that single
    outlier must not prevent the rest of the array from collapsing.
    """
    idx = [_trailing_index(s) for s in services]
    n = len(services)
    best = max(
        sum(1 for k, v in enumerate(idx) if v == k + base) for base in (0, 1)
    )
    return best * 2 > n


def _same_shape(a, b) -> bool:
    """Two fields an array could be built from: everything but position."""
    return (
        a.name == b.name
        and a.bit_size == b.bit_size
        and a.unit == b.unit
        and a.signed == b.signed
        and a.byteswap == b.byteswap
        and a.ascii == b.ascii
        and a.hexdump == b.hexdump
        and a.scales == b.scales
    )


def _find_array_runs(fields: list, services: list[str]) -> list[tuple[int, int, int]]:
    """Maximal collapsible runs in a bit-ordered field list -> (start, count, stride).

    A run qualifies (§5.1b) when it has three or more members that share a
    presentation qualifier and geometry, sit at a constant non-overlapping bit
    stride, are adjacent in bit order, come from services identical except for
    their numbers, and whose numbers read as indices.

    Every one of those guards earns its place against a real factory database.
    Drop the name-normalisation guard and `mini`'s own 0x0102 trap — maximum,
    minimum and averaged temperatures, three identical presentations at a
    constant stride — fuses into a bogus 3-element array; the same shape
    recurs throughout a real database wherever several distinctly-named
    readings happen to share one presentation and geometry. Drop the
    *adjacency* requirement and an aggregate reading that sits one stride
    before an array's first element, with that array's own presentation and
    geometry, is swallowed as that array's element 0.
    """
    runs: list[tuple[int, int, int]] = []
    i, n = 0, len(fields)
    while i < n:
        j = i + 1
        stride = None
        while j < n:
            prev, cur = fields[j - 1], fields[j]
            if not _same_shape(prev, cur):
                break
            if _strip_numbers(services[j]) != _strip_numbers(services[j - 1]):
                break
            step = cur.bit_pos - prev.bit_pos
            if step < cur.bit_size:  # elements may not overlap
                break
            if stride is None:
                stride = step
            elif step != stride:
                break
            j += 1
        count = j - i
        if count >= 3 and _reads_as_indices(services[i:j]):
            runs.append((i, count, stride))
            i = j
        else:
            i += 1
    return runs


def _collapse_arrays(fields: list, services: list[str], warnings: list[str]):
    """-> (fields, services, array_info) with runs folded into one record each."""
    runs = _find_array_runs(fields, services)
    if not runs:
        return fields, services, {}
    out_fields, out_services, info = [], [], {}
    consumed = {}
    for start, count, stride in runs:
        consumed[start] = (count, stride)
    i, n = 0, len(fields)
    while i < n:
        if i in consumed:
            count, stride = consumed[i]
            if count > catalog.MAX_REPEAT_COUNT:
                raise CompileError(
                    f"{fields[i].name}: array of {count} elements exceeds the u8 "
                    f"repeat_count ceiling {catalog.MAX_REPEAT_COUNT}"
                )
            base = fields[i]
            out_fields.append(
                catalog.Field(
                    name=base.name,
                    bit_pos=base.bit_pos,
                    bit_size=base.bit_size,
                    unit=base.unit,
                    signed=base.signed,
                    byteswap=base.byteswap,
                    ascii=base.ascii,
                    hexdump=base.hexdump,
                    scales=base.scales,
                    repeat_count=count,
                    repeat_stride=stride,
                )
            )
            # The array's rule-(b) fallback name drops only the index from the
            # shared service qualifier, so it stays a factory name.
            out_services.append(_strip_numbers(services[i]))
            info[len(out_fields) - 1] = {
                "count": count,
                "stride": stride,
                "first_service": services[i],
                "last_service": services[i + count - 1],
            }
            warnings.append(
                f"{base.name}: collapsed {count} fields at stride {stride} into one "
                f"array record ({services[i]} .. {services[i + count - 1]})"
            )
            i += count
        else:
            out_fields.append(fields[i])
            out_services.append(services[i])
            i += 1
    return out_fields, out_services, info


def _assign_unique_names(group_name, fields, services, arrays, warnings):
    """§5.1a: give every field in a group a distinct name -> (fields, notes).

    First the presentation qualifier, then the originating service qualifier,
    then the presentation qualifier plus `@<byte>.<bit>`. A name is invented
    only when nothing factual is left, and the manifest records which rule
    produced each one so an unusual name is explainable rather than mysterious.

    `@<byte>.<bit>` counts from byte 0 of the positive response, the same
    origin as `bit_pos` — the format doc's example does not say which origin it
    means, and rebasing to the payload would be the one thing this toolchain
    refuses to do anywhere else.
    """
    counts = Counter(f.name for f in fields)
    chosen: list[str | None] = [None] * len(fields)
    notes: list[dict] = [{} for _ in fields]

    # rule (a): the presentation qualifier, when it is already unique
    for i, f in enumerate(fields):
        if counts[f.name] == 1:
            chosen[i] = f.name
            notes[i] = {"rule": "presentation"}
    taken = {n for n in chosen if n}

    # rule (b): the originating service qualifier
    pending = [i for i in range(len(fields)) if chosen[i] is None]
    svc_counts = Counter(services[i] for i in pending)
    for i in pending:
        cand = services[i]
        if cand and svc_counts[cand] == 1 and cand not in taken:
            chosen[i] = cand
            notes[i] = {
                "rule": "service",
                "would_have_been": fields[i].name,
                "why": f"{fields[i].name!r} names "
                       f"{counts[fields[i].name]} fields in this group",
            }
            taken.add(cand)

    # rule (c): the presentation qualifier plus its position
    for i in range(len(fields)):
        if chosen[i] is not None:
            continue
        f = fields[i]
        cand = f"{f.name}@{f.bit_pos // 8}.{f.bit_pos % 8}"
        if cand in taken:
            raise CompileError(
                f"group {group_name!r}: cannot make field names unique — "
                f"{cand!r} is already used, so two fields share both a "
                f"presentation qualifier and a bit position"
            )
        chosen[i] = cand
        notes[i] = {
            "rule": "position",
            "would_have_been": f.name,
            "why": f"{f.name!r} appears {counts[f.name]} times in this group and "
                   f"service {services[i]!r} does not separate them",
        }
        taken.add(cand)
        warnings.append(
            f"{group_name}: {f.name!r} at bit {f.bit_pos} needed a positional "
            f"name ({cand!r}) — no factory name distinguishes it"
        )

    renamed = [
        catalog.Field(
            name=chosen[i],
            bit_pos=f.bit_pos,
            bit_size=f.bit_size,
            unit=f.unit,
            signed=f.signed,
            byteswap=f.byteswap,
            ascii=f.ascii,
            hexdump=f.hexdump,
            scales=f.scales,
            repeat_count=f.repeat_count,
            repeat_stride=f.repeat_stride,
        )
        for i, f in enumerate(fields)
    ]
    if len(set(chosen)) != len(chosen):
        dup = [n for n, c in Counter(chosen).items() if c > 1]
        raise CompileError(
            f"group {group_name!r}: field names still collide after all three "
            f"rules: {dup}"
        )
    for i, arr in arrays.items():
        notes[i] = {**notes[i], "array": arr}
    return renamed, notes


def _com_params(ecu_json: dict) -> dict:
    for subtype in ecu_json.get("interface_subtypes", []):
        values = {
            p["name"]: p["value"]
            for p in subtype.get("com_parameters", [])
            if p.get("value") is not None
        }
        if "CP_REQUEST_CANIDENTIFIER" in values:
            return values
    raise CompileError("no interface subtype carries CP_REQUEST_CANIDENTIFIER")


def _match_selector(sel: dict, groups: dict) -> str:
    """Resolve one profile `groups:` entry to a request-byte hex key."""
    if "request" in sel:
        key = sel["request"].replace(" ", "").upper()
        if key in groups:
            return key
        raise CompileError(f"profile: no group with request bytes {sel['request']!r}")
    if "did" in sel:
        did = sel["did"] if isinstance(sel["did"], int) else int(str(sel["did"]), 0)
        for key, svcs in groups.items():
            if key.startswith("22") and len(key) == 6 and int(key[2:], 16) == did:
                return key
        known = ", ".join(
            f"0x{int(k[2:], 16):04X}" for k in sorted(groups) if k.startswith("22") and len(k) == 6
        )
        raise CompileError(
            f"profile: DID 0x{did:04X} is not in this variant — known read DIDs: {known}"
        )
    if "service" in sel:
        want = sel["service"]
        for key, svcs in groups.items():
            if any(s["qualifier"] == want for s in svcs):
                return key
        import difflib

        universe = [s["qualifier"] for svcs in groups.values() for s in svcs]
        near = difflib.get_close_matches(want, universe, n=5, cutoff=0.5)
        hint = f" — did you mean: {', '.join(near)}" if near else ""
        raise CompileError(f"profile: unknown service {want!r}{hint}")
    raise CompileError(f"profile: group entry {sel!r} has no did/service/request key")


def compile_catalog(
    db: dict,
    ecu_name: str,
    variant_name: str,
    profile: dict | None = None,
    include_all: bool = False,
):
    """Factory-export JSON + profile -> (Catalog, manifest dict, warnings).

    Field geometry (`bit_position`, `size_in_bits`) is taken from the export
    as-is: it counts from the first byte of the positive response, which is
    the same origin the `.dcat` format uses, so nothing is rebased anywhere —
    rebasing is how off-by-one-byte decode bugs get in.
    """
    warnings: list[str] = []
    skipped: list[dict] = []
    dropped: list[dict] = []

    ecu_json = next(
        (e for e in db["ecus"] if e["qualifier"] == ecu_name), None
    )
    if ecu_json is None:
        raise CompileError(
            f"no ECU {ecu_name!r} — file has: "
            + ", ".join(e["qualifier"] for e in db["ecus"])
        )
    variant = next(
        (v for v in ecu_json["variants"] if v["qualifier"] == variant_name), None
    )
    if variant is None:
        raise CompileError(
            f"no variant {variant_name!r} — ECU has: "
            + ", ".join(v["qualifier"] for v in ecu_json["variants"])
        )

    services = [
        ecu_json["global_diag_services"][i] for i in variant["diag_service_indices"]
    ]
    # Group by request bytes, preserving file order (format doc §4).
    groups: dict[str, list[dict]] = {}
    for svc in services:
        if not svc["request_bytes"]:
            continue  # negative-response templates and jobs without a request
        groups.setdefault(svc["request_bytes"].upper(), []).append(svc)

    overrides = (profile or {}).get("invalid_overrides") or []
    intervals: dict[str, str] = {}
    if include_all:
        selected = list(groups)
    else:
        if not profile or not profile.get("groups"):
            raise CompileError("profile selects no groups and --all not given")
        selected = []
        for sel in profile["groups"]:
            key = _match_selector(sel, groups)
            if key not in selected:
                selected.append(key)
            if sel.get("interval"):
                intervals[key] = str(sel["interval"])
        # emit in database file order, not profile order, so the blob does not
        # churn when a profile line moves
        selected = [k for k in groups if k in selected]

    built_groups: list = []
    manifest_groups: list[dict] = []
    for key in selected:
        svcs = groups[key]
        qualifiers = [s["qualifier"] for s in svcs]
        name = canonical_group_name(qualifiers)
        fields = []
        field_services = []
        for svc in svcs:
            for inner in svc["output_preparations"]:
                for prep in inner:
                    f = _build_field(
                        svc, prep, ecu_json, overrides, warnings, skipped, dropped
                    )
                    if f is not None:
                        fields.append(f)
                        field_services.append(svc["qualifier"])
        # Response order, not database pool order: the pool lists services
        # alphabetically, which would put a group's "Averaged" qualifier
        # before its "Maximum" one (mini's own 0x0102 has exactly this pair).
        # A stable sort by bit position keeps ties in file order, and
        # ascending bit_pos is normative (§5.1b) so element k means the same
        # everywhere.
        order = sorted(range(len(fields)), key=lambda i: fields[i].bit_pos)
        fields = [fields[i] for i in order]
        field_services = [field_services[i] for i in order]
        # §5.1b before §5.1a: collapsing runs the duplicate names out of
        # existence, so naming sees one array record instead of dozens of
        # clones.
        fields, field_services, arrays = _collapse_arrays(
            fields, field_services, warnings
        )
        fields, name_notes = _assign_unique_names(
            name, fields, field_services, arrays, warnings
        )
        group = catalog.Group(
            name=name,
            request=bytes.fromhex(key),
            fields=tuple(fields),
            aliases=tuple(qualifiers),
            needs_session=any(s["client_access_level"] > 1 for s in svcs),
            needs_security=any(s["security_access_level"] > 0 for s in svcs),
            security_level=min(max(s["security_access_level"] for s in svcs), 15),
        )
        built_groups.append(group)
        manifest_groups.append(
            {
                "name": name,
                "request": key,
                "sid": group.sid,
                "did": None if group.did == catalog.NO_INDEX else group.did,
                "safe_read": group.safe_read,
                "needs_session": group.needs_session,
                "needs_security": group.needs_security,
                "resp_min_len": group.resp_min_len,
                "interval": intervals.get(key, "default"),
                "services": qualifiers,
                "fields": [
                    {
                        "name": f.name,
                        "service": svc_q,
                        "name_rule": note.get("rule"),
                        **(
                            {"would_have_been": note["would_have_been"],
                             "why_renamed": note["why"]}
                            if "would_have_been" in note
                            else {}
                        ),
                        **(
                            {"element_count": note["array"]["count"],
                             "element_stride_bits": note["array"]["stride"],
                             "element_0": note["array"]["first_service"],
                             "element_last": note["array"]["last_service"]}
                            if "array" in note
                            else {}
                        ),
                        "bit_pos": f.bit_pos,
                        "size_bits": f.bit_size,
                        "signed": f.signed,
                        "byteswap": f.byteswap,
                        "ascii": f.ascii,
                        "hexdump": f.hexdump,
                        "enum": f.is_enum,
                        "unit": f.unit,
                        "scales": [
                            {
                                "low": s.low,
                                "high": s.high,
                                "factor": s.factor,
                                "offset": s.offset,
                                "text": s.text,
                                "invalid": s.invalid,
                            }
                            for s in f.scales
                        ],
                    }
                    for f, svc_q, note in zip(fields, field_services, name_notes)
                ],
            }
        )

    cp = _com_params(ecu_json)

    def cp_get(name: str, default: int = 0) -> int:
        if name not in cp:
            warnings.append(f"com parameter {name} missing; wrote {default}")
            return default
        return cp[name]

    request_id = cp_get("CP_REQUEST_CANIDENTIFIER")
    ecu = catalog.Ecu(
        name=ecu_json["qualifier"],
        request_id=request_id,
        response_id=cp_get("CP_RESPONSE_CANIDENTIFIER"),
        extended_id=request_id > 0x7FF,
        block_size=cp_get("CP_BLOCKSIZE_SUG"),
        st_min_raw=cp_get("CP_STMIN_SUG"),
        p2_ms=cp_get("CP_P2_TIMEOUT"),
        p2_ext_ms=cp_get("CP_P2_EXT_TIMEOUT_7F_78"),
        groups=tuple(built_groups),
    )
    cat = catalog.Catalog(ecus=(ecu,))

    manifest = {
        "format": "uds-catalog-manifest/1",
        "ecu": {
            "name": ecu.name,
            "variant": variant_name,
            "request_id": f"0x{ecu.request_id:03X}",
            "response_id": f"0x{ecu.response_id:03X}",
            "block_size": ecu.block_size,
            "st_min_raw": ecu.st_min_raw,
            "p2_ms": ecu.p2_ms,
            "p2_ext_ms": ecu.p2_ext_ms,
        },
        "totals": {
            "groups": len(built_groups),
            "fields": sum(len(g.fields) for g in built_groups),
            "scale_rows": sum(len(f.scales) for g in built_groups for f in g.fields),
            "array_fields": sum(
                1 for g in built_groups for f in g.fields if f.repeat_count > 1
            ),
            "array_elements": sum(
                f.repeat_count for g in built_groups for f in g.fields
            ),
            # How many names each §5.1a rule had to produce: 'service' and
            # 'position' counts are the interesting review numbers, because a
            # rising 'position' count means the database stopped being able to
            # name its own fields.
            "names_by_rule": dict(
                Counter(
                    f["name_rule"]
                    for g in manifest_groups
                    for f in g["fields"]
                )
            ),
        },
        "skipped_fields": skipped,
        "dropped_scale_rows": dropped,
        "warnings": sorted(set(warnings)),
        "groups": manifest_groups,
    }
    return cat, manifest, warnings


# --- partition table (flash) ---------------------------------------------------

PART_MAGIC = b"\xaa\x50"
PART_ENTRY = struct.Struct("<2sBBII16sI")


def parse_partition_table(data: bytes) -> list[dict]:
    """Decode gen_esp32part.py's binary table: 32-byte entries, magic 0xAA50.

    Offsets are auto-placed by the generator, so reading them back is the only
    correct way to know where a partition landed — assuming an offset is how a
    catalog gets written over an app slot.
    """
    parts = []
    for off in range(0, len(data) - PART_ENTRY.size + 1, PART_ENTRY.size):
        magic, ptype, subtype, offset, size, label, flags = PART_ENTRY.unpack_from(
            data, off
        )
        if magic != PART_MAGIC:
            continue  # MD5 checksum entry (0xEBEB) or terminator (0xFFFF)
        parts.append(
            {
                "label": label.rstrip(b"\x00").decode("utf-8", "replace"),
                "type": ptype,
                "subtype": subtype,
                "offset": offset,
                "size": size,
                "flags": flags,
            }
        )
    return parts


def _find_partition_table(build_dir: Path) -> Path:
    hits = sorted(build_dir.rglob("partition-table.bin"))
    if not hits:
        raise CompileError(f"no partition-table.bin under {build_dir}")
    if len(hits) > 1:
        raise CompileError(
            "several partition-table.bin found; pass --table explicitly:\n  "
            + "\n  ".join(str(h) for h in hits)
        )
    return hits[0]


# --- commands -------------------------------------------------------------------


def cmd_compile(args) -> int:
    profile = None
    ecu_name, variant_name = args.ecu, args.variant
    if args.profile:
        import yaml

        profile = yaml.safe_load(Path(args.profile).read_text(encoding="utf-8"))
        ecu_name = ecu_name or profile.get("ecu")
        variant_name = variant_name or profile.get("variant")
    if not ecu_name or not variant_name:
        print("compile: need --ecu/--variant or a profile naming them", file=sys.stderr)
        return 2

    db = json.loads(Path(args.database).read_text(encoding="utf-8"))
    cat, manifest, warnings = compile_catalog(
        db, ecu_name, variant_name, profile, include_all=args.all
    )
    blob = catalog.write_catalog(cat)
    out = Path(args.output)
    out.write_bytes(blob)

    manifest["source"] = Path(args.database).name
    manifest["profile"] = Path(args.profile).name if args.profile else None
    manifest["totals"]["size_bytes"] = len(blob)
    manifest_path = (
        Path(args.manifest) if args.manifest else out.with_suffix(".manifest.json")
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    for w in sorted(set(warnings)):
        print(f"warning: {w}", file=sys.stderr)
    t = manifest["totals"]
    print(
        f"{out}: {len(blob)} bytes — {t['groups']} groups, {t['fields']} fields, "
        f"{t['scale_rows']} scale rows; manifest {manifest_path}"
    )
    return 0


def cmd_dump(args) -> int:
    cat = catalog.read_catalog(Path(args.file).read_bytes())
    for ecu in cat.ecus:
        print(
            f"ecu {ecu.name}: req 0x{ecu.request_id:03X} resp 0x{ecu.response_id:03X} "
            f"BS {ecu.block_size} STmin {ecu.st_min_raw} "
            f"P2 {ecu.p2_ms} ms / P2* {ecu.p2_ext_ms} ms — {len(ecu.groups)} groups"
        )
        for g in ecu.groups:
            did = "" if g.did == catalog.NO_INDEX else f" DID 0x{g.did:04X}"
            flags = "".join(
                s
                for s, on in (
                    (" SAFE_READ", g.safe_read),
                    (" SESSION", g.needs_session),
                    (" SECURITY", g.needs_security),
                )
                if on
            )
            print(
                f"  {g.request.hex(' ').upper()}{did}  {g.name}  "
                f"[{len(g.fields)} fields, resp>={g.resp_min_len}B{flags}]"
            )
            if g.aliases:
                for a in g.aliases:
                    print(f"      = {a}")
            if not args.fields:
                continue
            for f in g.fields:
                kind = "ascii" if f.ascii else "hex" if f.hexdump else (
                    "i" if f.signed else "u"
                ) + str(f.bit_size)
                unit = f" [{f.unit}]" if f.unit else ""
                arr = (
                    f" x{f.repeat_count}@{f.repeat_stride}b"
                    if f.repeat_count > 1
                    else ""
                )
                print(f"      bit {f.bit_pos:5d} {kind:>6}{arr}  {f.name}{unit}")
                for s in f.scales:
                    mark = " INVALID" if s.invalid else ""
                    text = f" {s.text!r}" if s.text else ""
                    print(
                        f"          [{s.low} .. {s.high}] x{s.factor:g} +{s.offset:g}"
                        f"{text}{mark}"
                    )
    return 0


def cmd_verify(args) -> int:
    data = Path(args.file).read_bytes()
    try:
        cat = catalog.read_catalog(data)
    except catalog.CatalogError as exc:
        print(f"INVALID {args.file}: {exc}", file=sys.stderr)
        return 1
    # write_catalog re-runs the §8 limit checks on the parsed structure.
    catalog.write_catalog(cat)
    groups = cat.groups
    fields = cat.fields
    print(
        f"OK {args.file}: {len(data)} bytes, {len(cat.ecus)} ecu(s), "
        f"{len(groups)} groups, {len(fields)} fields"
    )
    return 0


def cmd_flash(args) -> int:
    blob_path = Path(args.file)
    blob = blob_path.read_bytes()
    try:
        catalog.read_catalog(blob)
    except catalog.CatalogError as exc:
        print(f"refusing to flash an invalid catalog: {exc}", file=sys.stderr)
        return 1

    table_path = Path(args.table) if args.table else _find_partition_table(
        Path(args.build_dir)
    )
    parts = parse_partition_table(table_path.read_bytes())
    part = next((p for p in parts if p["label"] == args.partition), None)
    if part is None:
        print(
            f"no partition {args.partition!r} in {table_path} — found: "
            + ", ".join(p["label"] for p in parts),
            file=sys.stderr,
        )
        return 1
    if len(blob) > part["size"]:
        print(
            f"refusing: {blob_path} is {len(blob)} bytes but partition "
            f"{args.partition!r} holds {part['size']} (offset 0x{part['offset']:X})",
            file=sys.stderr,
        )
        return 1

    cmd = [args.esptool]
    if args.port:
        cmd += ["--port", args.port]
    if args.baud:
        cmd += ["--baud", str(args.baud)]
    cmd += ["write_flash", f"0x{part['offset']:X}", str(blob_path)]
    print(
        f"partition {args.partition!r} at 0x{part['offset']:X} "
        f"({part['size']} bytes), writing {len(blob)} bytes"
    )
    print(" ".join(cmd))
    if args.dry_run:
        return 0
    return subprocess.run(cmd).returncode


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("compile", help="factory-database JSON + profile -> .dcat")
    p.add_argument("database", help="factory diagnostic database JSON export")
    p.add_argument("--profile", help="profile YAML naming ecu, variant, groups")
    p.add_argument("--ecu", help="ECU qualifier (overrides the profile)")
    p.add_argument("--variant", help="variant qualifier (overrides the profile)")
    p.add_argument("--all", action="store_true", help="include every group")
    p.add_argument("-o", "--output", required=True, help=".dcat output path")
    p.add_argument("--manifest", help="manifest path (default: <output>.manifest.json)")
    p.set_defaults(func=cmd_compile)

    p = sub.add_parser("dump", help="human-readable catalog listing")
    p.add_argument("file")
    p.add_argument("--fields", action="store_true", help="also list fields and scales")
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("verify", help="header/CRC/bounds check plus limit enforcement")
    p.add_argument("file")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("flash", help="write a .dcat to the device's partition")
    p.add_argument("file")
    p.add_argument("--table", help="partition-table.bin from the build")
    p.add_argument("--build-dir", default=".esphome/build", help="search here for it")
    p.add_argument("--partition", default="diag", help="partition label (default: diag)")
    p.add_argument("--port")
    p.add_argument("--baud", type=int)
    p.add_argument("--esptool", default="esptool", help="esptool executable")
    p.add_argument("--dry-run", action="store_true", help="print the command only")
    p.set_defaults(func=cmd_flash)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except (CompileError, catalog.CatalogError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
