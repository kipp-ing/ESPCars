"""Schema tests for can_gateway.set_patch (cfg-v10) and can_gateway.inject (cfg-v11).

Inline bounds are enforced by the action schemas; cross-references against the
gateway block (declared patch shape, listen-only ports) are enforced by the
component's final-validate step, which scans the full config for gateway
actions.
"""

from __future__ import annotations

import logging

import pytest

from esphome import config_validation as cv
import esphome.codegen as cg
from esphome.core import CORE, ID

from .common import PORT_A, PORT_B, gateway, port, route, setup_c6


def _schemas():
    from esphome.components.can_gateway import (
        INJECT_ACTION_SCHEMA,
        SET_PATCH_ACTION_SCHEMA,
    )

    return SET_PATCH_ACTION_SCHEMA, INJECT_ACTION_SCHEMA


def _final_validate(gateway_config, actions, set_component_config):
    """Run the component final-validate with `actions` planted in the full config."""
    from esphome.components.can_gateway import FINAL_VALIDATE_SCHEMA

    set_component_config("can_gateway", gateway_config)
    # Where the automation lives is irrelevant; the scan is recursive.
    set_component_config(
        "button",
        [{"platform": "template", "on_press": [{"then": actions}]}],
    )
    return FINAL_VALIDATE_SCHEMA(gateway_config)


def _patch_rule_gateway(set_core_config, *, with_can_id: bool = False):
    """A validated gateway whose route declares updatable rule `charge_limit`."""
    from esphome.components.can_gateway import CONFIG_SCHEMA

    setup_c6(set_core_config)
    modify = {"data": [{"index": 2, "value": 0x64}]}
    if with_can_id:
        modify["can_id"] = 0x581
    return CONFIG_SCHEMA(
        gateway(
            routes=[
                route(
                    filters=[{"id": "charge_limit", "can_id": 0x355, "modify": modify}]
                )
            ]
        )
    )


# ---------------------------------------------------------------- cfg-v10


def test_v10_declared_byte_accepted(set_core_config, set_component_config) -> None:
    set_patch_schema, _ = _schemas()
    config = _patch_rule_gateway(set_core_config)
    action = set_patch_schema(
        {"id": "charge_limit", "data": [{"index": 2, "value": 0x42}]}
    )
    _final_validate(config, [{"can_gateway.set_patch": action}], set_component_config)


def test_v10_undeclared_byte_rejected(set_core_config, set_component_config) -> None:
    set_patch_schema, _ = _schemas()
    config = _patch_rule_gateway(set_core_config)
    action = set_patch_schema(
        {"id": "charge_limit", "data": [{"index": 3, "value": 0x42}]}
    )
    with pytest.raises(cv.Invalid, match="declare"):
        _final_validate(
            config, [{"can_gateway.set_patch": action}], set_component_config
        )


def test_v10_can_id_only_when_declared(set_core_config, set_component_config) -> None:
    set_patch_schema, _ = _schemas()
    config = _patch_rule_gateway(set_core_config)
    action = set_patch_schema({"id": "charge_limit", "can_id": 0x582})
    with pytest.raises(cv.Invalid, match="can_id"):
        _final_validate(
            config, [{"can_gateway.set_patch": action}], set_component_config
        )


def test_v10_declared_can_id_accepted(set_core_config, set_component_config) -> None:
    set_patch_schema, _ = _schemas()
    config = _patch_rule_gateway(set_core_config, with_can_id=True)
    action = set_patch_schema({"id": "charge_limit", "can_id": 0x582})
    _final_validate(config, [{"can_gateway.set_patch": action}], set_component_config)


def test_v10_can_id_outside_output_frame_type_rejected(
    set_core_config, set_component_config
) -> None:
    # The rule's output frame type is standard (11-bit); a static 29-bit
    # value would be silently truncated on the wire, so it must be rejected.
    set_patch_schema, _ = _schemas()
    config = _patch_rule_gateway(set_core_config, with_can_id=True)
    action = set_patch_schema({"id": "charge_limit", "can_id": 0x18DAF110})
    with pytest.raises(cv.Invalid, match="0x7FF"):
        _final_validate(
            config, [{"can_gateway.set_patch": action}], set_component_config
        )


def test_v10_unknown_rule_rejected(set_core_config, set_component_config) -> None:
    set_patch_schema, _ = _schemas()
    config = _patch_rule_gateway(set_core_config)
    action = set_patch_schema(
        {"id": "other_rule", "data": [{"index": 2, "value": 0x42}]}
    )
    with pytest.raises(cv.Invalid, match="filter rule"):
        _final_validate(
            config, [{"can_gateway.set_patch": action}], set_component_config
        )


def test_v10_empty_action_rejected(set_core_config) -> None:
    set_patch_schema, _ = _schemas()
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        set_patch_schema({"id": "charge_limit"})


# ---------------------------------------------------------------- cfg-v11


def test_v11_inject_accepted(set_core_config) -> None:
    _, inject_schema = _schemas()
    setup_c6(set_core_config)
    inject_schema({"port": "port_a", "can_id": 0x100, "data": [1, 2, 3]})
    inject_schema({"port": "port_a", "can_id": 0x100})  # no data = DLC 0
    inject_schema(
        {
            "port": "port_a",
            "can_id": 0x18DAF110,
            "use_extended_id": True,
            "data": [1, 2, 3, 4, 5, 6, 7, 8],
        }
    )
    inject_schema(
        {"port": "port_a", "can_id": 0x100, "remote_transmission_request": True}
    )


def test_v11_nine_bytes_rejected(set_core_config) -> None:
    _, inject_schema = _schemas()
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        inject_schema({"port": "port_a", "can_id": 0x100, "data": list(range(9))})


def test_v11_rtr_with_data_rejected(set_core_config) -> None:
    _, inject_schema = _schemas()
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="RTR"):
        inject_schema(
            {
                "port": "port_a",
                "can_id": 0x100,
                "remote_transmission_request": True,
                "data": [1],
            }
        )


def test_v11_standard_id_bound_rejected(set_core_config) -> None:
    _, inject_schema = _schemas()
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="0x7FF"):
        inject_schema({"port": "port_a", "can_id": 0x800})


def test_v11_inject_on_listen_only_rejected(
    set_core_config, set_component_config
) -> None:
    from esphome.components.can_gateway import CONFIG_SCHEMA

    _, inject_schema = _schemas()
    setup_c6(set_core_config)
    config = CONFIG_SCHEMA(
        gateway(
            ports=[port(PORT_A, listen_only=True), dict(PORT_B)],
            routes=[{"from": "port_a", "to": "port_b"}],
        )
    )
    action = inject_schema({"port": "port_a", "can_id": 0x100})
    with pytest.raises(cv.Invalid, match="listen.only"):
        _final_validate(config, [{"can_gateway.inject": action}], set_component_config)
    # Injecting on the normal port is fine.
    action = inject_schema({"port": "port_b", "can_id": 0x100})
    _final_validate(config, [{"can_gateway.inject": action}], set_component_config)


# ---------------------------------------------------------------- V26
# `can_gateway.inject` is the pre-v0.6 spelling of `can_gateway.send`. It is
# deprecated for one release cycle: it must WARN, never reject, and must keep
# producing exactly the code the old YAML produced before.


_COMPONENT_LOGGER = "esphome.components.can_gateway"


def _registry_entries():
    from esphome.automation import ACTION_REGISTRY
    from esphome.components.can_gateway import ACTION_INJECT, ACTION_SEND

    return ACTION_REGISTRY[ACTION_INJECT], ACTION_REGISTRY[ACTION_SEND]


def _component_warnings(caplog) -> list[str]:
    return [
        record.getMessage()
        for record in caplog.records
        if record.name == _COMPONENT_LOGGER and record.levelno >= logging.WARNING
    ]


def test_v26_inject_alias_warns_deprecated(set_core_config, caplog) -> None:
    """The alias emits a config-time deprecation warning naming its successor."""
    inject_entry, _ = _registry_entries()
    setup_c6(set_core_config)
    with caplog.at_level(logging.WARNING, logger=_COMPONENT_LOGGER):
        inject_entry.schema({"port": "port_a", "can_id": 0x100, "data": [1]})
    warnings = _component_warnings(caplog)
    assert warnings, "can_gateway.inject validated without a deprecation warning"
    message = warnings[0]
    assert "can_gateway.inject" in message
    assert "deprecated" in message
    assert "can_gateway.send" in message  # the migration target must be named


def test_v26_send_does_not_warn(set_core_config, caplog) -> None:
    """The supported spelling must stay quiet — otherwise the warning is noise."""
    _, send_entry = _registry_entries()
    setup_c6(set_core_config)
    with caplog.at_level(logging.WARNING, logger=_COMPONENT_LOGGER):
        send_entry.schema({"port": "port_a", "can_id": 0x100, "data": [1]})
    assert _component_warnings(caplog) == []


def test_v26_inject_alias_warning_is_deduped(set_core_config, caplog) -> None:
    """One warning per run, not one per call site."""
    inject_entry, _ = _registry_entries()
    setup_c6(set_core_config)
    with caplog.at_level(logging.WARNING, logger=_COMPONENT_LOGGER):
        for _ in range(3):
            inject_entry.schema({"port": "port_a", "can_id": 0x100})
    assert len(_component_warnings(caplog)) == 1


def test_v26_inject_alias_still_validates(set_core_config) -> None:
    """Deprecated, not rejected: the alias yields exactly send's config."""
    inject_entry, send_entry = _registry_entries()
    setup_c6(set_core_config)
    raw = {
        "port": "port_a",
        "can_id": 0x100,
        "use_extended_id": False,
        "data": [1, 2, 3],
    }
    assert inject_entry.schema(dict(raw)) == send_entry.schema(dict(raw))
    # Same C++ action class, so the two names cannot drift apart in codegen.
    assert inject_entry.type_id == send_entry.type_id


def test_v26_inject_alias_still_generates_code(set_core_config) -> None:
    """The alias must still reach codegen — a warning must not short-circuit it."""
    from esphome.components.can_gateway import InjectAction

    inject_entry, _ = _registry_entries()
    setup_c6(set_core_config)
    action = inject_entry.schema(
        {"port": "port_a", "can_id": 0x123, "data": [0x01, 0x02]}
    )
    # Stand in for the port variable the gateway block would have registered.
    CORE.register_variable(action["port"], cg.MockObj("port_a", "->"))
    action_id = ID("gen_inject", is_declaration=True, type=InjectAction.template())
    CORE.add_job(
        inject_entry.coroutine_fun, action, action_id, cg.TemplateArguments(), []
    )
    CORE.flush_tasks()

    generated = "\n".join(str(s) for s in CORE.main_statements)
    assert "can_gateway::InjectAction" in generated
    assert "gen_inject->set_parent(port_a);" in generated
    assert "gen_inject->set_can_id(291);" in generated  # 0x123
    assert "gen_inject->set_data_static(gen_inject_data, 2);" in generated
