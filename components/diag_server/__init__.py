"""Static diagnostic responder for an isolated bench."""

from dataclasses import dataclass
import json
from pathlib import Path
import re

import esphome.codegen as cg
from esphome.components.isotp import (
    CONF_MAX_MESSAGE_SIZE,
    CONF_RX_ID,
    CONF_TX_ID,
    IsoTpProtocol,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, ID
import esphome.final_validate as fv

CODEOWNERS = ["@swifty99"]
DEPENDENCIES = ["isotp"]
MULTI_CONF = True

CONF_DIAG_SERVER_ID = "diag_server_id"
CONF_ISOTP_ID = "isotp_id"
CONF_TABLE = "table"
CONF_ECU = "ecu"
CONF_ENABLED = "enabled"
CONF_S3_TIMEOUT = "s3_timeout"
CONF_FAULTS = "faults"
CONF_FAULT = "fault"
CONF_DELAY = "delay"
CONF_PENDING_78 = "pending_78"
CONF_DROP_EVERY = "drop_every"

TABLE_LIMIT = 64 * 1024
ENTRY_SIZE = 12  # sizeof(Entry) in diag_server_core.h
ANY_SESSION = 0x100
MAX_FAULT_VALUE = (1 << 24) - 1  # Runtime number entities use float.
HEX_BYTE = re.compile(r"[0-9A-Fa-f]{2}")

diag_server_ns = cg.esphome_ns.namespace("diag_server")
DiagServer = diag_server_ns.class_("DiagServer", cg.Component)
EntryType = diag_server_ns.struct("Entry")


@dataclass(frozen=True)
class Entry:
    request: bytes
    reply: bytes
    prefix: bool
    session: int


@dataclass(frozen=True)
class Ecu:
    req_id: int
    resp_id: int
    entries: list[Entry]


def _object(value, required, optional=()):
    if not isinstance(value, dict):
        raise ValueError(f"expected object, got {value!r}")
    if unknown := value.keys() - set(required) - set(optional):
        raise ValueError(f"unknown keys: {', '.join(sorted(unknown))}")
    if missing := set(required) - value.keys():
        raise ValueError(f"missing keys: {', '.join(sorted(missing))}")


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON key {key!r}")
        value[key] = item
    return value


def _bytes(tokens, field):
    if not tokens or not all(HEX_BYTE.fullmatch(token) for token in tokens):
        raise ValueError(
            f"{field} requires space-separated hex bytes, got {' '.join(tokens)!r}"
        )
    return bytes(int(token, 16) for token in tokens)


def _entry(value):
    _object(value, ("req", "resp"), ("src", "session"))
    for key in ("req", "resp", "src", "session"):
        if key in value and not isinstance(value[key], str):
            raise ValueError(f"{key} must be a string, got {value[key]!r}")
    request = value["req"].split()
    prefix = bool(request and request[-1] == "*")
    if prefix:
        request.pop()
    session = ANY_SESSION
    if "session" in value:
        if not HEX_BYTE.fullmatch(value["session"]):
            raise ValueError(f"session must be one hex byte, got {value['session']!r}")
        session = int(value["session"], 16)
    return Entry(
        _bytes(request, "req"), _bytes(value["resp"].split(), "resp"), prefix, session
    )


def _can_id(value, field):
    if isinstance(value, str) and re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
        value = int(value, 16)
    if type(value) is not int or not 0 <= value <= 0x1FFFFFFF:
        raise ValueError(
            f"{field} CAN ID {value!r} is outside 0..0x1FFFFFFF or is not an ID"
        )
    if value == 0x7DF:
        raise ValueError(f"{field} CAN ID 0x7DF uses unsupported functional addressing")
    return value


def load_table(path):
    """Read and validate the complete JSON table, excluding source notes from flash size."""
    try:
        table = json.loads(
            Path(path).read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
        _object(table, ("format", "ecus"))
        if table["format"] != "diag-sim/1":
            raise ValueError(f"format must be 'diag-sim/1', got {table['format']!r}")
        if not isinstance(table["ecus"], list):
            raise ValueError("ecus must be a list")
        ecus = {}
        size = 0
        for raw_ecu in table["ecus"]:
            _object(raw_ecu, ("name", "req", "resp", "entries"))
            name = raw_ecu["name"]
            if not isinstance(name, str) or not name.strip():
                raise ValueError(f"ECU name must be a nonempty string, got {name!r}")
            if name in ecus:
                raise ValueError(f"duplicate ECU name {name!r}")
            if not isinstance(raw_ecu["entries"], list):
                raise ValueError(f"ECU {name!r} entries must be a list")
            entries = []
            seen = set()
            for index, raw_entry in enumerate(raw_ecu["entries"]):
                try:
                    entry = _entry(raw_entry)
                    key = (entry.request, entry.prefix)
                    if key in seen:
                        raise ValueError(f"duplicate req {raw_entry['req']!r}")
                    seen.add(key)
                    entries.append(entry)
                    size += ENTRY_SIZE + len(entry.request) + len(entry.reply)
                except ValueError as err:
                    raise ValueError(f"ECU {name!r}, entry {index}: {err}") from err
            ecus[name] = Ecu(
                _can_id(raw_ecu["req"], f"ECU {name!r} req"),
                _can_id(raw_ecu["resp"], f"ECU {name!r} resp"),
                entries,
            )
        if size > TABLE_LIMIT:
            raise ValueError(
                f"table requires {size} bytes of flash; limit is {TABLE_LIMIT} bytes (64 KB)"
            )
        return ecus
    except (OSError, ValueError) as err:
        raise cv.Invalid(f"{path}: {err}", path=[CONF_TABLE]) from err


def _duration(value, limit, minimum=0):
    period = cv.positive_time_period_milliseconds(value)
    if not minimum <= period.total_milliseconds <= limit:
        raise cv.Invalid(
            f"{period.total_milliseconds}ms is outside {minimum}..{limit}ms"
        )
    return period


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DiagServer),
        cv.Required(CONF_ISOTP_ID): cv.use_id(IsoTpProtocol),
        cv.Required(CONF_TABLE): cv.file_,
        cv.Required(CONF_ECU): cv.string_strict,
        cv.Optional(CONF_ENABLED, default=False): cv.boolean,
        cv.Optional(CONF_S3_TIMEOUT, default="5s"): lambda value: _duration(
            value, 0xFFFFFFFF, minimum=1
        ),
        cv.Optional(CONF_FAULTS, default={}): cv.Schema(
            {
                cv.Optional(CONF_DELAY, default="0ms"): lambda value: _duration(
                    value, MAX_FAULT_VALUE
                ),
                cv.Optional(CONF_PENDING_78, default=False): cv.boolean,
                cv.Optional(CONF_DROP_EVERY, default=0): cv.int_range(
                    min=0, max=MAX_FAULT_VALUE
                ),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    full = fv.full_config.get()
    transport_id = config[CONF_ISOTP_ID]
    for domain in ("diag_server", "uds"):
        blocks = full.get(domain, [])
        if isinstance(blocks, dict):
            blocks = [blocks]
        for other in blocks:
            if (
                other.get(CONF_ISOTP_ID) == transport_id
                and other[CONF_ID] != config[CONF_ID]
            ):
                raise cv.Invalid(
                    f"isotp id {transport_id} is already used by {domain} {other[CONF_ID]}; "
                    "IsoTpProtocol::set_consumer() takes exactly one consumer",
                    path=[CONF_ISOTP_ID],
                )
    ecus = load_table(config[CONF_TABLE])
    name = config[CONF_ECU]
    if name not in ecus:
        raise cv.Invalid(
            f"ECU {name!r} not found in {config[CONF_TABLE]}", path=[CONF_ECU]
        )
    ecu = ecus[name]
    transport = fv.get_declaring_config(transport_id)
    for table_key, table_id, transport_key in (
        ("req", ecu.req_id, CONF_RX_ID),
        ("resp", ecu.resp_id, CONF_TX_ID),
    ):
        transport_can_id = transport[transport_key]
        if table_id != transport_can_id:
            raise cv.Invalid(
                f"ECU {name!r} {table_key} 0x{table_id:X} differs from "
                f"isotp {transport_id} {transport_key} 0x{transport_can_id:X}",
                path=[CONF_TABLE],
            )
    limit = transport[CONF_MAX_MESSAGE_SIZE]
    for index, entry in enumerate(ecu.entries):
        if len(entry.reply) > limit:
            raise cv.Invalid(
                f"ECU {name!r} entry {index} reply is {len(entry.reply)} bytes, "
                f"exceeding isotp {transport_id} max_message_size {limit} bytes",
                path=[CONF_TABLE],
            )
    full.data.setdefault("diag_server", {})[str(config[CONF_ID])] = ecu
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _const_array(name, kind, lines):
    return cg.static_const_array(
        ID(name, is_declaration=True, type=kind),
        cg.ArrayInitializer(
            *(cg.RawExpression(line) for line in lines), multiline=True
        ),
    )


def _emit_table(name, entries):
    if not entries:
        return cg.nullptr, cg.nullptr
    payload = bytearray()
    rows = []
    for entry in entries:
        req_offset = len(payload)
        payload.extend(entry.request)
        resp_offset = len(payload)
        payload.extend(entry.reply)
        rows.append(
            f"{{{req_offset}, {resp_offset}, {len(entry.request)}, {len(entry.reply)}, "
            f"{entry.session}, {str(entry.prefix).lower()}}}"
        )
    lines = (
        ", ".join(f"0x{byte:02X}" for byte in payload[offset : offset + 16])
        for offset in range(0, len(payload), 16)
    )
    return (
        _const_array(f"{name}_payload", cg.uint8, lines),
        _const_array(f"{name}_entries", EntryType, rows),
    )


async def to_code(config):
    name = str(config[CONF_ID])
    entries = CORE.config.data["diag_server"][name].entries
    payload, table = _emit_table(name, entries)
    transport = await cg.get_variable(config[CONF_ISOTP_ID])
    faults = config[CONF_FAULTS]
    var = cg.new_Pvariable(
        config[CONF_ID],
        transport,
        name,
        payload,
        table,
        len(entries),
        max(6, max((len(entry.reply) for entry in entries), default=0)),
        config[CONF_ENABLED],
        config[CONF_S3_TIMEOUT],
        faults[CONF_DELAY],
        faults[CONF_PENDING_78],
        faults[CONF_DROP_EVERY],
    )
    await cg.register_component(var, config)
