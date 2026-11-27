"""Shared helpers for the diag_server config and table validation suites.

Expectations come from docs/diag_server-spec.md. The data is fictional: fixture tables are built in
``tmp_path`` per test, and the CAN IDs are an arbitrary request/reply pair.

``refusal()`` runs the whole config-time pipeline (block schema, then the final validation that
reads the table and cross-checks the isotp instance) and hands back the error text, so a test
states *what* is refused without caring which stage refuses it.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
from typing import Any

from esphome.components.esp32.const import KEY_BOARD, KEY_VARIANT, VARIANT_ESP32C6
from esphome.config import Config
import esphome.config_validation as cv
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework
import esphome.final_validate as fv

ISOTP_ID = "iso_engine"
SERVER_ID = "sim_engine"
REQ_ID = 0x7E0  # the tester's request ID: the isotp instance's rx_id
RESP_ID = 0x7E8  # the ECU's reply ID: the isotp instance's tx_id
UDS_FIXTURE = Path(__file__).resolve().parent.parent / "uds" / "fixtures" / "mini.dcat"


def setup_c6(set_core_config) -> None:
    set_core_config(
        PlatformFramework.ESP32_IDF,
        core_data={KEY_FRAMEWORK_VERSION: cv.Version(5, 5, 4)},
        platform_data={
            KEY_BOARD: "esp32-c6-devkitc-1",
            KEY_VARIANT: VARIANT_ESP32C6,
        },
    )


def entry(req: str, resp: str, **extra: Any) -> dict[str, Any]:
    return {"req": req, "resp": resp, **extra}


def ecu(entries: list[dict[str, Any]], **overrides: Any) -> dict[str, Any]:
    block = {
        "name": "engine",
        "req": f"0x{REQ_ID:X}",
        "resp": f"0x{RESP_ID:X}",
        "entries": entries,
    }
    block.update(overrides)
    return {key: value for key, value in block.items() if value is not None}


def table_doc(
    entries: list[dict[str, Any]] | None = None, **overrides: Any
) -> dict[str, Any]:
    if entries is None:
        entries = [entry("22 F1 90", "62 F1 90 41 42 43")]
    doc = {"format": "diag-sim/1", "ecus": [ecu(entries)]}
    doc.update(overrides)
    return {key: value for key, value in doc.items() if value is not None}


def write_table(tmp_path: Path, doc: Any, name: str = "table.json") -> str:
    path = tmp_path / name
    path.write_text(doc if isinstance(doc, str) else json.dumps(doc), encoding="utf-8")
    return str(path)


def hex_bytes(count: int, fill: str = "AB") -> str:
    return " ".join([fill] * count)


def isotp_block(**overrides: Any) -> dict[str, Any]:
    """A validated isotp block as the spec swaps it: tx_id is the ECU reply, rx_id the request."""
    from esphome.components.isotp import CONFIG_SCHEMA

    raw = {
        "id": ISOTP_ID,
        "port_id": "sim_bus",
        "tx_id": RESP_ID,
        "rx_id": REQ_ID,
        **overrides,
    }
    return CONFIG_SCHEMA({k: v for k, v in raw.items() if v is not None})


def server(table: str, **overrides: Any) -> dict[str, Any]:
    """A raw diag_server block."""
    raw = {
        "id": SERVER_ID,
        "isotp_id": ISOTP_ID,
        "table": table,
        "ecu": "engine",
        **overrides,
    }
    return {key: value for key, value in raw.items() if value is not None}


def validate(config: dict[str, Any]):
    from esphome.components.diag_server import CONFIG_SCHEMA

    return CONFIG_SCHEMA(config)


def uds_hub(**overrides: Any) -> dict[str, Any]:
    """A validated uds hub on the fixture isotp instance (any consumer would do)."""
    from esphome.components.uds import CONFIG_SCHEMA

    raw = {
        "id": "bench_hub",
        "isotp_id": ISOTP_ID,
        "catalog": {"file": str(UDS_FIXTURE), "embed": True},
        **overrides,
    }
    return CONFIG_SCHEMA(raw)


def full_config(
    servers: list[dict[str, Any]],
    *,
    isotps: list[dict[str, Any]] | None = None,
    hubs: list[dict[str, Any]] | None = None,
) -> Config:
    """A real esphome Config, so ``get_declaring_config`` resolves ids as it does in a build."""
    config = Config()
    isotps = [isotp_block()] if isotps is None else isotps
    config["isotp"] = isotps
    config["diag_server"] = servers
    if hubs:
        config["uds"] = hubs
    for index, block in enumerate(isotps):
        config.declare_ids.append((block["id"], ["isotp", index, "id"]))
    return config


def final_validate(server_config: dict[str, Any], config: Config):
    from esphome.components.diag_server import _final_validate

    token = fv.full_config.set(config)
    try:
        return _final_validate(server_config)
    finally:
        fv.full_config.reset(token)


def accept(tmp_path: Path, doc: Any, *, isotp: dict[str, Any] | None = None, **cfg):
    """Run the full pipeline for one server and return its final config."""
    block = validate(server(write_table(tmp_path, doc), **cfg))
    return final_validate(
        block, full_config([block], isotps=None if isotp is None else [isotp])
    )


def refusal(
    tmp_path: Path, doc: Any, *, isotp: dict[str, Any] | None = None, **cfg
) -> str:
    """The error text the pipeline gives for this table/config. Fails the test if none is raised."""
    try:
        accept(tmp_path, doc, isotp=isotp, **cfg)
    except cv.Invalid as err:
        return str(err)
    raise AssertionError("expected the config to be refused, but it validated")


def names_number(message: str, value: int) -> bool:
    """True if the message gives ``value`` as a standalone decimal or as hex (0x7E0, 7E0)."""
    decimal = rf"(?<![0-9A-Za-z]){value}(?![0-9A-Za-z])"
    hexa = rf"(?i:(?<![0-9A-Za-z])(0x)?{value:X}(?![0-9A-Za-z]))"
    return bool(re.search(decimal, message) or re.search(hexa, message))


def names_size(message: str, value: int) -> bool:
    """True if the message gives ``value`` as a standalone decimal number."""
    return bool(re.search(rf"(?<![0-9A-Za-z.]){value}(?![0-9A-Za-z])", message))
