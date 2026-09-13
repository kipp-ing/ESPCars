"""linbus multi-instance validation.

The CAN_LIN_ESP32 reference board carries two LIN segments, so the component has
to accept more than one instance. It must also reject the two ways of getting
that wrong, because both fail *silently* on hardware: a shared UART and a shared
GPIO leave a correctly wired bus mute, which on the bench is indistinguishable
from a dead transceiver.
"""

from __future__ import annotations

import pytest

import esphome.config_validation as cv

from .common import LIN0, LIN1, final_validate, instance, setup_esp32, validate


def test_component_is_multi_conf(set_core_config) -> None:
    """MULTI_CONF is what lets a YAML declare two `linbus:` blocks at all."""
    setup_esp32(set_core_config)

    from esphome.components import linbus

    assert linbus.MULTI_CONF is True


def test_two_segments_accepted(set_core_config, set_component_config) -> None:
    """The Mr. Purple layout: distinct UARTs, distinct pins."""
    setup_esp32(set_core_config)

    result = final_validate([LIN0, LIN1], set_component_config)

    assert result["uart_port"] == "UART_NUM_1"


def test_single_instance_still_valid(set_core_config, set_component_config) -> None:
    """One instance must keep validating exactly as before — no regression."""
    setup_esp32(set_core_config)

    result = final_validate([LIN0], set_component_config)

    assert result["uart_port"] == "UART_NUM_1"


@pytest.mark.parametrize("index", [0, 1])
def test_shared_uart_port_rejected(
    set_core_config, set_component_config, index: int
) -> None:
    """Two segments on one UART break both; the error names the other instance."""
    setup_esp32(set_core_config)
    clash = instance(LIN1, uart_port="UART_NUM_1")

    with pytest.raises(cv.Invalid, match=r"uart_port UART_NUM_1 is already used"):
        final_validate([LIN0, clash], set_component_config, index=index)


@pytest.mark.parametrize(
    ("overrides", "their_role", "my_key"),
    [
        # LIN1 puts something on a GPIO that LIN0 already uses, in every
        # combination of roles — a pin is a pin, whatever job each side gives it.
        ({"tx_pin": "GPIO13"}, "tx_pin", "rx_pin"),
        ({"rx_pin": "GPIO12"}, "rx_pin", "tx_pin"),
        ({"cs_pin": "GPIO27"}, "cs_pin", "cs_pin"),
        ({"tx_pin": "GPIO27"}, "tx_pin", "cs_pin"),
    ],
)
def test_shared_pin_rejected(
    set_core_config,
    set_component_config,
    overrides: dict,
    their_role: str,
    my_key: str,
) -> None:
    """A GPIO may belong to only one segment, whatever role it plays in each.

    The message names the role the *other* instance gives the pin, while the
    error path points at the key in the instance being validated — so whichever
    block the user is reading, they are told what to change and what it clashes
    with.
    """
    setup_esp32(set_core_config)
    gpio = list(overrides.values())[0]

    with pytest.raises(cv.Invalid) as excinfo:
        final_validate([LIN0, instance(LIN1, **overrides)], set_component_config)

    assert f"{gpio} is already used as {their_role} by linbus 'lin1'" in str(
        excinfo.value
    )
    assert excinfo.value.path == [my_key]


def test_distinct_pins_and_uarts_survive_both_directions(
    set_core_config, set_component_config
) -> None:
    """A clean pair validates from either side, not just the first."""
    setup_esp32(set_core_config)

    assert final_validate([LIN0, LIN1], set_component_config, index=0)
    assert final_validate([LIN0, LIN1], set_component_config, index=1)


def test_cs_pin_is_optional_and_not_compared_when_absent(
    set_core_config, set_component_config
) -> None:
    """Omitting cs_pin must not make two instances look like they collide."""
    setup_esp32(set_core_config)
    a = {k: v for k, v in LIN0.items() if k != "cs_pin"}
    b = {k: v for k, v in LIN1.items() if k != "cs_pin"}

    assert final_validate([a, b], set_component_config)


def test_mode_validation_still_applies_per_instance(set_core_config) -> None:
    """Per-instance rules are untouched by MULTI_CONF."""
    setup_esp32(set_core_config)

    with pytest.raises(cv.Invalid, match=r"schedule is only valid with mode: master"):
        validate(
            instance(LIN1, mode="slave", schedule=[{"lin_id": 4, "interval": "50ms"}])
        )
