"""Optional diagnostic responder counters."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
)

from . import CONF_DIAG_SERVER_ID, DiagServer, diag_server_ns

DEPENDENCIES = ["diag_server"]

COUNTERS = ("requests", "replies", "misses")

Diagnostics = diag_server_ns.class_("Diagnostics", cg.PollingComponent)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Diagnostics),
            cv.Required(CONF_DIAG_SERVER_ID): cv.use_id(DiagServer),
            **{
                cv.Optional(key): sensor.sensor_schema(
                    accuracy_decimals=0,
                    state_class=STATE_CLASS_TOTAL_INCREASING,
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                )
                for key in COUNTERS
            },
        }
    ).extend(cv.polling_component_schema("60s")),
    cv.has_at_least_one_key(*COUNTERS),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DIAG_SERVER_ID])
    var = cg.new_Pvariable(config[CONF_ID], parent)
    await cg.register_component(var, config)
    for key in COUNTERS:
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(entity))
