"""portal: one self-describing state tree per device, served as one web page plus a JSON API from
ESPHome's shared `web_server_base`, with optional controls (actions) behind a login.

Contract: docs/portal-spec.md. Wire format `schema_version: 2`, checked by script/portal_schema.py.

The component consumes no bus frames itself; it reads other components' already-decoded entities
(sensor / text_sensor / binary_sensor) and, for actions, drives switch / button / select / number
entities, only from the main loop and only when the YAML declares the action.

Another external component can add data providers at import time with
`register_absent_provider()` and `register_series_provider()`; the portal ships none.
"""

import gzip
import logging
import re
from pathlib import Path

import esphome.codegen as cg
from esphome.components import (
    binary_sensor,
    button,
    number,
    select,
    sensor,
    socket,
    switch,
    text_sensor,
    web_server_base,
)
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_KEY,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_USERNAME,
    CONF_VALUE,
)
from esphome.core import CORE, ID
import esphome.final_validate as fv

CODEOWNERS = ["@swifty99"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["web_server_base", "binary_sensor", "sensor", "text_sensor", "socket"]

_LOGGER = logging.getLogger(__name__)

portal_ns = cg.esphome_ns.namespace("portal")
Portal = portal_ns.class_("Portal", cg.Component)

# The built-in page; `web_asset:` replaces it (spec 4).
BUILTIN_ASSET = Path(__file__).parent / "index.html"

CONF_BOARD = "board"
CONF_NODE_ID = "node_id"
CONF_MAC = "mac"
CONF_BUILD = "build"
CONF_OWNS = "owns"
CONF_ROOT_LABEL = "root_label"
CONF_BUDGET_MS = "budget_ms"
CONF_TEXT_MAX = "text_max"
CONF_SUBMODULE_SIZE = "submodule_size"
CONF_WEB_ASSET = "web_asset"
CONF_WEB_ASSET_ID = "web_asset_id"
CONF_AUTH = "auth"
CONF_TREE = "tree"

CONF_SEGMENT = "segment"
CONF_KIND = "kind"
CONF_LABEL = "label"
CONF_CHILDREN = "children"
CONF_VALUES = "values"
CONF_ACTIONS = "actions"
CONF_ROLE = "role"
CONF_ROLES_AVAILABLE = "roles_available"
CONF_ABSENT = "absent"
CONF_ABSENT_WHEN = "absent_when"
CONF_UNKNOWN_WHEN = "unknown_when"
CONF_TRUST_FROM = "trust_from"
CONF_SERIES = "series"
CONF_INDEX_BASE = "index_base"
CONF_INDEX_FRAME = "index_frame"
CONF_COUNT = "count"
CONF_SUBMODULE_POSITION = "submodule_position"
CONF_UNIT = "unit"
CONF_DECIMALS = "decimals"
CONF_SENSOR = "sensor"
CONF_TEXT_SENSOR = "text_sensor"
CONF_BINARY_SENSOR = "binary_sensor"
CONF_SWITCH = "switch"
CONF_BUTTON = "button"
CONF_SELECT = "select"
CONF_NUMBER = "number"
CONF_CADENCE_MS = "cadence_ms"
CONF_EQUALS = "equals"
CONF_WHY = "why"
CONF_SOURCE = "source"
CONF_PROVIDER = "provider"

# Written back during validation so `esphome config` shows the effective result, and so to_code
# reads the very values the tests assert on.
CONF_RESOLVED_PACK_INDEX_BASE = "resolved_pack_index_base"
CONF_RESOLVED_OWNS = "resolved_owns"
CONF_RESOLVED_COMPONENTS = "resolved_components"

# The one frame name the pack-index offset is valid for: never inferred, never defaulted to.
SUBMODULE_CELL_FRAME = "submodule_cell"

_IDENT_RE = re.compile(r"^[a-z][a-z0-9_]*$")
_MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")

# Top-level config keys that are infrastructure, not behaviour: never part of served_by.components.
_NON_COMPONENT_TOP_LEVEL_KEYS = frozenset({"substitutions", "packages", "esphome", "external_components"})


def _identifier(what):
    def validate(value):
        value = cv.string_strict(value)
        if not _IDENT_RE.match(value):
            raise cv.Invalid(f"{what} {value!r} must match [a-z][a-z0-9_]*")
        return value

    return validate


_segment = _identifier("segment")
_value_key = _identifier("key")
_why_code = _identifier("why code")
_role_name = _identifier("role")

# These why-codes derive their meaning from evidence (counters, response codes) that a
# config-declared why cannot carry, so they are unrepresentable in `unknown_when.why`, and in
# `absent` unless the chosen `absent_when` provider declares evidence for exactly that code.
_EVIDENCE_GATED_WHY_CODES = frozenset({"never_announced", "no_transfer", "crc_fail", "negative_response"})


def _config_why_code(value):
    """Backs `unknown_when.why`: unconditionally refuses the evidence-gated codes."""
    value = _why_code(value)
    if value in _EVIDENCE_GATED_WHY_CODES:
        raise cv.Invalid(
            f"why code {value!r} is evidence-gated: its verdict is drawn from evidence this "
            "component has no mechanism to attach to a config-declared why, so declaring it here "
            "would put an evidence-free claim on the wire. 'absent' with an 'absent_when' "
            "provider that declares matching evidence is the one place it is legal."
        )
    return value


# ---------------------------------------------------------------------------------------------
# Provider registry (spec 4.6). The portal ships none; another external component calls the two
# register functions at import time.
#   ABSENT_PROVIDERS[name]  = {args, source_type, cast, body, evidence, for_why}
#   SERIES_PROVIDERS[name]  = {args, source_type, cast, body}
# `body` is C++ text, `str.format`-ed with the provider's resolved args; it sees `source`, the
# `source` object as a `cast *`. An absent body returns bool (true -> absent). A series body
# returns bool and fills `out` for the zero-based `index` (SeriesElement: cls, has_value, value).
ABSENT_PROVIDERS: dict = {}
SERIES_PROVIDERS: dict = {}


def register_absent_provider(name, *, args, source_type, body, evidence=None, for_why=()):
    ABSENT_PROVIDERS[name] = {
        "args": dict(args),
        "source_type": source_type,
        "cast": str(source_type),
        "body": body,
        "evidence": evidence,
        "for_why": tuple(for_why),
    }


def register_series_provider(name, *, args, source_type, body):
    SERIES_PROVIDERS[name] = {
        "args": dict(args),
        "source_type": source_type,
        "cast": str(source_type),
        "body": body,
    }


def _registered(registry):
    names = sorted(registry)
    return names if names else "none registered"


def _provider_backs_why(entry: dict, code: str) -> bool:
    """`for_why` alone is not enough: a provider naming a code but carrying no `evidence`
    fragment would ship the wire-invalid `"evidence":{}`. Both must be present together."""
    return bool(entry.get("evidence")) and code in entry.get("for_why", ())


def _validate_absent_evidence(value):
    code = value.get(CONF_ABSENT)
    if code is None or code not in _EVIDENCE_GATED_WHY_CODES:
        return value
    absent_when = value.get(CONF_ABSENT_WHEN)
    provider = absent_when[CONF_PROVIDER] if absent_when else None
    spec = ABSENT_PROVIDERS.get(provider) if provider else None
    if spec is None or not _provider_backs_why(spec, code):
        permitted = sorted(n for n, e in ABSENT_PROVIDERS.items() if _provider_backs_why(e, code))
        raise cv.Invalid(
            f"why code {code!r} is evidence-gated: its verdict needs evidence this component has "
            "no mechanism to attach to a config-declared why on its own. 'absent_when' opens it "
            f"only for a provider that declares matching evidence (permitted here: "
            f"{permitted or 'none'}); declare one, or use a different code."
        )
    return value


def _check_provider_args(what, provider, spec, present_args):
    declared = set(spec["args"])
    missing = declared - present_args
    if missing:
        raise cv.Invalid(f"{what}.provider {provider!r} requires {sorted(missing)}")
    extra = present_args - declared
    if extra:
        raise cv.Invalid(f"{what}.provider {provider!r} does not accept {sorted(extra)}")


def _validated_args(what, spec, value):
    out = {}
    for arg_key, validator in spec["args"].items():
        try:
            out[arg_key] = validator(value[arg_key])
        except cv.Invalid as err:
            raise cv.Invalid(f"{what}.{arg_key}: {err}") from err
    return out


def _absent_when_schema(value):
    if not isinstance(value, dict):
        raise cv.Invalid("absent_when must be a mapping")
    if CONF_PROVIDER not in value:
        raise cv.Invalid("absent_when requires 'provider'")
    provider = value[CONF_PROVIDER]
    if provider not in ABSENT_PROVIDERS:
        raise cv.Invalid(f"absent_when.provider {provider!r} is not one of {_registered(ABSENT_PROVIDERS)}")
    if CONF_SOURCE not in value:
        raise cv.Invalid("absent_when requires 'source'")
    spec = ABSENT_PROVIDERS[provider]
    _check_provider_args("absent_when", provider, spec, set(value) - {CONF_SOURCE, CONF_PROVIDER})
    out = dict(value)
    out[CONF_SOURCE] = cv.use_id(spec["source_type"])(value[CONF_SOURCE])
    out.update(_validated_args("absent_when", spec, value))
    return out


def _nonempty_string(value):
    value = cv.string_strict(value)
    if value == "":
        raise cv.Invalid("must not be empty")
    return value


def _port(value):
    value = cv.port(value)
    if value in (32767, 65535):
        raise cv.Invalid(f"port {value} is forbidden: it wedges httpd_stop() and OTA")
    return value


def _mac(value):
    value = cv.string_strict(value)
    if not _MAC_RE.match(value):
        raise cv.Invalid(f"mac {value!r} must be 6 colon-separated hex octets")
    return value.upper()


def _node_id(value):
    if isinstance(value, bool):
        raise cv.Invalid("node_id must be an int or a string")
    if isinstance(value, int):
        return f"0x{value:X}"
    if isinstance(value, str):
        try:
            parsed = int(value, 0)
        except ValueError as err:
            raise cv.Invalid(f"node_id {value!r} is not a valid integer") from err
        return f"0x{parsed:X}"
    raise cv.Invalid("node_id must be an int or a string")


def _budget_ms(value):
    """A node budget: a positive number of milliseconds, or an explicit null.

    Explicit null is distinct from omitting the key (omitted -> derived from `cadence_ms`). Every
    null spelling is accepted: `budget_ms:`, `budget_ms: null` and `budget_ms: ~` all reach a
    validator as Python None, while `cv.none` accepts only the strings "none"/"None".
    """
    if value is None or value in ("none", "None"):
        return None
    return cv.int_range(min=1)(value)


def _component_level_budget_removed(value):
    raise cv.Invalid(
        "budget_ms is a per-node key, not a component-level one: put it on a 'tree' node, or "
        "omit it and declare 'cadence_ms' on that node's values so the budget is derived. A "
        "component-level default would be the fallback number a node with unjudgeable "
        "freshness must never serve."
    )


def _unique_segments(nodes):
    seen = set()
    for node in nodes:
        segment = node[CONF_SEGMENT]
        if segment in seen:
            raise cv.Invalid(f"duplicate segment {segment!r} among siblings")
        seen.add(segment)
    return nodes


VALUE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_KEY): _value_key,
        cv.Required(CONF_LABEL): _nonempty_string,
        cv.Optional(CONF_UNIT): cv.string_strict,
        cv.Optional(CONF_DECIMALS, default=0): cv.int_range(min=0, max=6),
        cv.Optional(CONF_CADENCE_MS): cv.int_range(min=1),
        cv.Exclusive(CONF_SENSOR, "entity"): cv.use_id(sensor.Sensor),
        cv.Exclusive(CONF_TEXT_SENSOR, "entity"): cv.use_id(text_sensor.TextSensor),
        cv.Exclusive(CONF_BINARY_SENSOR, "entity"): cv.use_id(binary_sensor.BinarySensor),
    }
)

_ENTITY_KEYS = (CONF_SENSOR, CONF_TEXT_SENSOR, CONF_BINARY_SENSOR)


def _require_one_entity(value):
    present = [key for key in _ENTITY_KEYS if key in value]
    if len(present) != 1:
        raise cv.Invalid("a value needs exactly one of 'sensor', 'text_sensor' or 'binary_sensor'")
    return value


def _no_cadence_on_binary_sensor(value):
    """ESPHome fires no state callback when a boolean republishes an unchanged value, so a
    `cadence_ms` on a binary_sensor value would be a number the portal cannot observe."""
    if CONF_BINARY_SENSOR in value and CONF_CADENCE_MS in value:
        raise cv.Invalid(
            "a value bound to 'binary_sensor' may not declare 'cadence_ms': a cadence this "
            "component cannot observe (no callback fires on an unchanged boolean republish) is "
            "an invented number"
        )
    return value


VALUE_SCHEMA = cv.All(VALUE_SCHEMA, _require_one_entity, _no_cadence_on_binary_sensor)

_index_frame_name = _identifier("index_frame")


def _index_frame(value):
    """Required-but-nullable: a producer that cannot name the frame must STATE that explicitly.
    Every YAML null spelling resolves to Python None before this runs, so None always means "the
    config said null"; omission is rejected by `cv.Required`."""
    if value is None:
        return None
    return _index_frame_name(value)


_SERIES_BASE_KEYS = frozenset(
    {CONF_KEY, CONF_LABEL, CONF_UNIT, CONF_INDEX_BASE, CONF_INDEX_FRAME, CONF_COUNT, CONF_DECIMALS}
    | {CONF_SOURCE, CONF_PROVIDER}
)


def _validate_series_provider(value):
    value = dict(value)
    has_source = CONF_SOURCE in value
    has_provider = CONF_PROVIDER in value
    if has_provider and not has_source:
        raise cv.Invalid("series.provider requires series.source")
    if has_source and not has_provider:
        raise cv.Invalid("series.source requires series.provider")

    present_args = {key for key in value if key not in _SERIES_BASE_KEYS}
    if not has_provider:
        if present_args:
            raise cv.Invalid(f"series {sorted(present_args)} may only be given alongside 'provider'")
        return value

    provider = value[CONF_PROVIDER]
    if provider not in SERIES_PROVIDERS:
        raise cv.Invalid(f"series.provider {provider!r} is not one of {_registered(SERIES_PROVIDERS)}")
    spec = SERIES_PROVIDERS[provider]
    _check_provider_args("series", provider, spec, present_args)
    value[CONF_SOURCE] = cv.use_id(spec["source_type"])(value[CONF_SOURCE])
    value.update(_validated_args("series", spec, value))
    return value


SERIES_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_KEY): _value_key,
            cv.Required(CONF_LABEL): _nonempty_string,
            cv.Optional(CONF_UNIT): cv.string_strict,
            cv.Required(CONF_INDEX_BASE): cv.int_range(min=0),
            cv.Required(CONF_INDEX_FRAME): _index_frame,
            cv.Required(CONF_COUNT): cv.int_range(min=1, max=64),
            cv.Optional(CONF_DECIMALS, default=0): cv.int_range(min=0, max=6),
            cv.Optional(CONF_SOURCE): cv.string_strict,
            cv.Optional(CONF_PROVIDER): cv.string_strict,
        },
        extra=cv.ALLOW_EXTRA,  # provider arguments: checked against the chosen provider
    ),
    _validate_series_provider,
)

UNKNOWN_WHEN_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_VALUE): _value_key,
        cv.Required(CONF_EQUALS): cv.string_strict,
        cv.Required(CONF_WHY): _config_why_code,
    }
)

_ACTION_CONTROL_KEYS = (CONF_SWITCH, CONF_BUTTON, CONF_SELECT, CONF_NUMBER)
DEFAULT_ACTION_DECIMALS = 2


def _validate_action(value):
    present = [key for key in _ACTION_CONTROL_KEYS if key in value]
    if len(present) != 1:
        raise cv.Invalid("an action needs exactly one of 'switch', 'button', 'select' or 'number'")
    value = dict(value)
    if CONF_DECIMALS in value and present[0] != CONF_NUMBER:
        raise cv.Invalid("'decimals' is only valid on a 'number' action")
    if present[0] == CONF_NUMBER:
        value.setdefault(CONF_DECIMALS, DEFAULT_ACTION_DECIMALS)
    return value


ACTION_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_KEY): _value_key,
            cv.Required(CONF_LABEL): _nonempty_string,
            cv.Optional(CONF_SWITCH): cv.use_id(switch.Switch),
            cv.Optional(CONF_BUTTON): cv.use_id(button.Button),
            cv.Optional(CONF_SELECT): cv.use_id(select.Select),
            cv.Optional(CONF_NUMBER): cv.use_id(number.Number),
            cv.Optional(CONF_DECIMALS): cv.int_range(min=0, max=6),
        }
    ),
    _validate_action,
)


def _node_schema(value):
    return NODE_SCHEMA(value)


def _validate_node_cross_fields(value):
    if CONF_ABSENT_WHEN in value and CONF_ABSENT not in value:
        raise cv.Invalid("'absent_when' requires 'absent' on the same node")

    if CONF_ABSENT in value:
        if CONF_ABSENT_WHEN in value:
            # A conditionally-absent node is a real node: values/children/series/actions stay.
            exclusive_with_absent = [key for key in (CONF_ROLE, CONF_UNKNOWN_WHEN) if key in value]
        else:
            exclusive_with_absent = [
                key
                for key in (
                    CONF_VALUES,
                    CONF_CHILDREN,
                    CONF_ROLE,
                    CONF_UNKNOWN_WHEN,
                    CONF_SERIES,
                    CONF_ACTIONS,
                )
                if key in value
            ]
        if exclusive_with_absent:
            raise cv.Invalid(f"'absent' is mutually exclusive with {exclusive_with_absent}")
        _validate_absent_evidence(value)

    if CONF_ROLE in value:
        roles_available = value.get(CONF_ROLES_AVAILABLE)
        if not roles_available:
            raise cv.Invalid("'role' requires a non-empty 'roles_available' list")
        if value[CONF_ROLE] not in roles_available:
            raise cv.Invalid(f"role {value[CONF_ROLE]!r} is not in roles_available {roles_available!r}")

    own_keys = {v[CONF_KEY] for v in value.get(CONF_VALUES, [])}

    if CONF_SERIES in value and value[CONF_SERIES][CONF_KEY] in own_keys:
        raise cv.Invalid(
            f"series key {value[CONF_SERIES][CONF_KEY]!r} collides with one of this node's "
            "own value keys -- a node's own values and its series share one key namespace"
        )

    action_keys = [a[CONF_KEY] for a in value.get(CONF_ACTIONS, [])]
    for key in action_keys:
        if action_keys.count(key) > 1:
            raise cv.Invalid(f"duplicate action key {key!r} on one node")

    if CONF_UNKNOWN_WHEN in value:
        if CONF_CHILDREN in value:
            raise cv.Invalid("'unknown_when' cannot be combined with 'children'")
        target = value[CONF_UNKNOWN_WHEN][CONF_VALUE]
        if target not in own_keys:
            raise cv.Invalid(f"unknown_when.value {target!r} is not one of this node's own values")

    if CONF_TRUST_FROM in value and value[CONF_TRUST_FROM] not in own_keys:
        raise cv.Invalid(f"trust_from {value[CONF_TRUST_FROM]!r} is not one of this node's own values")

    # A node carrying a binary_sensor value may not declare a positive budget_ms: a boolean's age
    # is since its last CHANGE, so a finite budget would let a healthy, still-reconfirming boolean
    # tip 'stale'. `budget_ms: null` stays legal.
    if value.get(CONF_BUDGET_MS) is not None and any(CONF_BINARY_SENSOR in v for v in value.get(CONF_VALUES, [])):
        raise cv.Invalid(
            "a node carrying a 'binary_sensor' value may not declare an explicit positive "
            "'budget_ms': a boolean's age is since its last CHANGE, not its last publish (no "
            "callback fires on an unchanged republish), so a finite budget would let a "
            "healthy, still-reconfirming boolean tip 'stale'. Omit budget_ms, or declare it "
            "explicitly null"
        )

    return value


def _apply_derived_budget(value):
    """Resolves every node's effective `budget_ms` at schema time and writes it back, so
    `esphome config` shows it. An explicit `budget_ms:` (any null spelling normalised to None)
    wins; otherwise no own values, or any own value without `cadence_ms`, means None; else 3x the
    slowest cadence."""
    value = dict(value)
    if CONF_BUDGET_MS in value:
        return value
    values = value.get(CONF_VALUES, [])
    if not values:
        value[CONF_BUDGET_MS] = None
        return value
    cadences = [v.get(CONF_CADENCE_MS) for v in values]
    value[CONF_BUDGET_MS] = None if any(c is None for c in cadences) else 3 * max(cadences)
    return value


NODE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_SEGMENT): _segment,
            cv.Required(CONF_KIND): _nonempty_string,
            cv.Required(CONF_LABEL): _nonempty_string,
            cv.Optional(CONF_BUDGET_MS): _budget_ms,
            cv.Optional(CONF_CHILDREN): cv.All(cv.ensure_list(_node_schema), cv.Length(min=1), _unique_segments),
            cv.Optional(CONF_VALUES): cv.All(cv.ensure_list(VALUE_SCHEMA), cv.Length(min=1)),
            cv.Optional(CONF_ACTIONS): cv.All(cv.ensure_list(ACTION_SCHEMA), cv.Length(min=1)),
            cv.Optional(CONF_ROLE): _role_name,
            cv.Optional(CONF_ROLES_AVAILABLE): cv.All(cv.ensure_list(_role_name), cv.Length(min=1)),
            cv.Optional(CONF_ABSENT): _why_code,
            cv.Optional(CONF_ABSENT_WHEN): _absent_when_schema,
            cv.Optional(CONF_UNKNOWN_WHEN): UNKNOWN_WHEN_SCHEMA,
            cv.Optional(CONF_TRUST_FROM): _value_key,
            cv.Optional(CONF_SERIES): SERIES_SCHEMA,
            cv.Optional(CONF_SUBMODULE_POSITION): cv.int_range(min=1),
        }
    ),
    _validate_node_cross_fields,
    _apply_derived_budget,
)

# `auth:` is optional, also with actions. When set, it guards the POST actions only; viewing
# (the page and /api/v1/state) is always open.
AUTH_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_USERNAME): _nonempty_string,
        cv.Required(CONF_PASSWORD): _nonempty_string,
    }
)


def _web_asset(value):
    """`web_asset:` replaces the built-in page. Relative paths resolve against the YAML."""
    value = cv.string_strict(value)
    path = Path(value)
    if not path.is_absolute():
        try:
            path = CORE.relative_config_path(value)
        except (TypeError, AttributeError):  # no YAML context (direct schema call)
            path = Path(value)
    path = Path(path)
    if not path.is_file():
        raise cv.Invalid(f"web_asset file {str(path)!r} does not exist")
    return str(path)


def _iter_tree(nodes, prefix):
    """Depth-first (dotted_id, node_config) pairs. `tree:` is the synthetic root's children, so a
    top-level node's id is its bare segment; every level below prefixes with its parent."""
    for node in nodes:
        node_id = node[CONF_SEGMENT] if not prefix else f"{prefix}.{node[CONF_SEGMENT]}"
        yield node_id, node
        yield from _iter_tree(node.get(CONF_CHILDREN, []), node_id)


def _iter_tree_with_parent(nodes, prefix, parent_id):
    for node in nodes:
        node_id = node[CONF_SEGMENT] if not prefix else f"{prefix}.{node[CONF_SEGMENT]}"
        yield node_id, node, parent_id
        yield from _iter_tree_with_parent(node.get(CONF_CHILDREN, []), node_id, node_id)


def _resolve_submodule_positions(config):
    """Written back onto each series config as `resolved_pack_index_base` (needs the whole tree).

    1. No two nodes may declare the same `submodule_position`, and a position needs the
       top-level `submodule_size`.
    2. Every node carrying a `series` resolves `pack_index_base` from the NEAREST ancestor at or
       above itself that declares a position. None -> null. Found: `index_frame` decides:
       `submodule_cell` -> (position - 1) * submodule_size + index_base; explicit null -> error
       (an unnamed frame never gets an offset); any other named frame -> null."""
    all_nodes = list(_iter_tree_with_parent(config[CONF_TREE], "", None))
    by_id = {node_id: node for node_id, node, _ in all_nodes}
    parent_of = {node_id: parent_id for node_id, _, parent_id in all_nodes}

    positions_seen: dict[int, str] = {}
    for node_id, node, _ in all_nodes:
        if CONF_SUBMODULE_POSITION not in node:
            continue
        position = node[CONF_SUBMODULE_POSITION]
        if position in positions_seen:
            raise cv.Invalid(
                f"submodule_position {position} is declared by both {positions_seen[position]!r} "
                f"and {node_id!r}: a submodule's position is declared once"
            )
        positions_seen[position] = node_id
        if CONF_SUBMODULE_SIZE not in config:
            raise cv.Invalid(
                f"node {node_id!r} declares submodule_position but the top-level 'submodule_size' is not set"
            )

    for node_id, node, _ in all_nodes:
        if CONF_SERIES not in node:
            continue
        walk = node_id
        position = None
        position_node_id = None
        while walk is not None:
            candidate = by_id[walk]
            if CONF_SUBMODULE_POSITION in candidate:
                position = candidate[CONF_SUBMODULE_POSITION]
                position_node_id = walk
                break
            walk = parent_of.get(walk)
        series = node[CONF_SERIES]
        if position is None:
            series[CONF_RESOLVED_PACK_INDEX_BASE] = None
            continue
        index_frame = series.get(CONF_INDEX_FRAME)
        if index_frame is None:
            raise cv.Invalid(
                f"series {series.get(CONF_KEY)!r} on node {node_id!r} states index_frame: null, but "
                f"{position_node_id!r} declares submodule_position {position}: deriving "
                "pack_index_base would add a submodule-sized offset to indices whose frame this "
                f"producer declined to name. State index_frame: {SUBMODULE_CELL_FRAME} if these "
                "indices do count submodule cells, or keep the series out from under a declared "
                "submodule_position; the frame is never recovered from kind, key, label or unit."
            )
        if index_frame == SUBMODULE_CELL_FRAME:
            series[CONF_RESOLVED_PACK_INDEX_BASE] = (position - 1) * config[CONF_SUBMODULE_SIZE] + series[
                CONF_INDEX_BASE
            ]
        else:
            series[CONF_RESOLVED_PACK_INDEX_BASE] = None


def _validate_owns(config):
    """Spec 4.2: `owns` is YAML, never derived from which components are compiled."""
    owns = list(config.get(CONF_OWNS, []))
    all_ids = {node_id for node_id, _ in _iter_tree(config[CONF_TREE], "")}
    seen = set()
    for owned in owns:
        if owned not in all_ids:
            raise cv.Invalid(f"owns entry {owned!r} names no node in 'tree'")
        if owned in seen:
            raise cv.Invalid(f"owns entry {owned!r} is listed twice")
        seen.add(owned)
    identity_complete = CONF_BOARD in config and CONF_NODE_ID in config and CONF_MAC in config
    if identity_complete and not owns:
        raise cv.Invalid(
            "board/node_id/mac are all set, so served_by.state would be 'ok', but 'owns' is empty: "
            "an identity that serves state 'ok' must own something. List the node ids this device "
            "owns under 'owns:', or drop part of the identity."
        )
    config[CONF_RESOLVED_OWNS] = owns


def _check_tree(config):
    """Rules that need the whole tree. Run at schema time (also reachable without the loader) and
    again in the final validation; both are idempotent."""
    _validate_owns(config)
    _resolve_submodule_positions(config)
    return config


def _consume_sockets(config):
    # The listening socket belongs to web_server_base.
    socket.consume_sockets(3, "portal")(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Portal),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.GenerateID(CONF_WEB_ASSET_ID): cv.declare_id(cg.uint8),
            cv.Optional(CONF_PORT, default=80): _port,
            cv.Optional(CONF_BOARD): cv.string_strict,
            cv.Optional(CONF_NODE_ID): _node_id,
            cv.Optional(CONF_MAC): _mac,
            cv.Required(CONF_BUILD): cv.string_strict,
            cv.Optional(CONF_OWNS): cv.ensure_list(cv.string_strict),
            cv.Optional(CONF_ROOT_LABEL, default="Vehicle"): _nonempty_string,
            # Removed in favour of per-node budgets, kept as an explaining refusal.
            cv.Optional(CONF_BUDGET_MS): _component_level_budget_removed,
            cv.Optional(CONF_TEXT_MAX, default=48): cv.int_range(min=16, max=256),
            cv.Optional(CONF_SUBMODULE_SIZE): cv.int_range(min=1),
            cv.Optional(CONF_WEB_ASSET): _web_asset,
            cv.Optional(CONF_AUTH): AUTH_SCHEMA,
            cv.Required(CONF_TREE): cv.All(cv.ensure_list(_node_schema), cv.Length(min=1), _unique_segments),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
    _check_tree,
    _consume_sockets,
)


def _final_validate(config):
    full = fv.full_config.get()

    if "web_server" in full:
        raise cv.Invalid(
            "portal and web_server cannot be combined: both serve '/' on the one web_server_base, "
            "and web_server's auth defines/credentials would govern the portal's. The portal "
            "replaces web_server; remove 'web_server:'."
        )

    _check_tree(config)
    config[CONF_RESOLVED_COMPONENTS] = sorted(key for key in full if key not in _NON_COMPONENT_TOP_LEVEL_KEYS)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _string_array(prefix, values):
    """A `static const char *const NAME[] = {...}` global, the shape Identity.owns and
    Identity.components need."""
    arr_id = ID(f"{prefix}_data", is_declaration=True, type=cg.global_ns.namespace("constexpr char * const"))
    return cg.static_const_array(arr_id, cg.ArrayInitializer(*values))


def _provider_thunk_name(provider: str, args: dict) -> str:
    """Deterministic, never from a counter: first letter of each declared arg name plus its
    resolved value, in the provider's declared order (`cells` + {a: 0, b: 3} ->
    `portal_prov_cells_a0_b3`)."""
    suffix = "".join(f"_{key[0]}{value}" for key, value in args.items())
    return f"portal_prov_{provider}{suffix}"


# `cg.add_global` lands this text right after `#include "esphome.h"` in main.cpp, so every
# compiled component's headers are visible with no include added to any component source. An
# absence predicate must return TRUE for a null source ("a node whose provider is not there is
# absent, never silently present"), which a provider's body folds into its own expression.
_ABSENT_THUNK_TEMPLATE = """
inline bool {name}(void *ctx) {{
  auto *source = static_cast<{cast} *>(ctx);
  {body}
}}
"""

_SERIES_THUNK_TEMPLATE = """
inline bool {name}(void *ctx, uint16_t index, esphome::portal::core::SeriesElement *out) {{
  auto *source = static_cast<{cast} *>(ctx);
  if (source == nullptr)
    return false;
  {body}
}}
"""


async def to_code(config):
    cg.add_define("USE_PORTAL")

    nodes = list(_iter_tree(config[CONF_TREE], ""))
    cg.add_define("PORTAL_MAX_NODES", len(nodes) + 1)  # +1 for the synthetic root
    cg.add_define("PORTAL_MAX_VALUES", max(sum(len(n.get(CONF_VALUES, [])) for _, n in nodes), 1))
    cg.add_define("PORTAL_MAX_SERIES", max(sum(1 for _, n in nodes if CONF_SERIES in n), 1))
    cg.add_define("PORTAL_MAX_ACTIONS", max(sum(len(n.get(CONF_ACTIONS, [])) for _, n in nodes), 1))
    cg.add_define("PORTAL_TEXT_MAX", config[CONF_TEXT_MAX])

    auth = config.get(CONF_AUTH)
    if auth is None and any(CONF_ACTIONS in node for _, node in nodes):
        _LOGGER.warning(
            "portal: actions are reachable without a login; access control is the network "
            "(add 'auth:' for an additional login on the actions)"
        )
    if auth is not None:
        cg.add_define("USE_WEBSERVER_AUTH")
        cg.add_define("USE_WEBSERVER_AUTH_DIGEST")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_base(paren))
    cg.add(paren.set_port(config[CONF_PORT]))

    asset_path = Path(config[CONF_WEB_ASSET]) if CONF_WEB_ASSET in config else BUILTIN_ASSET
    compressed = gzip.compress(asset_path.read_bytes(), compresslevel=9, mtime=0)
    asset = cg.progmem_array(config[CONF_WEB_ASSET_ID], list(compressed))
    cg.add(var.set_web_asset(asset, len(compressed)))

    cg.add(var.set_build(config[CONF_BUILD]))
    if CONF_BOARD in config:
        cg.add(var.set_board(config[CONF_BOARD]))
    if CONF_NODE_ID in config:
        cg.add(var.set_node_id(config[CONF_NODE_ID]))
    if CONF_MAC in config:
        cg.add(var.set_mac(config[CONF_MAC]))
    if auth is not None:
        cg.add(var.set_auth(auth[CONF_USERNAME], auth[CONF_PASSWORD]))

    owns = config[CONF_RESOLVED_OWNS]
    components_list = config[CONF_RESOLVED_COMPONENTS]
    owns_id = str(config[CONF_ID])
    owns_arr = _string_array(f"{owns_id}_owns", owns) if owns else cg.RawExpression("nullptr")
    components_arr = (
        _string_array(f"{owns_id}_components", components_list) if components_list else cg.RawExpression("nullptr")
    )
    cg.add(var.set_owns(owns_arr, len(owns)))
    cg.add(var.set_components(components_arr, len(components_list)))

    null = cg.RawExpression("nullptr")
    counter = {"next": 0}
    # One thunk per distinct (provider, args), deduplicated by its deterministic name and scoped
    # to this call: dedup must hold within one image and never leak across validation runs.
    emitted_thunks: set[str] = set()

    def emit_thunk(registry, provider, args, template):
        spec = registry[provider]
        name = _provider_thunk_name(provider, args)
        if name not in emitted_thunks:
            emitted_thunks.add(name)
            body = spec["body"].format(**args)
            cg.add_global(cg.RawStatement(template.format(name=name, cast=spec["cast"], body=body)))
        return name

    async def emit(node_conf, dotted_id, parent_index):
        index = counter["next"]
        counter["next"] += 1

        absent_why = node_conf.get(CONF_ABSENT)
        role = node_conf.get(CONF_ROLE)
        roles_available = node_conf.get(CONF_ROLES_AVAILABLE)
        roles_csv = ",".join(roles_available) if roles_available else None
        unknown_when = node_conf.get(CONF_UNKNOWN_WHEN)
        trust_from = node_conf.get(CONF_TRUST_FROM)
        # Resolved for every node by _apply_derived_budget. Indexed, never .get(): a node that
        # skipped derivation must stop codegen, not quietly acquire a budget nobody derived.
        derived_budget = node_conf[CONF_BUDGET_MS]
        budget_known = derived_budget is not None
        budget_ms = derived_budget if budget_known else 0

        absent_fn = null
        absent_ctx = null
        absent_evidence = null
        absent_when = node_conf.get(CONF_ABSENT_WHEN)
        if absent_when is not None:
            provider = absent_when[CONF_PROVIDER]
            spec = ABSENT_PROVIDERS[provider]
            args = {key: absent_when[key] for key in spec["args"]}
            absent_fn = cg.RawExpression(emit_thunk(ABSENT_PROVIDERS, provider, args, _ABSENT_THUNK_TEMPLATE))
            absent_ctx = await cg.get_variable(absent_when[CONF_SOURCE])
            if spec.get("evidence"):
                absent_evidence = spec["evidence"]

        cg.add(
            var.add_node(
                dotted_id,
                node_conf[CONF_KIND],
                node_conf[CONF_LABEL],
                parent_index,
                budget_ms,
                budget_known,
                absent_why if absent_why is not None else null,
                role if role is not None else null,
                roles_csv if roles_csv is not None else null,
                unknown_when[CONF_VALUE] if unknown_when else null,
                unknown_when[CONF_EQUALS] if unknown_when else null,
                unknown_when[CONF_WHY] if unknown_when else null,
                trust_from if trust_from is not None else null,
                absent_fn,
                absent_ctx,
                absent_evidence,
            )
        )

        for value_conf in node_conf.get(CONF_VALUES, []):
            unit = value_conf.get(CONF_UNIT)
            unit_arg = unit if unit is not None else null
            decimals = value_conf[CONF_DECIMALS]
            if CONF_SENSOR in value_conf:
                entity = await cg.get_variable(value_conf[CONF_SENSOR])
                cg.add(var.add_value_sensor(value_conf[CONF_KEY], value_conf[CONF_LABEL], unit_arg, decimals, entity))
            elif CONF_TEXT_SENSOR in value_conf:
                entity = await cg.get_variable(value_conf[CONF_TEXT_SENSOR])
                cg.add(
                    var.add_value_text_sensor(value_conf[CONF_KEY], value_conf[CONF_LABEL], unit_arg, decimals, entity)
                )
            else:
                entity = await cg.get_variable(value_conf[CONF_BINARY_SENSOR])
                cg.add(var.add_value_binary_sensor(value_conf[CONF_KEY], value_conf[CONF_LABEL], unit_arg, entity))

        series_conf = node_conf.get(CONF_SERIES)
        if series_conf is not None:
            element = null
            ctx = null
            if CONF_PROVIDER in series_conf:
                provider = series_conf[CONF_PROVIDER]
                spec = SERIES_PROVIDERS[provider]
                args = {key: series_conf[key] for key in spec["args"]}
                element = cg.RawExpression(emit_thunk(SERIES_PROVIDERS, provider, args, _SERIES_THUNK_TEMPLATE))
                ctx = await cg.get_variable(series_conf[CONF_SOURCE])
            series_unit = series_conf.get(CONF_UNIT)
            index_frame = series_conf[CONF_INDEX_FRAME]
            resolved_pack_index_base = series_conf.get(CONF_RESOLVED_PACK_INDEX_BASE)
            pack_index_known = resolved_pack_index_base is not None
            cg.add(
                var.add_series(
                    series_conf[CONF_KEY],
                    series_conf[CONF_LABEL],
                    series_unit if series_unit is not None else null,
                    index_frame if index_frame is not None else null,
                    series_conf[CONF_INDEX_BASE],
                    resolved_pack_index_base if pack_index_known else 0,
                    pack_index_known,
                    series_conf[CONF_COUNT],
                    series_conf[CONF_DECIMALS],
                    element,
                    ctx,
                )
            )

        for action_conf in node_conf.get(CONF_ACTIONS, []):
            key, label = action_conf[CONF_KEY], action_conf[CONF_LABEL]
            if CONF_SWITCH in action_conf:
                entity = await cg.get_variable(action_conf[CONF_SWITCH])
                cg.add(var.add_action_switch(key, label, entity))
            elif CONF_BUTTON in action_conf:
                entity = await cg.get_variable(action_conf[CONF_BUTTON])
                cg.add(var.add_action_button(key, label, entity))
            elif CONF_SELECT in action_conf:
                entity = await cg.get_variable(action_conf[CONF_SELECT])
                cg.add(var.add_action_select(key, label, entity))
            else:
                entity = await cg.get_variable(action_conf[CONF_NUMBER])
                cg.add(var.add_action_number(key, label, action_conf[CONF_DECIMALS], entity))

        for child in node_conf.get(CONF_CHILDREN, []):
            child_id = child[CONF_SEGMENT] if index == 0 else f"{dotted_id}.{child[CONF_SEGMENT]}"
            await emit(child, child_id, index)

    # The synthetic root (id and kind stay `car` for wire compatibility): `tree:` is its
    # children. It never passes through NODE_SCHEMA, so it is put through the same budget
    # derivation rather than handed a hand-written value.
    await emit(
        _apply_derived_budget(
            {
                CONF_KIND: "car",
                CONF_LABEL: config[CONF_ROOT_LABEL],
                CONF_SEGMENT: "car",
                CONF_CHILDREN: config[CONF_TREE],
            }
        ),
        "car",
        0xFFFF,
    )
