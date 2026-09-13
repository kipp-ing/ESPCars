"""Shared helpers for linbus config-validation tests.

Mirrors ``tests/can_gateway/common.py``: core setup, a ``validate()`` that runs
the component's ``CONFIG_SCHEMA`` (imported late, after the core is set up), and
minimal valid base configs that individual tests copy and mutate.

The target here is the **classic ESP32** rather than the C6, because that is the
chip with three UARTs and therefore the one that can actually host two LIN
segments — see ``docs/commissioning-mr-purple.md``.
"""

from __future__ import annotations

from typing import Any

from esphome.components.esp32.const import KEY_BOARD, KEY_VARIANT, VARIANT_ESP32
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework

# Pins follow the CAN_LIN_ESP32 reference board ("Mr. Purple"):
# LIN0 = TX GPIO12 / RX GPIO13 / enable GPIO27 on UART1,
# LIN1 = TX GPIO33 / RX GPIO32 / enable GPIO21 on UART2.
LIN0 = {
    "id": "lin0",
    "tx_pin": "GPIO12",
    "rx_pin": "GPIO13",
    "cs_pin": "GPIO27",
    "uart_port": "UART_NUM_1",
}
LIN1 = {
    "id": "lin1",
    "tx_pin": "GPIO33",
    "rx_pin": "GPIO32",
    "cs_pin": "GPIO21",
    "uart_port": "UART_NUM_2",
    "mode": "listener",
}


def setup_esp32(set_core_config, full_config: dict[str, Any] | None = None) -> None:
    """Set the core up as a classic ESP32 / IDF target."""
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={
            KEY_BOARD: "esp32dev",
            KEY_VARIANT: VARIANT_ESP32,
        },
        full_config=full_config,
    )


def validate(config: dict[str, Any]) -> dict[str, Any]:
    """Run the component's CONFIG_SCHEMA (imported late, after setup_esp32)."""
    from esphome.components.linbus import CONFIG_SCHEMA

    return CONFIG_SCHEMA(config)


def instance(base: dict[str, Any], **overrides: Any) -> dict[str, Any]:
    """A copy of an instance config with overrides applied."""
    return {**base, **overrides}


def final_validate(configs: list[dict[str, Any]], set_component_config, index: int = 0):
    """Validate every instance, publish them, then final-validate one of them.

    ``index`` picks which instance's FINAL_VALIDATE_SCHEMA runs — a conflict must
    be reported from either side, so tests exercise both.
    """
    from esphome.components.linbus import FINAL_VALIDATE_SCHEMA

    validated = [validate(c) for c in configs]
    set_component_config("linbus", validated)
    return FINAL_VALIDATE_SCHEMA(validated[index])
