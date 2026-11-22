"""Every component rejects unsupported platforms/frameworks at schema time."""

import importlib

import pytest

import esphome.config_validation as cv
from esphome.const import PlatformFramework

COMPONENTS = ("can_gateway", "ccp", "isotp", "linbus", "portal", "sd_logger", "uds")


@pytest.mark.parametrize("component", COMPONENTS)
@pytest.mark.parametrize(
    ("platform", "error"),
    [
        (PlatformFramework.ESP8266_ARDUINO, "only available on.*esp32"),
        (
            PlatformFramework.ESP32_ARDUINO,
            r"only available with framework\(s\) esp-idf",
        ),
    ],
)
def test_platform_rejected(set_core_config, component, platform, error):
    set_core_config(platform)
    schema = importlib.import_module(f"esphome.components.{component}").CONFIG_SCHEMA
    # portal already places its platform guards after structural validation.
    raw = (
        {
            "build": "demo",
            "tree": [{"segment": "demo", "kind": "device", "label": "Demo"}],
        }
        if component == "portal"
        else {}
    )
    with pytest.raises(cv.Invalid, match=error):
        schema(raw)
