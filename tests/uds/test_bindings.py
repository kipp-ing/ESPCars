"""How a field is addressed — U5, U6, U7, U8, U9 (design §3.0, format §5.1a/§5.1b).

This is the suite that earns the design's central claim: *a typo cannot survive to runtime, even
though nothing is hardcoded*. Every assertion below runs against the checked-in `.dcat` fixtures,
so it also fails if the compiler ever stops guaranteeing what §5.1a promises.

The subtle case is worth stating up front. A field whose presentation name collided is renamed to
its **originating service qualifier**, which is also that group's alias in the name index — so
`DT_Mini_Voltage_Maximum_Cell` is simultaneously a legal `service:` and a legal `field:`. Resolution
is therefore keyed by *which YAML key asked*, never by preferring one kind, because silently
picking one is exactly the wrong-record failure the format doc warns about.
"""

from __future__ import annotations

import pytest

import esphome.config_validation as cv

from .glue import MINI, bind, catalog, hub, sensor_entry, setup_c6, validate, validate_sensor

# ------------------------------------------------------------------- U5: a field resolves


def test_field_resolves_to_its_group(set_core_config) -> None:
    setup_c6(set_core_config)
    config = bind("DT_Mini_Voltage_Maximum_Cell", update_interval="10s")
    assert config["resolved_group"] == "DT_Mini_Voltage"
    assert config["resolved_unit"] == "V"


def test_unknown_field_lists_close_matches(set_core_config) -> None:
    """A one-character typo has to be a one-edit fix, not a trip to `dump`."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        bind("DT_Mini_Voltage_Maximum_Cel", update_interval="10s")
    message = str(excinfo.value)
    assert "unknown field" in message
    assert "DT_Mini_Voltage_Maximum_Cell" in message
    # And it says where the authoritative list is.
    assert "manifest.json" in message


def test_unknown_field_within_a_named_group(set_core_config) -> None:
    """With `service:` given, the candidate list narrows to that group's own fields."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        bind("PRES_Nonsense", service="DT_Mini_Temperature", update_interval="10s")
    message = str(excinfo.value)
    assert "DT_Mini_Temperature" in message
    assert "has no field" in message


def test_field_name_that_is_also_a_group_alias_resolves_as_a_field(set_core_config) -> None:
    """`field:` means field. The same string under `service:` means the group (see below)."""
    setup_c6(set_core_config)
    assert bind("DT_Mini_Temperature_Averaged", update_interval="10s")["resolved_group"] == "DT_Mini_Temperature"


def test_the_same_string_under_service_resolves_as_a_group(set_core_config) -> None:
    from esphome.components.uds import catalog_of, resolve_group

    setup_c6(set_core_config)
    hub_config = validate(hub(catalog=catalog(MINI)))
    import esphome.final_validate as fv

    from .glue import full_config

    token = fv.full_config.set(full_config([hub_config]))
    try:
        _cat, ecu = catalog_of(hub_config)
    finally:
        fv.full_config.reset(token)
    assert resolve_group(ecu, "DT_Mini_Temperature_Averaged").name == "DT_Mini_Temperature"


# ------------------------------------------------------- U6: the same name in several groups

# Real-catalog cross-group-ambiguity cases (a field name real in two DIDs) live in
# private/tests/test_bms_extra.py (docs/CONVENTIONS.md) — they hardcode real field
# names/DIDs, which is the thing this rule exists to keep out of the tracked repo.


def test_within_a_group_names_are_unique(set_core_config) -> None:
    """Format §5.1a is a compiler obligation, so a group's field names must never collide.

    Asserted against the checked-in `mini` catalog and, when present locally, any
    private bench catalog too: this is the property the whole three-key
    addressing scheme rests on, and it is the compiler's to keep.
    """
    from pathlib import Path

    from .common import PRIVATE_CATALOGS, load_catalog_module

    dcat = load_catalog_module()
    root = Path(__file__).resolve().parent.parent.parent
    paths = [root / "tests" / "uds" / "fixtures" / "mini.dcat"]
    private_bms = PRIVATE_CATALOGS / "bms-live.dcat"
    if private_bms.exists():
        paths.append(private_bms)
    for path in paths:
        cat = dcat.read_catalog(path.read_bytes())
        for ecu in cat.ecus:
            for group in ecu.groups:
                names = [f.name for f in group.fields]
                assert len(names) == len(set(names)), f"{path}: {group.name} repeats a field name"


# --------------------------------------------------------------- U7: what `service:` accepts


@pytest.mark.parametrize(
    "service",
    [
        "DT_Mini_Voltage",  # the canonical name
        "DT_Mini_Voltage_Minimum_Cell",  # an original service qualifier folded into it
        0x0101,  # the DID as an int
        "0x0101",  # the DID as a hex string
        257,  # the DID in decimal
    ],
)
def test_service_reference_forms(set_core_config, service) -> None:
    setup_c6(set_core_config)
    config = bind("DT_Mini_Voltage_Maximum_Cell", service=service, update_interval="10s")
    assert config["resolved_group"] == "DT_Mini_Voltage"


def test_unknown_service_lists_close_matches(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        bind("DT_Mini_Voltage_Maximum_Cell", service="DT_Mini_Voltag", update_interval="10s")
    assert "unknown service" in str(excinfo.value)
    assert "DT_Mini_Voltage" in str(excinfo.value)


def test_unknown_did_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="DID 0xBEEF"):
        bind("DT_Mini_Voltage_Maximum_Cell", service=0xBEEF, update_interval="10s")


# ------------------------------------------------------------------------- U8: `element:`


def test_array_element_accepted(set_core_config) -> None:
    """A collapsed array is one name plus an index — cell 2 is `element: 1` (format §5.1b)."""
    setup_c6(set_core_config)
    config = bind("PRES_Mini_Cell_2Byte", element=3, update_interval="10s")
    assert config["resolved_group"] == "DT_Mini_Cells_Voltage_Cell"
    assert config["element"] == 3


def test_element_defaults_to_zero(set_core_config) -> None:
    setup_c6(set_core_config)
    assert bind("PRES_Mini_Cell_2Byte", update_interval="10s")["element"] == 0


def test_element_out_of_range_names_both_numbers(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        bind("PRES_Mini_Cell_2Byte", element=4, update_interval="10s")
    message = str(excinfo.value)
    assert "4 element(s)" in message  # what the catalog holds
    assert "element: 4" in message  # what was asked for
    assert "0..3" in message  # and the range that would work


def test_element_on_a_scalar_rejected(set_core_config) -> None:
    """A scalar has repeat_count 1, so only element 0 exists."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="1 element"):
        bind("DT_Mini_Voltage_Maximum_Cell", element=1, update_interval="10s")


# Real-catalog element-addressing cases live in
# private/tests/test_bms_extra.py — same reason as above.


# ----------------------------------------------------- U9: the right platform for the field


@pytest.mark.parametrize(
    "field",
    ["PRES_Mini_State", "PRES_Mini_VIN_4Byte", "PRES_Mini_Hex_2Byte", "PRES_Mini_Hex_2Byte@3.0"],
)
def test_non_numeric_fields_rejected_on_sensor(set_core_config, field: str) -> None:
    """Enum, ASCII and HEXDUMP fields have no numeric value; the error names the right platform."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        bind(field, update_interval="10s")
    assert "text_sensor" in str(excinfo.value)


@pytest.mark.parametrize(
    "field",
    ["PRES_Mini_State", "PRES_Mini_VIN_4Byte", "PRES_Mini_Hex_2Byte", "PRES_Mini_Hex_2Byte@5.0"],
)
def test_non_numeric_fields_accepted_on_text_sensor(set_core_config, field: str) -> None:
    setup_c6(set_core_config)
    assert bind(field, text=True)["resolved_group"]


@pytest.mark.parametrize("field", ["DT_Mini_Voltage_Maximum_Cell", "PRES_Mini_Cell_2Byte"])
def test_numeric_fields_rejected_on_text_sensor(set_core_config, field: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        bind(field, text=True)
    assert "declare it under 'sensor:'" in str(excinfo.value)


def test_enum_field_on_text_sensor_needs_no_map(set_core_config) -> None:
    """The catalog carries the state texts, so nothing enumerated is transcribed into YAML."""
    setup_c6(set_core_config)
    config = bind("PRES_Mini_State", text=True)
    assert config["resolved_group"] == "DT_Mini_Contactor_State"


# The real-catalog text-slot-fit case lives in private/tests/test_bms_extra.py.


def test_text_wider_than_the_slot_rejected(set_core_config, tmp_path) -> None:
    """A synthetic catalog, because no shipped one is anywhere near the limit."""
    from .common import load_catalog_module

    dcat = load_catalog_module()
    long_text = "x" * 80
    group = dcat.Group(
        name="DT_Long",
        request=bytes([0x22, 0x01, 0x01]),
        fields=(dcat.Field("PRES_Long", 24, 8, scales=(dcat.Scale(0, 255, text=long_text),)),),
    )
    ecu = dcat.Ecu(name="LONG", request_id=0x7E7, response_id=0x7EF, p2_ms=150, groups=(group,))
    blob = tmp_path / "long.dcat"
    blob.write_bytes(dcat.write_catalog(dcat.Catalog(ecus=(ecu,))))

    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        bind("PRES_Long", text=True, hub_file=str(blob))
    message = str(excinfo.value)
    assert "80 characters" in message
    assert "63" in message


# ------------------------------------------------------------- units and update intervals


def test_catalog_supplies_the_unit(set_core_config) -> None:
    setup_c6(set_core_config)
    config = bind("DT_Mini_Temperature_Maximum", update_interval="10s")
    assert config["unit_of_measurement"] == "°C"


def test_yaml_unit_wins(set_core_config) -> None:
    """A corrected unit must not need a catalog rebuild."""
    setup_c6(set_core_config)
    config = bind("DT_Mini_Temperature_Maximum", update_interval="10s", unit_of_measurement="K")
    assert config["unit_of_measurement"] == "K"


def test_unitless_field_gets_no_unit(set_core_config) -> None:
    setup_c6(set_core_config)
    config = bind("PRES_Mini_Internal_1Byte", service="RT_Mini_Routine_Start", update_interval="10s")
    assert "unit_of_measurement" not in config


def test_never_becomes_interval_zero(set_core_config) -> None:
    """`never` is a first-class choice: bound, decoded on demand, and never polled (design §5)."""
    from esphome.components.uds import binding_interval

    setup_c6(set_core_config)
    assert binding_interval(bind("PRES_Mini_VIN_4Byte", text=True)) == 0
    assert binding_interval(bind("DT_Mini_Voltage_Maximum_Cell", update_interval="10s")) == 10000


def test_text_sensor_defaults_to_never(set_core_config) -> None:
    """A VIN does not change; polling one every minute is noise on a vehicle bus."""
    from esphome.components.uds import binding_interval

    setup_c6(set_core_config)
    assert binding_interval(bind("PRES_Mini_VIN_4Byte", text=True)) == 0


def test_sensor_defaults_to_sixty_seconds(set_core_config) -> None:
    from esphome.components.uds import binding_interval

    setup_c6(set_core_config)
    assert binding_interval(bind("DT_Mini_Voltage_Maximum_Cell")) == 60000


# --------------------------------------------------- the sensor platform's two shapes


def test_sensor_platform_needs_field_or_a_counter(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        validate_sensor(sensor_entry())
    message = str(excinfo.value)
    assert "'field:'" in message
    assert "requests_sent" in message


def test_diagnostics_shape_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate_sensor(
        {"uds_id": "bms", "requests_sent": {"name": "sent"}, "timeouts": {"name": "timed out"}}
    )
    assert "requests_sent" in config
    assert "field" not in config


def test_partial_response_counters_are_declarable(set_core_config) -> None:
    """A short response is a success, so "some fields never decode" needs its own two counters.

    `partial_responses` and `fields_uncovered` are the pair that explains an unexpectedly
    unavailable entity without a debugger — read together, a steady partial count with a stable
    uncovered count is an ECU sending fewer array elements than its database declares.
    """
    setup_c6(set_core_config)
    config = validate_sensor(
        {
            "uds_id": "bms",
            "partial_responses": {"name": "partial"},
            "fields_uncovered": {"name": "uncovered"},
        }
    )
    assert "partial_responses" in config
    assert "fields_uncovered" in config


def test_every_counter_is_settable_at_once(set_core_config) -> None:
    """Each counter name has to match a `set_<name>_sensor` setter on UdsDiagnostics."""
    from esphome.components.uds.sensor import COUNTERS, CONF_LAST_NRC

    setup_c6(set_core_config)
    entry = {"uds_id": "bms"}
    for counter in (*COUNTERS, CONF_LAST_NRC):
        entry[counter] = {"name": counter.replace("_", " ")}
    config = validate_sensor(entry)
    for counter in (*COUNTERS, CONF_LAST_NRC):
        assert counter in config


def test_diagnostics_entry_is_not_field_validated(set_core_config) -> None:
    """A counters-only entry has no `field:`, so the resolution pass must skip it entirely."""
    from esphome.components.uds.sensor import _final_validate

    setup_c6(set_core_config)
    config = validate_sensor({"uds_id": "bms", "requests_sent": {"name": "sent"}})
    assert _final_validate(config) is config
