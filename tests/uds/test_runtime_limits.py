"""UDS scheduler limits must fail at configuration time."""

import pytest

import esphome.config_validation as cv

from .glue import (
    hub,
    run_hub_final_validate,
    sensor_entry,
    setup_c6,
    validate,
    validate_sensor,
    validate_text_sensor,
)


@pytest.mark.parametrize("text", [False, True])
@pytest.mark.parametrize("interval", ["25d", "2147483648ms", "4294967295ms"])
def test_update_interval_rejects_scheduler_horizon(set_core_config, text, interval):
    setup_c6(set_core_config)
    validator = validate_text_sensor if text else validate_sensor
    with pytest.raises(cv.Invalid, match="2147483647"):
        validator(sensor_entry(field="unused", update_interval=interval))


@pytest.mark.parametrize("text", [False, True])
@pytest.mark.parametrize("interval", ["never", "2147483647ms", "1ms"])
def test_update_interval_accepts_scheduler_boundaries(set_core_config, text, interval):
    setup_c6(set_core_config)
    validator = validate_text_sensor if text else validate_sensor
    validator(sensor_entry(field="unused", update_interval=interval))


@pytest.mark.parametrize("count", [255, 256])
def test_polled_group_capacity(set_core_config, monkeypatch, count):
    setup_c6(set_core_config)
    from esphome.components import uds
    from esphome.core import TimePeriodMilliseconds

    config = validate(hub())
    # Extend MINI's fictional group shape; no large on-card fixture is needed.
    from dataclasses import replace

    _, ecu = uds.catalog_of(config)
    prototype = ecu.groups[0]
    groups = tuple(
        replace(prototype, name=f"mini_{i}", request=bytes([0x22, i >> 8, i & 255]))
        for i in range(count)
    )
    monkeypatch.setattr(
        uds, "catalog_of", lambda _: (None, replace(ecu, groups=groups))
    )
    entries = [
        dict(
            sensor_entry(field=g.fields[0].name, service=g.name),
            uds_id=config["id"],
            platform="uds",
            update_interval=TimePeriodMilliseconds(milliseconds=1000),
        )
        for g in groups
    ]
    if count == 256:
        with pytest.raises(cv.Invalid, match="255"):
            run_hub_final_validate(config, sensors=entries)
    else:
        run_hub_final_validate(config, sensors=entries)


@pytest.mark.parametrize("count", [65535, 65536])
def test_binding_capacity(set_core_config, monkeypatch, count):
    setup_c6(set_core_config)
    from esphome.components import uds

    config = validate(hub())
    entry = validate_sensor(
        sensor_entry(field="DT_Mini_Voltage_Maximum_Cell", update_interval="never")
    )
    monkeypatch.setattr(
        uds,
        "bound_entity_configs",
        lambda *_: (("sensor", entry) for _ in range(count)),
    )
    if count == 65536:
        with pytest.raises(cv.Invalid, match="65535"):
            run_hub_final_validate(config)
    else:
        run_hub_final_validate(config)


def test_many_bindings_share_one_poll_slot(set_core_config):
    setup_c6(set_core_config)
    config = validate(hub())
    entry = validate_sensor(sensor_entry(field="DT_Mini_Voltage_Maximum_Cell"))
    run_hub_final_validate(config, sensors=[dict(entry, platform="uds")] * 256)
