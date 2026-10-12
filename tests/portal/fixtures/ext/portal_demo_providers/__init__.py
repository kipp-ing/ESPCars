"""Test-only external component: registers fictional providers with the portal (spec 4.6).

This is exactly how a real consuming project adds data providers: another external component
calls the registry functions at import time. Nothing here refers to a real vehicle.
"""

import esphome.config_validation as cv
from esphome.components import sensor
from esphome.components.portal import register_absent_provider, register_series_provider

CODEOWNERS = []
CONFIG_SCHEMA = cv.Schema({})

register_absent_provider(
    "demo_absent",
    args={"n": cv.int_range(min=0, max=3)},
    source_type=sensor.Sensor,
    body="if (source == nullptr)\n  return true;\nreturn {n} > 1;",
    evidence='"announced":false',
    for_why=("never_announced",),
)
register_absent_provider(
    "demo_absent_plain",
    args={},
    source_type=sensor.Sensor,
    body="return false;",
)
register_series_provider(
    "demo_series",
    args={"n": cv.int_range(min=0, max=5)},
    source_type=sensor.Sensor,
    body="if (index >= {n}) {{\n  return false;\n}}\nout->cls = '-';\nout->has_value = false;\nreturn true;",
)


register_absent_provider(
    "demo_absent_bad",
    args={},
    source_type=sensor.Sensor,
    body="return {zzz};",
)


async def to_code(config):
    pass
