"""Debug-only writer freeze switch for sd_logger."""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import SdLogger, sd_logger_ns

CONF_SD_LOGGER_ID = "sd_logger_id"

DebugWriterFreezeSwitch = sd_logger_ns.class_(
    "DebugWriterFreezeSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = switch.switch_schema(
    DebugWriterFreezeSwitch,
    default_restore_mode="ALWAYS_OFF",
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    icon="mdi:pause-circle-outline",
).extend(
    {
        cv.Required(CONF_SD_LOGGER_ID): cv.use_id(SdLogger),
    }
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_SD_LOGGER_ID])
    parent = await cg.get_variable(config[CONF_SD_LOGGER_ID])
    cg.add(parent.set_debug_writer_freeze_switch(var))
