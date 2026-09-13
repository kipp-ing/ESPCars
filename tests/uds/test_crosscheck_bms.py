"""THE CROSS-CHECK (design doc §8): two independent derivations of a real
ECU's interface — the factory database (compiled into a private
`bms-live.dcat`) and reverse-engineered tables curated independently —
asserted against each other, DID by DID, aspect by aspect.

The fixture kwp-bms-expected.yaml is the curated side; the catalog is the
factory side. Agreement turns a ⚠ mark into confirmation; disagreement must
be enumerated in the fixture's `discrepancies` list, so this test fails when
a NEW one appears — and when a listed one silently heals, because a stale
discrepancy list is a lie about the interface.

Both inputs are real, bench-specific diagnostic data and never go in this
repo (see docs/CONVENTIONS.md): they live under a gitignored `private/`
directory, absent on a fresh clone and in CI, so this whole module skips
without them.
"""

import struct

import pytest
import yaml

from .common import PRIVATE_CATALOGS, PRIVATE_FIXTURES, load_catalog_module

catalog = load_catalog_module()

REL_TOL = 1e-4  # scale factors travel through f32 twice; 2.4% (0x0208) still fails

DCAT = PRIVATE_CATALOGS / "bms-live.dcat"
EXPECTED_YAML = PRIVATE_FIXTURES / "kwp-bms-expected.yaml"

pytestmark = pytest.mark.skipif(
    not (DCAT.exists() and EXPECTED_YAML.exists()),
    reason="private bms-live catalog/fixture not present (see docs/CONVENTIONS.md)",
)


@pytest.fixture(scope="module")
def cat():
    return catalog.read_catalog(DCAT.read_bytes())


@pytest.fixture(scope="module")
def expected():
    return yaml.safe_load(EXPECTED_YAML.read_text(encoding="utf-8"))


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def close(a: float, b: float) -> bool:
    a, b = f32(a), f32(b)
    if a == b:
        return True
    return abs(a - b) <= REL_TOL * max(abs(a), abs(b))


def linear_row(field):
    """The row that carries the field's conversion: first non-invalid,
    textless row; a field without scales is a raw pass-through (§6)."""
    for s in field.scales:
        if not s.invalid and not s.text:
            return s
    return catalog.Scale(0, 0, 1.0, 0.0)


def find_at_bit(group, bit_pos, size_bits):
    """The field covering bit_pos -> (field, element index or None).

    Since arrays are collapsed (§5.1b), the curated table addresses one cell
    as a byte offset into the raw response rather than by element number, so
    this resolves through `repeat_count`/`repeat_stride` — which also checks
    that element k really does land where the curated table says it does.
    """
    for f in group.fields:
        if f.bit_size != size_bits:
            continue
        if f.bit_pos == bit_pos:
            return f, (0 if f.repeat_count > 1 else None)
        if f.repeat_count > 1 and f.repeat_stride:
            delta = bit_pos - f.bit_pos
            if delta > 0 and delta % f.repeat_stride == 0:
                k = delta // f.repeat_stride
                if k < f.repeat_count:
                    return f, k
    return None, None


def check_entry(cat, entry):
    """-> list of (aspect, agrees: bool, detail)."""
    results = []
    groups = {g.did: g for g in cat.ecus[0].groups if g.did != catalog.NO_INDEX}
    group = groups.get(entry["did"])
    if group is None:
        return [("missing-did", False, f"DID 0x{entry['did']:04X} not in catalog")]

    bit_pos = 24 + 8 * entry["byte"]
    field, element = find_at_bit(group, bit_pos, entry["size_bits"])
    if field is None:
        near = [
            f"{f.name}@bit{f.bit_pos}/{f.bit_size}b"
            + (f"x{f.repeat_count}@{f.repeat_stride}" if f.repeat_count > 1 else "")
            for f in group.fields
            if abs(f.bit_pos - bit_pos) <= 16 or f.bit_pos <= bit_pos < f.bit_end
        ]
        return [
            (
                "position",
                False,
                f"no {entry['size_bits']}-bit field at rd[{entry['byte']}] "
                f"(bit {bit_pos}); nearby: {near or 'none'}",
            )
        ]
    where = field.name if element is None else f"{field.name}[{element}]"
    results.append(("position", True, where))

    if "signed" in entry:
        ok = field.signed == entry["signed"]
        results.append(
            ("signed", ok, f"curated {entry['signed']}, factory {field.signed}")
        )
    if "ascii" in entry:
        results.append(
            ("ascii", field.ascii == entry["ascii"], f"factory ascii={field.ascii}")
        )
    if "factor" in entry:
        row = linear_row(field)
        results.append(
            (
                "factor",
                close(row.factor, entry["factor"]),
                f"curated {f32(entry['factor'])!r}, factory {row.factor!r}",
            )
        )
    if "offset" in entry:
        row = linear_row(field)
        results.append(
            (
                "offset",
                close(row.offset, entry["offset"]),
                f"curated {f32(entry['offset'])!r}, factory {row.offset!r}",
            )
        )
    if "sentinel_raw" in entry:
        raw = entry["sentinel_raw"]
        hit = any(s.invalid and s.low <= raw <= s.high for s in field.scales)
        results.append(
            (
                "sentinel",
                hit,
                f"curated says raw {raw} is invalid; factory rows "
                + "; ".join(
                    f"[{s.low}..{s.high}]{' INV' if s.invalid else ''}"
                    for s in field.scales
                ),
            )
        )
    for raw, label in (entry.get("enum") or {}).items():
        hit = any(
            s.text and not s.invalid and s.low <= int(raw) <= s.high
            for s in field.scales
        )
        texts = {s.text for s in field.scales if s.low <= int(raw) <= s.high}
        results.append(
            ("enum", hit, f"curated raw {raw} = {label!r}; factory {texts}")
        )
    return results


def test_crosscheck(cat, expected):
    expected_disagreements = {
        (d["key"], d["aspect"]) for d in expected["discrepancies"]
    }
    details = {}
    agreements = 0
    for entry in expected["entries"]:
        for aspect, agrees, detail in check_entry(cat, entry):
            if agrees:
                agreements += 1
            else:
                details[(entry["key"], aspect)] = detail
    actual_disagreements = set(details)

    new = actual_disagreements - expected_disagreements
    assert not new, (
        "NEW disagreement(s) between the factory catalog and the curated "
        "tables — investigate, then either fix the compiler or record the "
        "finding in kwp-bms-expected.yaml discrepancies:\n  "
        + "\n  ".join(f"{k}:{a} — {details[(k, a)]}" for k, a in sorted(new))
    )
    healed = expected_disagreements - actual_disagreements
    assert not healed, (
        "listed discrepancies now AGREE — the list went stale, prune it "
        f"(and ask why the catalog changed): {sorted(healed)}"
    )
    # the cross-check only means something if it actually compared things
    assert agreements >= 45, f"only {agreements} aspects agreed — fixture eroded?"


# test_curated_cell_indices_land_on_array_elements moved to
# private/tests/test_crosscheck_cells.py: it hardcodes the private catalog's
# real element-addressing layout (byte offsets, cell counts) in its own
# source, not just in loaded data (docs/CONVENTIONS.md's data-hygiene rule).
# The generic version of this property — collapsed-array element addressing
# — is covered against the `mini` fixture in tests/uds/test_golden_mini.py.


def test_transport_ids_agree(cat, expected):
    assert cat.ecus[0].request_id == expected["ecu"]["request_id"]
    assert cat.ecus[0].response_id == expected["ecu"]["response_id"]


def test_suspect_marks_all_resolved(expected):
    """Every ⚠ entry must have reached a verdict: either it agrees with the
    factory data (the mark was over-caution) or its disagreement is recorded.
    This is the deliverable: no ⚠ left unexamined."""
    suspects = {e["key"] for e in expected["entries"] if e.get("suspect")}
    assert suspects, "the ⚠ entries are the point of the cross-check"
    # ⚠ entries that are also in discrepancies: resolved as 'curated wrong'
    # ⚠ entries not in discrepancies: resolved as 'curated right after all'
    # — both are fine; what must not exist is a ⚠ entry the test cannot
    # evaluate (missing DID would surface as a disagreement anyway).
