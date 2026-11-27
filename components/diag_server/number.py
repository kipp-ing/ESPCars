"""Expose a bench responder's delay and request-drop cadence."""

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG
import esphome.final_validate as fv

from . import (
    CONF_DELAY,
    CONF_DIAG_SERVER_ID,
    CONF_DROP_EVERY,
    CONF_FAULT,
    MAX_FAULT_VALUE,
    DiagServer,
    diag_server_ns,
)

DEPENDENCIES = ["diag_server"]

FaultNumber = diag_server_ns.class_("FaultNumber", number.Number, cg.Component)

CONFIG_SCHEMA = (
    number.number_schema(FaultNumber, entity_category=ENTITY_CATEGORY_CONFIG)
    .extend(
        {
            cv.Required(CONF_DIAG_SERVER_ID): cv.use_id(DiagServer),
            cv.Required(CONF_FAULT): cv.one_of(CONF_DELAY, CONF_DROP_EVERY),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = fv.unique_platform_key(
    domain="number",
    platform="diag_server",
    keys=(CONF_DIAG_SERVER_ID, CONF_FAULT),
    message="Only one number for {diag_server_id} with fault {fault} is supported",
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DIAG_SERVER_ID])
    is_delay = config[CONF_FAULT] == CONF_DELAY
    var = await number.new_number(
        config, parent, is_delay, min_value=0, max_value=MAX_FAULT_VALUE, step=1
    )
    await cg.register_component(var, config)
