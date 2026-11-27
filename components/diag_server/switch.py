"""Enable a bench responder or inject response-pending NRCs."""

import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import (
    CONF_INITIAL_STATE,
    CONF_RESTORE_MODE,
    CONF_RESTORE_STATE,
    ENTITY_CATEGORY_CONFIG,
)
import esphome.final_validate as fv

from . import (
    CONF_DIAG_SERVER_ID,
    CONF_FAULT,
    CONF_PENDING_78,
    DiagServer,
    diag_server_ns,
)

DEPENDENCIES = ["diag_server"]

DiagServerSwitch = diag_server_ns.class_(
    "DiagServerSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = (
    switch.switch_schema(
        DiagServerSwitch, block_inverted=True, entity_category=ENTITY_CATEGORY_CONFIG
    )
    .extend(
        {
            cv.Required(CONF_DIAG_SERVER_ID): cv.use_id(DiagServer),
            cv.Optional(CONF_FAULT): cv.one_of(CONF_PENDING_78),
            cv.Optional(CONF_INITIAL_STATE): cv.invalid(
                "Set the initial state in diag_server"
            ),
            cv.Optional(CONF_RESTORE_MODE): cv.invalid(
                "Set the initial state in diag_server"
            ),
            cv.Optional(CONF_RESTORE_STATE): cv.invalid(
                "Set the initial state in diag_server"
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = fv.unique_platform_key(
    domain="switch",
    platform="diag_server",
    keys=(CONF_DIAG_SERVER_ID, CONF_FAULT),
    message="Only one switch for {diag_server_id} with fault {fault} is supported",
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DIAG_SERVER_ID])
    var = await switch.new_switch(config, parent, CONF_FAULT in config)
    await cg.register_component(var, config)
