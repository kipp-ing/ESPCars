"""Debug-only writer freeze switch for sd_logger."""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import (
    CONF_INITIAL_STATE,
    CONF_RESTORE_MODE,
    CONF_RESTORE_STATE,
    CONF_STATE,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core.entity_helpers import RESTORE_STATE_INITIAL
from esphome.types import ConfigType

from . import SdLogger, sd_logger_ns

CONF_SD_LOGGER_ID = "sd_logger_id"

DebugWriterFreezeSwitch = sd_logger_ns.class_(
    "DebugWriterFreezeSwitch", switch.Switch, cg.Component
)


def _validate_boot_off(config: ConfigType) -> ConfigType:
    # The runtime always unfreezes the writer at boot. Check the normalized
    # switch schema too, so initial_state/restore_state cannot bypass the rule.
    if (
        config.get(CONF_RESTORE_MODE, "ALWAYS_OFF") != "ALWAYS_OFF"
        or config.get(CONF_INITIAL_STATE, False)
        or config.get(CONF_RESTORE_STATE, {}).get(CONF_STATE, RESTORE_STATE_INITIAL)
        != RESTORE_STATE_INITIAL
    ):
        raise cv.Invalid(
            "sd_logger writer freeze switch only supports restore_mode: ALWAYS_OFF; "
            "the writer must start unfrozen at boot"
        )
    return config


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


CONFIG_SCHEMA.add_extra(_validate_boot_off)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_SD_LOGGER_ID])
    parent = await cg.get_variable(config[CONF_SD_LOGGER_ID])
    cg.add(parent.set_debug_writer_freeze_switch(var))
