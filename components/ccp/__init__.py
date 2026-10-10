# CCP 2.1 single-frame master ESPHome codegen and automation registration.
"""CCP 2.1 single-frame master over a can_gateway observation port."""

from __future__ import annotations

from esphome import automation
import esphome.codegen as cg
from esphome.components.can_gateway import GatewayPort
import esphome.config_validation as cv
from esphome.const import CONF_ID
import esphome.final_validate as fv

CODEOWNERS = ["@kipp-ing"]
DEPENDENCIES = ["can_gateway"]
MULTI_CONF = True

ccp_ns = cg.esphome_ns.namespace("ccp")
CcpHub = ccp_ns.class_("CcpHub", cg.Component)
CcpConnectAction = ccp_ns.class_("CcpConnectAction", automation.Action)
CcpDisconnectAction = ccp_ns.class_("CcpDisconnectAction", automation.Action)
CcpGetVersionAction = ccp_ns.class_("CcpGetVersionAction", automation.Action)
CcpExchangeIdAction = ccp_ns.class_("CcpExchangeIdAction", automation.Action)
CcpSetMtaAction = ccp_ns.class_("CcpSetMtaAction", automation.Action)
CcpUploadAction = ccp_ns.class_("CcpUploadAction", automation.Action)
CcpShortUploadAction = ccp_ns.class_("CcpShortUploadAction", automation.Action)
CcpDownloadAction = ccp_ns.class_("CcpDownloadAction", automation.Action)
CcpSelectCalPageAction = ccp_ns.class_("CcpSelectCalPageAction", automation.Action)
CcpReadMemoryAction = ccp_ns.class_("CcpReadMemoryAction", automation.Action)
CcpWriteMemoryAction = ccp_ns.class_("CcpWriteMemoryAction", automation.Action)
CcpDaqAction = ccp_ns.class_("CcpDaqAction", automation.Action)
CcpDaqReadAction = ccp_ns.class_("CcpDaqReadAction", automation.Action)
CcpRawAction = ccp_ns.class_("CcpRawAction", automation.Action)
CcpSetEnabledAction = ccp_ns.class_("CcpSetEnabledAction", automation.Action)

CCP_ODT_PAYLOAD = 7

CONF_CAN_GATEWAY_ID = "can_gateway_id"
CONF_COMMAND_ID = "command_id"
CONF_RESPONSE_ID = "response_id"
CONF_STATION_ADDRESS = "station_address"
CONF_BYTE_ORDER = "byte_order"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_ALLOW_WRITE = "allow_write"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_RESPONSE = "on_response"
CONF_ON_ERROR = "on_error"
CONF_ON_DAQ = "on_daq"
CONF_ON_DAQ_READ = "on_daq_read"
CONF_DAQ_ANCHOR = "daq_anchor"
CONF_DATA = "data"
CONF_END = "end"
CONF_MTA = "mta"
CONF_EXT = "ext"
CONF_ADDRESS = "address"
CONF_SIZE = "size"
CONF_LENGTH = "length"
CONF_ENABLED = "enabled"
CONF_EXPECT = "expect"
CONF_FIELDS = "fields"
CONF_LIST = "list"
CONF_DTO_ID = "dto_id"
CONF_EVENT = "event"
CONF_PRESCALER = "prescaler"
CONF_SAMPLES = "samples"
CONF_COLLECT_TIMEOUT = "collect_timeout"
CONF_REQUIRE_ANCHOR = "require_anchor"


def _can_id(value):
    value = cv.hex_uint32_t(value)
    if value > 0x7FF:
        raise cv.Invalid("must be an 11-bit CAN identifier (0x000..0x7FF)")
    return value


def _byte_order(value):
    return cv.one_of("little", "big", lower=True)(value)


DAQ_ANCHOR_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ADDRESS): cv.hex_uint32_t,
        cv.Optional(CONF_EXT, default=0): cv.hex_uint8_t,
        cv.Required(CONF_EXPECT): cv.All(
            cv.ensure_list(cv.hex_uint8_t), cv.Length(min=1, max=CCP_ODT_PAYLOAD)
        ),
    }
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(CcpHub),
        cv.Required(CONF_CAN_GATEWAY_ID): cv.use_id(GatewayPort),
        cv.Optional(CONF_COMMAND_ID, default=0x700): _can_id,
        cv.Optional(CONF_RESPONSE_ID, default=0x701): _can_id,
        cv.Optional(CONF_STATION_ADDRESS, default=0x0001): cv.hex_uint16_t,
        cv.Optional(CONF_BYTE_ORDER, default="little"): _byte_order,
        cv.Optional(CONF_RESPONSE_TIMEOUT, default="100ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ALLOW_WRITE, default=False): cv.boolean,
        cv.Optional(CONF_DAQ_ANCHOR): DAQ_ANCHOR_SCHEMA,
        cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(),
        cv.Optional(CONF_ON_RESPONSE): automation.validate_automation(),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(),
        cv.Optional(CONF_ON_DAQ): automation.validate_automation(),
        cv.Optional(CONF_ON_DAQ_READ): automation.validate_automation(),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    """One CCP master owns a port, avoiding independent strict-response slots on it."""
    hubs = fv.full_config.get().get("ccp", [])
    this_id = str(config[CONF_ID])
    port = str(config[CONF_CAN_GATEWAY_ID])
    for hub in hubs:
        if str(hub[CONF_ID]) != this_id and str(hub[CONF_CAN_GATEWAY_ID]) == port:
            raise cv.Invalid(f"CCP hub port '{port}' is already used by another CCP hub")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    port = await cg.get_variable(config[CONF_CAN_GATEWAY_ID])
    var = cg.new_Pvariable(config[CONF_ID], port)
    await cg.register_component(var, config)
    cg.add_define("USE_CAN_GATEWAY_OBSERVE")
    if config[CONF_ALLOW_WRITE]:
        cg.add_define("USE_CCP_WRITE")
    cg.add(var.set_command_id(config[CONF_COMMAND_ID]))
    cg.add(var.set_response_id(config[CONF_RESPONSE_ID]))
    cg.add(var.set_station_address(config[CONF_STATION_ADDRESS]))
    cg.add(var.set_byte_order(cg.RawExpression("ccp::ByteOrder::LITTLE" if config[CONF_BYTE_ORDER] == "little" else "ccp::ByteOrder::BIG")))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT].total_milliseconds))
    cg.add(var.set_allow_write(config[CONF_ALLOW_WRITE]))
    if CONF_DAQ_ANCHOR in config:
        anchor = config[CONF_DAQ_ANCHOR]
        cg.add(var.set_daq_anchor(anchor[CONF_EXT], anchor[CONF_ADDRESS], anchor[CONF_EXPECT]))
    for conf in config.get(CONF_ON_CONNECTED, []):
        await automation.build_callback_automation(var, "add_on_connected_callback", [], conf)
    for conf in config.get(CONF_ON_RESPONSE, []):
        await automation.build_callback_automation(var, "add_on_response_callback", [(cg.uint8, "command"), (cg.uint8, "return_code"), (cg.std_vector.template(cg.uint8), "data")], conf)
    for conf in config.get(CONF_ON_ERROR, []):
        await automation.build_callback_automation(var, "add_on_error_callback", [(cg.uint8, "command"), (cg.uint8, "return_code")], conf)
    for conf in config.get(CONF_ON_DAQ, []):
        await automation.build_callback_automation(var, "add_on_daq_callback", [(cg.uint8, "pid"), (cg.std_vector.template(cg.uint8), "data")], conf)
    for conf in config.get(CONF_ON_DAQ_READ, []):
        await automation.build_callback_automation(var, "add_on_daq_read_callback", [(cg.bool_, "ok"), (cg.uint16, "anchor_ok"), (cg.uint16, "frames_seen"), (cg.std_vector.template(cg.std_vector.template(cg.uint8)), "values")], conf)


_HUB = {cv.Required(CONF_ID): cv.use_id(CcpHub)}
_BYTES = cv.templatable(cv.ensure_list(cv.hex_uint8_t))
_U8 = lambda default=None: cv.templatable(cv.int_range(min=0, max=0xFF))
_U16 = lambda: cv.templatable(cv.int_range(min=0, max=0xFFFF))
_U32 = lambda: cv.templatable(cv.int_range(min=0, max=0xFFFFFFFF))


DAQ_FIELD_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ADDRESS): cv.hex_uint32_t,
        cv.Optional(CONF_EXT, default=0): cv.hex_uint8_t,
        cv.Required(CONF_SIZE): cv.int_range(min=1, max=CCP_ODT_PAYLOAD),
    }
)


def _daq_fields_fit(fields):
    total = sum(field[CONF_SIZE] for field in fields)
    if total > CCP_ODT_PAYLOAD:
        raise cv.Invalid(
            f"ccp.daq_read fields total {total} bytes; one ODT carries {CCP_ODT_PAYLOAD}"
        )
    return fields


DAQ_READ_SCHEMA = cv.Schema(
    {
        **_HUB,
        cv.Required(CONF_FIELDS): cv.All(
            cv.ensure_list(DAQ_FIELD_SCHEMA), cv.Length(min=1), _daq_fields_fit
        ),
        cv.Optional(CONF_LIST, default=0): _U8(),
        cv.Optional(CONF_DTO_ID, default=0x702): cv.templatable(_can_id),
        cv.Optional(CONF_EVENT, default=0): _U8(),
        cv.Optional(CONF_PRESCALER, default=10): _U16(),
        cv.Optional(CONF_SAMPLES, default=30): cv.templatable(
            cv.int_range(min=1, max=0xFFFF)
        ),
        cv.Optional(CONF_COLLECT_TIMEOUT, default=5000): cv.templatable(
            cv.int_range(min=1, max=0xFFFFFFFF)
        ),
        cv.Optional(CONF_REQUIRE_ANCHOR, default=True): cv.templatable(cv.boolean),
    }
)


async def _action(config, action_id, template_arg, cls):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action("ccp.connect", CcpConnectAction, cv.Schema(_HUB), synchronous=True)
async def ccp_connect(config, action_id, template_arg, args): return await _action(config, action_id, template_arg, CcpConnectAction)

@automation.register_action("ccp.disconnect", CcpDisconnectAction, cv.Schema({**_HUB, cv.Optional(CONF_END, default=False): cv.templatable(cv.boolean)}), synchronous=True)
async def ccp_disconnect(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpDisconnectAction); cg.add(var.set_end(await cg.templatable(config[CONF_END], args, bool))); return var

@automation.register_action("ccp.get_version", CcpGetVersionAction, cv.Schema(_HUB), synchronous=True)
async def ccp_version(config, action_id, template_arg, args): return await _action(config, action_id, template_arg, CcpGetVersionAction)

@automation.register_action("ccp.exchange_id", CcpExchangeIdAction, cv.Schema(_HUB), synchronous=True)
async def ccp_exchange_id(config, action_id, template_arg, args): return await _action(config, action_id, template_arg, CcpExchangeIdAction)

@automation.register_action("ccp.set_mta", CcpSetMtaAction, cv.Schema({**_HUB, cv.Optional(CONF_MTA, default=0): _U8(), cv.Required(CONF_EXT): _U8(), cv.Required(CONF_ADDRESS): _U32()}), synchronous=True)
async def ccp_set_mta(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpSetMtaAction)
    cg.add(var.set_mta(await cg.templatable(config[CONF_MTA], args, cg.uint8))); cg.add(var.set_ext(await cg.templatable(config[CONF_EXT], args, cg.uint8))); cg.add(var.set_address(await cg.templatable(config[CONF_ADDRESS], args, cg.uint32))); return var

@automation.register_action("ccp.upload", CcpUploadAction, cv.Schema({**_HUB, cv.Required(CONF_SIZE): cv.templatable(cv.int_range(min=1, max=5))}), synchronous=True)
async def ccp_upload(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpUploadAction); cg.add(var.set_size(await cg.templatable(config[CONF_SIZE], args, cg.uint8))); return var

@automation.register_action("ccp.short_upload", CcpShortUploadAction, cv.Schema({**_HUB, cv.Required(CONF_SIZE): cv.templatable(cv.int_range(min=1, max=5)), cv.Required(CONF_EXT): _U8(), cv.Required(CONF_ADDRESS): _U32()}), synchronous=True)
async def ccp_short_upload(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpShortUploadAction)
    cg.add(var.set_size(await cg.templatable(config[CONF_SIZE], args, cg.uint8))); cg.add(var.set_ext(await cg.templatable(config[CONF_EXT], args, cg.uint8))); cg.add(var.set_address(await cg.templatable(config[CONF_ADDRESS], args, cg.uint32))); return var

async def _data_action(config, action_id, template_arg, args, cls):
    var = await _action(config, action_id, template_arg, cls)
    if cg.is_template(config[CONF_DATA]): cg.add(var.set_data_template(await cg.templatable(config[CONF_DATA], args, cg.std_vector.template(cg.uint8))))
    else: cg.add(var.set_data_static(config[CONF_DATA]))
    return var

@automation.register_action("ccp.download", CcpDownloadAction, cv.Schema({**_HUB, cv.Required(CONF_DATA): _BYTES}), synchronous=True)
async def ccp_download(config, action_id, template_arg, args): return await _data_action(config, action_id, template_arg, args, CcpDownloadAction)

@automation.register_action("ccp.select_cal_page", CcpSelectCalPageAction, cv.Schema(_HUB), synchronous=True)
async def ccp_select_page(config, action_id, template_arg, args): return await _action(config, action_id, template_arg, CcpSelectCalPageAction)

@automation.register_action("ccp.read_memory", CcpReadMemoryAction, cv.Schema({**_HUB, cv.Required(CONF_EXT): _U8(), cv.Required(CONF_ADDRESS): _U32(), cv.Required(CONF_LENGTH): cv.templatable(cv.int_range(min=1, max=0xFFFF))}), synchronous=True)
async def ccp_read_memory(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpReadMemoryAction)
    cg.add(var.set_ext(await cg.templatable(config[CONF_EXT], args, cg.uint8))); cg.add(var.set_address(await cg.templatable(config[CONF_ADDRESS], args, cg.uint32))); cg.add(var.set_length(await cg.templatable(config[CONF_LENGTH], args, cg.uint16))); return var

@automation.register_action("ccp.write_memory", CcpWriteMemoryAction, cv.Schema({**_HUB, cv.Required(CONF_EXT): _U8(), cv.Required(CONF_ADDRESS): _U32(), cv.Required(CONF_DATA): _BYTES}), synchronous=True)
async def ccp_write_memory(config, action_id, template_arg, args):
    var = await _data_action(config, action_id, template_arg, args, CcpWriteMemoryAction)
    cg.add(var.set_ext(await cg.templatable(config[CONF_EXT], args, cg.uint8))); cg.add(var.set_address(await cg.templatable(config[CONF_ADDRESS], args, cg.uint32))); return var

@automation.register_action("ccp.start_daq", CcpDaqAction, cv.Schema(_HUB), synchronous=True)
async def ccp_start_daq(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpDaqAction); cg.add(var.set_start(await cg.templatable(True, args, bool))); return var

@automation.register_action("ccp.stop_daq", CcpDaqAction, cv.Schema(_HUB), synchronous=True)
async def ccp_stop_daq(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpDaqAction); cg.add(var.set_start(await cg.templatable(False, args, bool))); return var

@automation.register_action("ccp.daq_read", CcpDaqReadAction, DAQ_READ_SCHEMA, synchronous=True)
async def ccp_daq_read(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpDaqReadAction)
    for field in config[CONF_FIELDS]:
        cg.add(var.add_field(field[CONF_SIZE], field[CONF_EXT], field[CONF_ADDRESS]))
    cg.add(var.set_list(await cg.templatable(config[CONF_LIST], args, cg.uint8)))
    cg.add(var.set_dto_id(await cg.templatable(config[CONF_DTO_ID], args, cg.uint32)))
    cg.add(var.set_event(await cg.templatable(config[CONF_EVENT], args, cg.uint8)))
    cg.add(var.set_prescaler(await cg.templatable(config[CONF_PRESCALER], args, cg.uint16)))
    cg.add(var.set_samples(await cg.templatable(config[CONF_SAMPLES], args, cg.uint16)))
    cg.add(var.set_collect_timeout(await cg.templatable(config[CONF_COLLECT_TIMEOUT], args, cg.uint32)))
    cg.add(var.set_require_anchor(await cg.templatable(config[CONF_REQUIRE_ANCHOR], args, bool)))
    return var

@automation.register_action("ccp.raw", CcpRawAction, cv.Schema({**_HUB, cv.Required(CONF_DATA): _BYTES}), synchronous=True)
async def ccp_raw(config, action_id, template_arg, args): return await _data_action(config, action_id, template_arg, args, CcpRawAction)

@automation.register_action("ccp.set_enabled", CcpSetEnabledAction, cv.Schema({**_HUB, cv.Required(CONF_ENABLED): cv.templatable(cv.boolean)}), synchronous=True)
async def ccp_enabled(config, action_id, template_arg, args):
    var = await _action(config, action_id, template_arg, CcpSetEnabledAction); cg.add(var.set_enabled(await cg.templatable(config[CONF_ENABLED], args, bool))); return var
