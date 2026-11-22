"""Canonical LIN roles and one-release compatibility for released YAML."""

import logging

import pytest

import esphome.config_validation as cv
from esphome.core import CORE

from .common import LIN0, instance, setup_esp32, validate

# Avoid unrelated strapping-pin warnings while asserting deprecations.
BASE = instance(LIN0, tx_pin="GPIO17")


@pytest.mark.parametrize(
    ("old", "new", "number"),
    [("master", "commander", 0), ("slave", "responder", 1)],
)
@pytest.mark.parametrize("uppercase", [False, True])
def test_deprecated_modes_normalize_and_warn(
    set_core_config, caplog, old, new, number, uppercase
):
    setup_esp32(set_core_config)
    raw_mode = old.upper() if uppercase else old
    with caplog.at_level(logging.WARNING):
        legacy = validate(instance(BASE, mode=raw_mode))
    canonical = validate(instance(BASE, mode=new))

    assert legacy == canonical
    assert legacy["mode"] == new
    assert legacy["mode"].enum_value == number
    assert len(caplog.records) == 1
    assert f"mode: {old} is deprecated; use mode: {new}" in caplog.text
    assert "one release cycle" in caplog.text


@pytest.mark.parametrize(
    ("mode", "number"), [("commander", 0), ("responder", 1), ("listener", 2)]
)
def test_canonical_modes_accept_case_without_warning(
    set_core_config, caplog, mode, number
):
    setup_esp32(set_core_config)
    result = validate(instance(BASE, mode=mode.upper()))
    assert result["mode"] == mode
    assert result["mode"].enum_value == number
    assert not caplog.records


def test_default_remains_commander(set_core_config, caplog):
    setup_esp32(set_core_config)
    result = validate(BASE)
    assert result["mode"] == "commander"
    assert result["mode"].enum_value == 0
    assert not caplog.records


@pytest.mark.parametrize("mode", ["responder", "listener", "slave"])
@pytest.mark.parametrize(
    ("options", "error"),
    [
        ({"schedule": [{"lin_id": 4, "interval": "50ms"}]}, "schedule is only valid"),
        ({"self_test": True}, "self_test requires"),
    ],
)
def test_active_options_require_commander(set_core_config, mode, options, error):
    setup_esp32(set_core_config)
    with pytest.raises(cv.Invalid, match=rf"{error}.*mode: commander"):
        validate(instance(BASE, mode=mode, **options))


def test_listener_still_rejects_responses(set_core_config):
    setup_esp32(set_core_config)
    with pytest.raises(cv.Invalid, match="responses are not valid with mode: listener"):
        validate(
            instance(BASE, mode="listener", responses=[{"lin_id": 4, "data": [1]}])
        )


@pytest.mark.parametrize("mode", ["unknown", "comander", 0, None, True, []])
def test_invalid_mode_errors_use_canonical_terms(set_core_config, mode):
    setup_esp32(set_core_config)
    with pytest.raises(cv.Invalid) as exc:
        validate(instance(BASE, mode=mode))
    assert "master" not in str(exc.value)
    assert "slave" not in str(exc.value)
    if mode == "unknown":
        assert "'commander', 'responder', 'listener'" in str(exc.value)


@pytest.mark.parametrize(
    ("old", "new", "number"),
    [("master", "commander", 0), ("slave", "responder", 1)],
)
def test_deprecated_mode_generates_identical_code(set_core_config, old, new, number):
    from esphome.components.linbus import to_code

    def generate(mode):
        CORE.reset()
        setup_esp32(set_core_config)
        options = {"responses": [{"lin_id": 4, "data": [1, 2], "checksum": "classic"}]}
        if new == "commander":
            options.update(
                schedule=[{"lin_id": 4, "interval": "50ms", "length": 2}],
                self_test=True,
            )
        config = validate(instance(BASE, mode=mode, **options))
        for key in ("tx_pin", "rx_pin", "cs_pin"):
            config[key]["id"].id = f"lin0_{key}"
        CORE.component_ids.add("lin0")
        CORE.add_job(to_code, config)
        CORE.flush_tasks()
        return CORE.cpp_global_section + CORE.cpp_main_section

    legacy = generate(old)
    assert legacy == generate(new)
    assert f"lin0->set_mode({number});" in legacy
    assert "lin0->add_response(4," in legacy


def test_request_sensor_alias_normalizes_and_warns(set_core_config, caplog):
    from esphome.components.linbus.sensor import CONFIG_SCHEMA

    setup_esp32(set_core_config)
    sensor = {"id": "requests", "name": "LIN requests"}
    legacy = CONFIG_SCHEMA({"linbus_id": "lin0", "master_requests": sensor})
    CORE.reset()
    setup_esp32(set_core_config)
    canonical = CONFIG_SCHEMA({"linbus_id": "lin0", "commander_requests": sensor})
    assert legacy == canonical
    assert "master_requests" not in legacy
    assert "'master_requests' is deprecated, use 'commander_requests'" in caplog.text
    assert len(caplog.records) == 1


def test_request_sensor_alias_rejects_both_keys(set_core_config):
    from esphome.components.linbus.sensor import CONFIG_SCHEMA

    setup_esp32(set_core_config)
    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA(
            {"master_requests": {"name": "old"}, "commander_requests": {"name": "new"}}
        )


def test_request_sensor_alias_generates_identical_code(set_core_config):
    import esphome.codegen as cg
    from esphome.components.linbus.sensor import CONFIG_SCHEMA, to_code

    def generate(key):
        CORE.reset()
        setup_esp32(set_core_config)
        config = CONFIG_SCHEMA(
            {"linbus_id": "lin0", key: {"id": "requests", "name": "LIN requests"}}
        )
        CORE.register_variable(config["linbus_id"], cg.MockObj("lin0", "->"))
        CORE.add_job(to_code, config)
        CORE.flush_tasks()
        return CORE.cpp_global_section + CORE.cpp_main_section

    legacy = generate("master_requests")
    assert legacy == generate("commander_requests")
    assert "lin0->set_commander_requests_sensor(requests);" in legacy
