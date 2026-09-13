"""Shared helpers for the `uds` codegen suites (test_schema, test_bindings, test_gates).

Kept apart from `common.py` on purpose: that module loads the catalog library by file location so
the byte-level suites need no ESPHome core at all, while everything here imports
`esphome.components.uds` through the path hook in `tests/conftest.py` and therefore does.

`CORE.config_path` is set to `tests/uds/dummy.yaml` by the autouse fixture in conftest, so a
`file:` in these configs resolves relative to `tests/uds/` — which is why the fixtures are named
`fixtures/mini.dcat` rather than by absolute path.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from esphome.components.esp32.const import KEY_BOARD, KEY_VARIANT, VARIANT_ESP32C6
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework
import esphome.final_validate as fv

MINI = "fixtures/mini.dcat"
BMS = "../../private/catalogs/bms-live.dcat"

MINI_PATH = Path(__file__).resolve().parent / "fixtures" / "mini.dcat"
BMS_PATH = Path(__file__).resolve().parent.parent.parent / "private" / "catalogs" / "bms-live.dcat"

HUB_ID = "bms"
ISOTP_ID = "tp"


def setup_c6(set_core_config) -> None:
    """Set the core up as an ESP32-C6 / IDF target, the way the isotp suites do."""
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={KEY_BOARD: "esp32-c6-devkitc-1", KEY_VARIANT: VARIANT_ESP32C6},
    )


def catalog(file: str = MINI, **overrides: Any) -> dict[str, Any]:
    """A `catalog:` block. `embed: true` by default — the source that needs nothing flashed."""
    block = {"file": file, "embed": True, **overrides}
    return {key: value for key, value in block.items() if value is not None}


def partition_catalog(file: str = MINI, **overrides: Any) -> dict[str, Any]:
    block = {"file": file, "partition": "diag", **overrides}
    return {key: value for key, value in block.items() if value is not None}


def hub(**overrides: Any) -> dict[str, Any]:
    """A raw (unvalidated) `uds:` entry, ready for CONFIG_SCHEMA."""
    config = {"id": HUB_ID, "isotp_id": ISOTP_ID, "catalog": catalog(), **overrides}
    return {key: value for key, value in config.items() if value is not None}


def validate(config: dict[str, Any]):
    """Run the hub's CONFIG_SCHEMA (imported late, after the core is set up)."""
    from esphome.components.uds import CONFIG_SCHEMA

    return CONFIG_SCHEMA(config)


def validate_sensor(config: dict[str, Any]):
    from esphome.components.uds.sensor import CONFIG_SCHEMA

    return CONFIG_SCHEMA(config)


def validate_text_sensor(config: dict[str, Any]):
    from esphome.components.uds.text_sensor import CONFIG_SCHEMA

    return CONFIG_SCHEMA(config)


def sensor_entry(**overrides: Any) -> dict[str, Any]:
    """A platform entry WITHOUT `platform:`, which ESPHome strips before the platform schema runs.

    The full config keeps the key, so `bound_entity_configs()` can find these entries; that
    asymmetry is real and worth encoding here rather than in every test.
    """
    entry = {"uds_id": HUB_ID, "name": "x", **overrides}
    return {key: value for key, value in entry.items() if value is not None}


def isotp_entry(**overrides: Any) -> dict[str, Any]:
    """A validated-shaped isotp block: only the keys §3.1's check reads.

    `id` is an `ID` object, not a string, because that is what `cv.declare_id` leaves in a real
    config — and `ID.__eq__` returns NotImplemented against a str, so a string here would silently
    never match the hub's `use_id` reference and every cross-block check would vacuously pass.
    """
    from esphome.core import ID, TimePeriodMilliseconds

    entry = {
        "id": ID(ISOTP_ID),
        "port_id": "diag_bus",
        "tx_id": 0x7E7,
        "rx_id": 0x7EF,
        "max_message_size": 512,
        "block_size": 8,
        "st_min": 20,
        "n_cr_timeout": TimePeriodMilliseconds(milliseconds=2500),
    }
    entry.update(overrides)
    if isinstance(entry["id"], str):
        entry["id"] = ID(entry["id"])
    return entry


def full_config(
    hubs: list[dict[str, Any]],
    *,
    isotp: list[dict[str, Any]] | None = None,
    sensors: list[dict[str, Any]] | None = None,
    text_sensors: list[dict[str, Any]] | None = None,
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    config: dict[str, Any] = {"uds": hubs, "isotp": isotp if isotp is not None else [isotp_entry()]}
    if sensors is not None:
        config["sensor"] = sensors
    if text_sensors is not None:
        config["text_sensor"] = text_sensors
    if extra:
        config.update(extra)
    return config


def run_hub_final_validate(hub_config: dict[str, Any], **kwargs):
    """Invoke the hub's FINAL_VALIDATE_SCHEMA with a populated full-config context."""
    from esphome.components.uds import _final_validate

    token = fv.full_config.set(full_config([hub_config], **kwargs))
    try:
        return _final_validate(hub_config)
    finally:
        fv.full_config.reset(token)


def run_entity_final_validate(entity_config: dict[str, Any], hub_config: dict[str, Any], *, text: bool = False):
    """Invoke a platform's FINAL_VALIDATE_SCHEMA — where `field:` is actually resolved."""
    if text:
        from esphome.components.uds.text_sensor import _final_validate
    else:
        from esphome.components.uds.sensor import _final_validate

    key = "text_sensors" if text else "sensors"
    listed = {**entity_config, "platform": "uds"}
    token = fv.full_config.set(full_config([hub_config], **{key: [listed]}))
    try:
        return _final_validate(entity_config)
    finally:
        fv.full_config.reset(token)


def bind(field: str, *, text: bool = False, hub_file: str = MINI, **overrides: Any):
    """Validate one bound entity end to end: platform schema, then final validation.

    Returns the resolved config, so a test can assert what codegen will emit.
    """
    entry = sensor_entry(field=field, **overrides)
    validated = validate_text_sensor(entry) if text else validate_sensor(entry)
    hub_config = validate(hub(catalog=catalog(hub_file)))
    return run_entity_final_validate(validated, hub_config, text=text)
