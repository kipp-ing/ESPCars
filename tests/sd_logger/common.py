"""Shared helpers for sd_logger config-validation tests.

Mirrors the sibling suites: the same ESP32-C6 / IDF core setup, a ``validate()``
that runs the component's ``CONFIG_SCHEMA`` (imported late, after the core is set
up), and a minimal valid base config that individual tests copy and mutate.
"""

from __future__ import annotations

from typing import Any

from esphome.components.esp32.const import KEY_BOARD, KEY_VARIANT, VARIANT_ESP32C6
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework

# A minimal valid logger: four non-strapping SPI pins. clk sits at GPIO6 (top of
# the ADC1 range) so a vcc_monitor adc_pin can be made to collide with it.
BASE = {
    "clk_pin": "GPIO6",
    "mosi_pin": "GPIO7",
    "miso_pin": "GPIO10",
    "cs_pin": "GPIO11",
}


def setup_c6(set_core_config) -> None:
    """Set the core up as an ESP32-C6 / IDF target."""
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={
            KEY_BOARD: "esp32-c6-devkitc-1",
            KEY_VARIANT: VARIANT_ESP32C6,
        },
    )


def validate(config):
    """Run the component's CONFIG_SCHEMA (imported late, after setup_c6)."""
    from esphome.components.sd_logger import CONFIG_SCHEMA

    return CONFIG_SCHEMA(config)


def sd_logger(**overrides: Any) -> dict[str, Any]:
    """A copy of the base config with overrides applied.

    A value of ``None`` removes the key, so tests can drop a required key.
    """
    config = {**BASE, **overrides}
    return {key: value for key, value in config.items() if value is not None}


def vcc_monitor(**overrides: Any) -> dict[str, Any]:
    """A valid vcc_monitor block (ADC on a free ADC1 pin)."""
    defaults: dict[str, Any] = {
        "adc_pin": 0,
        "threshold": "10.5V",
        "divider": 4.0,
    }
    return {**defaults, **overrides}


def recovery(**overrides: Any) -> dict[str, Any]:
    """A `recovery:` block. Empty by default, so the block's own defaults apply.

    A value of ``None`` removes the key, so a test can assert what defaulting does.
    """
    config = {**overrides}
    return {key: value for key, value in config.items() if value is not None}


def logger_config(**overrides: Any) -> dict[str, Any]:
    """A stand-in resolved `logger:` block, as V15-V17 see it in the full config."""
    defaults: dict[str, Any] = {
        "level": "DEBUG",
        "task_log_buffer_size": 768,
    }
    return {**defaults, **overrides}
