"""Schema/codegen tests for modify.signals, modify.e2e and can_gateway.set_signal
(cfg-v32 … cfg-v38).

The expected masks are hand-computed from the DBC bit numbering, not from the
code under test:
  Motorola, start (MSB) bit 4, 13 bits: byte 0 bits 4..0 hold the top 5 bits,
  byte 1 bits 7..0 the low 8 bits. raw 0x1234 -> byte 0 |= 0x12, byte 1 = 0x34.
  Intel, start (LSB) bit 12, 8 bits: byte 1 bits 7..4 = low nibble, byte 2
  bits 3..0 = high nibble. raw 0xA5 -> byte 1 |= 0x50, byte 2 |= 0x0A.
"""

from __future__ import annotations

import pytest

from esphome import config_validation as cv

from .common import gateway, route, setup_c6, validate

FULL = 0xFFFF_FFFF_FFFF_FFFF


def _rule(modify, **extra):
    rule = {"can_id": 0x1A0, "modify": modify}
    rule.update(extra)
    return rule


def _validated_rule(set_core_config, modify, **extra):
    setup_c6(set_core_config)
    config = validate(gateway(routes=[route(filters=[_rule(modify, **extra)])]))
    return config["routes"][0]["filters"][0]


# ---------------------------------------------------------------- cfg-v32 signals


def test_v32_motorola_signal_with_value_accepted(set_core_config) -> None:
    rule = _validated_rule(
        set_core_config,
        {"signals": [{"bit_offset": 4, "bit_length": 13, "byte_order": "big", "value": 0x1234}]},
    )
    assert rule["modify"]["signals"][0]["byte_order"] == "big"


def test_v32_signal_defaults_to_intel(set_core_config) -> None:
    rule = _validated_rule(
        set_core_config, {"signals": [{"bit_offset": 12, "bit_length": 8, "value": 1}]}
    )
    assert rule["modify"]["signals"][0]["byte_order"] == "little"


def test_v32_value_wider_than_field_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="does not fit"):
        _validated_rule(
            set_core_config, {"signals": [{"bit_offset": 0, "bit_length": 4, "value": 16}]}
        )


def test_v32_motorola_past_byte_7_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="past byte 7"):
        _validated_rule(
            set_core_config,
            {"signals": [{"bit_offset": 57, "bit_length": 4, "byte_order": "big", "value": 0}]},
        )


def test_v32_intel_past_bit_63_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="64-bit frame"):
        _validated_rule(
            set_core_config, {"signals": [{"bit_offset": 60, "bit_length": 8, "value": 0}]}
        )


# ---------------------------------------------------------------- cfg-v34 overlaps


def test_v34_signal_overlapping_data_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="patched by both"):
        _validated_rule(
            set_core_config,
            {
                "data": [{"index": 1, "value": 0, "mask": 0x01}],
                "signals": [{"bit_offset": 4, "bit_length": 13, "byte_order": "big", "value": 0}],
            },
        )


def test_v34_signals_overlapping_each_other_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="patched by both"):
        _validated_rule(
            set_core_config,
            {
                "signals": [
                    {"bit_offset": 0, "bit_length": 8, "value": 0},
                    {"bit_offset": 7, "bit_length": 2, "value": 0},
                ]
            },
        )


def test_v34_adjacent_signals_accepted(set_core_config) -> None:
    _validated_rule(
        set_core_config,
        {
            "data": [{"index": 0, "value": 0, "mask": 0xE0}],
            "signals": [{"bit_offset": 4, "bit_length": 13, "byte_order": "big", "value": 0}],
        },
    )


# ---------------------------------------------------------------- cfg-v36 id


def test_v36_valueless_signal_needs_rule_id(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="needs the rule's id"):
        _validated_rule(set_core_config, {"signals": [{"bit_offset": 0, "bit_length": 8}]})


def test_v36_valueless_signal_with_id_accepted(set_core_config) -> None:
    _validated_rule(
        set_core_config, {"signals": [{"bit_offset": 0, "bit_length": 8}]}, id="limit"
    )


# ---------------------------------------------------------------- cfg-v33/v35 e2e


def test_v33_e2e_defaults(set_core_config) -> None:
    rule = _validated_rule(
        set_core_config,
        {"signals": [{"bit_offset": 0, "bit_length": 8, "value": 1}], "e2e": {"crc_byte": 7}},
    )
    e2e = rule["modify"]["e2e"]
    assert (e2e["first_byte"], e2e["last_byte"], e2e["polynomial"]) == (0, 6, 0x1D)


def test_v33_crc_inside_range_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="inside the covered range"):
        _validated_rule(
            set_core_config,
            {
                "signals": [{"bit_offset": 8, "bit_length": 8, "value": 1}],
                "e2e": {"crc_byte": 0, "first_byte": 0, "last_byte": 6},
            },
        )


def test_v33_crc_byte_0_needs_range(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="explicit"):
        _validated_rule(
            set_core_config,
            {"signals": [{"bit_offset": 8, "bit_length": 8, "value": 1}], "e2e": {"crc_byte": 0}},
        )


def test_v33_crc_first_byte_explicit_accepted(set_core_config) -> None:
    rule = _validated_rule(
        set_core_config,
        {
            "signals": [{"bit_offset": 8, "bit_length": 8, "value": 1}],
            "e2e": {"crc_byte": 0, "first_byte": 1, "last_byte": 7},
        },
    )
    assert rule["modify"]["e2e"]["last_byte"] == 7


def test_v35_patched_crc_byte_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="also patched"):
        _validated_rule(
            set_core_config,
            {"data": [{"index": 7, "value": 0}], "e2e": {"crc_byte": 7}},
        )


def test_v35_e2e_without_payload_patch_rejected(set_core_config) -> None:
    with pytest.raises(cv.Invalid, match="add data or signals"):
        _validated_rule(set_core_config, {"can_id": 0x1A1, "e2e": {"crc_byte": 7}})


# ---------------------------------------------------------------- codegen masks


def test_codegen_masks_motorola(set_core_config) -> None:
    from esphome.components.can_gateway import _declared_mask, _packed_patch_masks

    rule = _validated_rule(
        set_core_config,
        {"signals": [{"bit_offset": 4, "bit_length": 13, "byte_order": "big", "value": 0x1234}]},
    )
    and_mask, or_value = _packed_patch_masks(rule["modify"])
    assert and_mask == FULL & ~0x0000_FF1F
    assert or_value == 0x3412
    assert _declared_mask(rule["modify"]) == 0xFF1F


def test_codegen_masks_intel_cross_byte(set_core_config) -> None:
    from esphome.components.can_gateway import _declared_mask, _packed_patch_masks

    rule = _validated_rule(
        set_core_config, {"signals": [{"bit_offset": 12, "bit_length": 8, "value": 0xA5}]}
    )
    and_mask, or_value = _packed_patch_masks(rule["modify"])
    assert and_mask == FULL & ~0x000F_F000
    assert or_value == 0x000A_5000
    assert _declared_mask(rule["modify"]) == 0x000F_F000


def test_codegen_valueless_signal_is_declared_but_identity(set_core_config) -> None:
    from esphome.components.can_gateway import _declared_mask, _packed_patch_masks

    rule = _validated_rule(
        set_core_config, {"signals": [{"bit_offset": 12, "bit_length": 8}]}, id="lim"
    )
    and_mask, or_value = _packed_patch_masks(rule["modify"])
    assert (and_mask, or_value) == (FULL, 0)
    assert _declared_mask(rule["modify"]) == 0x000F_F000


# ---------------------------------------------------------------- cfg-v37/v38 set_signal


def _set_signal_schema():
    from esphome.components.can_gateway import SET_SIGNAL_ACTION_SCHEMA

    return SET_SIGNAL_ACTION_SCHEMA


def _final_validate(gateway_config, actions, set_component_config):
    from esphome.components.can_gateway import FINAL_VALIDATE_SCHEMA

    set_component_config("can_gateway", gateway_config)
    set_component_config(
        "button", [{"platform": "template", "on_press": [{"then": actions}]}]
    )
    return FINAL_VALIDATE_SCHEMA(gateway_config)


def _signal_gateway(set_core_config):
    from esphome.components.can_gateway import CONFIG_SCHEMA

    setup_c6(set_core_config)
    return CONFIG_SCHEMA(
        gateway(
            routes=[
                route(
                    filters=[
                        {
                            "id": "current_limit",
                            "can_id": 0x1A0,
                            "modify": {
                                "signals": [{"bit_offset": 4, "bit_length": 13, "byte_order": "big"}],
                                "e2e": {"crc_byte": 7},
                            },
                        }
                    ]
                )
            ]
        )
    )


def test_v37_set_signal_defaults(set_core_config) -> None:
    setup_c6(set_core_config)
    action = _set_signal_schema()(
        {"id": "current_limit", "bit_offset": 4, "bit_length": 13, "byte_order": "big", "value": 12.5}
    )
    assert (action["factor"], action["offset"], action["signed"], action["enabled"]) == (
        1.0,
        0.0,
        False,
        True,
    )


def test_v37_zero_factor_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="factor"):
        _set_signal_schema()(
            {"id": "x", "bit_offset": 0, "bit_length": 8, "value": 1, "factor": 0}
        )


def test_v37_field_outside_frame_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="past byte 7"):
        _set_signal_schema()(
            {"id": "x", "bit_offset": 56, "bit_length": 2, "byte_order": "big", "value": 1}
        )


def test_v38_declared_field_accepted(set_core_config, set_component_config) -> None:
    config = _signal_gateway(set_core_config)
    action = _set_signal_schema()(
        {"id": "current_limit", "bit_offset": 4, "bit_length": 13, "byte_order": "big", "value": 1}
    )
    _final_validate(config, [{"can_gateway.set_signal": action}], set_component_config)


def test_v38_undeclared_field_rejected(set_core_config, set_component_config) -> None:
    config = _signal_gateway(set_core_config)
    # Same bits, wrong byte order: a different field, so it must not pass.
    action = _set_signal_schema()(
        {"id": "current_limit", "bit_offset": 4, "bit_length": 13, "value": 1}
    )
    with pytest.raises(cv.Invalid, match="does not declare the signal"):
        _final_validate(config, [{"can_gateway.set_signal": action}], set_component_config)


def test_v38_unknown_rule_rejected(set_core_config, set_component_config) -> None:
    config = _signal_gateway(set_core_config)
    action = _set_signal_schema()(
        {"id": "nope", "bit_offset": 4, "bit_length": 13, "byte_order": "big", "value": 1}
    )
    with pytest.raises(cv.Invalid, match="not an updatable"):
        _final_validate(config, [{"can_gateway.set_signal": action}], set_component_config)
