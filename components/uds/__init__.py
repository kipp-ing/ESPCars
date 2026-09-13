"""UDS diagnostic client (docs/DESIGN-uds.md).

The decode rules are neither in the firmware nor in YAML: they live in a `.dcat` catalog compiled
from the factory diagnostic database, and this module reads that catalog *at `esphome config` time*
with the very same Python module the compiler used (`catalog.py`). So `field:` resolves before a
build runs, a typo fails with the closest matches listed, and the ambiguity the database really
contains is reported with candidates instead of guessed at.

What is emitted into the generated code is **names, never indices**, plus the catalog's CRC. The
flashed catalog may legitimately be newer than the firmware; a name still resolves against it, an
index would silently point at a different field.

Validation rules are numbered U1-U13 so test names and error messages stay traceable:

U1   `catalog:` names exactly one source — a partition or `embed: true`, never both, never neither.
U2   `catalog.file` is a readable, valid `.dcat` (every §2 check, including the CRC).
U3   the partition is 4 KB aligned, big enough for the blob, and legally named.
U4   `ecu:` resolves; it is required when the catalog holds more than one ECU.
U5   `field:` resolves, and an unknown name lists close matches.
U6   `field:` in several groups needs `service:`, and the error names the candidate groups.
U7   `service:` resolves by canonical name, by any original service qualifier, or by DID.
U8   `element:` is inside the field's `repeat_count`, and both numbers are named when it is not.
U9   a numeric field belongs on `sensor:`; ASCII, HEXDUMP and enum fields belong on `text_sensor:`.
U10  `uds.read` accepts SAFE_READ groups only, whatever `allow_active_services` says.
U11  `uds.execute` / `uds.raw` need `allow_active_services: true`, and are rejected without it.
U12  §3.1's transport check: isotp's `max_message_size` must hold the largest response read.
U13  one hub per isotp instance (`set_consumer()` takes one consumer), and hubs sharing a
     catalog partition must agree on its size.
"""

from __future__ import annotations

import difflib
import logging
from pathlib import Path
import re
import struct
from typing import Any

from esphome import automation, core
import esphome.codegen as cg
from esphome.components.esp32 import add_partition
from esphome.components.isotp import (
    CONF_BLOCK_SIZE,
    CONF_MAX_MESSAGE_SIZE,
    CONF_N_CR_TIMEOUT,
    CONF_ST_MIN,
    IsoTpProtocol,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA,
    CONF_FILE,
    CONF_ID,
    CONF_PLATFORM,
    CONF_SERVICE,
    CONF_SIZE,
    CONF_UPDATE_INTERVAL,
    SCHEDULER_DONT_RUN,
)
from esphome.core import CORE, ID
import esphome.final_validate as fv

from . import catalog as dcat

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@swifty99"]
DEPENDENCIES = ["isotp"]
MULTI_CONF = True

uds_ns = cg.esphome_ns.namespace("uds")
UdsHub = uds_ns.class_("UdsHub", cg.Component)
UdsValue = uds_ns.struct("UdsValue")
UdsError = uds_ns.struct("UdsError")
UdsReadAction = uds_ns.class_("UdsReadAction", automation.Action)
UdsExecuteAction = uds_ns.class_("UdsExecuteAction", automation.Action)
UdsRawAction = uds_ns.class_("UdsRawAction", automation.Action)
UdsSetEnabledAction = uds_ns.class_("UdsSetEnabledAction", automation.Action)

CONF_UDS_ID = "uds_id"
CONF_ISOTP_ID = "isotp_id"
CONF_CATALOG = "catalog"
CONF_PARTITION = "partition"
CONF_EMBED = "embed"
CONF_ECU = "ecu"
CONF_FIELD = "field"
CONF_ELEMENT = "element"
CONF_ENABLED = "enabled"
CONF_MAX_CONSECUTIVE_FAILURES = "max_consecutive_failures"
CONF_ALLOW_ACTIVE_SERVICES = "allow_active_services"
CONF_ON_VALUE = "on_value"
CONF_ON_ERROR = "on_error"

# Resolution results written back into the config by FINAL_VALIDATE_SCHEMA, where the catalog and
# the referenced hub are both reachable. `inherit_property_from` sets a precedent for mutating the
# config there; the alternative is resolving twice and letting the two answers drift.
CONF_RESOLVED_GROUP = "resolved_group"
CONF_RESOLVED_UNIT = "resolved_unit"

ACTION_EXECUTE = "uds.execute"
ACTION_RAW = "uds.raw"
ACTION_READ = "uds.read"

# 256 KiB holds a real compiled catalog (tens of KB) with room to grow, and costs 128 KiB of each
# 1.8 MB app slot (design §2). A partition table is binary, so KB means 1024 here.
DEFAULT_PARTITION_SIZE = 0x40000
PARTITION_ALIGN = 0x1000
# ESP-IDF stores a partition label in a 16-byte field, NUL padded.
PARTITION_LABEL_MAX = 16
_PARTITION_LABEL_RE = re.compile(r"^[A-Za-z0-9_]+$")

# Mirrors UDS_TEXT_SLOT_CAP in uds.h. A text binding's value slot is fixed, so a field wider than
# it has to be refused here rather than truncated on the device.
TEXT_SLOT_CAP = 64
# Mirrors UDS_MAX_REQUEST in uds.h.
MAX_REQUEST = 32

_SIZE_UNITS = {
    "": 1,
    "b": 1,
    "k": 1024,
    "kb": 1024,
    "kib": 1024,
    "m": 1024 * 1024,
    "mb": 1024 * 1024,
    "mib": 1024 * 1024,
}
_SIZE_RE = re.compile(r"^\s*(\d+)\s*([A-Za-z]*)\s*$")

# --- the catalog, read once per file -------------------------------------------------------
#
# Validation touches the same blob many times (once per hub, once per entity, once for the
# transport check), and the 50 KB production catalog costs a few milliseconds to parse. Keyed on
# mtime and size so an edited catalog is never served from the cache.

_catalog_cache: dict[tuple[str, int, int], tuple[dcat.Catalog, int, int]] = {}


def load_catalog(path: Path) -> tuple[dcat.Catalog, int, int]:
    """Return (catalog, crc32, blob size) for a `.dcat` file, or raise cv.Invalid (U2)."""
    resolved = Path(path)
    try:
        stat = resolved.stat()
    except OSError as exc:
        raise cv.Invalid(f"cannot read catalog {resolved}: {exc}") from exc
    key = (str(resolved), stat.st_mtime_ns, stat.st_size)
    if key in _catalog_cache:
        return _catalog_cache[key]
    blob = resolved.read_bytes()
    try:
        cat = dcat.read_catalog(blob)
    except dcat.CatalogError as exc:
        raise cv.Invalid(
            f"{resolved} is not a usable catalog: {exc}. Compile one with "
            f"`script/uds_catalog.py compile`, and check it with `script/uds_catalog.py verify`."
        ) from exc
    # Format §2: the header's CRC lives at 0x10 and covers [64, total_size). Read the file's own
    # value rather than recomputing, so what codegen emits is exactly what `verify` reported.
    (crc,) = struct.unpack_from("<I", blob, 0x10)
    result = (cat, crc, len(blob))
    _catalog_cache[key] = result
    return result


# --- resolution ------------------------------------------------------------------------------


def _ecu_of(cat: dcat.Catalog, name: str | None) -> dcat.Ecu:
    """U4: pick the Ecu record, requiring `ecu:` when the catalog holds several."""
    if name is None:
        if len(cat.ecus) != 1:
            raise cv.Invalid(
                f"this catalog holds {len(cat.ecus)} ECUs, so 'ecu:' is required — "
                f"one of: {', '.join(e.name for e in cat.ecus)}",
                path=[CONF_ECU],
            )
        return cat.ecus[0]
    for ecu in cat.ecus:
        if ecu.name == name:
            return ecu
    raise cv.Invalid(
        f"unknown ecu {name!r} — this catalog holds: {', '.join(e.name for e in cat.ecus)}",
        path=[CONF_ECU],
    )


def resolve_group(ecu: dcat.Ecu, service: str | int) -> dcat.Group:
    """U7: a group by canonical name, by any original service qualifier, or by DID."""
    if isinstance(service, int):
        # `did` is NO_INDEX (0xFFFF) for every group that is not a SID 0x22 read (format §4), so
        # that value must not be matchable — it would select an arbitrary routine or local read.
        hits = [g for g in ecu.groups if g.did == service and g.did != dcat.NO_INDEX]
        if not hits:
            raise cv.Invalid(
                f"no group in ECU {ecu.name!r} reads DID 0x{service:04X}",
                path=[CONF_SERVICE],
            )
        # A DID identifies one request, and one request is one group by construction (format §4).
        return hits[0]
    for group in ecu.groups:
        if service == group.name or service in group.aliases:
            return group
    raise cv.Invalid(
        f"unknown service {service!r} in ECU {ecu.name!r}"
        + _suggest(service, [g.name for g in ecu.groups] + [a for g in ecu.groups for a in g.aliases]),
        path=[CONF_SERVICE],
    )


def _suggest(name: str, universe: list[str]) -> str:
    close = difflib.get_close_matches(name, universe, n=5, cutoff=0.5)
    return " — did you mean: " + ", ".join(close) if close else ""


def resolve_field(ecu: dcat.Ecu, field: str, service: str | int | None, element: int) -> tuple[dcat.Group, dcat.Field]:
    """U5/U6/U8: the field a `field:`/`service:`/`element:` triple addresses.

    Format §5.1a makes a group's field names unique, so `service:` is enough to break every tie
    the database produces — and the compiler has already collapsed arrays (§5.1b), so a cell is an
    `element:` rather than one of many names differing by an offset.
    """
    groups = [resolve_group(ecu, service)] if service is not None else list(ecu.groups)
    hits = [(g, f) for g in groups for f in g.fields if f.name == field]
    if not hits:
        if service is not None:
            group = groups[0]
            raise cv.Invalid(
                f"group {group.name!r} has no field {field!r}"
                + _suggest(field, [f.name for f in group.fields]),
                path=[CONF_FIELD],
            )
        raise cv.Invalid(
            f"unknown field {field!r} in ECU {ecu.name!r}"
            + _suggest(field, [f.name for g in ecu.groups for f in g.fields])
            + ". The '.manifest.json' beside the catalog lists every field's final name.",
            path=[CONF_FIELD],
        )
    if len(hits) > 1:
        # U6. Format §5.1a guarantees uniqueness *within* a group, so every remaining tie is
        # across groups and `service:` settles it.
        where = "; ".join(f"group {g.name!r} (DID 0x{g.did:04X})" for g, _ in hits)
        raise cv.Invalid(
            f"field {field!r} occurs in {len(hits)} groups, so 'service:' is required to say "
            f"which one: {where}",
            path=[CONF_FIELD],
        )
    group, found = hits[0]
    if element >= found.repeat_count:
        raise cv.Invalid(
            f"field {field!r} holds {found.repeat_count} element(s), so 'element: {element}' is "
            f"out of range (valid: 0..{found.repeat_count - 1})",
            path=[CONF_ELEMENT],
        )
    return group, found


def hub_config_of(uds_id: ID) -> dict[str, Any]:
    """The `uds:` block an entity or action points at, from the full config."""
    blocks = fv.full_config.get().get("uds") or []
    if not isinstance(blocks, list):
        blocks = [blocks]
    for block in blocks:
        if block.get(CONF_ID) == uds_id:
            return block
    # The id resolved at schema time, so a miss means an unexpected config shape rather than a
    # user error; leave the report to whoever owns that shape.
    raise cv.Invalid(f"no 'uds:' block with id {uds_id}")


def catalog_of(hub: dict[str, Any]) -> tuple[dcat.Catalog, dcat.Ecu]:
    cat, _crc, _size = load_catalog(hub[CONF_CATALOG][CONF_FILE])
    return cat, _ecu_of(cat, hub.get(CONF_ECU))


# --- schema ----------------------------------------------------------------------------------


def _partition_size(value: object) -> int:
    """A flash partition size in bytes. Accepts an integer or `256KB` / `256KiB` / `1MB`.

    KB means 1024 here, deliberately: a partition table is binary and 4 KB aligned, and 256 * 1000
    is not a legal partition size at all.
    """
    if isinstance(value, bool):
        raise cv.Invalid("expected a size in bytes")
    if isinstance(value, int):
        size = value
    else:
        match = _SIZE_RE.match(str(value))
        if match is None:
            raise cv.Invalid(f"expected a size like '256KB' or 262144, got {value!r}")
        unit = match.group(2).lower()
        if unit not in _SIZE_UNITS:
            raise cv.Invalid(f"unknown size unit {match.group(2)!r}; use B, KB/KiB or MB/MiB")
        size = int(match.group(1)) * _SIZE_UNITS[unit]
    if size < PARTITION_ALIGN or size % PARTITION_ALIGN != 0:
        raise cv.Invalid(f"partition size must be a non-zero multiple of 4 KB (0x1000); got {size}")
    return size


def _partition_name(value: object) -> str:
    name = cv.string_strict(value)
    if not _PARTITION_LABEL_RE.match(name) or len(name) > PARTITION_LABEL_MAX:
        raise cv.Invalid(
            f"partition name {name!r} must be 1-{PARTITION_LABEL_MAX} characters of "
            f"[A-Za-z0-9_] — it becomes the label in the partition table"
        )
    return name


def _service_ref(value: object) -> str | int:
    """A group reference: a qualifier, or a DID as an int or a `0x….` string.

    0xFFFF is excluded because the format uses it as the group record's "not a DID" marker
    (format §4), so accepting it would let `service: 0xFFFF` select an arbitrary routine.
    """
    if isinstance(value, bool):
        raise cv.Invalid("expected a service qualifier or a DID")
    if isinstance(value, int):
        return cv.int_range(min=0, max=0xFFFE)(value)
    text = cv.string_strict(value)
    if re.fullmatch(r"0[xX][0-9a-fA-F]{1,4}", text):
        return int(text, 16)
    return text


def _validate_catalog(config):
    """U1/U2/U3: one source, a valid blob, and a partition that can hold it."""
    has_partition = CONF_PARTITION in config
    if has_partition == config[CONF_EMBED]:
        raise cv.Invalid(
            "'catalog:' needs exactly one source: 'partition: <name>' to memory-map a flashed "
            "partition (the normal case), or 'embed: true' to compile the blob into the firmware "
            "(build tests and tiny catalogs)"
        )
    if not has_partition and CONF_SIZE in config:
        raise cv.Invalid("'size:' belongs to 'partition:'; an embedded catalog is exactly as big "
                         "as its file", path=[CONF_SIZE])

    _cat, _crc, blob_size = load_catalog(config[CONF_FILE])

    if has_partition:
        size = config.setdefault(CONF_SIZE, DEFAULT_PARTITION_SIZE)
        if blob_size > size:
            raise cv.Invalid(
                f"{config[CONF_FILE]} is {blob_size} bytes and does not fit a {size}-byte "
                f"partition. Raise 'size:' to at least "
                f"{-(-blob_size // PARTITION_ALIGN) * PARTITION_ALIGN}.",
                path=[CONF_SIZE],
            )
    elif blob_size > 64 * 1024:
        _LOGGER.warning(
            "embedding a %d-byte catalog puts it in the firmware image, so changing it needs a "
            "recompile and it costs flash in both app slots. Prefer 'partition:' above ~64 KB.",
            blob_size,
        )
    return config


CATALOG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_FILE): cv.file_,
            cv.Optional(CONF_PARTITION): _partition_name,
            cv.Optional(CONF_EMBED, default=False): cv.boolean,
            cv.Optional(CONF_SIZE): _partition_size,
        }
    ),
    _validate_catalog,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UdsHub),
        cv.Required(CONF_ISOTP_ID): cv.use_id(IsoTpProtocol),
        cv.Required(CONF_CATALOG): CATALOG_SCHEMA,
        cv.Optional(CONF_ECU): cv.string_strict,
        # Three failures is the same threshold isotp's own retries settle on: enough that one
        # missed answer on a busy bus is not an outage, few enough that a real one shows up fast.
        cv.Optional(CONF_MAX_CONSECUTIVE_FAILURES, default=3): cv.int_range(min=1, max=255),
        # Never widen this default (design §6). Everything that can change ECU state is behind it.
        cv.Optional(CONF_ALLOW_ACTIVE_SERVICES, default=False): cv.boolean,
        cv.Optional(CONF_ON_VALUE): automation.validate_automation(),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(),
    }
).extend(cv.COMPONENT_SCHEMA)


# --- final validation ------------------------------------------------------------------------


def _walk_actions(node: Any, names: tuple[str, ...]):
    """Yield every (action name, action config) in the full config, at any nesting depth.

    Actions have no final-validate hook of their own, and the gate they need is a property of the
    *hub* they point at — which is only reachable from the full config. So the hub's final
    validation looks for them instead of the other way round.
    """
    if isinstance(node, dict):
        for key, value in node.items():
            if key in names:
                for item in value if isinstance(value, list) else [value]:
                    if isinstance(item, dict):
                        yield key, item
            yield from _walk_actions(value, names)
    elif isinstance(node, list):
        for item in node:
            yield from _walk_actions(item, names)


def bound_entity_configs(full_config, hub_id: ID):
    """Every `platform: uds` sensor/text_sensor entry bound to one hub.

    The hub has to enumerate its own entities twice: once at final validation, to know the largest
    response §3.1 has to fit, and once at codegen, to size the fixed arrays. Both read the config
    rather than a registry filled during platform validation, because validation may run more than
    once in one process and a registry would then carry stale entries.
    """
    for domain in ("sensor", "text_sensor"):
        for entry in full_config.get(domain) or []:
            if not isinstance(entry, dict) or entry.get(CONF_PLATFORM) != "uds":
                continue
            if entry.get(CONF_UDS_ID) != hub_id or CONF_FIELD not in entry:
                continue
            yield domain, entry


def _find_isotp_config(isotp_id: ID) -> dict[str, Any] | None:
    blocks = fv.full_config.get().get("isotp") or []
    if not isinstance(blocks, list):
        blocks = [blocks]
    for block in blocks:
        if block.get(CONF_ID) == isotp_id:
            return block
    return None


def _check_transport(config, ecu: dcat.Ecu, groups: list[dcat.Group]) -> None:
    """U12 — design §3.1. The catalog knows how long a response gets; isotp owns the buffer.

    The failure this prevents is quiet: a real multi-block response against a too-small
    `max_message_size: 256` is an `OVERFLOW_LOCAL`, which on a bench looks exactly like a flaky
    ECU. This does not *fix*
    the isotp config — ownership of a key belongs to the component whose schema declares it, and a
    silent cross-component correction would make the effective transport settings unreadable from
    the YAML.
    """
    isotp_conf = _find_isotp_config(config[CONF_ISOTP_ID])
    if isotp_conf is None:
        return
    hub = config[CONF_ID]
    if groups:
        widest = max(groups, key=lambda g: g.resp_min_len)
        capacity = isotp_conf[CONF_MAX_MESSAGE_SIZE]
        if capacity < widest.resp_min_len:
            raise cv.Invalid(
                f"group {widest.name!r} (DID 0x{widest.did:04X}) answers with at least "
                f"{widest.resp_min_len} bytes, but isotp id {config[CONF_ISOTP_ID]} sets "
                f"'{CONF_MAX_MESSAGE_SIZE}: {capacity}'. The transfer would fail as OVERFLOW_LOCAL, "
                f"which looks like a flaky ECU. Raise '{CONF_MAX_MESSAGE_SIZE}' to at least "
                f"{widest.resp_min_len}.",
                path=[CONF_ISOTP_ID],
            )
    # Suggestions, not requirements: CP_BLOCKSIZE_SUG / CP_STMIN_SUG are what the database
    # proposes, and a slower client is still conforming.
    if ecu.block_size and isotp_conf[CONF_BLOCK_SIZE] != ecu.block_size:
        _LOGGER.info(
            "uds %s: the catalog suggests block_size %d, isotp %s grants %d — allowed, just slower "
            "or burstier than the database expects",
            hub, ecu.block_size, config[CONF_ISOTP_ID], isotp_conf[CONF_BLOCK_SIZE],
        )
    if ecu.st_min_raw and isotp_conf[CONF_ST_MIN] != ecu.st_min_raw:
        _LOGGER.info(
            "uds %s: the catalog suggests STmin raw 0x%02X, isotp %s requests 0x%02X",
            hub, ecu.st_min_raw, config[CONF_ISOTP_ID], isotp_conf[CONF_ST_MIN],
        )
    n_cr = isotp_conf[CONF_N_CR_TIMEOUT].total_milliseconds
    if ecu.p2_ext_ms and n_cr < ecu.p2_ext_ms:
        _LOGGER.warning(
            "uds %s: the catalog's P2* is %d ms but isotp %s sets n_cr_timeout %d ms. An ECU that "
            "answers 0x78 intends to take longer than that, and the transfer will time out "
            "mid-response.",
            hub, ecu.p2_ext_ms, config[CONF_ISOTP_ID], n_cr,
        )


def _check_siblings(config) -> None:
    """U13: one hub per transport, and one size per shared catalog partition."""
    blocks = fv.full_config.get().get("uds") or []
    if not isinstance(blocks, list):
        blocks = [blocks]
    catalog = config[CONF_CATALOG]
    for other in blocks:
        if other.get(CONF_ID) == config[CONF_ID]:
            continue
        if other[CONF_ISOTP_ID] == config[CONF_ISOTP_ID]:
            raise cv.Invalid(
                f"isotp id {config[CONF_ISOTP_ID]} is already used by uds hub "
                f"{other[CONF_ID]}. IsoTpProtocol::set_consumer() takes exactly one consumer, so "
                f"the second hub would silently never receive a response — give each hub its own "
                f"isotp instance.",
                path=[CONF_ISOTP_ID],
            )
        other_catalog = other[CONF_CATALOG]
        if (
            CONF_PARTITION in catalog
            and other_catalog.get(CONF_PARTITION) == catalog[CONF_PARTITION]
            and other_catalog.get(CONF_SIZE) != catalog[CONF_SIZE]
        ):
            raise cv.Invalid(
                f"hubs {config[CONF_ID]} and {other[CONF_ID]} share catalog partition "
                f"{catalog[CONF_PARTITION]!r} but ask for {catalog[CONF_SIZE]} and "
                f"{other_catalog.get(CONF_SIZE)} bytes. One partition has one size.",
                path=[CONF_CATALOG, CONF_SIZE],
            )


def _final_validate(config):
    _cat, ecu = catalog_of(config)
    groups: dict[str, dcat.Group] = {}

    for _domain, entry in bound_entity_configs(fv.full_config.get(), config[CONF_ID]):
        group, _field = resolve_field(
            ecu, entry[CONF_FIELD], entry.get(CONF_SERVICE), entry.get(CONF_ELEMENT, 0)
        )
        groups[group.name] = group

    for name, action in _walk_actions(fv.full_config.get(), (ACTION_READ, ACTION_EXECUTE, ACTION_RAW)):
        if action.get(CONF_ID) != config[CONF_ID]:
            continue
        if name in (ACTION_EXECUTE, ACTION_RAW) and not config[CONF_ALLOW_ACTIVE_SERVICES]:
            # U11. Sessions, routines, IO control, writes, security access and reset are reachable
            # only through these two actions AND only behind the flag; both are explicit on purpose.
            raise cv.Invalid(
                f"'{name}' on hub {config[CONF_ID]} needs '{CONF_ALLOW_ACTIVE_SERVICES}: true' on "
                f"that hub. Everything that is not a pure read can change ECU state, so the gate "
                f"is deliberate — set the flag only if transmitting it to this bus is intended.",
                path=[CONF_ALLOW_ACTIVE_SERVICES],
            )
        if name == ACTION_READ:
            group = resolve_group(ecu, action[CONF_SERVICE])
            if not group.safe_read:
                # U10: the scheduler and uds.read are the two paths that may run unattended, so
                # both are SAFE_READ-only whatever the flag says.
                raise cv.Invalid(
                    f"'uds.read' accepts SAFE_READ groups only, and {group.name!r} uses service "
                    f"0x{group.sid:02X}, which can change ECU state. Use 'uds.execute' on a hub "
                    f"with '{CONF_ALLOW_ACTIVE_SERVICES}: true' if that is really intended."
                )
            groups[group.name] = group
        elif name == ACTION_EXECUTE:
            group = resolve_group(ecu, action[CONF_SERVICE])
            groups[group.name] = group

    _check_transport(config, ecu, list(groups.values()))
    _check_siblings(config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


# --- codegen ---------------------------------------------------------------------------------


def _emit_blob(config_id: ID, blob: bytes):
    """The `embed: true` source: a flash-resident `const uint8_t[]` in the generated code.

    Sixteen bytes per line rather than one expression per byte: an ArrayInitializer of a few
    thousand elements is slow to render and unreadable in main.cpp, and the compiler sees the same
    array either way.
    """
    lines = [
        cg.RawExpression(", ".join(f"0x{byte:02X}" for byte in blob[offset : offset + 16]))
        for offset in range(0, len(blob), 16)
    ]
    arr_id = ID(f"{config_id}_catalog", is_declaration=True, type=cg.uint8)
    return cg.static_const_array(arr_id, cg.ArrayInitializer(*lines, multiline=True))


def _sizes(config) -> tuple[int, int, int]:
    """(bindings, text slots, poll slots) for one hub, from the entities that reference it.

    Counted here rather than accumulated during platform codegen because the arrays have to be
    allocated before the first `add_*_binding()` runs, and this is the only place that knows the
    whole list at that moment.
    """
    _cat, ecu = catalog_of(config)
    bindings = 0
    texts = 0
    polled: set[str] = set()
    for domain, entry in bound_entity_configs(CORE.config or {}, config[CONF_ID]):
        bindings += 1
        if domain == "text_sensor":
            texts += 1
        if binding_interval(entry) != 0:
            group, _field = resolve_field(
                ecu, entry[CONF_FIELD], entry.get(CONF_SERVICE), entry.get(CONF_ELEMENT, 0)
            )
            polled.add(group.name)
    return bindings, texts, len(polled)


async def to_code(config):
    isotp = await cg.get_variable(config[CONF_ISOTP_ID])
    bindings, texts, polled = _sizes(config)
    var = cg.new_Pvariable(config[CONF_ID], isotp)
    # Emitted with no `await` in between, so these land in main.cpp immediately after the `new`.
    # A platform's codegen cannot run before that point — it has to await this variable first — so
    # every array is allocated before the first binding is added.
    cg.add(var.reserve_bindings(bindings))
    cg.add(var.reserve_text_slots(texts))
    cg.add(var.reserve_poll_slots(polled))
    # The hub's id, so every line it logs says which hub said it. A config that talks to two ECUs
    # runs two hubs under one `uds` tag, and without this their lines are indistinguishable — two
    # hubs reading the same catalog even emit a byte-identical "catalog ok:" line. That ambiguity
    # cost a bench diagnosis: the hub whose YAML happened to carry an `on_value` lambda was the only
    # one anything could be attributed to, and the other one's silence read as a broken component.
    cg.add(var.set_log_name(str(config[CONF_ID])))
    await cg.register_component(var, config)

    catalog = config[CONF_CATALOG]
    _cat, crc, blob_size = load_catalog(catalog[CONF_FILE])
    cg.add(var.set_expected_crc(crc))
    if catalog[CONF_EMBED]:
        arr = _emit_blob(config[CONF_ID], Path(catalog[CONF_FILE]).read_bytes())
        cg.add(var.set_catalog_embedded(arr, blob_size))
    else:
        name, size = catalog[CONF_PARTITION], catalog[CONF_SIZE]
        # Subtype 0x06 is the generic "undefined" data subtype, which is what a private blob wants:
        # no IDF subsystem claims it, so nothing mounts or formats it behind our back. Offsets are
        # auto-placed by gen_esp32part.py, which is why the flasher reads them back from
        # partition-table.bin instead of assuming (design §7).
        if name not in CORE.data.setdefault("uds", {}).setdefault("partitions", set()):
            add_partition(name, "data", 0x06, size)
            CORE.data["uds"]["partitions"].add(name)
        cg.add(var.set_catalog_partition(name))

    if (ecu := config.get(CONF_ECU)) is not None:
        cg.add(var.set_ecu_name(ecu))
    cg.add(var.set_max_consecutive_failures(config[CONF_MAX_CONSECUTIVE_FAILURES]))
    cg.add(var.set_allow_active_services(config[CONF_ALLOW_ACTIVE_SERVICES]))

    for conf in config.get(CONF_ON_VALUE, []):
        await automation.build_callback_automation(var, "add_on_value_callback", [(UdsValue, "x")], conf)
    for conf in config.get(CONF_ON_ERROR, []):
        await automation.build_callback_automation(var, "add_on_error_callback", [(UdsError, "x")], conf)


# --- shared entity plumbing (sensor.py, text_sensor.py) ---------------------------------------

BINDING_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_UDS_ID): cv.use_id(UdsHub),
        cv.Required(CONF_FIELD): cv.string_strict,
        cv.Optional(CONF_SERVICE): _service_ref,
        cv.Optional(CONF_ELEMENT, default=0): cv.int_range(min=0, max=254),
    }
)


def binding_final_validate(config, *, want_text: bool):
    """U5-U9 for one bound entity, plus the unit the catalog owns.

    Runs here rather than in the platform schema because `field:` can only be resolved against the
    hub's catalog, and the hub is a `use_id` reference that does not exist until the full config
    does.
    """
    hub = hub_config_of(config[CONF_UDS_ID])
    _cat, ecu = catalog_of(hub)
    group, field = resolve_field(ecu, config[CONF_FIELD], config.get(CONF_SERVICE), config[CONF_ELEMENT])

    is_text = field.ascii or field.hexdump or field.is_enum
    if is_text != want_text:
        # U9. The catalog knows which of the two an entity has to be, so saying it wrongly is a
        # config error with a one-line fix rather than an entity that publishes nothing useful.
        kind = "an ASCII" if field.ascii else "a HEXDUMP" if field.hexdump else "an enumerated"
        if want_text:
            raise cv.Invalid(
                f"field {field.name!r} in group {group.name!r} is numeric; declare it under "
                f"'sensor:' instead of 'text_sensor:'",
                path=[CONF_FIELD],
            )
        raise cv.Invalid(
            f"field {field.name!r} in group {group.name!r} is {kind} field, so it has no numeric "
            f"value; declare it under 'text_sensor:' instead of 'sensor:'",
            path=[CONF_FIELD],
        )
    if want_text:
        # A text binding's value slot is a fixed char array, so what a field can render to has to
        # be bounded here — the device's copy is truncating, and a silently shortened state text is
        # a wrong value. HEXDUMP renders two characters per byte; an enum renders its scale text,
        # measured in UTF-8 bytes because that is what the catalog's string pool stores.
        if field.hexdump:
            rendered = field.bit_size // 4
            what = "renders to"
        elif field.ascii:
            rendered = field.bit_size // 8
            what = "is"
        else:
            rendered = max((len(s.text.encode("utf-8")) for s in field.scales), default=0)
            what = "has a state text of"
        if rendered + 1 > TEXT_SLOT_CAP:
            raise cv.Invalid(
                f"field {field.name!r} {what} {rendered} characters, past the {TEXT_SLOT_CAP - 1} a "
                f"text slot holds",
                path=[CONF_FIELD],
            )
    if len(group.request) > MAX_REQUEST:
        raise cv.Invalid(
            f"group {group.name!r} has a {len(group.request)}-byte request; this component "
            f"assembles at most {MAX_REQUEST}",
            path=[CONF_FIELD],
        )

    # Written back for to_code: the canonical group name is what the device resolves against, and
    # `service:` may have been an alias or a DID.
    config[CONF_RESOLVED_GROUP] = group.name
    config[CONF_RESOLVED_UNIT] = field.unit
    return config


def binding_interval(config) -> int:
    """`update_interval: never` means "read on demand only" (design §5), which the runtime spells 0."""
    ms = config[CONF_UPDATE_INTERVAL].total_milliseconds
    return 0 if ms >= SCHEDULER_DONT_RUN else ms


async def register_binding(config, entity, *, adder: str):
    hub = await cg.get_variable(config[CONF_UDS_ID])
    cg.add(
        getattr(hub, adder)(
            config[CONF_RESOLVED_GROUP],
            config[CONF_FIELD],
            config[CONF_ELEMENT],
            binding_interval(config),
            entity,
        )
    )


# --- actions ----------------------------------------------------------------------------------

_GROUP_ACTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(UdsHub),
        cv.Required(CONF_SERVICE): _service_ref,
    }
)


def _hub_config_from_core(uds_id: ID) -> dict[str, Any]:
    """The hub's config during codegen, where `fv.full_config` is no longer set.

    `CORE.config` is the validated config, so this is the same dict final validation saw — which is
    what lets an action turn `service: 0x0207` into the canonical group name the device resolves.
    """
    blocks = (CORE.config or {}).get("uds") or []
    if not isinstance(blocks, list):
        blocks = [blocks]
    for block in blocks:
        if block.get(CONF_ID) == uds_id:
            return block
    raise core.EsphomeError(f"no 'uds:' block with id {uds_id}")


async def _group_action_to_code(config, action_id, template_arg):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    _cat, ecu = catalog_of(_hub_config_from_core(config[CONF_ID]))
    cg.add(var.set_group(resolve_group(ecu, config[CONF_SERVICE]).name))
    return var


@automation.register_action(ACTION_READ, UdsReadAction, _GROUP_ACTION_SCHEMA, synchronous=True)
async def uds_read_to_code(config, action_id, template_arg, args):
    return await _group_action_to_code(config, action_id, template_arg)


@automation.register_action(ACTION_EXECUTE, UdsExecuteAction, _GROUP_ACTION_SCHEMA, synchronous=True)
async def uds_execute_to_code(config, action_id, template_arg, args):
    return await _group_action_to_code(config, action_id, template_arg)


RAW_ACTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(UdsHub),
        cv.Required(CONF_DATA): cv.templatable(
            cv.All(cv.ensure_list(cv.hex_uint8_t), cv.Length(min=1, max=MAX_REQUEST))
        ),
    }
)


@automation.register_action(ACTION_RAW, UdsRawAction, RAW_ACTION_SCHEMA, synchronous=True)
async def uds_raw_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    data = config[CONF_DATA]
    if isinstance(data, core.Lambda):
        templ = await cg.templatable(data, args, cg.std_vector.template(cg.uint8))
        cg.add(var.set_data_template(templ))
    else:
        cg.add(var.set_data_static(data))
    return var


SET_ENABLED_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(UdsHub),
        cv.Required(CONF_ENABLED): cv.templatable(cv.boolean),
    }
)


@automation.register_action(
    "uds.set_enabled", UdsSetEnabledAction, SET_ENABLED_SCHEMA, synchronous=True
)
async def uds_set_enabled_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(config[CONF_ENABLED], args, bool)
    cg.add(var.set_enabled(templ))
    return var
