"""can_gateway: ISR-level CAN<->CAN gateway on the ESP32-C6 dual TWAI.

Validation rules are numbered V1-V26: each validator's docstring below carries
its number, and the component tests in tests/component_tests/can_gateway/
reference the same numbers (test_v01_..., cfg-v01 section markers).

v0.6 relaxes the two-port bridge into a component that is also a single-bus
node: 1 or 2 ports (V17), routes optional (V18), plus a receive path to the
application (observation ring + signal decode).
"""

from __future__ import annotations

import logging

from esphome import automation, final_validate as fv, pins
import esphome.codegen as cg
from esphome.components.esp32 import (
    add_idf_sdkconfig_option,
    get_esp32_variant,
    include_builtin_idf_component,
    only_on_variant,
)
from esphome.components.esp32.const import (
    VARIANT_ESP32,
    VARIANT_ESP32C3,
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTION,
    CONF_DATA,
    CONF_FILTERS,
    CONF_FROM,
    CONF_ID,
    CONF_INDEX,
    CONF_INTERVAL,
    CONF_LENGTH,
    CONF_OFFSET,
    CONF_PORT,
    CONF_RX_PIN,
    CONF_TO,
    CONF_TRIGGER_ID,
    CONF_TX_PIN,
    CONF_VALUE,
)
from esphome.core import CORE, ID

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@kipp-ing"]
DEPENDENCIES = ["esp32"]
# esp32_can drives the same TWAI controllers through the legacy driver/twai
# API; the two drivers cannot share a controller and fail only at boot, so
# reject the combination at config time.
CONFLICTS_WITH = ["esp32_can"]

# ---------------------------------------------------------------------------
# C++ symbol contract — the single reconciliation point between this module's
# code generation and the C++ written in Phases 2/3. Everything the generated
# code calls is listed here; change it in lockstep with the headers.
#
# namespace esphome::can_gateway (gateway_core.h / can_gateway.h / .cpp)
#
# CanGateway : public Component
#   CanGateway(uint8_t interrupt_priority)
#   void add_port(GatewayPort *port)                  // once or twice (V17)
#   void add_route(GatewayRoute *route)               // per route (optional)
#   void add_cyclic_send(CyclicSend *cyclic)          // per cyclic_sends entry
#   void set_stats_log_interval(uint32_t interval_ms) // statistics block
#   void set_id_timings_enabled(bool enabled)         // statistics.id_timings
#   void set_bus_off_binary_sensor(GatewayPort *, binary_sensor::BinarySensor *)
#   void set_last_frame_text_sensor(GatewayPort *, text_sensor::TextSensor *,
#                                   uint32_t throttle_ms)
#   void set_enabled(bool enabled); bool is_enabled() const
#
# GatewayPort
#   GatewayPort(uint8_t index, int8_t rx_pin, int8_t tx_pin, uint32_t bit_rate)
#   void set_listen_only(bool); void set_self_test(bool)
#   void set_open_drain_tx(bool); void set_tx_queue_depth(uint8_t)
#   template<typename F> void add_on_bus_off_callback(F &&)
#   template<typename F> void add_on_recovered_callback(F &&)
#   bool inject(uint32_t can_id, bool extended, bool rtr,
#               const uint8_t *data, uint8_t len)     // used by InjectAction
#
# GatewayRoute
#   GatewayRoute(GatewayPort *from, GatewayPort *to, uint8_t rule_count,
#                bool default_accept)
#   void add_rule(uint32_t match_id, uint32_t match_mask, uint8_t flags,
#                 uint32_t new_id, uint64_t and_mask, uint64_t or_value,
#                 RulePatch *patch)                   // patch may be nullptr
#   // flags: _rule_flags() emits the RULE_FLAG_* constants symbolically, so
#   // the bit values live only in gateway_core.h (bits 0-4, straight into
#   // the core RuleEntry) and can_gateway.h (bits 5-6: OUT_EXT, REPLACE_ID,
#   // consumed by add_rule when it builds the prepared PatchData).
#   // REPLACE_ID is set iff modify declares can_id or changes the frame
#   // type; only then is new_id meaningful. and_mask/or_value pack payload
#   // patch bytes little-endian by index: byte i lives at bits [8*i, 8*i+8).
#   // Patch application: data[i] = (data[i] & and[i]) | or[i].
#
# RulePatch                                            // A8 double bank
#   void set_byte(uint8_t index, uint8_t value)        // stages
#   void set_can_id(uint32_t can_id)                   // stages
#   void commit()                                      // atomic bank flip
#
# template<typename... Ts> SetPatchAction : Action<Ts...>, Parented<RulePatch>
#   void init_bytes(uint8_t count)                     // before the add_* calls
#   void add_static_byte(uint8_t index, uint8_t value)
#   void add_templated_byte(uint8_t index, uint8_t (*value)(Ts...))
#   void set_can_id(uint32_t) / set_can_id_template(uint32_t (*)(Ts...))
#   // play(): stage all entries on the parent RulePatch, then commit()
#
# template<typename... Ts> InjectAction : Action<Ts...>, Parented<GatewayPort>
#   void set_flags(bool extended, bool rtr)              // can_gateway.send
#   void set_can_id(uint32_t) / set_can_id_template(uint32_t (*)(Ts...))
#   void set_data_static(const uint8_t *data, uint8_t len)
#   void set_data_template(std::vector<uint8_t> (*func)(Ts...))
#
# CyclicSend                                           // timed send (Tier 1)
#   CyclicSend(GatewayPort *port, uint32_t can_id, bool extended, bool rtr,
#              uint32_t interval_ms, bool enabled)
#   void set_data(const uint8_t *data, uint8_t len)    // stages payload
#   void start(); void stop()
#
# template<typename... Ts> SetCyclicDataAction : Action<Ts...>,
#                                                Parented<CyclicSend>
#   void set_data_static(const uint8_t *data, uint8_t len)
#   void set_data_template(std::vector<uint8_t> (*func)(Ts...))
# template<typename... Ts> StartCyclicAction / StopCyclicAction
#     : Action<Ts...>, Parented<CyclicSend>           // play() -> start()/stop()
#
# CanGatewaySwitch : switch_::Switch, Component, Parented<CanGateway>
# CanGatewaySensorHub : PollingComponent
#   void set_route(GatewayRoute *) / void set_port(GatewayPort *)
#   void set_counter_sensor(uint8_t kind, sensor::Sensor *)
#   // kind = COUNTER_* index below, same order as ROUTE_COUNTERS+PORT_COUNTERS
#   //        +PORT_GAUGES+PORT_STATS (bus_load == index 11 == KIND_BUS_LOAD)
# ---------------------------------------------------------------------------

can_gateway_ns = cg.esphome_ns.namespace("can_gateway")
CanGateway = can_gateway_ns.class_("CanGateway", cg.Component)
GatewayPort = can_gateway_ns.class_("GatewayPort")
GatewayRoute = can_gateway_ns.class_("GatewayRoute")
CanGatewayFrameTrigger = can_gateway_ns.class_(
    "CanGatewayFrameTrigger", automation.Trigger.template()
)
RulePatch = can_gateway_ns.class_("RulePatch")
SetPatchAction = can_gateway_ns.class_("SetPatchAction", automation.Action)
InjectAction = can_gateway_ns.class_("InjectAction", automation.Action)
CyclicSend = can_gateway_ns.class_("CyclicSend")
SetCyclicDataAction = can_gateway_ns.class_("SetCyclicDataAction", automation.Action)
StartCyclicAction = can_gateway_ns.class_("StartCyclicAction", automation.Action)
StopCyclicAction = can_gateway_ns.class_("StopCyclicAction", automation.Action)

CONF_PORTS = "ports"
CONF_ROUTES = "routes"
CONF_INTERRUPT_PRIORITY = "interrupt_priority"
CONF_BIT_RATE = "bit_rate"
CONF_LISTEN_ONLY = "listen_only"
CONF_TX_QUEUE_DEPTH = "tx_queue_depth"
CONF_SELF_TEST = "self_test"
CONF_OPEN_DRAIN_TX = "open_drain_tx"
CONF_ON_BUS_OFF = "on_bus_off"
CONF_ON_RECOVERED = "on_recovered"
CONF_ON_FRAME = "on_frame"
CONF_DEFAULT_ACTION = "default_action"
CONF_CAN_ID = "can_id"
CONF_CAN_ID_MASK = "can_id_mask"
CONF_USE_EXTENDED_ID = "use_extended_id"
CONF_REMOTE_TRANSMISSION_REQUEST = "remote_transmission_request"
CONF_MODIFY = "modify"
CONF_MASK = "mask"
CONF_ROUTE_ID = "route_id"
CONF_PORT_ID = "port_id"
CONF_CAN_GATEWAY_ID = "can_gateway_id"
CONF_CYCLIC_SENDS = "cyclic_sends"
CONF_ENABLED = "enabled"
CONF_STATISTICS = "statistics"
CONF_LOG_INTERVAL = "log_interval"
CONF_ID_TIMINGS = "id_timings"
CONF_ID_TIMINGS_MAX = "id_timings_max"
CONF_OBSERVE_QUEUE_DEPTH = "observe_queue_depth"
CONF_LOG_TAP = "log_tap"
CONF_LOG_TAP_QUEUE_DEPTH = "log_tap_queue_depth"
CONF_MAX_FRAMES_PER_LOOP = "max_frames_per_loop"

# Fixed bound on the per-port subscribed-ID membership set (spec N7). Must match
# CAN_GATEWAY_MAX_OBSERVE_IDS in the C++ header default (defines.h).
OBSERVE_MAX_IDS = 128
OBSERVE_QUEUE_DEPTH_DEFAULT = 32
# Datalogger tap ring depth (sd_logger source S2). Sized for one SD
# write-latency spike: at 90% of 500 kbps a segment delivers ~3600 frames/s,
# so 1024 records absorbs ~280 ms of stall before the tap sheds.
LOG_TAP_DEPTH_DEFAULT = 1024

# Upper bound on `routes[].filters`. The rule count is carried to C++ as the
# `uint8_t rule_count` argument of GatewayRoute (can_gateway.h), so 256 rules
# would narrow to 0 and silently disable the whole rule table.
MAX_ROUTE_FILTERS = 255

ACTION_SET_PATCH = "can_gateway.set_patch"
ACTION_SEND = "can_gateway.send"
ACTION_INJECT = "can_gateway.inject"  # v0.4/v0.5 name; kept as a can_gateway.send alias
ACTION_SET_CYCLIC_DATA = "can_gateway.set_cyclic_data"
ACTION_START_CYCLIC = "can_gateway.start_cyclic"
ACTION_STOP_CYCLIC = "can_gateway.stop_cyclic"

STANDARD_ID_MAX = 0x7FF
EXTENDED_ID_MAX = 0x1FFFFFFF

ACTION_ACCEPT = "accept"
ACTION_DROP = "drop"

# V9: bit rate as integer Hz or a '125kbps' / '1Mbps' style string, in the
# classic-CAN range. cv.bps handles the unit grammar shared across ESPHome.
validate_bit_rate = cv.All(cv.bps, cv.int_, cv.int_range(min=10_000, max=1_000_000))


def _id_bound(extended: bool) -> int:
    return EXTENDED_ID_MAX if extended else STANDARD_ID_MAX


def _check_id_fits(can_id: int, extended: bool, what: str) -> None:
    bound = _id_bound(extended)
    if can_id > bound:
        id_kind = "extended" if extended else "standard"
        raise cv.Invalid(
            f"{what} 0x{can_id:X} exceeds the {id_kind}-ID maximum 0x{bound:X}"
        )


def _validate_rule(rule):
    """V2-V6 on a single filter rule."""
    extended = rule[CONF_USE_EXTENDED_ID]
    can_id = rule[CONF_CAN_ID]
    _check_id_fits(can_id, extended, "can_id")

    # V3: mask defaults to the full bound of the rule's ID type.
    if CONF_CAN_ID_MASK not in rule:
        rule[CONF_CAN_ID_MASK] = _id_bound(extended)
    mask = rule[CONF_CAN_ID_MASK]
    if mask > _id_bound(extended):
        raise cv.Invalid(
            f"can_id_mask 0x{mask:X} exceeds 0x{_id_bound(extended):X} "
            f"for this rule's ID type"
        )

    # V4: a match ID with bits outside the mask can never match.
    if can_id & ~mask:
        raise cv.Invalid(
            f"can_id 0x{can_id:X} has bits outside can_id_mask 0x{mask:X}; "
            f"this rule could never match"
        )

    modify = rule.get(CONF_MODIFY)
    if modify is None:
        # V15: a rule id exists only to make its modify values updatable.
        if CONF_ID in rule:
            raise cv.Invalid(
                "a filter rule id makes its modify values updatable at runtime; "
                "without a modify block there is nothing can_gateway.set_patch "
                "could target — add a modify block or remove the id"
            )
        return rule

    # V5: modifying a dropped frame is contradictory.
    if rule[CONF_ACTION] == ACTION_DROP:
        raise cv.Invalid("modify cannot be combined with action: drop")

    out_extended = modify.get(CONF_USE_EXTENDED_ID, extended)
    if (new_id := modify.get(CONF_CAN_ID)) is not None:
        _check_id_fits(new_id, out_extended, "modify.can_id")
    elif extended and not out_extended:
        # Demoting extended -> standard keeps the matched (29-bit) ID; that
        # cannot fit an 11-bit frame, so an explicit new ID is required.
        raise cv.Invalid(
            "changing use_extended_id to false requires an explicit modify.can_id"
        )
    elif out_extended != extended and mask != _id_bound(extended):
        # A masked rule matches a whole ID range; changing the frame type
        # without a new ID would collapse that range onto the rule's can_id.
        raise cv.Invalid(
            "changing the frame type on a masked rule requires an explicit "
            "modify.can_id"
        )
    return rule


def _validate_patch_bytes(patches):
    """V6: at most one patch per byte index."""
    seen: set[int] = set()
    for patch in patches:
        index = patch[CONF_INDEX]
        if index in seen:
            raise cv.Invalid(f"byte index {index} may only be patched once")
        seen.add(index)
    return patches


def _validate_port(port):
    """V12: bench aids that contradict each other. V23: drain cap vs ring depth."""
    if port[CONF_SELF_TEST] and port[CONF_LISTEN_ONLY]:
        raise cv.Invalid("self_test and listen_only are mutually exclusive")
    # V23: max_frames_per_loop defaults to the ring depth and may not exceed it
    # (you cannot drain more records per loop than the ring can hold).
    depth = port[CONF_OBSERVE_QUEUE_DEPTH]
    if CONF_MAX_FRAMES_PER_LOOP not in port:
        port[CONF_MAX_FRAMES_PER_LOOP] = depth
    elif port[CONF_MAX_FRAMES_PER_LOOP] > depth:
        raise cv.Invalid(
            f"max_frames_per_loop ({port[CONF_MAX_FRAMES_PER_LOOP]}) cannot exceed "
            f"observe_queue_depth ({depth})",
            path=[CONF_MAX_FRAMES_PER_LOOP],
        )
    return port


# A classic-CAN payload literal: 0-8 bytes, each 0x00-0xFF.
_validate_frame_data = cv.All(cv.ensure_list(cv.hex_uint8_t), cv.Length(max=8))


def _validate_tx_frame(config):
    """A transmitted frame's static constraints, shared by cyclic sends (V16)
    and can_gateway.send (V11). Cross-references live elsewhere."""
    # can_id is templatable on send; only a literal can be bounds-checked here.
    can_id = config[CONF_CAN_ID]
    if not cg.is_template(can_id):
        _check_id_fits(can_id, config[CONF_USE_EXTENDED_ID], "can_id")
    data = config[CONF_DATA]
    # Templatable data (send) may be a lambda; only literal lists can be
    # checked against the RTR rule here.
    if config[CONF_REMOTE_TRANSMISSION_REQUEST] and isinstance(data, list) and data:
        raise cv.Invalid("RTR frames carry no data")
    return config


# On-chip TWAI controllers per ESP32 variant, from the IDF `soc_caps.h`
# (`SOC_TWAI_CONTROLLER_NUM`). A variant absent from this table has no TWAI at
# all — `only_on_variant` rejects it with a clearer message than a link error.
#
# Note the P4 really has three; the component's own data plane is built for at
# most two ports (`route_for[2]` in CanGateway::setup), so MAX_PORTS caps every
# entry. Raising that cap is a component change, not a table edit.
MAX_PORTS = 2
TWAI_CONTROLLERS = {
    VARIANT_ESP32: 1,
    VARIANT_ESP32S2: 1,
    VARIANT_ESP32S3: 1,
    VARIANT_ESP32C3: 1,
    VARIANT_ESP32H2: 1,
    VARIANT_ESP32C5: 2,
    VARIANT_ESP32C6: 2,
    VARIANT_ESP32P4: 3,
}

# Variants whose TWAI HAL the data plane is not verified against. The C5 is the
# only SOC_TWAI_SUPPORT_FD target in IDF 5.5.4, so it builds twai_hal_v2.c, where
# a frame halted by bus-off still completes — the opposite of the contract
# OutstandingTracker's eager reclaim is written against. can_gateway.cpp rejects
# it with an #error; this table makes the rejection land at config time instead,
# where the message is readable. See docs/HANDOVER.md Todo 4.
UNVERIFIED_HAL_VARIANTS = {
    VARIANT_ESP32C5: (
        "builds the TWAI-FD HAL (twai_hal_v2.c), whose bus-off completion "
        "behavior the forwarding slot tracker is not verified against; "
        "running it risks transmitting a stale frame after a bus-off recovery"
    ),
}


def _ports_for_variant(variant):
    return min(TWAI_CONTROLLERS.get(variant, 0), MAX_PORTS)


def _validate_hal_variant(config):
    """V27: reject a variant whose TWAI HAL the data plane is unverified against.

    can_gateway.cpp carries an #error for the same condition, so this only moves
    the failure earlier — but a config-time message naming the reason beats a
    preprocessor error buried in a build log. Drop the entry from
    UNVERIFIED_HAL_VARIANTS (and the #error) once the tracker is taught the v2
    event model and it is confirmed on silicon.
    """
    variant = get_esp32_variant()
    reason = UNVERIFIED_HAL_VARIANTS.get(variant)
    if reason is not None:
        raise cv.Invalid(
            f"can_gateway: the {variant} is not supported — it {reason}. "
            "See docs/HANDOVER.md Todo 4."
        )
    return config


def _validate_port_count_for_variant(config):
    """V17: a port needs a hardware controller to live on.

    Runs after `only_on_variant`, so the variant is known to be in the table.
    Without this, a two-port config on a single-TWAI chip validates cleanly and
    then fails at runtime inside `twai_new_node_onchip` with
    `ESP_ERR_NOT_FOUND` — a boot-time message on a bench board nobody is
    watching. Catch it where the user can read it.
    """
    variant = get_esp32_variant()
    available = _ports_for_variant(variant)
    ports = config[CONF_PORTS]
    if len(ports) > available:
        raise cv.Invalid(
            f"can_gateway: {len(ports)} ports configured but the {variant} has "
            f"{TWAI_CONTROLLERS[variant]} on-chip TWAI controller"
            f"{'s' if TWAI_CONTROLLERS[variant] != 1 else ''}"
            + (
                f" (this component uses at most {MAX_PORTS})"
                if TWAI_CONTROLLERS[variant] > MAX_PORTS
                else ""
            )
            + ". A single-port can_gateway is a monitor/node: it reads and sends "
            "on one bus, but bridges nothing.",
            path=[CONF_PORTS],
        )
    return config


def _validate_gateway(config):
    """V1/V17 (port count), V7/V18 (routes): cross-references between ports
    and routes."""
    ports = config[CONF_PORTS]
    # V17: the component's data plane handles at most MAX_PORTS. The tighter,
    # variant-aware bound is applied by _validate_port_count_for_variant once
    # only_on_variant has confirmed the chip has TWAI at all.
    # Wording is the released one (MAX_PORTS == 2) — it is asserted by
    # tests/can_gateway/test_single_bus.py, so keep them in step if the cap moves.
    if not 1 <= len(ports) <= MAX_PORTS:
        raise cv.Invalid(f"can_gateway supports 1 or 2 ports (got {len(ports)})")
    routes = config.get(CONF_ROUTES, [])
    # V18: forwarding needs a second controller to forward to. A single-port
    # block is a monitor/node — it reads and sends, but bridges nothing.
    if len(ports) < 2 and routes:
        raise cv.Invalid(
            "routes require two ports; a single-port can_gateway is a "
            "monitor/node with no forwarding — remove routes or add a second port",
            path=[CONF_ROUTES],
        )
    port_names = [str(port[CONF_ID]) for port in ports]
    listen_only_ports = {str(port[CONF_ID]) for port in ports if port[CONF_LISTEN_ONLY]}
    seen_sources: set[str] = set()
    for index, route in enumerate(routes):
        source = str(route[CONF_FROM])
        target = str(route[CONF_TO])
        if source in seen_sources:
            # The fast path receives each frame exactly once, straight into
            # the single outbound route's TX slot (N4); a second route from
            # the same port would need a second copy.
            raise cv.Invalid(
                f"port '{source}' already has a route; at most one route per direction",
                path=[CONF_ROUTES, index],
            )
        seen_sources.add(source)
        for name in (source, target):
            if name not in port_names:
                raise cv.Invalid(
                    f"'{name}' is not a declared port of this gateway",
                    path=[CONF_ROUTES, index],
                )
        if source == target:
            raise cv.Invalid(
                "route 'from' and 'to' must differ", path=[CONF_ROUTES, index]
            )
        if target in listen_only_ports:
            raise cv.Invalid(
                f"route cannot target listen-only port '{target}'",
                path=[CONF_ROUTES, index],
            )
    # Cyclic sends transmit on a port, so the same listen-only rule applies.
    for index, cyclic in enumerate(config.get(CONF_CYCLIC_SENDS, [])):
        target = str(cyclic[CONF_PORT])
        if target not in port_names:
            raise cv.Invalid(
                f"'{target}' is not a declared port of this gateway",
                path=[CONF_CYCLIC_SENDS, index],
            )
        if target in listen_only_ports:
            raise cv.Invalid(
                f"cyclic_send cannot target listen-only port '{target}'",
                path=[CONF_CYCLIC_SENDS, index],
            )
    return config


PATCH_BYTE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_INDEX): cv.int_range(min=0, max=7),
        cv.Required(CONF_VALUE): cv.int_range(min=0, max=255),
        cv.Optional(CONF_MASK, default=0xFF): cv.int_range(min=0, max=255),
    }
)

MODIFY_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_CAN_ID): cv.int_range(min=0, max=EXTENDED_ID_MAX),
            cv.Optional(CONF_USE_EXTENDED_ID): cv.boolean,
            cv.Optional(CONF_DATA): cv.All(
                cv.ensure_list(PATCH_BYTE_SCHEMA),
                cv.Length(min=1),
                _validate_patch_bytes,
            ),
        }
    ),
    cv.has_at_least_one_key(CONF_CAN_ID, CONF_USE_EXTENDED_ID, CONF_DATA),
)

FILTER_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_ID): cv.declare_id(RulePatch),
            cv.Required(CONF_CAN_ID): cv.int_range(min=0, max=EXTENDED_ID_MAX),
            cv.Optional(CONF_CAN_ID_MASK): cv.int_range(min=0, max=EXTENDED_ID_MAX),
            cv.Optional(CONF_USE_EXTENDED_ID, default=False): cv.boolean,
            cv.Optional(CONF_REMOTE_TRANSMISSION_REQUEST): cv.boolean,
            cv.Optional(CONF_ACTION, default=ACTION_ACCEPT): cv.one_of(
                ACTION_ACCEPT, ACTION_DROP, lower=True
            ),
            cv.Optional(CONF_MODIFY): MODIFY_SCHEMA,
        }
    ),
    _validate_rule,
)

PORT_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.declare_id(GatewayPort),
            cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
            cv.Required(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_BIT_RATE): validate_bit_rate,
            cv.Optional(CONF_LISTEN_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_TX_QUEUE_DEPTH, default=8): cv.int_range(min=1, max=64),
            # v0.6 observation ring depth (A13) and per-loop drain cap (B21).
            cv.Optional(
                CONF_OBSERVE_QUEUE_DEPTH, default=OBSERVE_QUEUE_DEPTH_DEFAULT
            ): cv.int_range(min=4, max=128),
            # V28: arm the datalogger tap on this port. Off by default — a tap
            # allocates a ring and adds an ISR push per frame, so it is opt-in.
            cv.Optional(CONF_LOG_TAP, default=False): cv.boolean,
            cv.Optional(CONF_MAX_FRAMES_PER_LOOP): cv.int_range(min=1, max=128),
            cv.Optional(CONF_SELF_TEST, default=False): cv.boolean,
            cv.Optional(CONF_OPEN_DRAIN_TX, default=False): cv.boolean,
            cv.Optional(CONF_ON_BUS_OFF): automation.validate_automation({}),
            cv.Optional(CONF_ON_RECOVERED): automation.validate_automation({}),
            # Raw sniffer (A14). Unlike every other subscriber this names no id,
            # so it forces observe_all on the port: hardware filter offload off,
            # every frame through the observation ring. Diagnostics, not a
            # default — see CanGatewayFrameTrigger.
            cv.Optional(CONF_ON_FRAME): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CanGatewayFrameTrigger)}
            ),
        }
    ),
    _validate_port,
)

ROUTE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(GatewayRoute),
        cv.Required(CONF_FROM): cv.use_id(GatewayPort),
        cv.Required(CONF_TO): cv.use_id(GatewayPort),
        # max=MAX_ROUTE_FILTERS: len(filters) is passed to the C++
        # GatewayRoute(…, uint8_t rule_count, …) and sizes a FixedVector whose
        # push_back silently no-ops when full. 256 filters would narrow to 0,
        # dropping every rule and degrading the route to its bare
        # default_action — an allow-list route with `default_action: accept`
        # would then forward everything onto the bus. Reject at config time.
        cv.Optional(CONF_FILTERS): cv.All(
            cv.ensure_list(FILTER_SCHEMA),
            cv.Length(min=1, max=MAX_ROUTE_FILTERS),
        ),
        cv.Optional(CONF_DEFAULT_ACTION, default=ACTION_ACCEPT): cv.one_of(
            ACTION_ACCEPT, ACTION_DROP, lower=True
        ),
    }
)

CYCLIC_SEND_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.declare_id(CyclicSend),
            cv.Required(CONF_PORT): cv.use_id(GatewayPort),
            cv.Required(CONF_CAN_ID): cv.int_range(min=0, max=EXTENDED_ID_MAX),
            cv.Required(CONF_INTERVAL): cv.All(
                cv.positive_not_null_time_period, cv.positive_time_period_milliseconds
            ),
            cv.Optional(CONF_USE_EXTENDED_ID, default=False): cv.boolean,
            cv.Optional(CONF_REMOTE_TRANSMISSION_REQUEST, default=False): cv.boolean,
            cv.Optional(CONF_DATA, default=[]): _validate_frame_data,
            cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        }
    ),
    _validate_tx_frame,
)

STATISTICS_SCHEMA = cv.Schema(
    {
        # 0 disables the periodic log line (dump_config still summarizes).
        cv.Optional(
            CONF_LOG_INTERVAL, default="60s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ID_TIMINGS, default=False): cv.boolean,
        cv.Optional(CONF_ID_TIMINGS_MAX, default=32): cv.int_range(min=1, max=128),
    }
)

# ---------------------------------------------------------------------------
# Signal decode (v0.6, S9) — shared schema/validation for the entity platforms
# ---------------------------------------------------------------------------

CONF_BIT_OFFSET = "bit_offset"
CONF_BIT_LENGTH = "bit_length"
CONF_BYTE_ORDER = "byte_order"
CONF_SIGNED = "signed"
CONF_BIT = "bit"
CONF_MAP = "map"
# `sna:` names the raw value a DBC reserves for "signal not available". The key
# is the DBC's own word for it, and it takes a value like every other decode key
# (byte_order, signed) rather than being a flag plus a second value key.
CONF_SNA = "sna"

BYTE_ORDER_LITTLE = "little"
BYTE_ORDER_BIG = "big"
# The dominant SNA convention: all ones for the signal's own bit width. Measured
# on a real 165-message HV-battery DBC — 377 of 377 SNA entries in its VAL_
# tables are all-ones, none is anything else. The integer form is the escape
# hatch for the rest.
SNA_ALL_ONES = "all_ones"

SnaMode = can_gateway_ns.enum("SnaMode", is_class=True)


def decode_source_schema() -> dict:
    """The keys naming where a signal lives: source port, message ID, type."""
    return {
        cv.Required(CONF_PORT_ID): cv.use_id(GatewayPort),
        cv.Required(CONF_CAN_ID): cv.int_range(min=0, max=EXTENDED_ID_MAX),
        cv.Optional(CONF_USE_EXTENDED_ID, default=False): cv.boolean,
    }


def validate_decode_id(config):
    """V19: a decode entry's can_id fits the frame type it selects."""
    _check_id_fits(config[CONF_CAN_ID], config[CONF_USE_EXTENDED_ID], "can_id")
    return config


def validate_signal_position(config):
    """V20: the signal lies inside the frame; the byte-aligned and bit-level
    forms are mutually exclusive; both are bounded to 32 bits."""
    # Each form needs both of its keys.
    if (CONF_OFFSET in config) != (CONF_LENGTH in config):
        raise cv.Invalid("the byte-aligned form needs both offset and length")
    if (CONF_BIT_OFFSET in config) != (CONF_BIT_LENGTH in config):
        raise cv.Invalid("the bit-level form needs both bit_offset and bit_length")
    has_byte = CONF_OFFSET in config
    has_bit = CONF_BIT_OFFSET in config
    if has_byte and has_bit:
        raise cv.Invalid(
            "use either offset/length (byte-aligned) or bit_offset/bit_length "
            "(bit-level), not both"
        )
    if not has_byte and not has_bit:
        raise cv.Invalid(
            "a decode signal needs a position: offset + length (byte-aligned) "
            "or bit_offset + bit_length (bit-level)"
        )
    if has_byte:
        offset = config[CONF_OFFSET]
        length = config[CONF_LENGTH]
        if offset + length > 8:
            raise cv.Invalid(
                f"offset ({offset}) + length ({length}) runs past the 8-byte frame"
            )
        if length > 4:
            raise cv.Invalid(
                "byte-aligned signals wider than 32 bits (length > 4) are not "
                "supported; >32-bit decode is deferred — use a lambda filter"
            )
    else:
        bit_offset = config[CONF_BIT_OFFSET]
        bit_length = config[CONF_BIT_LENGTH]
        if bit_offset + bit_length > 64:
            raise cv.Invalid(
                f"bit_offset ({bit_offset}) + bit_length ({bit_length}) runs past "
                f"the 64-bit frame"
            )
        # Bit-level extraction uses little-endian (Intel) bit numbering. The
        # Motorola backward-bit-numbering case is deferred (spec), so a
        # big-endian bit-level field would decode wrong — reject it.
        if config.get(CONF_BYTE_ORDER, BYTE_ORDER_LITTLE) == BYTE_ORDER_BIG:
            raise cv.Invalid(
                "big-endian byte order is not supported for a bit-level field "
                "(Motorola backward-bit-numbering is deferred); use the "
                "byte-aligned form, or little-endian"
            )
    return config


def validate_sna_value(value):
    """`sna:` is either the literal `all_ones` or an explicit raw value."""
    if isinstance(value, bool):
        # The key reads flag-like, and `sna: true` would otherwise slip through
        # as the raw value 1 (bool is an int in Python) — a signal quietly
        # publishing unknown whenever it reads 1.
        raise cv.Invalid(
            f"sna takes a value, not a flag: use '{SNA_ALL_ONES}' for the "
            f"all-ones convention, or the raw value the DBC reserves"
        )
    if isinstance(value, str):
        if value.strip().lower() == SNA_ALL_ONES:
            return SNA_ALL_ONES
        raise cv.Invalid(
            f"sna must be '{SNA_ALL_ONES}' or a non-negative raw value, "
            f"got {value!r}"
        )
    return cv.int_range(min=0, max=0xFFFFFFFF)(value)


def signal_width_bits(config) -> int:
    """The signal's own width: bit_length, or 8 bits per byte of length.

    Only valid after validate_signal_position() has settled which form is in
    use; it is the width sign_extend() and the SNA marker are both taken at.
    """
    if CONF_BIT_OFFSET in config:
        return config[CONF_BIT_LENGTH]
    return config[CONF_LENGTH] * 8


def validate_sna(config):
    """V29: an explicit `sna:` raw value must fit the signal it marks.

    A wider value could never come out of the extraction, so it would simply
    never match — a silent no-op is worse than a config error, because the
    sensor would go on publishing the bogus reading the key was added to stop.
    """
    sna = config.get(CONF_SNA)
    if sna is None or sna == SNA_ALL_ONES:
        return config
    width = signal_width_bits(config)
    limit = (1 << width) - 1
    if sna > limit:
        raise cv.Invalid(
            f"sna ({sna}) does not fit this {width}-bit signal (max {limit}); "
            f"sna is compared against the raw extracted value, before sign "
            f"extension, so give the unsigned raw bit pattern"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CanGateway),
            cv.Required(CONF_PORTS): cv.ensure_list(PORT_SCHEMA),
            # V18: routes are optional (a single-port block is a monitor/node
            # with no forwarding). Two ports remain required to declare any.
            cv.Optional(CONF_ROUTES): cv.All(
                cv.ensure_list(ROUTE_SCHEMA), cv.Length(min=1)
            ),
            cv.Optional(CONF_INTERRUPT_PRIORITY, default=2): cv.int_range(min=1, max=3),
            # The cap keeps the per-port inject slot pool well inside its
            # uint8_t sizing (see CAN_GATEWAY_INJECT_SLOTS in to_code).
            cv.Optional(CONF_CYCLIC_SENDS): cv.All(
                cv.ensure_list(CYCLIC_SEND_SCHEMA), cv.Length(max=32)
            ),
            cv.Optional(
                CONF_LOG_TAP_QUEUE_DEPTH, default=LOG_TAP_DEPTH_DEFAULT
            ): cv.int_range(min=64, max=8192),
            cv.Optional(CONF_STATISTICS): STATISTICS_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_gateway,
    # Every TWAI-capable variant, not just the C6: the component is written
    # against the generic esp_driver_twai node API, and single-controller chips
    # (ESP32 classic, S2, S3, C3, H2) are perfectly good single-bus monitors.
    # Forwarding still needs two controllers — enforced just below.
    only_on_variant(supported=list(TWAI_CONTROLLERS), msg_prefix="can_gateway"),
    _validate_hal_variant,
    _validate_port_count_for_variant,
    # The node-based esp_driver_twai API this component is built on first
    # shipped in ESP-IDF 5.5.
    cv.require_framework_version(esp_idf=cv.Version(5, 5, 0)),
)


# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------


SET_PATCH_ACTION_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(RulePatch),
            cv.Optional(CONF_CAN_ID): cv.templatable(
                cv.int_range(min=0, max=EXTENDED_ID_MAX)
            ),
            cv.Optional(CONF_DATA): cv.All(
                cv.ensure_list(
                    cv.Schema(
                        {
                            cv.Required(CONF_INDEX): cv.int_range(min=0, max=7),
                            cv.Required(CONF_VALUE): cv.templatable(
                                cv.int_range(min=0, max=255)
                            ),
                        }
                    )
                ),
                cv.Length(min=1),
            ),
        }
    ),
    cv.has_at_least_one_key(CONF_CAN_ID, CONF_DATA),
)


def _validate_send_data(value):
    """Payload for can_gateway.send: a 0-8 byte list, or an ASCII string (like
    canbus.send). Lambdas pass through cv.templatable before reaching here."""
    if isinstance(value, str):
        data = list(value.encode("utf-8"))
        if len(data) > 8:
            raise cv.Invalid("a string payload must be at most 8 bytes")
        return data
    return _validate_frame_data(value)


def _send_shorthand(value):
    """Shorthand `can_gateway.send: [0x01, 0x02]` / "abc" -> {data: ...}. A
    can_id is still required (a gateway port has no default ID), so the
    shorthand is only useful together with a dict — but a bare list/string maps
    to the data key for symmetry with canbus.send."""
    if isinstance(value, (list, str)):
        return {CONF_DATA: value}
    return value


# can_gateway.send (v0.6). port omittable when exactly one port is declared
# (resolved / enforced in final validation, V22); can_id templatable.
SEND_ACTION_SCHEMA = cv.All(
    _send_shorthand,
    cv.Schema(
        {
            cv.Optional(CONF_PORT): cv.use_id(GatewayPort),
            cv.Required(CONF_CAN_ID): cv.templatable(
                cv.int_range(min=0, max=EXTENDED_ID_MAX)
            ),
            cv.Optional(CONF_USE_EXTENDED_ID, default=False): cv.boolean,
            cv.Optional(CONF_REMOTE_TRANSMISSION_REQUEST, default=False): cv.boolean,
            cv.Optional(CONF_DATA, default=[]): cv.templatable(_validate_send_data),
        }
    ),
    _validate_tx_frame,
)


# Deduped through CORE.data (cleared between runs): one warning per
# `esphome config|compile|run` is enough, however many call sites there are.
_INJECT_DEPRECATED_KEY = "_can_gateway_inject_deprecated_warned"


def _warn_inject_deprecated(value):
    """V26: warn (never reject) on the pre-v0.6 action name.

    `can_gateway.inject` was renamed to `can_gateway.send` in v0.6. The alias
    keeps working for one release cycle so existing YAML validates unchanged;
    this shim is the migration signal users need before it is removed.
    """
    if not CORE.data.get(_INJECT_DEPRECATED_KEY):
        _LOGGER.warning(
            "'%s' is deprecated and will be removed in a future release. "
            "Rename it to '%s'; the options are identical.",
            ACTION_INJECT,
            ACTION_SEND,
        )
        CORE.data[_INJECT_DEPRECATED_KEY] = True
    return value


# Backwards-compatible alias name for the reshaped action: same schema as
# can_gateway.send, plus the deprecation warning.
INJECT_ACTION_SCHEMA = cv.All(_warn_inject_deprecated, SEND_ACTION_SCHEMA)


# ---------------------------------------------------------------------------
# Final validation (V10, V11 cross-references, single enable switch)
# ---------------------------------------------------------------------------


def _collect_actions(node, found: list) -> None:
    """Recursively find gateway actions anywhere in the validated config."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key in (ACTION_SET_PATCH, ACTION_SEND, ACTION_INJECT):
                found.append((key, value))
            _collect_actions(value, found)
    elif isinstance(node, list):
        for item in node:
            _collect_actions(item, found)


def _declared_patch_shapes(config) -> dict[str, tuple[set[int], bool, bool]]:
    """Map rule-id name -> (declared byte indices, declares can_id, output
    frame type is extended)."""
    shapes: dict[str, tuple[set[int], bool, bool]] = {}
    for route in config.get(CONF_ROUTES, []):
        for rule in route.get(CONF_FILTERS, []):
            if (rule_id := rule.get(CONF_ID)) is None:
                continue
            modify = rule.get(CONF_MODIFY, {})
            indices = {patch[CONF_INDEX] for patch in modify.get(CONF_DATA, [])}
            out_extended = modify.get(CONF_USE_EXTENDED_ID, rule[CONF_USE_EXTENDED_ID])
            shapes[str(rule_id)] = (indices, CONF_CAN_ID in modify, out_extended)
    return shapes


def _final_validate(config):
    full_config = fv.full_config.get()
    routes = config.get(CONF_ROUTES, [])

    # At most one enable switch (V8).
    gateway_switches = [
        entry
        for entry in full_config.get("switch", [])
        if entry.get("platform") == "can_gateway"
    ]
    if len(gateway_switches) > 1:
        raise cv.Invalid("only one can_gateway switch is allowed")
    # V21: the enable switch gates forwarding only. With no route there is
    # nothing to gate, so the switch would be a dead entity — reject it and
    # name the reason (B27).
    if gateway_switches and not routes:
        raise cv.Invalid(
            "a can_gateway switch gates forwarding on/off, but this gateway "
            "has no routes to gate; remove the switch (a single-bus node has "
            "no forwarding to disable)"
        )

    shapes = _declared_patch_shapes(config)
    listen_only_ports = {
        str(port[CONF_ID]) for port in config[CONF_PORTS] if port[CONF_LISTEN_ONLY]
    }

    actions: list = []
    _collect_actions(dict(full_config), actions)
    for action_name, action in actions:
        if action_name == ACTION_SET_PATCH:
            rule_name = str(action[CONF_ID])
            if rule_name not in shapes:
                raise cv.Invalid(
                    f"'{rule_name}' is not an updatable filter rule "
                    f"(give the rule an id and a modify block)"
                )
            indices, has_can_id, out_extended = shapes[rule_name]
            if (action_can_id := action.get(CONF_CAN_ID)) is not None:
                if not has_can_id:
                    raise cv.Invalid(
                        f"rule '{rule_name}' does not declare a can_id modification"
                    )
                # The rule's output frame type is fixed; a static value must
                # fit it (templated values are masked to it at runtime).
                if not cg.is_template(action_can_id):
                    _check_id_fits(action_can_id, out_extended, "can_id")
            for patch in action.get(CONF_DATA, []):
                if patch[CONF_INDEX] not in indices:
                    raise cv.Invalid(
                        f"rule '{rule_name}' does not declare byte index "
                        f"{patch[CONF_INDEX]} in its modify.data"
                    )
        elif action_name in (ACTION_SEND, ACTION_INJECT):
            # V22: `port` may be omitted only when exactly one port is declared;
            # resolve it to that port so codegen always has one.
            if CONF_PORT not in action:
                if len(config[CONF_PORTS]) != 1:
                    raise cv.Invalid(
                        f"'{action_name}' needs a 'port': more than one port is "
                        f"declared, so it cannot be inferred"
                    )
                action[CONF_PORT] = config[CONF_PORTS][0][CONF_ID]
            if str(action[CONF_PORT]) in listen_only_ports:
                raise cv.Invalid(
                    f"cannot send on listen-only port '{action[CONF_PORT]}'"
                )

    # V24: bus_load measures the traffic the gateway receives and transmits on
    # a port. A route source receives frames to forward; a single-bus / monitor
    # port (no route) receives them for observation — both are meaningful. Only
    # a pure route *destination* never receives through the gateway, so its
    # gauge would show the own-TX share alone; reject that one case.
    source_ports = {str(route[CONF_FROM]) for route in routes}
    dest_only_ports = {str(route[CONF_TO]) for route in routes} - source_ports
    for entry in full_config.get("sensor", []):
        if entry.get("platform") != "can_gateway":
            continue
        if (
            "bus_load" in entry
            and (port_ref := entry.get(CONF_PORT_ID)) is not None
            and str(port_ref) in dest_only_ports
        ):
            raise cv.Invalid(
                f"bus_load on port '{port_ref}' would only measure the "
                f"gateway's own transmissions; the port is a route destination "
                f"only, so received frames never reach the gateway"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------


def inject_slot_count(config) -> int:
    """Size the per-port inject slot pool (`CAN_GATEWAY_INJECT_SLOTS`).

    A slot is held from `GatewayPort::inject()` until the TX-done ISR releases
    it, so the pool is the real ceiling on how many injects can be in flight —
    and `inject()` itself already refuses once `outstanding_` reaches
    `tx_queue_depth`, so more than `tx_queue_depth + 1` slots can ever be used.

    Sizing it to the TX queue is therefore both necessary and sufficient. It
    matters for burst producers: a single main-loop pass that injects a whole
    queue's worth of frames — how a 500 kbit/s bus is driven to 90 % load, since
    `cyclic_sends` are floored by the ~16 ms loop period (issue #2) — was
    silently capped at four frames before, whatever `tx_queue_depth` said.
    """
    max_tx_queue_depth = max(port[CONF_TX_QUEUE_DEPTH] for port in config[CONF_PORTS])
    # Cyclic sends hold a slot each on their own port, so the busiest port's
    # cyclic count is a second floor; +2 leaves room for a manual inject
    # alongside them.
    cyclic_per_port: dict[str, int] = {}
    for cyclic_config in config.get(CONF_CYCLIC_SENDS, []):
        key = str(cyclic_config[CONF_PORT])
        cyclic_per_port[key] = cyclic_per_port.get(key, 0) + 1
    max_cyclic_per_port = max(cyclic_per_port.values(), default=0)
    return max(4, max_cyclic_per_port + 2, max_tx_queue_depth + 2)


def _rule_flags(rule) -> cg.RawExpression:
    """OR of the C++ RULE_FLAG_* constants this rule needs, emitted
    symbolically so the bit values live only in the C++ headers."""
    flags: list[str] = []
    extended = rule[CONF_USE_EXTENDED_ID]
    if extended:
        flags.append("RULE_FLAG_EXTENDED")
    if (rtr := rule.get(CONF_REMOTE_TRANSMISSION_REQUEST)) is not None:
        flags.append("RULE_FLAG_CHECK_RTR")
        if rtr:
            flags.append("RULE_FLAG_RTR_VALUE")
    if rule[CONF_ACTION] == ACTION_DROP:
        flags.append("RULE_FLAG_DROP")
    if (modify := rule.get(CONF_MODIFY)) is not None:
        flags.append("RULE_FLAG_HAS_PATCH")
        # OUT_EXT and REPLACE_ID only matter alongside HAS_PATCH: add_rule
        # consumes them when it builds the prepared PatchData.
        out_extended = modify.get(CONF_USE_EXTENDED_ID, extended)
        if out_extended:
            flags.append("RULE_FLAG_OUT_EXT")
        if CONF_CAN_ID in modify or out_extended != extended:
            flags.append("RULE_FLAG_REPLACE_ID")
    if not flags:
        return cg.RawExpression("0")
    return cg.RawExpression(
        " | ".join(str(getattr(can_gateway_ns, name)) for name in flags)
    )


def _packed_patch_masks(modify) -> tuple[int, int]:
    """Pack modify.data into (and_mask, or_value) uint64s, byte i at bits 8*i."""
    and_mask = 0
    or_value = 0
    for index in range(8):
        and_mask |= 0xFF << (8 * index)
    if modify is None:
        return and_mask, or_value
    for patch in modify.get(CONF_DATA, []):
        index = patch[CONF_INDEX]
        mask = patch[CONF_MASK]
        shift = 8 * index
        and_mask &= ~(mask << shift) & 0xFFFF_FFFF_FFFF_FFFF
        or_value |= (patch[CONF_VALUE] & mask) << shift
    return and_mask, or_value


async def to_code(config):
    # The modern node-based TWAI driver lives in its own IDF component.
    include_builtin_idf_component("esp_driver_twai")
    # Keep the data plane servicing while the flash cache is disabled
    # (OTA writes, NVS commits).
    add_idf_sdkconfig_option("CONFIG_TWAI_ISR_CACHE_SAFE", True)
    # ISR_CACHE_SAFE covers the driver ISR and twai_node_receive_from_isr,
    # but NOT twai_node_transmit/_node_queue_tx (driver linker.lf gates those
    # behind TWAI_IO_FUNC_IN_IRAM). The RX ISR transmits the forwarded frame,
    # so without this the first frame arriving during a cache-off window
    # panics with a cache error (observed on hardware: first boot after
    # flashing writes NVS preferences while the bus is live).
    add_idf_sdkconfig_option("CONFIG_TWAI_IO_FUNC_IN_IRAM", True)
    # The driver's ISR-callable enqueue path also calls task-context FreeRTOS
    # APIs (xEventGroupClearBits, xQueueSend, xQueueReceive in
    # esp_twai_onchip.c) which ESPHome's default
    # CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y places in flash — a frame
    # forwarded during a cache-off window then panics inside FreeRTOS
    # (observed on hardware after fixing the twai_node_transmit placement).
    # Force FreeRTOS back into internal RAM; costs ~8 KB RAM, required for
    # the "forwarding survives OTA/NVS flash writes" guarantee.
    add_idf_sdkconfig_option("CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH", False)
    # ESP-IDF 6.0 removes that option (flash placement becomes the default)
    # and offers CONFIG_FREERTOS_IN_IRAM to restore RAM placement. Set both so
    # the guarantee survives the IDF transition; the one the running IDF does
    # not know is ignored.
    add_idf_sdkconfig_option("CONFIG_FREERTOS_IN_IRAM", True)

    # Slot pools are static arrays sized at codegen: the driver
    # TX queue holds pointers, so pool >= queue depth, plus spare so RX
    # processing still has a slot while the queue is briefly full.
    max_depth = max(port[CONF_TX_QUEUE_DEPTH] for port in config[CONF_PORTS])
    cg.add_define("CAN_GATEWAY_TX_SLOTS", max_depth + 2)
    cg.add_define("CAN_GATEWAY_INJECT_SLOTS", inject_slot_count(config))
    # Observation ring size (A13) — one static ring per observing port, sized to
    # the largest configured observe_queue_depth. The subscribed-ID set is a
    # fixed N7 bound. Both are only instantiated under USE_CAN_GATEWAY_OBSERVE,
    # emitted by the decode entity platforms; harmless sizes otherwise.
    max_observe_depth = max(
        port[CONF_OBSERVE_QUEUE_DEPTH] for port in config[CONF_PORTS]
    )
    cg.add_define("CAN_GATEWAY_OBSERVE_QUEUE_DEPTH", max_observe_depth)
    cg.add_define("CAN_GATEWAY_MAX_OBSERVE_IDS", OBSERVE_MAX_IDS)
    # Datalogger tap (sd_logger S2). USE_CAN_GATEWAY_LOG_TAP also pulls in
    # CAN_GATEWAY_HAS_RX_TAPS, so an otherwise diagnostics-free build still
    # compiles the tap path and still receives shed frames for the log.
    if any(port[CONF_LOG_TAP] for port in config[CONF_PORTS]):
        cg.add_define("USE_CAN_GATEWAY_LOG_TAP")
    cg.add_define("CAN_GATEWAY_LOG_TAP_DEPTH", config[CONF_LOG_TAP_QUEUE_DEPTH])

    var = cg.new_Pvariable(config[CONF_ID], config[CONF_INTERRUPT_PRIORITY])
    await cg.register_component(var, config)

    for index, port_config in enumerate(config[CONF_PORTS]):
        port = cg.new_Pvariable(
            port_config[CONF_ID],
            index,
            port_config[CONF_RX_PIN],
            port_config[CONF_TX_PIN],
            port_config[CONF_BIT_RATE],
        )
        if port_config[CONF_LISTEN_ONLY]:
            cg.add(port.set_listen_only(True))
        if port_config[CONF_SELF_TEST]:
            cg.add(port.set_self_test(True))
        if port_config[CONF_OPEN_DRAIN_TX]:
            cg.add(port.set_open_drain_tx(True))
        cg.add(port.set_tx_queue_depth(port_config[CONF_TX_QUEUE_DEPTH]))
        if port_config[CONF_LOG_TAP]:
            cg.add(port.set_log_tap(True))
        cg.add(port.set_max_frames_per_loop(port_config[CONF_MAX_FRAMES_PER_LOOP]))
        cg.add(var.add_port(port))
        for conf in port_config.get(CONF_ON_BUS_OFF, []):
            await automation.build_callback_automation(
                port, "add_on_bus_off_callback", [], conf
            )
        for conf in port_config.get(CONF_ON_RECOVERED, []):
            await automation.build_callback_automation(
                port, "add_on_recovered_callback", [], conf
            )
        for conf in port_config.get(CONF_ON_FRAME, []):
            # The trigger subscribes in its constructor, which codegen emits
            # before App.setup() — the port freezes its subscriber set in
            # setup(), so registering any later would silently observe nothing.
            cg.add_define("USE_CAN_GATEWAY_OBSERVE")
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], port)
            await automation.build_automation(
                trigger,
                [
                    (cg.uint32, "can_id"),
                    (cg.uint8.operator("const").operator("ptr"), "data"),
                    (cg.uint8, "dlc"),
                    (cg.bool_, "extended"),
                    (cg.bool_, "rtr"),
                ],
                conf,
            )

    for route_config in config.get(CONF_ROUTES, []):
        source = await cg.get_variable(route_config[CONF_FROM])
        target = await cg.get_variable(route_config[CONF_TO])
        rules = route_config.get(CONF_FILTERS, [])
        route = cg.new_Pvariable(
            route_config[CONF_ID],
            source,
            target,
            len(rules),
            route_config[CONF_DEFAULT_ACTION] == ACTION_ACCEPT,
        )
        for rule in rules:
            modify = rule.get(CONF_MODIFY)
            new_id = rule[CONF_CAN_ID]
            if (
                modify is not None
                and (modify_id := modify.get(CONF_CAN_ID)) is not None
            ):
                new_id = modify_id
            and_mask, or_value = _packed_patch_masks(modify)
            patch = cg.nullptr
            if (rule_id := rule.get(CONF_ID)) is not None:
                patch = cg.new_Pvariable(rule_id)
            cg.add(
                route.add_rule(
                    rule[CONF_CAN_ID],
                    rule[CONF_CAN_ID_MASK],
                    _rule_flags(rule),
                    new_id,
                    and_mask,
                    or_value,
                    patch,
                )
            )
        cg.add(var.add_route(route))

    cyclic_sends = config.get(CONF_CYCLIC_SENDS, [])
    if cyclic_sends:
        cg.add_define("USE_CAN_GATEWAY_CYCLIC")
        cg.add_define("CAN_GATEWAY_CYCLIC_COUNT", len(cyclic_sends))
    for cyclic_config in cyclic_sends:
        cyclic_port = await cg.get_variable(cyclic_config[CONF_PORT])
        cyclic = cg.new_Pvariable(
            cyclic_config[CONF_ID],
            cyclic_port,
            cyclic_config[CONF_CAN_ID],
            cyclic_config[CONF_USE_EXTENDED_ID],
            cyclic_config[CONF_REMOTE_TRANSMISSION_REQUEST],
            cyclic_config[CONF_INTERVAL].total_milliseconds,
            cyclic_config[CONF_ENABLED],
        )
        if data := cyclic_config[CONF_DATA]:
            # Initial payload lives in flash; copied once into the sender at setup.
            arr_id = ID(
                f"{cyclic_config[CONF_ID]}_data", is_declaration=True, type=cg.uint8
            )
            arr = cg.static_const_array(arr_id, cg.ArrayInitializer(*data))
            cg.add(cyclic.set_data(arr, len(data)))
        cg.add(var.add_cyclic_send(cyclic))

    if (statistics := config.get(CONF_STATISTICS)) is not None:
        cg.add_define("USE_CAN_GATEWAY_STATS")
        cg.add(
            var.set_stats_log_interval(statistics[CONF_LOG_INTERVAL].total_milliseconds)
        )
        if statistics[CONF_ID_TIMINGS]:
            cg.add_define("USE_CAN_GATEWAY_ID_STATS")
            cg.add_define("CAN_GATEWAY_ID_STATS_MAX", statistics[CONF_ID_TIMINGS_MAX])
            cg.add(var.set_id_timings_enabled(True))


@automation.register_action(
    ACTION_SET_PATCH, SetPatchAction, SET_PATCH_ACTION_SCHEMA, synchronous=True
)
async def set_patch_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if (can_id := config.get(CONF_CAN_ID)) is not None:
        if cg.is_template(can_id):
            template_ = await cg.templatable(can_id, args, cg.uint32)
            cg.add(var.set_can_id_template(template_))
        else:
            cg.add(var.set_can_id(can_id))
    if patches := config.get(CONF_DATA, []):
        # The byte count is fixed by the YAML; one exact allocation.
        cg.add(var.init_bytes(len(patches)))
    for patch in patches:
        value = patch[CONF_VALUE]
        if cg.is_template(value):
            template_ = await cg.templatable(value, args, cg.uint8)
            cg.add(var.add_templated_byte(patch[CONF_INDEX], template_))
        else:
            cg.add(var.add_static_byte(patch[CONF_INDEX], value))
    return var


async def _add_payload_codegen(var, data, action_id, args):
    """Emit the static/templated payload hand-off shared by inject and
    set_cyclic_data (both parents mix in DataPayload)."""
    if cg.is_template(data):
        template_ = await cg.templatable(data, args, cg.std_vector.template(cg.uint8))
        cg.add(var.set_data_template(template_))
    elif data:
        # Static data lives in flash; no RAM copy.
        arr_id = ID(f"{action_id}_data", is_declaration=True, type=cg.uint8)
        arr = cg.static_const_array(arr_id, cg.ArrayInitializer(*data))
        cg.add(var.set_data_static(arr, len(data)))
    else:
        cg.add(var.set_data_static(cg.nullptr, 0))


async def _send_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_PORT])
    cg.add(
        var.set_flags(
            config[CONF_USE_EXTENDED_ID],
            config[CONF_REMOTE_TRANSMISSION_REQUEST],
        )
    )
    can_id = config[CONF_CAN_ID]
    if cg.is_template(can_id):
        template_ = await cg.templatable(can_id, args, cg.uint32)
        cg.add(var.set_can_id_template(template_))
    else:
        cg.add(var.set_can_id(can_id))
    await _add_payload_codegen(var, config[CONF_DATA], action_id, args)
    return var


@automation.register_action(
    ACTION_SEND, InjectAction, SEND_ACTION_SCHEMA, synchronous=True
)
async def send_action_to_code(config, action_id, template_arg, args):
    return await _send_action_to_code(config, action_id, template_arg, args)


# can_gateway.inject is the pre-v0.6 name, kept as an alias for one cycle.
# INJECT_ACTION_SCHEMA == SEND_ACTION_SCHEMA plus a config-time deprecation
# warning; the codegen path is shared, so old YAML still works unchanged.
@automation.register_action(
    ACTION_INJECT, InjectAction, INJECT_ACTION_SCHEMA, synchronous=True
)
async def inject_action_to_code(config, action_id, template_arg, args):
    return await _send_action_to_code(config, action_id, template_arg, args)


# ---------------------------------------------------------------------------
# Cyclic-send actions
# ---------------------------------------------------------------------------


SET_CYCLIC_DATA_ACTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(CyclicSend),
        cv.Required(CONF_DATA): cv.templatable(_validate_frame_data),
    }
)

CYCLIC_STATE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(CyclicSend),
    }
)


@automation.register_action(
    ACTION_SET_CYCLIC_DATA,
    SetCyclicDataAction,
    SET_CYCLIC_DATA_ACTION_SCHEMA,
    synchronous=True,
)
async def set_cyclic_data_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    await _add_payload_codegen(var, config[CONF_DATA], action_id, args)
    return var


@automation.register_action(
    ACTION_START_CYCLIC, StartCyclicAction, CYCLIC_STATE_ACTION_SCHEMA, synchronous=True
)
async def start_cyclic_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    ACTION_STOP_CYCLIC, StopCyclicAction, CYCLIC_STATE_ACTION_SCHEMA, synchronous=True
)
async def stop_cyclic_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
