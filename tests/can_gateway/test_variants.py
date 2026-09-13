"""can_gateway across ESP32 variants.

The component was gated to the ESP32-C6 because that is the chip with two TWAI
controllers and therefore the only one that can *bridge*. But a single-controller
chip is a perfectly good single-bus monitor, and the component is written against
the generic esp_driver_twai node API — so the gate is now per-variant controller
count rather than a single allowed variant.

The rule these tests pin down: a port needs a hardware controller to live on, and
that must be said at validation time. Without it a two-port config on a
single-TWAI chip validates cleanly and then fails at boot inside
`twai_new_node_onchip` with `ESP_ERR_NOT_FOUND`, on a bench board nobody is
watching.
"""

from __future__ import annotations

from typing import Any

import pytest

from esphome.components.esp32.const import (
    KEY_BOARD,
    KEY_VARIANT,
    VARIANT_ESP32,
    VARIANT_ESP32C2,
    VARIANT_ESP32C3,
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    VARIANT_ESP32P4,
    VARIANT_ESP32S3,
)
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework

from .common import gateway, setup_c6, validate

# Mr. Purple's native TWAI pins, plus a second pair that is also legal on the
# classic ESP32 (GPIO6-11 are flash and rejected there, so PORT_B's C6 pins
# cannot be reused).
ESP32_PORT_A: dict[str, Any] = {
    "id": "port_a",
    "tx_pin": "GPIO26",
    "rx_pin": "GPIO25",
    "bit_rate": "125kbps",
}
ESP32_PORT_B: dict[str, Any] = {
    "id": "port_b",
    "tx_pin": "GPIO17",
    "rx_pin": "GPIO16",
    "bit_rate": "125kbps",
}

# GPIO2-5 exist and are freely usable on every variant exercised below; the
# higher numbers are not (GPIO26 is SPI flash on the S3, SPI/PSRAM on the C6,
# and GPIO6-11 are flash on the classic ESP32).
GENERIC_PORT: dict[str, Any] = {
    "id": "bus",
    "tx_pin": "GPIO2",
    "rx_pin": "GPIO3",
    "bit_rate": "125kbps",
}
GENERIC_PORT_B: dict[str, Any] = {
    "id": "bus_b",
    "tx_pin": "GPIO4",
    "rx_pin": "GPIO5",
    "bit_rate": "125kbps",
}

SINGLE_CONTROLLER = [VARIANT_ESP32, VARIANT_ESP32S3, VARIANT_ESP32C3, VARIANT_ESP32H2]


def setup_variant(set_core_config, variant: str) -> None:
    """Set the core up as an arbitrary ESP32 variant on IDF."""
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={KEY_BOARD: "dummy", KEY_VARIANT: variant},
    )


# ------------------------------------------------------------------ acceptance


@pytest.mark.parametrize("variant", SINGLE_CONTROLLER)
def test_single_port_accepted_on_single_controller_chips(
    set_core_config, variant: str
) -> None:
    """One bus, no routes — the monitor/node shape every TWAI chip can run."""
    setup_variant(set_core_config, variant)

    validated = validate(gateway(ports=[dict(GENERIC_PORT)], routes=[]))

    assert len(validated["ports"]) == 1


def test_two_ports_still_accepted_on_c6(set_core_config) -> None:
    """The bridging case must not regress — this is what the bench runs."""
    setup_c6(set_core_config)

    validated = validate(gateway())

    assert len(validated["ports"]) == 2


def test_single_port_accepted_on_esp32_with_real_pins(set_core_config) -> None:
    """Mr. Purple's P4 config: native TWAI on GPIO26/25, single bus."""
    setup_variant(set_core_config, VARIANT_ESP32)

    validated = validate(gateway(ports=[dict(ESP32_PORT_A)], routes=[]))

    assert validated["ports"][0]["tx_pin"] == 26


# ------------------------------------------------------------------- rejection


@pytest.mark.parametrize("variant", SINGLE_CONTROLLER)
def test_two_ports_rejected_on_single_controller_chips(
    set_core_config, variant: str
) -> None:
    """The whole point of W3: catch it here, not at boot."""
    setup_variant(set_core_config, variant)

    with pytest.raises(cv.Invalid) as excinfo:
        validate(
            gateway(
                ports=[dict(GENERIC_PORT), dict(GENERIC_PORT_B)],
                routes=[{"from": "bus", "to": "bus_b"}],
            )
        )

    message = str(excinfo.value)
    assert "2 ports configured" in message
    assert "1 on-chip TWAI controller" in message
    assert variant in message


def test_rejection_names_the_variant_not_a_generic_error(set_core_config) -> None:
    """A message that names the chip is the difference between a fix and a hunt."""
    setup_variant(set_core_config, VARIANT_ESP32)

    with pytest.raises(cv.Invalid, match=r"the ESP32 has 1 on-chip TWAI controller"):
        validate(gateway(ports=[dict(ESP32_PORT_A), dict(ESP32_PORT_B)]))


def test_variant_without_twai_rejected(set_core_config) -> None:
    """The C2 has no TWAI at all — refused by only_on_variant, before ports."""
    setup_variant(set_core_config, VARIANT_ESP32C2)

    with pytest.raises(cv.Invalid, match=r"can_gateway"):
        validate(gateway(ports=[dict(GENERIC_PORT)], routes=[]))


# (The >2-port cap is already covered on the C6 by test_single_bus.py::
# test_v17_three_ports_rejected and test_gateway_schema.py::
# test_v01_three_ports_rejected — not duplicated here.)


# ----------------------------------------------------------------- the table


def test_p4_is_capped_by_the_component_not_the_silicon(set_core_config) -> None:
    """The P4 has three controllers; the data plane handles two.

    Guards against someone widening MAX_PORTS by editing the table alone —
    `route_for[2]` in CanGateway::setup would overflow.
    """
    setup_variant(set_core_config, VARIANT_ESP32P4)

    from esphome.components.can_gateway import (
        MAX_PORTS,
        TWAI_CONTROLLERS,
        _ports_for_variant,
    )

    assert TWAI_CONTROLLERS[VARIANT_ESP32P4] == 3
    assert _ports_for_variant(VARIANT_ESP32P4) == MAX_PORTS == 2


def test_unknown_variant_reports_zero_ports(set_core_config) -> None:
    """A variant absent from the table must not silently allow a port."""
    setup_variant(set_core_config, VARIANT_ESP32C2)

    from esphome.components.can_gateway import _ports_for_variant

    assert _ports_for_variant(VARIANT_ESP32C2) == 0


# ------------------------------------------------- V27: unverified HAL variant


def test_v27_c5_rejected_for_unverified_hal(set_core_config) -> None:
    """The C5 builds twai_hal_v2.c, where a bus-off-halted frame still completes.

    OutstandingTracker's eager reclaim assumes it never does, so the slot would
    be freed while the driver still owns it — a stale frame on a vehicle bus
    after recovery, then a double release. can_gateway.cpp has an #error for the
    same condition; this keeps the failure at config time where it is readable.
    """
    setup_variant(set_core_config, VARIANT_ESP32C5)

    with pytest.raises(cv.Invalid, match=r"the ESP32C5 is not supported"):
        validate(gateway(ports=[dict(GENERIC_PORT)], routes=[]))


def test_v27_rejection_names_the_reason_and_the_handover(set_core_config) -> None:
    """A bare "not supported" would read as an oversight rather than a hazard."""
    setup_variant(set_core_config, VARIANT_ESP32C5)

    with pytest.raises(cv.Invalid, match=r"TWAI-FD HAL.*HANDOVER\.md Todo 4"):
        validate(gateway(ports=[dict(GENERIC_PORT)], routes=[]))


def test_v27_does_not_reject_verified_two_controller_variants(set_core_config) -> None:
    """The guard must be C5-specific: the C6 is the same port count, verified."""
    setup_variant(set_core_config, VARIANT_ESP32C6)

    validate(gateway(ports=[dict(GENERIC_PORT)], routes=[]))

    from esphome.components.can_gateway import UNVERIFIED_HAL_VARIANTS

    assert VARIANT_ESP32C6 not in UNVERIFIED_HAL_VARIANTS
    assert VARIANT_ESP32C5 in UNVERIFIED_HAL_VARIANTS
