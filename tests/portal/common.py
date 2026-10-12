"""Shared helpers for the portal test suites (docs/portal-spec.md).

Everything here is fictional: a made-up vehicle ("demo"), locally administered MACs of the
form ``02:00:00:00:00:xx``, no real bus names. Nothing imports the component's internals
beyond its public names (``CONFIG_SCHEMA``, ``FINAL_VALIDATE_SCHEMA``, the two
``register_*_provider`` functions of spec section 4.6).
"""

from __future__ import annotations

import copy
import os
from pathlib import Path
import subprocess
import sys
import textwrap
from typing import Any

from esphome.components.esp32.const import KEY_BOARD, KEY_VARIANT, VARIANT_ESP32C6
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework
from esphome.core import CORE

HERE = Path(__file__).parent
FIXTURES = HERE / "fixtures"
REPO = HERE.parent.parent
COMPONENT = REPO / "components" / "portal"
BUILD_DIR = REPO / "tests" / "build" / "portal"


class _ExplicitNull:
    """Keep this key with the YAML value ``null``. A plain ``None`` override means "delete
    this key" throughout this module; spec 4.1/4.4/4.5 need both."""

    def __repr__(self) -> str:  # pragma: no cover
        return "NULL"


NULL = _ExplicitNull()


def _finalize(d: dict[str, Any]) -> dict[str, Any]:
    return {k: (None if v is NULL else v) for k, v in d.items() if v is not None}


BASE_VALUE: dict[str, Any] = {
    "key": "vcc_measured",
    "label": "VCC measured",
    "unit": "V",
    "decimals": 3,
    "sensor": "vcc_measured_sensor",
}

BASE_TREE: list[dict[str, Any]] = [
    {
        "segment": "ecu",
        "kind": "ecu",
        "label": "ECU",
        "children": [
            {
                "segment": "unit",
                "kind": "module",
                "label": "Unit",
                "role": "role_a",
                "roles_available": ["role_a"],
                "values": [dict(BASE_VALUE)],
            }
        ],
    }
]

BASE: dict[str, Any] = {"id": "ui", "build": "demo-1", "tree": BASE_TREE}


def setup_c6(set_core_config) -> None:
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={KEY_BOARD: "esp32-c6-devkitc-1", KEY_VARIANT: VARIANT_ESP32C6},
    )


def schema():
    from esphome.components.portal import CONFIG_SCHEMA

    return CONFIG_SCHEMA


def validate(value: dict[str, Any]):
    return schema()(value)


def portal(**overrides: Any) -> dict[str, Any]:
    """A minimal valid ``portal:`` block; ``None`` deletes a key, ``NULL`` keeps an explicit null."""
    config = copy.deepcopy(BASE)
    config.update(overrides)
    return _finalize(config)


def value_entry(**overrides: Any) -> dict[str, Any]:
    entry = dict(BASE_VALUE)
    entry.update(overrides)
    return _finalize(entry)


def tree_node(**overrides: Any) -> dict[str, Any]:
    node = {"segment": "extra", "kind": "component", "label": "Extra"}
    node.update(overrides)
    return _finalize(node)


BASE_SERIES: dict[str, Any] = {
    "key": "cells",
    "label": "Cell voltages",
    "unit": "mV",
    "index_base": 1,
    "index_frame": "submodule_cell",
    "count": 5,
}


def series_entry(**overrides: Any) -> dict[str, Any]:
    entry = dict(BASE_SERIES)
    entry.update(overrides)
    return _finalize(entry)


ACTION_ENTITY = {
    "switch": "demo_switch",
    "button": "demo_button",
    "select": "demo_select",
    "number": "demo_number",
}


def action_entry(control: str = "switch", **overrides: Any) -> dict[str, Any]:
    entry: dict[str, Any] = {"key": f"act_{control}", "label": control.title(), control: ACTION_ENTITY[control]}
    entry.update(overrides)
    return _finalize(entry)


AUTH = {"username": "demo", "password": "demo-pass"}

# Full-config fixture preamble (esphome config goes through the real loader).
YAML_PREAMBLE = """\
esphome:
  name: portal-test
esp32:
  board: esp32-c6-devkitc-1
  variant: esp32c6
  framework:
    type: esp-idf
external_components:
  - source:
      type: local
      path: {components}
{extra_components}
"""


def full_yaml(body: str, *, extra_components: str = "") -> str:
    head = YAML_PREAMBLE.format(components=(REPO / "components").as_posix(), extra_components=extra_components)
    return head + textwrap.dedent(body)


def _loaded(path: Path):
    from esphome import config as esphome_config

    CORE.config_path = path
    return esphome_config.load_config({})


def config_errors(fixture: str) -> list[str]:
    """Load a fixture from ``fixtures/`` through ESPHome's own loader; the rules that need the
    whole assembled config (owns, submodule positions, web_server exclusion) live in
    FINAL_VALIDATE_SCHEMA and CONFIG_SCHEMA(dict) never reaches them."""
    return [str(e) for e in _loaded(FIXTURES / fixture).errors]


def load_config(fixture: str) -> dict[str, Any]:
    result = _loaded(FIXTURES / fixture)
    assert not result.errors, f"{fixture} failed to load: {[str(e) for e in result.errors]}"
    return result["portal"]


def yaml_errors(tmp_path: Path, body: str, **kw: Any) -> list[str]:
    p = tmp_path / "t.yaml"
    p.write_text(full_yaml(body, **kw))
    return [str(e) for e in _loaded(p).errors]


def yaml_config(tmp_path: Path, body: str, **kw: Any) -> dict[str, Any]:
    p = tmp_path / "t.yaml"
    p.write_text(full_yaml(body, **kw))
    result = _loaded(p)
    assert not result.errors, [str(e) for e in result.errors]
    return result["portal"]


class Generated:
    """Result of ``esphome compile --only-generate`` (seconds, no toolchain)."""

    def __init__(self, root: Path, name: str, returncode: int, output: str) -> None:
        self.root = root
        self.returncode = returncode
        self.output = output
        self.build = root / ".esphome" / "build" / name

    @property
    def main_cpp(self) -> str:
        return (self.build / "src" / "main.cpp").read_text()

    @property
    def defines_h(self) -> str:
        return (self.build / "src" / "esphome" / "core" / "defines.h").read_text()


def generate(tmp_path: Path, yaml_text: str, name: str = "portal-test") -> Generated:
    """Write ``yaml_text`` next to a fresh build dir and generate the C++ without compiling."""
    yaml_path = tmp_path / "gen.yaml"
    yaml_path.write_text(yaml_text)
    env = dict(os.environ)
    proc = subprocess.run(
        [sys.executable, "-m", "esphome", "compile", "--only-generate", str(yaml_path)],
        capture_output=True,
        text=True,
        cwd=tmp_path,
        env=env,
        timeout=300,
    )
    return Generated(tmp_path, name, proc.returncode, proc.stdout + proc.stderr)


# Entities the generated configs need (template platforms, nothing on any bus).
ENTITIES_YAML = """\
sensor:
  - platform: template
    id: vcc_measured_sensor
    lambda: "return 1.0;"
  - platform: template
    id: b_sensor
    lambda: "return 2.0;"
text_sensor:
  - platform: template
    id: state_text
binary_sensor:
  - platform: template
    id: link_up
switch:
  - platform: template
    id: demo_switch
    optimistic: true
button:
  - platform: template
    id: demo_button
select:
  - platform: template
    id: demo_select
    options: [one, two]
    optimistic: true
number:
  - platform: template
    id: demo_number
    min_value: 0
    max_value: 10
    step: 1
    optimistic: true
"""


def snapshot_registry():
    """Snapshot the provider registry (whatever dict(s) the component keeps) so a test that
    registers a provider does not leak it. Implementation-agnostic: any module-level dict whose
    name contains PROVIDER."""
    import esphome.components.portal as mod

    saved = {}
    for name, val in vars(mod).items():
        if isinstance(val, dict) and "PROVIDER" in name.upper():
            saved[name] = (val, dict(val))
    return saved


def restore_registry(saved) -> None:
    for _name, (obj, copy_) in saved.items():
        obj.clear()
        obj.update(copy_)




def portal_yaml(cfg: dict[str, Any] | None = None, *, extra: str = "", with_entities: bool = True, extra_components: str = "") -> str:
    """A complete loadable config: preamble, logger, wifi (STA only), template entities, extra
    top-level YAML, and ``portal:`` dumped from ``cfg`` (default ``portal()``)."""
    import yaml

    cfg = portal() if cfg is None else cfg
    body = "logger:\nwifi:\n  ssid: demo-net\n  password: demo-pass-123\n"
    if with_entities:
        body += ENTITIES_YAML
    body += extra
    text = full_yaml(body, extra_components=extra_components)
    return text + "portal:\n" + textwrap.indent(yaml.safe_dump(cfg, sort_keys=False), "  ")


def errors_of(tmp_path: Path, text: str) -> list[str]:
    p = tmp_path / "t.yaml"
    p.write_text(text)
    return [str(e) for e in _loaded(p).errors]


def config_of(tmp_path: Path, text: str) -> dict[str, Any]:
    p = tmp_path / "t.yaml"
    p.write_text(text)
    result = _loaded(p)
    assert not result.errors, [str(e) for e in result.errors]
    return result["portal"]
