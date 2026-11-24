"""Regressions for unsafe polling and requests larger than the transport."""

from dataclasses import replace

import pytest

from esphome import core
from esphome.components import uds
import esphome.config_validation as cv

from .glue import (
    hub,
    isotp_entry,
    run_entity_final_validate,
    run_hub_final_validate,
    sensor_entry,
    setup_c6,
    validate,
    validate_sensor,
)


@pytest.mark.parametrize("active", [False, True])
@pytest.mark.parametrize("validator", ["entity", "hub"])
def test_unsafe_polled_binding_rejected(set_core_config, active, validator):
    setup_c6(set_core_config)
    config = validate(hub(allow_active_services=active))
    bound = validate_sensor(
        sensor_entry(field="PRES_Mini_Internal_1Byte", update_interval="60s")
    )
    with pytest.raises(cv.Invalid, match="SAFE_READ"):
        if validator == "entity":
            run_entity_final_validate(bound, config)
        else:
            run_hub_final_validate(config, sensors=[{**bound, "platform": "uds"}])


def test_unsafe_binding_without_polling_remains_valid(set_core_config):
    setup_c6(set_core_config)
    config = validate(hub())
    bound = validate_sensor(
        sensor_entry(field="PRES_Mini_Internal_1Byte", update_interval="never")
    )
    assert (
        run_entity_final_validate(bound, config)["resolved_group"]
        == "RT_Mini_Routine_Start"
    )
    assert (
        run_hub_final_validate(config, sensors=[{**bound, "platform": "uds"}]) is config
    )


@pytest.mark.parametrize("size", [8, 9, 32])
def test_static_raw_request_capacity(set_core_config, size):
    setup_c6(set_core_config)
    config = validate(hub(allow_active_services=True))
    kwargs = {
        "isotp": [isotp_entry(max_message_size=8)],
        "extra": {
            "on_boot": [{"uds.raw": {"id": config["id"], "data": [0x31] * size}}]
        },
    }
    if size <= 8:
        assert run_hub_final_validate(config, **kwargs) is config
    else:
        with pytest.raises(cv.Invalid, match="request.*max_message_size"):
            run_hub_final_validate(config, **kwargs)


def test_template_raw_capacity_checked_at_runtime(set_core_config):
    setup_c6(set_core_config)
    config = validate(hub(allow_active_services=True))
    assert (
        run_hub_final_validate(
            config,
            isotp=[isotp_entry(max_message_size=8)],
            extra={
                "on_boot": [
                    {"uds.raw": {"id": config["id"], "data": core.Lambda("return {};")}}
                ]
            },
        )
        is config
    )


@pytest.mark.parametrize("source", ["binding", "read", "execute"])
@pytest.mark.parametrize("size", [8, 9])
def test_group_request_capacity(set_core_config, monkeypatch, source, size):
    setup_c6(set_core_config)
    config = validate(hub(allow_active_services=True))
    cat, ecu = uds.catalog_of(config)
    group = ecu.groups[0]
    # Extend the fictional mini group in memory; keep its response below capacity to isolate TX.
    group = replace(group, request=bytes([0x22] * size))
    ecu = replace(ecu, groups=(group, *ecu.groups[1:]))
    monkeypatch.setattr(uds, "catalog_of", lambda _config: (cat, ecu))
    kwargs = {"isotp": [isotp_entry(max_message_size=8)]}
    if source == "binding":
        bound = validate_sensor(
            sensor_entry(field=group.fields[0].name, update_interval="never")
        )
        kwargs["sensors"] = [{**bound, "platform": "uds"}]
    else:
        kwargs["extra"] = {
            "on_boot": [{f"uds.{source}": {"id": config["id"], "service": group.name}}]
        }
    if size <= 8:
        assert run_hub_final_validate(config, **kwargs) is config
    else:
        with pytest.raises(cv.Invalid, match="request.*max_message_size"):
            run_hub_final_validate(config, **kwargs)
