"""`platform: uds` under `text_sensor:` — the fields that have no numeric value.

Three kinds land here, and the catalog decides which: ASCII blocks (a VIN), HEXDUMP blocks, and
enumerated fields whose scale rows carry text instead of a factor. A numeric field declared here is
a config error naming the platform it belongs on, and vice versa (U9) — the catalog already knows,
so an entity that would publish nothing useful never gets built.
"""

from __future__ import annotations

from esphome.components import text_sensor
import esphome.config_validation as cv

from . import BINDING_SCHEMA, binding_final_validate, register_binding

DEPENDENCIES = ["uds"]

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema()
    .extend(BINDING_SCHEMA)
    .extend(
        # A VIN or a serial number does not change, so `never` — read once through `uds.read` — is
        # the honest default for this platform, unlike the numeric one.
        cv.Schema({cv.Optional("update_interval", default="never"): cv.update_interval})
    )
)


def _final_validate(config):
    binding_final_validate(config, want_text=True)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await register_binding(config, var, adder="add_text_binding")
