"""Name resolution — the `esphome config`-time contract from the design doc
§3: an unknown name fails with the closest matches listed; an ambiguous one
fails naming every candidate and its group. A typo cannot survive to runtime.
"""

import pytest

from .common import load_catalog_module, tiny_catalog

catalog = load_catalog_module()


@pytest.fixture(scope="module")
def cat():
    # resolve() must work identically on a freshly built and a re-read catalog
    return catalog.read_catalog(catalog.write_catalog(tiny_catalog(catalog)))


def test_resolve_ecu(cat):
    r = cat.resolve("BMS")
    assert r.kind == "ecu" and r.obj.request_id == 0x7E7


def test_resolve_group_by_canonical_name(cat):
    r = cat.resolve("DT_Volt")
    assert r.kind == "group" and r.obj.did == 0x0207


def test_resolve_group_by_service_alias(cat):
    # The whole point of alias index entries: an original service qualifier
    # resolves to the merged group without anyone inventing names.
    r = cat.resolve("DT_Volt_Maximum")
    assert r.kind == "group" and r.obj.name == "DT_Volt"
    assert r.ecu.name == "BMS"


def test_resolve_unique_field(cat):
    r = cat.resolve("PRES_Volt_Avg_2Byte")
    assert r.kind == "field" and r.obj.bit_pos == 56
    assert r.group.name == "DT_Volt"


def test_resolve_kind_filter(cat):
    # 'DT_Volt' as a field must miss (and suggest), even though the group hits
    with pytest.raises(catalog.UnknownNameError):
        cat.resolve("DT_Volt", kind="field")


def test_unknown_name_carries_suggestions(cat):
    with pytest.raises(catalog.UnknownNameError) as exc:
        cat.resolve("PRES_Volt_Avg_2Bite")
    assert "PRES_Volt_Avg_2Byte" in exc.value.suggestions
    assert "PRES_Volt_Avg_2Byte" in str(exc.value)


def test_unknown_name_no_suggestions_still_readable(cat):
    with pytest.raises(catalog.UnknownNameError) as exc:
        cat.resolve("zzzzzzzz")
    assert exc.value.suggestions == []


def test_ambiguous_field_names_candidate_groups(cat):
    # Within a group, names are now unique by construction (§5.1a) — but the
    # same name in two groups is still real (an identity field can legitimately
    # live under two different DIDs in a factory database), and `service:` is
    # what disambiguates it. The error must name every candidate group with
    # its bit position.
    with pytest.raises(catalog.AmbiguousNameError) as exc:
        cat.resolve("PRES_Volt_2Byte")
    msg = str(exc.value)
    assert "DT_Volt" in msg and "DT_Cells" in msg
    assert "bit 24" in msg and "bit 88" in msg
    assert len(exc.value.candidates) == 2


def test_within_group_names_are_unique(cat):
    # The §5.1a invariant, asserted as an invariant rather than per name.
    for ecu in cat.ecus:
        for g in ecu.groups:
            names = [f.name for f in g.fields]
            assert len(names) == len(set(names)), g.name


def test_array_field_is_one_record(cat):
    r = cat.resolve("PRES_Cell_2Byte")
    assert r.kind == "field"
    assert r.obj.repeat_count == 4 and r.obj.repeat_stride == 16
    # element k is addressable by extraction, not by a separate name
    assert r.obj.bit_end == 24 + 3 * 16 + 16
