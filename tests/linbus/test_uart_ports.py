"""Reject unavailable HP UARTs before any driver can change pins."""

import pytest

from esphome.components.esp32.const import KEY_BOARD, KEY_VARIANT
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework

from .common import validate


@pytest.mark.parametrize(
    "variant",
    [
        "ESP32C2",
        "ESP32C3",
        "ESP32C5",
        "ESP32C6",
        "ESP32S2",
        "ESP32H2",
        "ESP32H4",
        "ESP32H21",
    ],
)
@pytest.mark.parametrize("port", ["UART_NUM_0", "UART_NUM_1", "UART_NUM_2"])
def test_uart_port_matches_available_hp_uarts(set_core_config, variant, port):
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={KEY_BOARD: "unknown", KEY_VARIANT: variant},
    )
    config = {"tx_pin": "GPIO4", "rx_pin": "GPIO5", "uart_port": port}
    if port == "UART_NUM_2":
        with pytest.raises(cv.Invalid, match=rf"UART_NUM_2.*{variant}"):
            validate(config)
    else:
        assert validate(config)["uart_port"] == port


@pytest.mark.parametrize(
    "variant", ["ESP32", "ESP32S3", "ESP32P4", "ESP32C61", "ESP32S31"]
)
def test_uart2_remains_valid_where_physically_available(set_core_config, variant):
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={KEY_BOARD: "unknown", KEY_VARIANT: variant},
    )
    assert (
        validate({"tx_pin": "GPIO4", "rx_pin": "GPIO5", "uart_port": "UART_NUM_2"})[
            "uart_port"
        ]
        == "UART_NUM_2"
    )


@pytest.mark.parametrize("port", ["UART_NUM_0", "UART_NUM_1", "UART_NUM_2"])
def test_uart_port_codegen_emits_selected_enum(set_core_config, port):
    from esphome.components.linbus import to_code
    from esphome.core import CORE

    from .common import setup_esp32

    setup_esp32(set_core_config)
    config = validate(
        {"id": "lin0", "tx_pin": "GPIO17", "rx_pin": "GPIO18", "uart_port": port}
    )
    for key in ("tx_pin", "rx_pin"):
        config[key]["id"].id = f"lin0_{key}"
    CORE.component_ids.add("lin0")
    CORE.add_job(to_code, config)
    CORE.flush_tasks()
    assert f"lin0->set_uart_port(::{port});" in CORE.cpp_main_section
