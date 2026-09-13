"""`on_frame:` — the raw sniffer trigger (A14).

The one subscriber that names no ID. Everything else in the observation path
(`sensor:`/`binary_sensor:`/`text_sensor:` decode entities) subscribes to a
specific can_id, which lets the port hand the ID set to the hardware filter;
`on_frame` cannot, so it sets `observe_all_` and the offload stays off (B25).
That cost is the reason it is diagnostics-only and never a default, and the
reason these tests pin *where* it may appear as much as *that* it validates.

Codegen depends on two schema properties that are easy to break silently:
`to_code` reads `port_config.get(CONF_ON_FRAME, [])`, so the key must stay
absent when unused (a default of `[]` would be harmless, a default of `None`
would crash), and it emits one trigger per automation block, so the validated
value must stay a *list* even for the single-block shorthand.
"""

from __future__ import annotations

import pytest

from esphome import config_validation as cv
from esphome.const import CONF_TRIGGER_ID

from .common import PORT_A, PORT_B, gateway, port, setup_c6, validate

# A lambda with the full trigger signature: can_id, data, dlc, extended, rtr.
# `data` is a borrowed `const uint8_t *`, never a container — the bench
# catcher's `topo::observe()` call has exactly this shape. Pre-wrapped in
# cv.Lambda because cv.lambda_ only attaches YAML source metadata to a plain
# str, which a dict literal has none of (same reason as test_send.py).
SNIFF = {
    "lambda": cv.Lambda(
        'ESP_LOGD("t", "%u %u %d %d", can_id, (unsigned) data[0], dlc, extended || rtr);'
    )
}


def on_frame(port_config: dict) -> list:
    """The validated on_frame automation list of a port."""
    from esphome.components.can_gateway import CONF_ON_FRAME

    return port_config[CONF_ON_FRAME]


# ---------------------------------------------------------------- acceptance


def test_on_frame_with_then_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(
        gateway(ports=[port(PORT_A, on_frame={"then": [SNIFF]}), dict(PORT_B)])
    )
    assert len(on_frame(config["ports"][0])) == 1


def test_on_frame_shorthand_action_list_accepted(set_core_config) -> None:
    """`on_frame: - lambda: ...` — the form the bench configs use."""
    setup_c6(set_core_config)
    config = validate(gateway(ports=[port(PORT_A, on_frame=[SNIFF]), dict(PORT_B)]))
    assert len(on_frame(config["ports"][0])) == 1


def test_on_frame_normalises_to_a_list(set_core_config) -> None:
    """to_code iterates the value; a bare dict would iterate its keys instead."""
    setup_c6(set_core_config)
    config = validate(
        gateway(ports=[port(PORT_A, on_frame={"then": [SNIFF]}), dict(PORT_B)])
    )
    assert isinstance(on_frame(config["ports"][0]), list)


def test_on_frame_generates_a_frame_trigger_id(set_core_config) -> None:
    """The generated ID must carry CanGatewayFrameTrigger: cg.new_Pvariable
    types the variable from it, and the class only exists under
    USE_CAN_GATEWAY_OBSERVE."""
    setup_c6(set_core_config)
    config = validate(gateway(ports=[port(PORT_A, on_frame=[SNIFF]), dict(PORT_B)]))
    trigger_id = on_frame(config["ports"][0])[0][CONF_TRIGGER_ID]
    assert "CanGatewayFrameTrigger" in str(trigger_id.type)


def test_two_automations_on_one_port_accepted(set_core_config) -> None:
    """Each block becomes its own trigger, so each needs its own ID."""
    setup_c6(set_core_config)
    config = validate(
        gateway(
            ports=[port(PORT_A, on_frame=[{"then": [SNIFF]}, {"then": [SNIFF]}])],
            routes=[],
        )
    )
    blocks = on_frame(config["ports"][0])
    assert len(blocks) == 2
    # Generated IDs are still unnamed here (the resolver names them later), so
    # identity is what distinguishes them: one shared ID object would emit two
    # variables under one name.
    assert blocks[0][CONF_TRIGGER_ID] is not blocks[1][CONF_TRIGGER_ID]


def test_on_frame_on_both_ports_accepted(set_core_config) -> None:
    """The catcher beacons and listens on every port it owns, cabled or not."""
    setup_c6(set_core_config)
    config = validate(
        gateway(
            ports=[
                port(PORT_A, on_frame=[SNIFF]),
                port(PORT_B, on_frame=[SNIFF]),
            ]
        )
    )
    assert len(on_frame(config["ports"][0])) == 1
    assert len(on_frame(config["ports"][1])) == 1


def test_on_frame_coexists_with_the_port_state_triggers(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(
        gateway(
            ports=[
                port(
                    PORT_A,
                    on_frame=[SNIFF],
                    on_bus_off={"then": [{"lambda": cv.Lambda('ESP_LOGW("t", "off");')}]},
                    on_recovered={"then": [{"lambda": cv.Lambda('ESP_LOGI("t", "up");')}]},
                ),
                dict(PORT_B),
            ]
        )
    )
    assert len(on_frame(config["ports"][0])) == 1


def test_on_frame_on_a_listen_only_port_accepted(set_core_config) -> None:
    """Sniffing a live vehicle bus without ACKing it is the safest use of this
    trigger and must stay reachable (V07)."""
    setup_c6(set_core_config)
    config = validate(
        gateway(
            ports=[port(PORT_A, listen_only=True, on_frame=[SNIFF]), dict(PORT_B)],
            routes=[],
        )
    )
    assert len(on_frame(config["ports"][0])) == 1


def test_on_frame_on_a_single_port_monitor_accepted(set_core_config) -> None:
    """One port, no routes — the shape of tests/hil/topo-live-1can.yaml."""
    setup_c6(set_core_config)
    config = validate(gateway(ports=[port(PORT_A, on_frame=[SNIFF])], routes=[]))
    assert len(on_frame(config["ports"][0])) == 1


def test_empty_on_frame_block_still_costs_observe_all(set_core_config) -> None:
    """`on_frame:` with no actions validates (esphome accepts any empty
    automation) and still builds a trigger — which still calls subscribe_all().
    Recorded rather than rejected: the point is that the cost is paid for an
    inert block, so an accidental empty one is not free."""
    setup_c6(set_core_config)
    config = validate(gateway(ports=[port(PORT_A, on_frame={}), dict(PORT_B)]))
    blocks = on_frame(config["ports"][0])
    assert len(blocks) == 1
    assert blocks[0]["then"] == []


def test_absent_on_frame_leaves_no_key(set_core_config) -> None:
    """to_code does `.get(CONF_ON_FRAME, [])`; a validated-in default of None
    would make every port without a sniffer raise at codegen time."""
    from esphome.components.can_gateway import CONF_ON_FRAME

    setup_c6(set_core_config)
    config = validate(gateway())
    assert CONF_ON_FRAME not in config["ports"][0]


# ----------------------------------------------------------------- rejection


def test_on_frame_scalar_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(gateway(ports=[port(PORT_A, on_frame=True), dict(PORT_B)]))


def test_unknown_key_in_on_frame_block_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="can_id"):
        validate(
            gateway(
                ports=[
                    # on_frame names no ID — filtering belongs in the lambda.
                    port(PORT_A, on_frame={"can_id": 0x100, "then": [SNIFF]}),
                    dict(PORT_B),
                ]
            )
        )


def test_unknown_action_in_on_frame_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(
            gateway(
                ports=[
                    port(PORT_A, on_frame=[{"can_gateway.no_such_action": {}}]),
                    dict(PORT_B),
                ]
            )
        )


def test_on_frame_at_the_component_level_rejected(set_core_config) -> None:
    """It is a per-port subscriber: `observe_all_` is a port property, and a
    component-level key would silently sniff nothing."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="on_frame"):
        validate(gateway(on_frame=[SNIFF]))
