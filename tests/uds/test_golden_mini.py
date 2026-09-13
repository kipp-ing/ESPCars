"""The golden fixture: mini.dcat is where the Python writer and the C++
reader meet (format doc, preamble). A fresh compile of the checked-in
mini-source.json must be byte-identical to the checked-in blob — any
divergence here means one side moved without the other.
"""

import json

import pytest

from .common import FIXTURES, load_catalog_module, load_cli_module

catalog = load_catalog_module()
cli = load_cli_module()


def compile_mini():
    db = json.loads((FIXTURES / "mini-source.json").read_text(encoding="utf-8"))
    cat, manifest, _warnings = cli.compile_catalog(
        db, "MINI", "Mini_Var", profile=None, include_all=True
    )
    return cat, manifest


def test_mini_dcat_is_byte_identical():
    cat, _ = compile_mini()
    fresh = catalog.write_catalog(cat)
    golden = (FIXTURES / "mini.dcat").read_bytes()
    assert fresh == golden, (
        "mini.dcat no longer matches a fresh compile — if the format changed "
        "on purpose, recompile the fixture AND rerun tests/host, because the "
        "C++ reader asserts against these same bytes"
    )


def test_mini_dcat_reads_back():
    cat = catalog.read_catalog((FIXTURES / "mini.dcat").read_bytes())
    ecu = cat.ecus[0]
    assert ecu.name == "MINI"
    assert ecu.request_id == 0x7E7 and ecu.response_id == 0x7EF
    assert ecu.block_size == 8 and ecu.st_min_raw == 20
    assert ecu.p2_ms == 150 and ecu.p2_ext_ms == 2000
    assert len(ecu.groups) == 9

    by_name = {g.name: g for g in ecu.groups}
    merged = by_name["DT_Mini_Voltage"]
    assert merged.aliases == (
        "DT_Mini_Voltage_Maximum_Cell",
        "DT_Mini_Voltage_Minimum_Cell",
    )
    assert [f.bit_pos for f in merged.fields] == [24, 40]
    # §5.1a rule (b): one presentation name for two services, so each field is
    # listed under the service that contributed it — a factory name, not an
    # invented one.
    assert [f.name for f in merged.fields] == [
        "DT_Mini_Voltage_Maximum_Cell",
        "DT_Mini_Voltage_Minimum_Cell",
    ]

    routine = by_name["RT_Mini_Routine_Start"]
    assert not routine.safe_read
    assert routine.fields[0].name == "PRES_Mini_Internal_1Byte"

    guarded = by_name["DT_Mini_Guarded_Block"]
    assert guarded.safe_read  # 0x22 cannot change ECU state...
    assert guarded.needs_session and guarded.needs_security  # ...but is gated
    assert guarded.security_level == 3

    vin = by_name["DT_Mini_VIN"].fields[0]
    assert vin.ascii and vin.bit_size == 32
    serial = by_name["DT_Mini_Serial"].fields[0]
    assert serial.hexdump and serial.bit_size == 16

    # §5.1b: four cell services collapsed into one array record.
    cells = by_name["DT_Mini_Cells_Voltage_Cell"]
    assert len(cells.fields) == 1
    arr = cells.fields[0]
    assert arr.name == "PRES_Mini_Cell_2Byte"
    assert arr.repeat_count == 4 and arr.repeat_stride == 16
    assert arr.bit_pos == 24 and cells.resp_min_len == 11

    # The anti-collapse trap: three identical presentations at constant stride
    # 16 whose services differ by words, not numbers, must stay three fields —
    # the shape a real database's max/min/mean readings would take.
    temps = by_name["DT_Mini_Temperature"]
    assert [f.repeat_count for f in temps.fields] == [1, 1, 1]
    assert [f.name for f in temps.fields] == [
        "DT_Mini_Temperature_Maximum",
        "DT_Mini_Temperature_Minimum",
        "DT_Mini_Temperature_Averaged",
    ]

    # §5.1a rule (c): one service, two identically named fields, so only the
    # position can separate them. `@<byte>.<bit>` counts from byte 0 of the
    # response, the same origin as bit_pos.
    dual = by_name["DT_Mini_Dual_Same"]
    assert [f.name for f in dual.fields] == [
        "PRES_Mini_Hex_2Byte@3.0",
        "PRES_Mini_Hex_2Byte@5.0",
    ]


def test_every_group_has_unique_field_names():
    """The §5.1a invariant over the whole golden catalog, as an invariant."""
    cat = catalog.read_catalog((FIXTURES / "mini.dcat").read_bytes())
    for ecu in cat.ecus:
        for g in ecu.groups:
            names = [f.name for f in g.fields]
            assert len(names) == len(set(names)), f"{g.name}: {names}"


def test_name_rules_are_recorded_in_the_manifest():
    """A YAML author must be able to see why a name looks unusual."""
    _, manifest = compile_mini()
    by_group = {g["name"]: g for g in manifest["groups"]}
    rules = {
        f["name"]: f["name_rule"]
        for g in manifest["groups"]
        for f in g["fields"]
    }
    assert rules["PRES_Mini_State"] == "presentation"
    assert rules["DT_Mini_Voltage_Maximum_Cell"] == "service"
    assert rules["PRES_Mini_Hex_2Byte@3.0"] == "position"

    renamed = by_group["DT_Mini_Voltage"]["fields"][0]
    assert renamed["would_have_been"] == "PRES_Mini_Voltage_2Byte"
    assert "2 fields" in renamed["why_renamed"]

    arr = by_group["DT_Mini_Cells_Voltage_Cell"]["fields"][0]
    assert arr["element_count"] == 4
    assert arr["element_stride_bits"] == 16
    assert arr["element_0"] == "DT_Mini_Cells_Voltage_Cell_1"
    assert arr["element_last"] == "DT_Mini_Cells_Voltage_Cell_4"
    assert manifest["totals"]["array_fields"] == 1
    assert manifest["totals"]["names_by_rule"]["position"] == 2


def test_a_group_that_cannot_be_made_unique_fails():
    """§5.1a: fail rather than emit a group whose names cannot be made unique.

    Two fields from one service, one presentation, one bit position: rule (a)
    collides, rule (b) collides, and rule (c) produces the same positional
    name for both. There is nothing left to disambiguate with.
    """
    db = json.loads((FIXTURES / "mini-source.json").read_text(encoding="utf-8"))
    ecu = db["ecus"][0]
    ecu["global_diag_services"].append(
        {
            "qualifier": "DT_Mini_Impossible",
            "service_type": 5,
            "client_access_level": 1,
            "security_access_level": 0,
            "request_bytes": "22EEEE",
            "output_preparations": [
                [
                    {"qualifier": "PRES_Mini_State", "bit_position": 24,
                     "size_in_bits": 8, "field_type": "Presentation",
                     "pres_pool_index": 2, "info_pool_index": 0,
                     "size_error": None},
                    {"qualifier": "PRES_Mini_State", "bit_position": 24,
                     "size_in_bits": 8, "field_type": "Presentation",
                     "pres_pool_index": 2, "info_pool_index": 0,
                     "size_error": None},
                ]
            ],
        }
    )
    ecu["variants"][0]["diag_service_indices"].append(
        len(ecu["global_diag_services"]) - 1
    )
    with pytest.raises(cli.CompileError) as exc:
        cli.compile_catalog(db, "MINI", "Mini_Var", include_all=True)
    assert "unique" in str(exc.value)


def test_mini_decode_semantics():
    cat = catalog.read_catalog((FIXTURES / "mini.dcat").read_bytes())
    # kind='field' is required: a rule-(b) name is also the group's alias.
    temp = cat.resolve("DT_Mini_Temperature_Maximum", kind="field").obj
    assert temp.signed
    # -512 raw / 64 = -8 °C: a legal value, NOT a sentinel
    value, text = catalog.evaluate(temp, -512)
    assert abs(value - (-8.0)) < 1e-9 and text is None
    assert catalog.evaluate(temp, -32768) == (None, "SNA")

    state = cat.resolve("PRES_Mini_State").obj
    assert state.is_enum
    assert catalog.evaluate(state, 2) == (0.0, "closed")
    assert catalog.evaluate(state, 255) == (None, "SNA")
    assert catalog.evaluate(state, 7) == (None, None)  # undeclared raw


def test_mini_manifest_matches_blob():
    _, manifest = compile_mini()
    checked_in = json.loads(
        (FIXTURES / "mini.manifest.json").read_text(encoding="utf-8")
    )
    # the CLI adds provenance (source, profile, size_bytes) after compile;
    # compare the derived content
    size = checked_in["totals"].pop("size_bytes")
    assert size == len((FIXTURES / "mini.dcat").read_bytes())
    for key in ("ecu", "totals", "groups", "skipped_fields"):
        assert manifest[key] == checked_in[key]
