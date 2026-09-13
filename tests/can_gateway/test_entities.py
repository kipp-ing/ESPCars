"""Schema tests for the can_gateway entity platforms (cfg-v08)."""

from __future__ import annotations

from pathlib import Path
import re

import pytest

from esphome import config_validation as cv

from .common import gateway, setup_c6


def _validated_gateway(set_core_config):
    from esphome.components.can_gateway import CONFIG_SCHEMA

    setup_c6(set_core_config)
    return CONFIG_SCHEMA(
        gateway(routes=[{"id": "route_ab", "from": "port_a", "to": "port_b"}])
    )


# ------------------------------------------------------------------ sensor


def test_sensor_kind_order_locked() -> None:
    """ALL_KINDS defines the positional `kind` indices handed to C++.

    The order must match the KindIndex enum in can_gateway.h exactly; this
    test freezes the Python side so an accidental insertion (which would
    silently shift every later sensor onto the wrong counter) fails loudly.
    Append new kinds at the end of BOTH sides.

    test_sensor_kind_order_matches_cpp_enum below closes the other half:
    this test alone would happily accept a Python tuple edited in lockstep
    with itself while the C++ enum stayed behind.
    """
    from esphome.components.can_gateway.sensor import ALL_KINDS

    assert ALL_KINDS == (
        "forwarded",
        "filtered",
        "tx_full",
        "bus_off",
        "disabled",
        "injected",
        "tx_fail",
        "bus_err",
        "recoveries",
        "tec",
        "rec",
        "bus_load",
        "observed",
        "observe_overflow",
    )


def _cpp_kind_index() -> tuple[str, ...]:
    """The KindIndex enumerators from can_gateway.h, lowercased, in order.

    Deliberately a dumb regex parse: the enum is a flat list of bare names
    (only the first carries `= 0`), so anything fancier would just be more to
    go wrong. The terminating KIND_COUNT sentinel is dropped.
    """
    import esphome.components.can_gateway as component

    header = Path(component.__file__).parent / "can_gateway.h"
    source = header.read_text(encoding="utf-8")
    block = re.search(r"enum\s+KindIndex\s*:[^{]*\{(.*?)\}\s*;", source, re.DOTALL)
    assert block is not None, f"KindIndex enum not found in {header}"
    body = re.sub(r"//[^\n]*", "", block.group(1))  # strip line comments
    names = re.findall(r"\bKIND_([A-Z0-9_]+)\b", body)
    assert names, f"KindIndex enum in {header} parsed as empty"
    assert names[-1] == "COUNT", (
        f"expected KIND_COUNT to terminate the KindIndex enum in {header}, "
        f"got KIND_{names[-1]}"
    )
    return tuple(name.lower() for name in names[:-1])


def test_sensor_kind_order_matches_cpp_enum() -> None:
    """ALL_KINDS (sensor.py) must equal KindIndex (can_gateway.h), in order.

    The `kind` argument of set_counter_sensor() is a bare positional index, so
    a one-line insertion on either side silently publishes every later counter
    into the wrong sensor — no config error, no compile error, no runtime
    error. This is the only place the two lists are compared.
    """
    from esphome.components.can_gateway.sensor import ALL_KINDS

    cpp = _cpp_kind_index()
    if cpp == ALL_KINDS:
        return

    drift = next(
        (
            f"index {i}: C++ KIND_{a.upper() if a else '<missing>'} vs "
            f"Python {b!r}"
            for i, (a, b) in enumerate(
                zip(cpp + ("",) * len(ALL_KINDS), ALL_KINDS + ("",) * len(cpp))
            )
            if a != b
        ),
        "length only",
    )
    pytest.fail(
        "KindIndex (components/can_gateway/can_gateway.h) and ALL_KINDS "
        "(components/can_gateway/sensor.py) have drifted, so counter sensors "
        "publish onto the wrong counters from the first mismatch onward.\n"
        f"  first drift: {drift}\n"
        f"  C++ KindIndex:    {list(cpp)}\n"
        f"  Python ALL_KINDS: {list(ALL_KINDS)}\n"
        "Fix by appending to BOTH sides in the same order."
    )


def test_v08_route_counters_accepted(set_core_config) -> None:
    from esphome.components.can_gateway.sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    CONFIG_SCHEMA(
        {
            "route_id": "route_ab",
            "forwarded": {"name": "fwd"},
            "filtered": {"name": "flt"},
            "tx_full": {"name": "shed"},
            "bus_off": {"name": "dead"},
            "disabled": {"name": "off"},
        }
    )


def test_v08_port_counters_and_gauges_accepted(set_core_config) -> None:
    from esphome.components.can_gateway.sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    CONFIG_SCHEMA(
        {
            "port_id": "port_a",
            "injected": {"name": "inj"},
            "tx_fail": {"name": "fail"},
            "bus_err": {"name": "err"},
            "recoveries": {"name": "rec"},
            "tec": {"name": "tec"},
            "rec": {"name": "rxe"},
        }
    )


def test_v08_route_and_port_id_together_rejected(set_core_config) -> None:
    from esphome.components.can_gateway.sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    with pytest.raises(cv.Invalid, match="more than one of route_id, port_id"):
        CONFIG_SCHEMA(
            {"route_id": "route_ab", "port_id": "port_a", "forwarded": {"name": "x"}}
        )


def test_v08_neither_route_nor_port_id_rejected(set_core_config) -> None:
    from esphome.components.can_gateway.sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    with pytest.raises(cv.Invalid, match="exactly one"):
        CONFIG_SCHEMA({"forwarded": {"name": "x"}})


def test_v08_kind_mismatch_rejected(set_core_config) -> None:
    from esphome.components.can_gateway.sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    with pytest.raises(cv.Invalid, match="port"):
        CONFIG_SCHEMA({"route_id": "route_ab", "injected": {"name": "mismatch a"}})
    with pytest.raises(cv.Invalid, match="route"):
        CONFIG_SCHEMA({"port_id": "port_a", "forwarded": {"name": "mismatch b"}})


def test_v08_no_subsensor_rejected(set_core_config) -> None:
    from esphome.components.can_gateway.sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    with pytest.raises(cv.Invalid, match="at least one"):
        CONFIG_SCHEMA({"route_id": "route_ab"})


# ----------------------------------------------------------- binary_sensor


def test_v08_bus_off_binary_sensor_accepted(set_core_config) -> None:
    from esphome.components.can_gateway.binary_sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    CONFIG_SCHEMA({"port_id": "port_a", "bus_off": {"name": "Bus A bus-off"}})


def test_v08_binary_sensor_requires_subsensor(set_core_config) -> None:
    from esphome.components.can_gateway.binary_sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA({"port_id": "port_a"})


# ------------------------------------------------------------- text_sensor


def test_v08_last_frame_text_sensor_accepted(set_core_config) -> None:
    from esphome.components.can_gateway.text_sensor import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    validated = CONFIG_SCHEMA(
        {"port_id": "port_a", "last_frame": {"name": "Port A last frame"}}
    )
    assert validated["throttle"].total_milliseconds == 1000  # default 1 s


# ------------------------------------------------------------------ switch


def test_v08_enable_switch_accepted(set_core_config) -> None:
    from esphome.components.can_gateway.switch import CONFIG_SCHEMA

    _validated_gateway(set_core_config)
    validated = CONFIG_SCHEMA({"name": "Gateway enable"})
    # Without explicit restore_mode the gateway boots enabled.
    assert "restore_mode" in validated


def test_v08_second_switch_instance_rejected(
    set_core_config, set_component_config
) -> None:
    from esphome.components.can_gateway import FINAL_VALIDATE_SCHEMA

    config = _validated_gateway(set_core_config)
    set_component_config("can_gateway", config)
    set_component_config(
        "switch",
        [
            {"platform": "can_gateway", "name": "one"},
            {"platform": "can_gateway", "name": "two"},
        ],
    )
    with pytest.raises(cv.Invalid, match="one"):
        FINAL_VALIDATE_SCHEMA(config)
