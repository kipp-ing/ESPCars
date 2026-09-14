# CCP schema, action-registration, and externally-emitted define coverage.
"""CCP hub schema, action registration, and externally-emitted observe/write defines."""
from __future__ import annotations

import ast
import inspect

import pytest
from esphome import automation, config_validation as cv

from .common import hub, setup_c6, validate


def test_defaults_are_observe_first_and_little_endian(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub())
    assert config["command_id"] == 0x700
    assert config["response_id"] == 0x701
    assert config["station_address"] == 0x0001
    assert config["byte_order"] == "little"
    assert config["allow_write"] is False
    assert config["response_timeout"].total_milliseconds == 100


@pytest.mark.parametrize("key", ["id", "can_gateway_id"])
def test_required_keys_rejected(set_core_config, key: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="required key not provided"):
        validate(hub(**{key: None}))


@pytest.mark.parametrize("key", ["command_id", "response_id"])
@pytest.mark.parametrize("value", [-1, 0x800, 0x123456])
def test_extended_or_bad_ids_rejected(set_core_config, key: str, value: int) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(hub(**{key: value}))


@pytest.mark.parametrize("value", ["network", "Intel", 3])
def test_invalid_byte_order_rejected(set_core_config, value) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(hub(byte_order=value))


def test_actions_are_registered() -> None:
    expected = {"ccp.connect", "ccp.disconnect", "ccp.get_version", "ccp.exchange_id", "ccp.set_mta", "ccp.upload", "ccp.short_upload", "ccp.download", "ccp.select_cal_page", "ccp.read_memory", "ccp.write_memory", "ccp.start_daq", "ccp.stop_daq", "ccp.daq_read", "ccp.raw", "ccp.set_enabled"}
    assert expected <= set(automation.ACTION_REGISTRY)


def test_codegen_emits_observe_and_configured_write_defines() -> None:
    import esphome.components.ccp as ccp

    tree = ast.parse(inspect.getsource(ccp))
    fn = next(node for node in tree.body if isinstance(node, ast.AsyncFunctionDef) and node.name == "to_code")
    defines = [node.args[0].value for node in ast.walk(fn) if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "add_define" and node.args and isinstance(node.args[0], ast.Constant)]
    assert "USE_CAN_GATEWAY_OBSERVE" in defines
    assert "USE_CCP_WRITE" in defines


def test_daq_anchor_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(
        hub(
            daq_anchor={
                "address": 0x10203050,
                "ext": 0,
                "expect": [0x01, 0xAD, 0x04, 0x02],
            }
        )
    )
    assert config["daq_anchor"]["address"] == 0x10203050
    assert config["daq_anchor"]["expect"] == [0x01, 0xAD, 0x04, 0x02]


@pytest.mark.parametrize(
    ("anchor", "message"),
    [
        ({"address": 0x10203050, "expect": []}, "length"),
        ({"expect": [1]}, "required key not provided"),
    ],
)
def test_daq_anchor_rejections(set_core_config, anchor, message: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match=message):
        validate(hub(daq_anchor=anchor))


def _daq_read_schema():
    return automation.ACTION_REGISTRY["ccp.daq_read"].schema


def test_daq_read_action_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = _daq_read_schema()(
        {
            "id": "bms_ccp",
            "fields": [
                {"address": 0x10203060, "size": 1},
                {"address": 0x10203070, "size": 2, "ext": 0},
            ],
            "samples": 30,
            "prescaler": 10,
            "require_anchor": True,
        }
    )
    assert config["dto_id"] == 0x702
    assert config["fields"][0]["ext"] == 0


@pytest.mark.parametrize(
    ("fields", "message"),
    [
        ([], "length"),
        ([{"address": 0x10203060, "size": 0}], "at least 1"),
        ([{"address": 0x10203060, "size": 8}], "at most 7"),
        (
            [
                {"address": 0x10203060, "size": 4},
                {"address": 0x10203070, "size": 4},
            ],
            "total 8 bytes",
        ),
    ],
)
def test_daq_read_action_rejections(set_core_config, fields, message: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match=message):
        _daq_read_schema()({"id": "bms_ccp", "fields": fields})
