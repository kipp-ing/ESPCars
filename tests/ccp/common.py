# Shared CCP schema test helpers.
from __future__ import annotations

from typing import Any

from esphome.components.esp32.const import KEY_BOARD, KEY_VARIANT, VARIANT_ESP32C6
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework


def setup_c6(set_core_config) -> None:
    set_core_config(PlatformFramework.ESP32_IDF, core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)}, platform_data={KEY_BOARD: "esp32-c6-devkitc-1", KEY_VARIANT: VARIANT_ESP32C6})


def hub(**overrides: Any) -> dict[str, Any]:
    value = {"id": "bms_ccp", "can_gateway_id": "diag_bus", **overrides}
    return {key: item for key, item in value.items() if item is not None}


def validate(value: dict[str, Any]):
    from esphome.components.ccp import CONFIG_SCHEMA
    return CONFIG_SCHEMA(value)
