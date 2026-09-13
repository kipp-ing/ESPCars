"""Two shapes share `platform: uds` under `sensor:`.

A **value sensor** names a catalog field and gets its unit, scaling and "not available" sentinels
from the catalog — nothing numeric is ever written in YAML (design §1). A **diagnostics** entry
names counters instead and reports on the hub itself.

They are told apart by `field:`, the way can_gateway's text_sensor platform tells its two shapes
apart by `can_id:`. One `platform:` key rather than two because both belong to the same component
and ESPHome has no sub-platform concept; dispatching explicitly keeps the error message precise
when an entry is neither.
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_UNIT_OF_MEASUREMENT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

from . import (
    BINDING_SCHEMA,
    CONF_FIELD,
    CONF_RESOLVED_UNIT,
    CONF_UDS_ID,
    UdsHub,
    binding_final_validate,
    register_binding,
    uds_ns,
)

DEPENDENCIES = ["uds"]

UdsDiagnostics = uds_ns.class_("UdsDiagnostics", cg.PollingComponent)

CONF_REQUESTS_SENT = "requests_sent"
CONF_RESPONSES_ACCEPTED = "responses_accepted"
CONF_TIMEOUTS = "timeouts"
CONF_NEGATIVE_RESPONSES = "negative_responses"
CONF_DECODE_UNMATCHED = "decode_unmatched"
CONF_GROUPS_SUSPENDED = "groups_suspended"
CONF_PARTIAL_RESPONSES = "partial_responses"
CONF_FIELDS_UNCOVERED = "fields_uncovered"
CONF_LAST_NRC = "last_nrc"

COUNTERS = (
    CONF_REQUESTS_SENT,
    CONF_RESPONSES_ACCEPTED,
    CONF_TIMEOUTS,
    CONF_NEGATIVE_RESPONSES,
    CONF_DECODE_UNMATCHED,
    CONF_GROUPS_SUSPENDED,
    # A response shorter than the group's resp_min_len is a success, not an error
    # — but "some fields never decode" has to be visible without a debugger, so
    # these two are the pair to read when an entity is unexpectedly unavailable.
    CONF_PARTIAL_RESPONSES,
    CONF_FIELDS_UNCOVERED,
)


def _counter_schema() -> cv.Schema:
    """Monotonic totals, so they all share one entity shape."""
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


VALUE_SCHEMA = sensor.sensor_schema(state_class=STATE_CLASS_MEASUREMENT).extend(BINDING_SCHEMA).extend(
    # `never` is a first-class choice, not a mistake: identity DIDs and a wide capacity block are
    # read on demand through `uds.read` and must not be polled (design §5).
    cv.Schema({cv.Optional("update_interval", default="60s"): cv.update_interval})
)

DIAGNOSTICS_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(UdsDiagnostics),
            cv.GenerateID(CONF_UDS_ID): cv.use_id(UdsHub),
            cv.Optional(CONF_LAST_NRC): sensor.sensor_schema(
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            **{cv.Optional(counter): _counter_schema() for counter in COUNTERS},
        }
    ).extend(cv.polling_component_schema("60s")),
    # An entry declaring no counter would create a polling component that publishes nothing, which
    # reads as a working config that does not work.
    cv.has_at_least_one_key(*COUNTERS, CONF_LAST_NRC),
)


def CONFIG_SCHEMA(config):  # noqa: N802 - ESPHome's platform entry point
    if isinstance(config, dict) and CONF_FIELD in config:
        return VALUE_SCHEMA(config)
    if isinstance(config, dict) and any(key in config for key in (*COUNTERS, CONF_LAST_NRC)):
        return DIAGNOSTICS_SCHEMA(config)
    raise cv.Invalid(
        "a 'platform: uds' sensor is either a decoded value — which needs 'field:' — or a block of "
        "hub diagnostics, which needs at least one of: " + ", ".join((*COUNTERS, CONF_LAST_NRC))
    )


def _final_validate(config):
    if CONF_FIELD not in config:
        return config
    binding_final_validate(config, want_text=False)
    # The catalog owns the unit (design §1), so YAML does not have to restate it — and when YAML
    # does state one, it wins, because a corrected unit must not need a catalog rebuild.
    if CONF_UNIT_OF_MEASUREMENT not in config and config[CONF_RESOLVED_UNIT]:
        config[CONF_UNIT_OF_MEASUREMENT] = config[CONF_RESOLVED_UNIT]
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    if CONF_FIELD in config:
        sens = await sensor.new_sensor(config)
        await register_binding(config, sens, adder="add_sensor_binding")
        return

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_UDS_ID])
    for counter in (*COUNTERS, CONF_LAST_NRC):
        if (conf := config.get(counter)) is not None:
            sens = await sensor.new_sensor(conf)
            cg.add(getattr(var, f"set_{counter}_sensor")(sens))
