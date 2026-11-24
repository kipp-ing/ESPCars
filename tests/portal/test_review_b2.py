"""Regressions for portal review rows po#5/6/7/8/10/11."""

import math
from pathlib import Path

import pytest

import esphome.config_validation as cv

from .common import (
    action_entry,
    config_of,
    errors_of,
    generate,
    portal,
    portal_yaml,
    series_entry,
    setup_c6,
    tree_node,
    validate,
    value_entry,
)

U32_MAX = 2**32 - 1
I32_MAX = 2**31 - 1
F32_MAX = float.fromhex("0x1.fffffep127")


@pytest.fixture(autouse=True)
def _core(set_core_config):
    setup_c6(set_core_config)


def test_reserved_root_id():
    with pytest.raises(cv.Invalid, match="car.*reserved"):
        validate(portal(tree=[tree_node(segment="car")]))
    validate(portal(tree=[tree_node(children=[tree_node(segment="car")])]))


def test_duplicate_value_keys():
    with pytest.raises(cv.Invalid, match="duplicate value key.*vcc_measured"):
        validate(portal(tree=[tree_node(values=[value_entry(), value_entry()])]))
    validate(portal(tree=[tree_node(values=[value_entry(), value_entry(key="other")])]))


@pytest.mark.parametrize(
    "limit",
    [
        math.nan,
        math.inf,
        -math.inf,
        1e40,
        -1e40,
        1e-50,
        math.nextafter(F32_MAX, math.inf),
    ],
)
def test_limit_must_fit_finite_float32(limit):
    with pytest.raises(cv.Invalid, match="finite.*float32"):
        validate(portal(tree=[tree_node(values=[value_entry(warn={"above": limit})])]))


@pytest.mark.parametrize("limit", [0, -1.25, F32_MAX, -F32_MAX, 2**-149])
def test_finite_float32_limits_accepted(limit):
    validate(portal(tree=[tree_node(values=[value_entry(bad={"below": limit})])]))


def test_budget_uint32_boundary():
    validate(portal(tree=[tree_node(budget_ms=U32_MAX)]))
    with pytest.raises(cv.Invalid, match=str(U32_MAX)):
        validate(portal(tree=[tree_node(budget_ms=U32_MAX + 1)]))


def test_cadence_and_derived_budget_boundaries():
    cfg = validate(
        portal(tree=[tree_node(values=[value_entry(cadence_ms=U32_MAX // 3)])])
    )
    assert cfg["tree"][0]["budget_ms"] == U32_MAX
    validate(
        portal(tree=[tree_node(budget_ms=1, values=[value_entry(cadence_ms=U32_MAX)])])
    )
    with pytest.raises(cv.Invalid, match=str(U32_MAX)):
        validate(
            portal(tree=[tree_node(values=[value_entry(cadence_ms=U32_MAX // 3 + 1)])])
        )
    with pytest.raises(cv.Invalid, match=str(U32_MAX)):
        validate(
            portal(
                tree=[
                    tree_node(budget_ms=1, values=[value_entry(cadence_ms=U32_MAX + 1)])
                ]
            )
        )


def test_series_index_int32_boundary():
    validate(portal(tree=[tree_node(series=series_entry(index_base=I32_MAX, count=1))]))
    with pytest.raises(cv.Invalid, match=str(I32_MAX)):
        validate(portal(tree=[tree_node(series=series_entry(index_base=I32_MAX + 1))]))


def test_derived_pack_index_int32_boundary():
    def cfg(size):
        return portal(
            submodule_size=size,
            tree=[tree_node(submodule_position=2, series=series_entry(count=1))],
        )

    assert (
        validate(cfg(I32_MAX - 1))["tree"][0]["series"]["resolved_pack_index_base"]
        == I32_MAX
    )
    with pytest.raises(cv.Invalid, match=f"pack_index_base.*{I32_MAX}"):
        validate(cfg(I32_MAX))


def test_role_byte_boundary():
    validate(portal(tree=[tree_node(role="r" * 39, roles_available=["r" * 39])]))
    with pytest.raises(cv.Invalid, match="role.*39 bytes"):
        validate(portal(tree=[tree_node(role="r" * 40, roles_available=["r" * 40])]))


@pytest.mark.parametrize("option", ["x" * 64, "é" * 32, "run\0suffix"])
def test_select_option_capacity_rejected(tmp_path: Path, option):
    cfg = portal(tree=[tree_node(actions=[action_entry("select")])])
    import yaml

    text = portal_yaml(cfg).replace(
        "options: [one, two]",
        f"options: {yaml.safe_dump([option], default_flow_style=True).strip()}",
    )
    errors = errors_of(tmp_path, text)
    assert errors and any("63 bytes" in error or "NUL" in error for error in errors), (
        errors
    )


def test_select_option_capacity_accepted(tmp_path: Path):
    cfg = portal(tree=[tree_node(actions=[action_entry("select")])])
    text = portal_yaml(cfg).replace(
        "options: [one, two]", "options: ['" + "é" * 31 + "x']"
    )
    config_of(tmp_path, text)


def test_path_capacity_follows_longest_dotted_id(tmp_path: Path):
    parent, child = "p" * 27, "c" * 40
    cfg = portal(tree=[tree_node(segment=parent, children=[tree_node(segment=child)])])
    gen = generate(tmp_path, portal_yaml(cfg))
    assert gen.returncode == 0, gen.output
    assert f"#define PORTAL_PATH_MAX {len(parent + '.' + child) + 1}\n" in gen.defines_h
