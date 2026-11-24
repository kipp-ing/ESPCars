"""Config regressions for LIN UART ownership, physical pins and explicit lengths."""

import pytest

import esphome.config_validation as cv

from .common import LIN0, instance, setup_esp32, validate


@pytest.mark.parametrize("port", [0, 1, 2])
def test_logger_uart_clash_rejected(set_core_config, set_component_config, port):
    setup_esp32(set_core_config)
    from esphome.components.linbus import FINAL_VALIDATE_SCHEMA

    set_component_config(
        "logger", {"hardware_uart": f"UART{port}", "baud_rate": 115200}
    )
    config = validate(instance(LIN0, uart_port=f"UART_NUM_{port}"))
    with pytest.raises(cv.Invalid, match="logger"):
        FINAL_VALIDATE_SCHEMA(config)


@pytest.mark.parametrize(
    "logger",
    [
        None,
        {"hardware_uart": "USB_SERIAL_JTAG", "baud_rate": 115200},
        {"hardware_uart": "UART1", "baud_rate": 0},
    ],
)
def test_unused_logger_uart_accepted(set_core_config, set_component_config, logger):
    setup_esp32(set_core_config)
    from esphome.components.linbus import FINAL_VALIDATE_SCHEMA

    if logger is not None:
        set_component_config("logger", logger)
    assert FINAL_VALIDATE_SCHEMA(validate(LIN0))


@pytest.mark.parametrize(
    "logger,occupied,free",
    [
        (None, 0, 1),
        ({"hardware_uart": "UART0", "baud_rate": 115200}, 1, 2),
        ({"hardware_uart": "UART1", "baud_rate": 115200}, 0, 2),
        ({"hardware_uart": "USB_SERIAL_JTAG", "baud_rate": 115200}, 0, 1),
        ({"hardware_uart": "UART0", "baud_rate": 0}, 0, 1),
    ],
)
def test_stock_uart_allocation_clash_rejected(
    set_core_config, set_component_config, logger, occupied, free
):
    setup_esp32(set_core_config)
    from esphome.components.linbus import FINAL_VALIDATE_SCHEMA

    if logger is not None:
        set_component_config("logger", logger)
    set_component_config(
        "uart", [{"id": "stock_uart", "baud_rate": 9600, "tx_pin": {"number": 17}}]
    )
    with pytest.raises(cv.Invalid, match="uart.*stock_uart"):
        FINAL_VALIDATE_SCHEMA(
            validate(instance(LIN0, uart_port=f"UART_NUM_{occupied}"))
        )
    assert FINAL_VALIDATE_SCHEMA(validate(instance(LIN0, uart_port=f"UART_NUM_{free}")))


@pytest.mark.parametrize("key", ["tx_pin", "rx_pin"])
def test_inverted_uart_pin_rejected(set_core_config, key):
    setup_esp32(set_core_config)
    with pytest.raises(cv.Invalid, match="inverted"):
        validate(
            instance(
                LIN0,
                **{key: {"number": 12 if key == "tx_pin" else 13, "inverted": True}},
            )
        )


@pytest.mark.parametrize("key", ["tx_pin", "rx_pin"])
def test_expander_uart_pin_rejected(set_core_config, key):
    setup_esp32(set_core_config)
    # Register the real expander schema so this would validate with generic pin schemas.
    from esphome.components import pcf8574  # noqa: F401 -- registers its GPIO schema.

    with pytest.raises(cv.Invalid, match="internal"):
        validate(instance(LIN0, **{key: {"pcf8574": "io", "number": 0}}))


def test_cs_inversion_and_expander_remain_valid(set_core_config):
    setup_esp32(set_core_config)
    from esphome.components import pcf8574  # noqa: F401 -- registers its GPIO schema.

    assert validate(instance(LIN0, cs_pin={"number": 27, "inverted": True}))
    assert validate(
        instance(LIN0, cs_pin={"pcf8574": "io", "number": 0, "inverted": True})
    )


def test_response_conflicting_explicit_length_rejected(set_core_config):
    setup_esp32(set_core_config)
    with pytest.raises(cv.Invalid, match="length"):
        validate(
            instance(
                LIN0,
                responses=[{"lin_id": 18, "data": [1, 2]}],
                rx_ids=[{"lin_id": 18, "length": 4}],
            )
        )


@pytest.mark.parametrize("key", ["schedule", "rx_ids", "on_frame"])
def test_matching_explicit_lengths_and_responses_stay_valid(set_core_config, key):
    setup_esp32(set_core_config)
    hint = {"lin_id": 18, "length": 2}
    if key == "schedule":
        hint["interval"] = "100ms"
    if key == "on_frame":
        hint["then"] = []
    assert validate(
        instance(LIN0, responses=[{"lin_id": 18, "data": [1, 2]}], **{key: [hint]})
    )


def test_conflicting_length_hints_rejected(set_core_config):
    setup_esp32(set_core_config)
    with pytest.raises(cv.Invalid, match="conflicting length"):
        validate(
            instance(
                LIN0,
                schedule=[{"lin_id": 18, "interval": "100ms", "length": 2}],
                rx_ids=[{"lin_id": 18, "length": 4}],
            )
        )


def test_stock_uart_allocator_skips_logger_in_middle(
    set_core_config, set_component_config
):
    setup_esp32(set_core_config)
    from esphome.components.linbus import FINAL_VALIDATE_SCHEMA

    set_component_config("logger", {"hardware_uart": "UART1", "baud_rate": 115200})
    set_component_config("uart", [{"id": "first"}, {"id": "second"}])
    with pytest.raises(cv.Invalid, match="uart.*second"):
        FINAL_VALIDATE_SCHEMA(validate(instance(LIN0, uart_port="UART_NUM_2")))
