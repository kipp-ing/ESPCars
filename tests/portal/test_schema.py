"""Acceptance and rejection cases for the portal YAML schema (docs/portal-spec.md section 4).

PS-M1-02 (every rule of 4.1, 4.3-4.5 has an accepting and a rejecting case), PS-M1-06 (node
level part), PS-M2-04 (auth/protect), PS-M3-01 (actions). Each test names its PS ID in the
docstring. Rules that need the assembled config (owns, submodule resolution, web_server
exclusion) are in test_config_full.py.
"""

from __future__ import annotations

import pytest

from esphome import config_validation as cv

from .common import (
    AUTH,
    NULL,
    action_entry,
    portal,
    series_entry,
    setup_c6,
    tree_node,
    validate,
    value_entry,
)


@pytest.fixture(autouse=True)
def _core(set_core_config):
    setup_c6(set_core_config)


def _tree(*nodes):
    return list(nodes)


def _budget(validated, index: int = 0):
    return validated["tree"][index]["budget_ms"]


# --- component level (4) ------------------------------------------------------------


def test_defaults_ps_m1_02() -> None:
    """PS-M1-02: documented defaults."""
    v = validate(portal())
    assert v["port"] == 80
    assert v["text_max"] == 48
    assert v["root_label"] == "Vehicle"
    assert "budget_ms" not in v


@pytest.mark.parametrize("port", [1, 80, 443, 8080, 65534])
def test_in_range_ports_are_accepted(port: int) -> None:
    """PS-M1-02 accept."""
    assert validate(portal(port=port))["port"] == port


@pytest.mark.parametrize("port", [32767, 65535])
def test_ota_wedging_ports_are_rejected(port: int) -> None:
    """PS-M1-02 reject: 32767 and 65535 wedge httpd_stop() (4.8)."""
    with pytest.raises(cv.Invalid, match=str(port)):
        validate(portal(port=port))


@pytest.mark.parametrize("port", [0, 70000])
def test_out_of_range_ports_are_rejected(port: int) -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(port=port))


def test_board_is_free_form() -> None:
    """PS-M1-02 accept."""
    for board in ("demo", "anything", "x1"):
        assert validate(portal(board=board))["board"] == board


def test_node_id_int_is_normalised() -> None:
    """PS-M1-02: node_id int -> '0xA'."""
    assert validate(portal(node_id=10))["node_id"] == "0xA"


def test_node_id_hex_string_is_normalised_to_uppercase() -> None:
    """PS-M1-02."""
    assert validate(portal(node_id="0xa"))["node_id"] == "0xA"


def test_mac_is_uppercased() -> None:
    """PS-M1-02."""
    assert validate(portal(mac="02:00:00:00:00:0a"))["mac"] == "02:00:00:00:00:0A"


@pytest.mark.parametrize("mac", ["02-00-00-00-00-01", "02:00:00:00:00", "02:00:00:00:00:01:02", "zz:00:00:00:00:01"])
def test_bad_mac_is_rejected(mac: str) -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(mac=mac))


def test_build_is_required() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(build=None))


def test_tree_is_required_and_non_empty() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=None))
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[]))


@pytest.mark.parametrize("text_max", [16, 48, 256])
def test_in_range_text_max_is_accepted(text_max: int) -> None:
    """PS-M1-02 accept: text_max 16..256."""
    assert validate(portal(text_max=text_max))["text_max"] == text_max


@pytest.mark.parametrize("text_max", [8, 15, 257, 512])
def test_out_of_range_text_max_is_rejected(text_max: int) -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(text_max=text_max))


def test_root_label_override() -> None:
    """PS-M1-05: root_label is carried (the default is checked in test_defaults)."""
    assert validate(portal(root_label="Demo car"))["root_label"] == "Demo car"


def test_root_label_must_not_be_empty() -> None:
    """PS-M1-05."""
    with pytest.raises(cv.Invalid):
        validate(portal(root_label=""))


def test_unknown_top_level_key_is_rejected() -> None:
    """PS-M1-02 reject."""
    config = portal()
    config["frobnicate"] = True
    with pytest.raises(cv.Invalid):
        validate(config)


def test_component_level_budget_ms_is_rejected_with_an_explanation() -> None:
    """PS-M1-02 / 4.4: a component-level budget_ms is an error that explains where budgets live."""
    with pytest.raises(cv.Invalid) as e:
        validate(portal(budget_ms=500))
    assert "cadence_ms" in str(e.value)


@pytest.mark.parametrize("old_key", ["asset", "web_asset_id"])
def test_removed_or_unknown_asset_keys_are_rejected(old_key: str) -> None:
    """PS-M1-10: the only asset key is web_asset; an old/unknown spelling is not silently accepted."""
    config = portal()
    config[old_key] = "x.html"
    with pytest.raises(cv.Invalid):
        validate(config)


# --- node (4.1) ---------------------------------------------------------------------


def test_nested_children_are_accepted() -> None:
    """PS-M1-02 accept."""
    tree = _tree(tree_node(segment="a", children=[tree_node(segment="b", values=[value_entry()])]))
    assert validate(portal(tree=tree))["tree"][0]["children"][0]["segment"] == "b"


@pytest.mark.parametrize("segment", ["a.b", "A", "", "1a", "a-b", "a b", "_a"])
def test_bad_segment_syntax_is_rejected(segment: str) -> None:
    """PS-M1-02 reject: segment pattern [a-z][a-z0-9_]*."""
    with pytest.raises(cv.Invalid, match="segment"):
        validate(portal(tree=_tree(tree_node(segment=segment))))


@pytest.mark.parametrize("segment", ["a", "a1", "a_b", "z9_"])
def test_good_segment_syntax_is_accepted(segment: str) -> None:
    """PS-M1-02 accept."""
    assert validate(portal(tree=_tree(tree_node(segment=segment))))["tree"][0]["segment"] == segment


def test_duplicate_sibling_segment_is_rejected() -> None:
    """PS-M1-02 reject: unique among siblings."""
    kids = [tree_node(segment="pack", values=[value_entry()]), tree_node(segment="pack", values=[value_entry()])]
    with pytest.raises(cv.Invalid, match="duplicate"):
        validate(portal(tree=_tree(tree_node(segment="ecu", children=kids))))


def test_duplicate_top_level_segment_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid, match="duplicate"):
        validate(portal(tree=_tree(tree_node(segment="ecu"), tree_node(segment="ecu"))))


def test_same_segment_under_different_parents_is_accepted() -> None:
    """PS-M1-02 accept: uniqueness is among siblings only."""
    tree = _tree(
        tree_node(segment="a", children=[tree_node(segment="x")]),
        tree_node(segment="b", children=[tree_node(segment="x")]),
    )
    validate(portal(tree=tree))


@pytest.mark.parametrize("field", ["kind", "label", "segment"])
def test_node_required_fields(field: str) -> None:
    """PS-M1-02 reject: segment, kind, label are required."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(**{field: None}))))


def test_empty_children_list_is_rejected() -> None:
    """PS-M1-02 reject: children >= 1."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(children=[]))))


def test_empty_values_list_is_rejected() -> None:
    """PS-M1-02 reject: values >= 1."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[]))))


def test_node_level_asset_key_is_rejected() -> None:
    """PS-M1-10: no per-node asset."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(asset="x.html"))))


# --- role (4.1) ---------------------------------------------------------------------


def test_role_with_roles_available_containing_role_is_accepted() -> None:
    """PS-M1-02 accept."""
    tree = _tree(tree_node(role="role_a", roles_available=["role_a", "role_b"], values=[value_entry()]))
    node = validate(portal(tree=tree))["tree"][0]
    assert node["role"] == "role_a" and node["roles_available"] == ["role_a", "role_b"]


def test_role_without_roles_available_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(role="role_a", values=[value_entry()]))))


def test_role_not_in_roles_available_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(role="role_a", roles_available=["role_b"]))))


def test_empty_roles_available_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(role="role_a", roles_available=[]))))


def test_role_must_be_an_identifier() -> None:
    """PS-M1-02 reject: role/roles_available are identifiers."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(role="Role A", roles_available=["Role A"]))))


# --- absent / absent_when (4.1) -----------------------------------------------------


def test_absent_alone_is_accepted() -> None:
    """PS-M1-02 accept."""
    assert validate(portal(tree=_tree(tree_node(absent="not_compiled"))))["tree"][0]["absent"] == "not_compiled"


@pytest.mark.parametrize(
    "extra",
    [
        {"values": [value_entry()]},
        {"children": [tree_node(segment="child")]},
        {"role": "role_a", "roles_available": ["role_a"]},
        {"series": series_entry()},
        {"actions": [action_entry("button")]},
        {"unknown_when": {"value": "vcc_measured", "equals": "x", "why": "w"}, "values": [value_entry()]},
    ],
    ids=["values", "children", "role", "series", "actions", "unknown_when"],
)
def test_absent_alone_is_exclusive_with_content(extra: dict) -> None:
    """PS-M1-02 reject: `absent` without `absent_when` excludes values/children/role/unknown_when/series/actions."""
    config = portal(tree=_tree(tree_node(absent="not_compiled", **extra)), auth=AUTH)
    with pytest.raises(cv.Invalid):
        validate(config)


def test_absent_when_without_absent_is_rejected() -> None:
    """PS-M1-02 reject: absent_when requires absent."""
    with pytest.raises(cv.Invalid, match="absent_when"):
        validate(portal(tree=_tree(tree_node(absent_when={"provider": "x", "source": "y"}))))


# --- unknown_when / trust_from (4.1) ------------------------------------------------

_WAKE = value_entry(key="wake_state", label="Wake state", unit=None, decimals=None, sensor=None, text_sensor="state_text")
_UW = {"value": "wake_state", "equals": "DOWN", "why": "no_instrument"}


def test_unknown_when_referencing_an_own_value_is_accepted() -> None:
    """PS-M1-02 accept."""
    node = validate(portal(tree=_tree(tree_node(values=[_WAKE], unknown_when=_UW))))["tree"][0]
    assert node["unknown_when"]["equals"] == "DOWN"


def test_unknown_when_value_not_among_own_values_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[value_entry(key="other")], unknown_when=_UW))))


def test_unknown_when_on_a_node_with_children_is_rejected() -> None:
    """PS-M1-02 reject: not combinable with children."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[_WAKE], unknown_when=_UW, children=[tree_node(segment="c")]))))


def test_trust_from_referencing_an_own_value_is_accepted() -> None:
    """PS-M1-02 accept."""
    node = validate(portal(tree=_tree(tree_node(values=[_WAKE], trust_from="wake_state"))))["tree"][0]
    assert node["trust_from"] == "wake_state"


def test_trust_from_not_among_own_values_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[value_entry(key="other")], trust_from="wake_state"))))


# --- evidence-gated why codes (4.1) -------------------------------------------------

GATED = ["never_announced", "no_transfer", "crc_fail", "negative_response"]


@pytest.mark.parametrize("code", GATED)
def test_absent_with_an_evidence_gated_code_is_rejected(code: str) -> None:
    """PS-M1-02 reject: evidence-gated codes need a provider declaring evidence for exactly that code."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(absent=code))))


@pytest.mark.parametrize("code", GATED)
def test_unknown_when_with_an_evidence_gated_why_is_rejected(code: str) -> None:
    """PS-M1-02 reject."""
    uw = dict(_UW, why=code)
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[_WAKE], unknown_when=uw))))


def test_absent_with_a_non_gated_code_is_accepted() -> None:
    """PS-M1-02 accept (negative control)."""
    validate(portal(tree=_tree(tree_node(absent="entities_unavailable"))))


@pytest.mark.parametrize("bad", ["Not Machine", "UPPER", "1x", ""])
def test_absent_code_must_be_machine_readable(bad: str) -> None:
    """PS-M1-02 reject: why-codes are identifiers."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(absent=bad))))


# --- values (4.3) -------------------------------------------------------------------


@pytest.mark.parametrize(
    "extra",
    [
        {"text_sensor": "a"},
        {"binary_sensor": "a"},
        {"text_sensor": "a", "binary_sensor": "b"},
    ],
)
def test_value_with_two_or_three_sources_is_rejected(extra: dict) -> None:
    """PS-M1-02 reject: exactly one of sensor/text_sensor/binary_sensor."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[value_entry(**extra)]))))


def test_value_with_text_and_binary_only_is_rejected() -> None:
    """PS-M1-02 reject."""
    entry = value_entry(sensor=None, text_sensor="a", binary_sensor="b")
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[entry]))))


def test_value_with_no_source_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[{"key": "orphan", "label": "Orphan"}]))))


@pytest.mark.parametrize("kind", ["sensor", "text_sensor", "binary_sensor"])
def test_each_single_source_is_accepted(kind: str) -> None:
    """PS-M1-02 accept."""
    entry = value_entry(**{"sensor": None, kind: "some_id"})
    value = validate(portal(tree=_tree(tree_node(values=[entry]))))["tree"][0]["values"][0]
    assert kind in value


@pytest.mark.parametrize("key", ["Key", "1k", "k.k", "k-k", ""])
def test_bad_value_key_is_rejected(key: str) -> None:
    """PS-M1-02 reject: value key pattern."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[value_entry(key=key)]))))


@pytest.mark.parametrize("decimals", [7, -1])
def test_out_of_range_decimals_is_rejected(decimals: int) -> None:
    """PS-M1-02 reject: decimals 0..6."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[value_entry(decimals=decimals)]))))


@pytest.mark.parametrize("decimals", [0, 6])
def test_boundary_decimals_are_accepted(decimals: int) -> None:
    """PS-M1-02 accept."""
    validate(portal(tree=_tree(tree_node(values=[value_entry(decimals=decimals)]))))


def test_unit_is_optional() -> None:
    """PS-M1-02 accept."""
    validate(portal(tree=_tree(tree_node(values=[value_entry(unit=None)]))))


def test_cadence_ms_zero_is_rejected() -> None:
    """PS-M1-02 reject: cadence_ms >= 1."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[value_entry(cadence_ms=0)]))))


# --- budget (4.4) -------------------------------------------------------------------


@pytest.mark.parametrize("spelling", [NULL, "none", "None"])
def test_every_spelling_of_an_explicit_null_budget_is_accepted(spelling) -> None:
    """PS-M1-02: null / ~ / empty (all Python None after the YAML loader) and 'none'."""
    tree = _tree(tree_node(budget_ms=spelling, values=[value_entry()]))
    assert _budget(validate(portal(tree=tree))) is None


@pytest.mark.parametrize("budget_ms", [1, 60000])
def test_in_range_node_budget_is_carried_verbatim(budget_ms: int) -> None:
    """PS-M1-02 accept."""
    tree = _tree(tree_node(budget_ms=budget_ms, values=[value_entry()]))
    assert _budget(validate(portal(tree=tree))) == budget_ms


@pytest.mark.parametrize("bad", [0, -1, "nope", ""])
def test_budget_neither_positive_int_nor_null_is_rejected(bad) -> None:
    """PS-M1-02 reject (0 and "" must not slide through a truthiness test)."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(budget_ms=bad, values=[value_entry()]))))


def test_no_values_and_no_explicit_budget_derives_null() -> None:
    """PS-M1-02 / 4.4: no own values -> null."""
    assert _budget(validate(portal(tree=_tree(tree_node(absent="not_compiled"))))) is None


def test_budget_is_three_times_the_largest_cadence() -> None:
    """PS-M1-02 / 4.4: 3 * max(cadence_ms)."""
    vals = [value_entry(key="a", cadence_ms=1000), value_entry(key="b", sensor="b_sensor", cadence_ms=5000)]
    assert _budget(validate(portal(tree=_tree(tree_node(values=vals))))) == 15000


def test_one_value_without_cadence_makes_the_derived_budget_null() -> None:
    """PS-M1-02 / 4.4."""
    vals = [value_entry(key="a", cadence_ms=1000), value_entry(key="b", sensor="b_sensor")]
    assert _budget(validate(portal(tree=_tree(tree_node(values=vals))))) is None


def test_explicit_budget_wins_over_cadence() -> None:
    """PS-M1-02 / 4.4."""
    vals = [value_entry(key="a", cadence_ms=1000)]
    assert _budget(validate(portal(tree=_tree(tree_node(budget_ms=750, values=vals))))) == 750


def test_explicit_null_budget_wins_over_cadence() -> None:
    """PS-M1-02 / 4.4."""
    tree = _tree(tree_node(budget_ms=NULL, values=[value_entry(cadence_ms=1000)]))
    assert _budget(validate(portal(tree=tree))) is None


def test_derivation_is_per_node_at_every_depth_and_written_back() -> None:
    """PS-M1-02 / 4.4: every node in the validated config carries a resolved budget_ms."""
    tree = _tree(
        tree_node(
            children=[
                tree_node(segment="deep", values=[value_entry(cadence_ms=2000)]),
                tree_node(segment="deeper", children=[tree_node(segment="leaf", values=[value_entry(cadence_ms=1000)])]),
            ]
        )
    )
    validated = validate(portal(tree=tree))
    seen = []

    def walk(nodes):
        for n in nodes:
            assert "budget_ms" in n, n["segment"]
            seen.append(n["segment"])
            walk(n.get("children", []))

    walk(validated["tree"])
    assert seen == ["extra", "deep", "deeper", "leaf"]
    assert validated["tree"][0]["children"][0]["budget_ms"] == 6000
    assert validated["tree"][0]["children"][1]["children"][0]["budget_ms"] == 3000


def test_binary_sensor_with_cadence_is_rejected() -> None:
    """PS-M1-02 reject: cadence_ms is forbidden on binary_sensor values."""
    entry = value_entry(sensor=None, binary_sensor="up", cadence_ms=1000)
    with pytest.raises(cv.Invalid, match="cadence_ms"):
        validate(portal(tree=_tree(tree_node(values=[entry]))))


def test_node_with_binary_sensor_and_positive_budget_is_rejected() -> None:
    """PS-M1-02 reject: a node with any binary_sensor value may not declare a positive budget_ms."""
    entry = value_entry(sensor=None, binary_sensor="up")
    with pytest.raises(cv.Invalid, match="budget_ms"):
        validate(portal(tree=_tree(tree_node(budget_ms=500, values=[entry]))))


def test_node_with_binary_sensor_derives_null_budget() -> None:
    """PS-M1-02 accept."""
    entry = value_entry(sensor=None, binary_sensor="up")
    assert _budget(validate(portal(tree=_tree(tree_node(values=[entry]))))) is None


def test_node_with_binary_sensor_and_explicit_null_budget_is_legal() -> None:
    """PS-M1-02 accept."""
    entry = value_entry(sensor=None, binary_sensor="up")
    assert _budget(validate(portal(tree=_tree(tree_node(budget_ms=NULL, values=[entry]))))) is None


# --- series (4.5) -------------------------------------------------------------------


def test_series_only_node_is_accepted() -> None:
    """PS-M1-02 accept."""
    s = validate(portal(tree=_tree(tree_node(series=series_entry()))))["tree"][0]["series"]
    assert (s["key"], s["index_frame"], s["count"]) == ("cells", "submodule_cell", 5)


def test_series_and_values_together_are_accepted() -> None:
    """PS-M1-02 accept."""
    node = validate(portal(tree=_tree(tree_node(values=[value_entry(key="crc_ok")], series=series_entry()))))["tree"][0]
    assert node["series"]["key"] == "cells" and node["values"][0]["key"] == "crc_ok"


def test_series_does_not_contribute_to_the_budget() -> None:
    """PS-M1-02 / 4.4: a series-only node derives null."""
    assert _budget(validate(portal(tree=_tree(tree_node(series=series_entry()))))) is None


def test_series_index_frame_explicit_null_is_accepted() -> None:
    """PS-M1-02 accept: index_frame is required but may be an explicit null."""
    s = validate(portal(tree=_tree(tree_node(series=series_entry(index_frame=NULL)))))["tree"][0]["series"]
    assert s["index_frame"] is None


def test_series_index_frame_omitted_is_rejected() -> None:
    """PS-M1-02 reject: required (an author must not acquire null by forgetting)."""
    s = series_entry(index_frame=None)
    assert "index_frame" not in s
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=s))))


@pytest.mark.parametrize("frame", ["None", "Cells", "1c", "a b", ""])
def test_series_index_frame_bad_pattern_is_rejected(frame: str) -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(index_frame=frame)))))


@pytest.mark.parametrize("count", [0, 65, -1])
def test_series_count_out_of_range_is_rejected(count: int) -> None:
    """PS-M1-02 reject: count 1..64."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(count=count)))))


@pytest.mark.parametrize("count", [1, 64])
def test_series_count_boundaries_are_accepted(count: int) -> None:
    """PS-M1-02 accept."""
    validate(portal(tree=_tree(tree_node(series=series_entry(count=count)))))


def test_series_index_base_negative_is_rejected_and_zero_accepted() -> None:
    """PS-M1-02: index_base >= 0."""
    validate(portal(tree=_tree(tree_node(series=series_entry(index_base=0)))))
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(index_base=-1)))))


def test_series_index_base_is_required() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(index_base=None)))))


def test_series_key_colliding_with_own_value_key_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(values=[value_entry(key="cells")], series=series_entry(key="cells")))))


def test_series_key_equal_to_a_value_key_on_another_node_is_accepted() -> None:
    """PS-M1-02 accept: the collision rule is per node."""
    tree = _tree(
        tree_node(segment="a", values=[value_entry(key="cells")]),
        tree_node(segment="b", series=series_entry(key="cells")),
    )
    validate(portal(tree=tree))


@pytest.mark.parametrize("extra", [{"values": "x"}, {"class": "r"}, {"index": 3}, {"pack_index_base": 9}])
def test_series_wire_only_keys_are_rejected(extra: dict) -> None:
    """PS-M1-02 reject: wire-only keys are not config keys."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(**extra)))))


def test_series_decimals_range() -> None:
    """PS-M1-02: decimals 0..6 on a series too."""
    validate(portal(tree=_tree(tree_node(series=series_entry(decimals=6)))))
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(decimals=7)))))


def test_series_provider_without_source_is_rejected() -> None:
    """PS-M1-02 reject: provider/source are all-or-nothing."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(provider="p")))))


def test_series_source_without_provider_is_rejected() -> None:
    """PS-M1-02 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(series=series_entry(source="s")))))


def test_series_unknown_provider_is_rejected_naming_it() -> None:
    """PS-M1-04: the registry is empty by default; an unknown provider is an error."""
    with pytest.raises(cv.Invalid) as e:
        validate(portal(tree=_tree(tree_node(series=series_entry(source="s", provider="not_a_provider")))))
    assert "not_a_provider" in str(e.value)


def test_submodule_position_must_be_positive() -> None:
    """PS-M1-06 reject (node level)."""
    for bad in (0, -1):
        with pytest.raises(cv.Invalid):
            validate(portal(submodule_size=12, tree=_tree(tree_node(submodule_position=bad))))


def test_submodule_size_must_be_positive() -> None:
    """PS-M1-06 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(submodule_size=0))
    assert validate(portal(submodule_size=12))["submodule_size"] == 12


# --- auth (4.8) and actions (4.7) ---------------------------------------------------


def test_auth_is_optional_also_with_actions() -> None:
    """PS-M2-04 (v1.1, owner decision): auth: is optional; no error for actions without it."""
    validate(portal())
    tree = _tree(tree_node(actions=[action_entry("switch")]))
    validate(portal(tree=tree))
    validate(portal(tree=tree, auth=AUTH))


def test_auth_empty_username_or_password_is_rejected() -> None:
    """PS-M2-04 reject: a half-configured login is caught at build time."""
    with pytest.raises(cv.Invalid):
        validate(portal(auth=dict(AUTH, username="")))
    with pytest.raises(cv.Invalid):
        validate(portal(auth=dict(AUTH, password="")))


@pytest.mark.parametrize("protect", ["all", "actions", "none"])
def test_protect_key_no_longer_exists(protect: str) -> None:
    """PS-M2-04 (v1.1): `protect:` is rejected as an unknown key."""
    with pytest.raises(cv.Invalid):
        validate(portal(auth=dict(AUTH, protect=protect)))


def test_auth_requires_both_credentials() -> None:
    """PS-M2-04 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(auth={"username": "demo"}))


@pytest.mark.parametrize("control", ["switch", "button", "select", "number"])
def test_each_control_type_is_accepted(control: str) -> None:
    """PS-M3-01 accept."""
    tree = _tree(tree_node(actions=[action_entry(control)]))
    act = validate(portal(tree=tree, auth=AUTH))["tree"][0]["actions"][0]
    assert control in act


def test_all_four_controls_on_one_node() -> None:
    """PS-M3-01 accept."""
    acts = [action_entry(c) for c in ("switch", "button", "select", "number")]
    validate(portal(tree=_tree(tree_node(actions=acts)), auth=AUTH))


def test_number_decimals_range() -> None:
    """PS-M3-01: number decimals 0..6."""
    for d in (0, 1, 6):
        validate(portal(tree=_tree(tree_node(actions=[action_entry("number", decimals=d)])), auth=AUTH))
    for d in (-1, 7):
        with pytest.raises(cv.Invalid):
            validate(portal(tree=_tree(tree_node(actions=[action_entry("number", decimals=d)])), auth=AUTH))


def test_zero_entity_keys_is_rejected() -> None:
    """PS-M3-01 reject: an action needs exactly one of ..."""
    with pytest.raises(cv.Invalid, match="exactly one"):
        validate(portal(tree=_tree(tree_node(actions=[{"key": "x", "label": "X"}])), auth=AUTH))


def test_two_entity_keys_is_rejected() -> None:
    """PS-M3-01 reject."""
    act = {"key": "x", "label": "X", "switch": "s", "button": "b"}
    with pytest.raises(cv.Invalid, match="exactly one"):
        validate(portal(tree=_tree(tree_node(actions=[act])), auth=AUTH))


@pytest.mark.parametrize("control", ["switch", "button", "select"])
def test_decimals_on_a_non_number_action_is_rejected(control: str) -> None:
    """PS-M3-01 reject."""
    with pytest.raises(cv.Invalid, match="decimals"):
        validate(portal(tree=_tree(tree_node(actions=[action_entry(control, decimals=1)])), auth=AUTH))


def test_duplicate_action_key_on_one_node_is_rejected() -> None:
    """PS-M3-01 reject."""
    acts = [action_entry("switch", key="same"), action_entry("button", key="same")]
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(actions=acts)), auth=AUTH))


def test_same_action_key_on_different_nodes_is_accepted() -> None:
    """PS-M3-01 accept: unique per node, not per tree."""
    tree = _tree(
        tree_node(segment="a", actions=[action_entry("switch", key="go")]),
        tree_node(segment="b", actions=[action_entry("switch", key="go")]),
    )
    validate(portal(tree=tree, auth=AUTH))


@pytest.mark.parametrize("key", ["Bad", "1x", "a.b", "a-b", ""])
def test_bad_action_key_pattern_is_rejected(key: str) -> None:
    """PS-M3-01 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(actions=[action_entry("switch", key=key)])), auth=AUTH))


def test_empty_actions_list_is_rejected() -> None:
    """PS-M3-01 reject: actions >= 1."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(actions=[])), auth=AUTH))


def test_action_label_is_required_and_non_empty() -> None:
    """PS-M3-01 reject."""
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(actions=[action_entry("switch", label=None)])), auth=AUTH))
    with pytest.raises(cv.Invalid):
        validate(portal(tree=_tree(tree_node(actions=[action_entry("switch", label="")])), auth=AUTH))
