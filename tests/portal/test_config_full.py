"""Rules that need the assembled config (FINAL_VALIDATE_SCHEMA and cross-component checks).

PS-M1-03 (owns), PS-M1-06 (submodule derivation), PS-M2-02 (web_server exclusion),
PS-M1-10 (web_asset missing), PS-M2-04 (auth optional), PS-M0 loader sanity.
Every case writes a full YAML into tmp_path and runs ESPHome's own config loader, the same
code path as ``esphome config``.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from .common import (
    AUTH,
    NULL,
    action_entry,
    config_of,
    errors_of,
    portal,
    portal_yaml,
    series_entry,
    tree_node,
    value_entry,
)

IDENT = {"board": "demo", "node_id": 10, "mac": "02:00:00:00:00:01"}


def _joined(errors: list[str]) -> str:
    return "\n".join(errors)


def test_baseline_config_loads(tmp_path: Path) -> None:
    """Loader sanity: the minimal fictional config is valid, so every rejection below is about its one change."""
    assert errors_of(tmp_path, portal_yaml()) == []


# --- owns (4.2), PS-M1-03 -----------------------------------------------------------


def test_owns_entry_naming_no_node_is_rejected(tmp_path: Path) -> None:
    """PS-M1-03 reject: `owns entry 'x' names no node in 'tree'`."""
    errs = errors_of(tmp_path, portal_yaml(portal(owns=["nope"])))
    assert "owns entry 'nope' names no node in 'tree'" in _joined(errs), errs


def test_owns_duplicate_is_rejected(tmp_path: Path) -> None:
    """PS-M1-03 reject."""
    errs = errors_of(tmp_path, portal_yaml(portal(owns=["ecu", "ecu"])))
    assert errs and "ecu" in _joined(errs)


def test_owns_dotted_child_id_is_accepted(tmp_path: Path) -> None:
    """PS-M1-03 accept: ids are dotted paths."""
    assert errors_of(tmp_path, portal_yaml(portal(owns=["ecu.unit"]))) == []


def test_complete_identity_without_owns_is_rejected(tmp_path: Path) -> None:
    """PS-M1-03 reject: board+node_id+mac all set requires a non-empty owns."""
    errs = errors_of(tmp_path, portal_yaml(portal(**IDENT)))
    assert errs and "owns" in _joined(errs), errs


def test_complete_identity_with_empty_owns_is_rejected(tmp_path: Path) -> None:
    """PS-M1-03 reject."""
    errs = errors_of(tmp_path, portal_yaml(portal(owns=[], **IDENT)))
    assert errs and "owns" in _joined(errs), errs


def test_complete_identity_with_owns_is_accepted(tmp_path: Path) -> None:
    """PS-M1-03 accept."""
    assert errors_of(tmp_path, portal_yaml(portal(owns=["ecu"], **IDENT))) == []


@pytest.mark.parametrize("missing", ["board", "node_id", "mac"])
def test_incomplete_identity_without_owns_is_accepted(tmp_path: Path, missing: str) -> None:
    """PS-M1-03 accept: with an incomplete identity `owns: []` is fine (wire emits state unknown)."""
    ident = {k: v for k, v in IDENT.items() if k != missing}
    assert errors_of(tmp_path, portal_yaml(portal(**ident))) == []


def test_no_identity_and_empty_owns_is_accepted(tmp_path: Path) -> None:
    """PS-M1-03 accept."""
    assert errors_of(tmp_path, portal_yaml(portal(owns=[]))) == []


ISOTP_EXTRA = """\
can_gateway:
  id: diag
  ports:
    - id: diag_bus
      rx_pin: GPIO3
      tx_pin: GPIO2
      bit_rate: 500kbps
      observe_queue_depth: 32
isotp:
  - id: ecu_engine
    port_id: diag_bus
    tx_id: 0x7E0
    rx_id: 0x7E8
"""


def test_compiled_isotp_does_not_force_owns(tmp_path: Path) -> None:
    """PS-M1-03: a config that compiles isotp but declares `owns: []` without identity is valid
    (owns is never derived from compiled components)."""
    text = portal_yaml(portal(owns=[]), extra=ISOTP_EXTRA)
    assert errors_of(tmp_path, text) == []


def test_compiled_isotp_does_not_satisfy_identity(tmp_path: Path) -> None:
    """PS-M1-03: a compiled owning component no longer stands in for `owns` when identity is complete."""
    text = portal_yaml(portal(**IDENT), extra=ISOTP_EXTRA)
    errs = errors_of(tmp_path, text)
    assert errs and "owns" in _joined(errs), errs


def test_resolved_owns_equal_yaml(tmp_path: Path) -> None:
    """PS-M1-03: the validated owns is exactly the YAML list, in order."""
    cfg = config_of(tmp_path, portal_yaml(portal(owns=["ecu.unit", "ecu"], **IDENT)))
    assert list(cfg["owns"]) == ["ecu.unit", "ecu"]


# --- web_server exclusion (4.8), PS-M2-02 ---------------------------------------------


def test_portal_with_web_server_is_rejected(tmp_path: Path) -> None:
    """PS-M2-02: both serve `/` on the one web_server_base; the portal replaces web_server."""
    errs = errors_of(tmp_path, portal_yaml(extra="web_server:\n  port: 81\n"))
    assert errs and "web_server" in _joined(errs), errs


def test_portal_without_web_server_loads_with_captive_portal_and_ap(tmp_path: Path) -> None:
    """PS-M2-02 control / PS-M2-03: captive_portal plus `wifi: ap:` coexist with the portal."""
    text = portal_yaml()
    text = text.replace("wifi:\n  ssid: demo-net\n  password: demo-pass-123\n",
                        "wifi:\n  ssid: demo-net\n  password: demo-pass-123\n  ap:\n    ssid: demo-ap\n    password: demo-ap-pass-1\ncaptive_portal:\n")
    assert errors_of(tmp_path, text) == []


# --- web_asset (4.8), PS-M1-10 --------------------------------------------------------


def test_missing_web_asset_is_rejected_naming_the_path(tmp_path: Path) -> None:
    """PS-M1-10 reject."""
    errs = errors_of(tmp_path, portal_yaml(portal(web_asset="no_such_page.html")))
    assert errs and "no_such_page.html" in _joined(errs), errs


def test_existing_web_asset_is_accepted_relative_to_the_yaml(tmp_path: Path) -> None:
    """PS-M1-10 accept: the path is relative to the YAML file."""
    (tmp_path / "my_page.html").write_text("<!doctype html><title>x</title>")
    assert errors_of(tmp_path, portal_yaml(portal(web_asset="my_page.html"))) == []


# --- actions need auth (4.7), PS-M2-04 / PS-M3-01 -------------------------------------


def test_actions_without_auth_are_accepted(tmp_path: Path) -> None:
    """PS-M2-04 (v1.1): auth: is optional, also with actions; no validation error."""
    tree = [tree_node(segment="dev", actions=[action_entry("switch")])]
    assert errors_of(tmp_path, portal_yaml(portal(tree=tree))) == []


def test_actions_with_auth_are_accepted(tmp_path: Path) -> None:
    """PS-M3-01 accept: all four controls bound to template entities."""
    acts = [action_entry(c) for c in ("switch", "button", "select", "number")]
    tree = [tree_node(segment="dev", actions=acts)]
    assert errors_of(tmp_path, portal_yaml(portal(tree=tree, auth=AUTH))) == []


def test_action_bound_to_a_missing_entity_is_rejected(tmp_path: Path) -> None:
    """PS-M3-01 reject: the bound entity id must exist."""
    tree = [tree_node(segment="dev", actions=[action_entry("switch", switch="no_such_switch")])]
    errs = errors_of(tmp_path, portal_yaml(portal(tree=tree, auth=AUTH)))
    assert errs and "no_such_switch" in _joined(errs), errs


def test_action_bound_to_the_wrong_domain_is_rejected(tmp_path: Path) -> None:
    """PS-M3-01 reject: a `switch:` key must name a switch, not a button."""
    tree = [tree_node(segment="dev", actions=[action_entry("switch", switch="demo_button")])]
    errs = errors_of(tmp_path, portal_yaml(portal(tree=tree, auth=AUTH)))
    assert errs, "binding a switch action to a button id must not load"


def test_no_actions_needs_no_auth(tmp_path: Path) -> None:
    """PS-M2-04: auth is required only as soon as an action is declared."""
    assert errors_of(tmp_path, portal_yaml(portal())) == []


# --- submodule derivation (4.5), PS-M1-06 ---------------------------------------------


def _pack_tree(*, position: int | None, frame, index_base: int = 7, depth: int = 2, own: bool = False):
    """ecu -> pack -> grp(position) -> [group ->]* seg(series)."""
    series = series_entry(index_base=index_base, index_frame=frame)
    leaf = tree_node(segment="seg0", kind="seg", series=series)
    node = leaf
    for i in range(depth - 1):
        node = tree_node(segment=f"g{i}", children=[node])
    if own:
        grp = dict(leaf)
        grp["segment"] = "grp1"
        if position is not None:
            grp["submodule_position"] = position
        return [tree_node(segment="ecu", children=[tree_node(segment="pack", children=[grp])])]
    grp = tree_node(segment="grp1", children=[node])
    if position is not None:
        grp["submodule_position"] = position
    return [tree_node(segment="ecu", children=[tree_node(segment="pack", children=[grp])])]


def _find_series(cfg: dict, segment: str) -> dict:
    def walk(nodes):
        for n in nodes:
            if n["segment"] == segment:
                return n
            hit = walk(n.get("children", []))
            if hit:
                return hit
        return None

    node = walk(cfg["tree"])
    assert node is not None, segment
    return node["series"]


def test_pack_index_base_derived_from_an_ancestor_two_levels_up(tmp_path: Path) -> None:
    """PS-M1-06: (position - 1) * submodule_size + index_base == (2-1)*12+7 == 19."""
    tree = _pack_tree(position=2, frame="submodule_cell", depth=2)
    cfg = config_of(tmp_path, portal_yaml(portal(tree=tree, submodule_size=12)))
    assert _find_series(cfg, "seg0")["resolved_pack_index_base"] == 19


def test_pack_index_base_derived_from_the_direct_parent(tmp_path: Path) -> None:
    """PS-M1-06."""
    tree = _pack_tree(position=3, frame="submodule_cell", depth=1)
    cfg = config_of(tmp_path, portal_yaml(portal(tree=tree, submodule_size=10)))
    assert _find_series(cfg, "seg0")["resolved_pack_index_base"] == 27


def test_pack_index_base_self_declaring_node(tmp_path: Path) -> None:
    """PS-M1-06: the walk starts at the series' own node, itself included."""
    tree = _pack_tree(position=2, frame="submodule_cell", index_base=9, own=True)
    cfg = config_of(tmp_path, portal_yaml(portal(tree=tree, submodule_size=12)))
    assert _find_series(cfg, "grp1")["resolved_pack_index_base"] == 21


def test_pack_index_base_none_when_no_position_anywhere(tmp_path: Path) -> None:
    """PS-M1-06: no position found -> null."""
    tree = _pack_tree(position=None, frame="submodule_cell")
    cfg = config_of(tmp_path, portal_yaml(portal(tree=tree)))
    assert _find_series(cfg, "seg0")["resolved_pack_index_base"] is None


def test_pack_index_base_none_for_another_named_frame(tmp_path: Path) -> None:
    """PS-M1-06: a position found but a different named frame -> null (not derived, not refused)."""
    tree = _pack_tree(position=2, frame="pack_cell")
    cfg = config_of(tmp_path, portal_yaml(portal(tree=tree, submodule_size=12)))
    assert _find_series(cfg, "seg0")["resolved_pack_index_base"] is None


def test_null_frame_with_no_position_is_accepted_and_resolves_to_none(tmp_path: Path) -> None:
    """PS-M1-06: the explicit null stays legal when nothing would be derived from it."""
    tree = _pack_tree(position=None, frame=NULL)
    cfg = config_of(tmp_path, portal_yaml(portal(tree=tree)))
    s = _find_series(cfg, "seg0")
    assert s["index_frame"] is None and s["resolved_pack_index_base"] is None


@pytest.mark.parametrize("own", [False, True], ids=["ancestor", "self"])
def test_null_frame_under_a_position_is_a_config_error_naming_the_node(tmp_path: Path, own: bool) -> None:
    """PS-M1-06 reject: an unnamed frame never gets an offset; the error names the series node."""
    tree = _pack_tree(position=2, frame=NULL, own=own)
    errs = errors_of(tmp_path, portal_yaml(portal(tree=tree, submodule_size=12)))
    msg = _joined(errs)
    assert errs and "index_frame" in msg and "ecu.pack.grp1" in msg, errs


def test_duplicate_submodule_position_is_rejected(tmp_path: Path) -> None:
    """PS-M1-06 reject: two nodes with the same position anywhere in the tree."""
    tree = [
        tree_node(segment="a", children=[tree_node(segment="x", submodule_position=1)]),
        tree_node(segment="b", children=[tree_node(segment="y", submodule_position=1)]),
    ]
    errs = errors_of(tmp_path, portal_yaml(portal(tree=tree, submodule_size=12)))
    assert errs and "submodule_position" in _joined(errs), errs


def test_distinct_submodule_positions_are_accepted(tmp_path: Path) -> None:
    """PS-M1-06 accept."""
    tree = [
        tree_node(segment="a", submodule_position=1),
        tree_node(segment="b", submodule_position=2),
    ]
    assert errors_of(tmp_path, portal_yaml(portal(tree=tree, submodule_size=12))) == []


def test_submodule_position_without_submodule_size_is_rejected(tmp_path: Path) -> None:
    """PS-M1-06 reject."""
    tree = [tree_node(segment="a", submodule_position=1)]
    errs = errors_of(tmp_path, portal_yaml(portal(tree=tree)))
    assert errs and "submodule_size" in _joined(errs), errs


def test_submodule_size_without_any_position_is_accepted(tmp_path: Path) -> None:
    """PS-M1-06: submodule_size is required iff a position is declared, not the reverse."""
    assert errors_of(tmp_path, portal_yaml(portal(submodule_size=12))) == []


def test_value_entries_still_resolve_with_entities_present(tmp_path: Path) -> None:
    """Loader sanity for the value sources used above."""
    tree = [tree_node(segment="v", values=[value_entry(), value_entry(key="t", sensor=None, text_sensor="state_text"),
                                           value_entry(key="b", sensor=None, binary_sensor="link_up")])]
    assert errors_of(tmp_path, portal_yaml(portal(tree=tree))) == []
