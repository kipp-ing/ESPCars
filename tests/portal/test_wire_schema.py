"""script/portal_schema.py: every rule of section 5 has a rejecting case with its violation code.

PS-M1-09 (rules, CLI exit codes, never raises), PS-M3-03 (action entry shape, validator side),
PS-M1-08 (fictional golden fixtures validate). Fictional envelopes only (tests/portal/wire.py).
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys

import pytest

from .wire import (
    LEGEND,
    REPO,
    act,
    action,
    codes,
    ecu,
    envelope,
    mutate,
    node,
    portal_schema,
    prov,
    result,
    series,
    value,
    why,
)

CLI = REPO / "script" / "portal_schema.py"
ENVELOPES = Path(__file__).parent / "fixtures" / "envelopes"


def test_baseline_is_valid() -> None:
    """PS-M1-09: the base envelope used by the mutations is valid."""
    assert codes(envelope()) == []


# --- envelope / served_by / node (5.1-5.3) ---------------------------------------------


def _rev(d):
    items = list(d.items())[::-1]
    d.clear()
    d.update(items)


def _set(path, val):
    def fn(env):
        target = env
        for p in path[:-1]:
            target = target[p]
        target[path[-1]] = val

    return fn


def _del(path):
    def fn(env):
        target = env
        for p in path[:-1]:
            target = target[p]
        del target[path[-1]]

    return fn


def _no_identity(env):
    sb = env["served_by"]
    sb.update(board=None, node_id=None, mac=None, state="unknown", why=why("no_identity"), owns=[])


def _legend_series(env):
    ecu(env)["series"] = series()
    env["series_legend"] = copy.deepcopy(LEGEND)


RULES = [
    ("schema_version", _set(["schema_version"], 3), "unsupported_version"),
    ("seq_negative", _set(["seq"], -1), "negative"),
    ("seq_type", _set(["seq"], "1"), "wrong_type"),
    ("envelope_missing_peers", _del(["peers"]), "missing_key"),
    ("envelope_key_order", _rev, "key_order"),
    ("envelope_unknown_key", _set(["extra"], 1), "unknown_key"),
    ("node_missing_key", _del(["tree", "children", 0, "actions"]), "missing_key"),
    ("node_unknown_key", _set(["tree", "children", 0, "bogus"], 1), "unknown_key"),
    ("node_key_order", lambda e: _rev(ecu(e)), "key_order"),
    ("node_state_vocab", _set(["tree", "children", 0, "state"], "weird"), "unknown_state"),
    ("why_null_iff_ok_1", _set(["tree", "children", 0, "state"], "degraded"), "why_state_mismatch"),
    ("why_null_iff_ok_2", _set(["tree", "children", 0, "why"], why()), "why_state_mismatch"),
    ("why_code_machine", lambda e: ecu(e).update(state="degraded", why=why("Not Machine")), "why_code_not_machine_readable"),
    ("why_keys", lambda e: ecu(e).update(state="degraded", why={"code": "x"}), "missing_key"),
    ("root_dotted", _set(["tree", "id"], "car.x"), "root_id_dotted"),
    ("id_hierarchy", _set(["tree", "children", 0, "id"], "a.b"), "id_not_hierarchical"),
    ("duplicate_id", lambda e: e["tree"]["children"].append(node("ecu")), "duplicate_id"),
    ("empty_label", _set(["tree", "children", 0, "label"], ""), "empty_label"),
    ("kind_type", _set(["tree", "children", 0, "kind"], ""), "wrong_type"),
    ("ok_past_budget", _set(["tree", "children", 0, "provenance", "age_ms"], 99999), "ok_past_budget"),
    ("stale_within_budget", lambda e: ecu(e).update(state="stale", why=why("stale_data")), "stale_within_budget"),
    ("provenance_null_inside", _set(["tree", "children", 0, "provenance"], None), "provenance_null_inside_mount"),
    ("provenance_on_skeleton", _set(["tree", "provenance"], prov()), "provenance_on_skeleton_node"),
    ("provenance_board_without_identity", lambda e: (_no_identity(e), None)[1], "provenance_names_board_without_identity"),
    ("mount_unresolved_owns", _set(["served_by", "owns"], ["nope"]), "mount_unresolved"),
    ("identity_incomplete", _set(["served_by", "owns"], []), "identity_incomplete"),
    ("no_identity_inconsistent", _set(["served_by", "state"], "unknown"), "no_identity_inconsistent"),
    ("served_by_missing", _del(["served_by", "build"]), "missing_key"),
    ("peers_type", _set(["peers"], {}), "wrong_type"),
    ("node_ok_with_failing_value", lambda e: ecu(e)["values"][0].update(state="degraded", why=why()), "node_ok_with_failing_value"),
    ("value_sna_mismatch", lambda e: ecu(e)["values"][0].update(value=None), "sna_value_mismatch"),
    ("value_classification", lambda e: ecu(e)["values"][0].update(classification="odd"), "unknown_classification"),
    ("value_raw_hex", lambda e: ecu(e)["values"][0].update(raw_hex="zz"), "raw_hex_not_canonical"),
    ("silent_without_frames", lambda e: ecu(e).update(state="silent", why=why("no_traffic", evidence={"rx_frames": 0})), "silent_without_frames"),
    ("absent_with_frames", lambda e: ecu(e).update(state="absent", why=why("x", evidence={"rx_frames": 4}), series=None), "absent_with_frames"),
    ("absent_with_announced", lambda e: ecu(e).update(state="absent", why=why("x", evidence={"announced": True})), "absent_with_announced"),
    ("never_announced_without_evidence", lambda e: ecu(e).update(state="absent", why=why("never_announced")), "evidence_without_discriminator"),
    ("never_announced_with_announced", lambda e: ecu(e).update(state="silent", why=why("never_announced", evidence={"rx_frames": 3, "announced": True})), "never_announced_with_announced"),
    # series (5.3)
    ("series_missing_key", lambda e: (_legend_series(e), ecu(e)["series"].pop("class"))[1], "missing_key"),
    ("series_class_length", lambda e: (_legend_series(e), ecu(e)["series"].update({"class": "r"}))[1], "class_length_mismatch"),
    ("series_class_char", lambda e: (_legend_series(e), ecu(e)["series"].update({"class": "rxr"}))[1], "unknown_class_char"),
    ("series_class_element", lambda e: (_legend_series(e), ecu(e)["series"].update({"class": "rrr"}))[1], "class_element_mismatch"),
    ("series_class_omitted_with_holes", lambda e: (_legend_series(e), ecu(e)["series"].update({"class": None}))[1], "class_omitted_with_holes"),
    ("series_frame_pattern", lambda e: (_legend_series(e), ecu(e)["series"].update({"index_frame": "Bad Frame"}))[1], "index_frame_not_canonical"),
    ("series_key_order", lambda e: (_legend_series(e), _rev(ecu(e)["series"]))[1], "key_order"),
    ("series_index_length", lambda e: (_legend_series(e), ecu(e)["series"].update({"index": [1, 2]}))[1], "index_length_mismatch"),
    ("series_index_order", lambda e: (_legend_series(e), ecu(e)["series"].update({"index": [3, 2, 1]}))[1], "index_not_increasing"),
    ("legend_missing", lambda e: (_legend_series(e), e.update(series_legend=None))[1], "series_legend_missing"),
    ("legend_without_series", _set(["series_legend"], dict(LEGEND)), "series_legend_without_series"),
    ("legend_incomplete_subset", lambda e: (_legend_series(e), e.update(series_legend={"r": "real"}))[1], "series_legend_not_canonical"),
    ("legend_wrong_text", lambda e: (_legend_series(e), e.update(series_legend=dict(LEGEND, r="x")))[1], "series_legend_not_canonical"),
    ("legend_type", _set(["series_legend"], 5), "series_legend_wrong_type"),
    # path / ancestor_role (5.1)
    ("path_without_ancestor_role", lambda e: e.update(path="car"), "path_without_ancestor_role"),
    ("ancestor_role_without_path", lambda e: e.update(ancestor_role=None), "ancestor_role_without_path"),
    ("path_mismatch", lambda e: (e.update(path="zzz", ancestor_role=None), None)[1], "path_mismatch"),
    ("role_not_available", lambda e: ecu(e).update(role="r", roles_available=["q"]), "role_not_available"),
]


@pytest.mark.parametrize("name,fn,code", RULES, ids=[r[0] for r in RULES])
def test_rule_has_a_rejecting_case(name, fn, code) -> None:
    """PS-M1-09: each rule of section 5 rejects with its own violation code."""
    env = copy.deepcopy(envelope())
    fn(env)
    assert code in codes(env), (name, codes(env))


def test_path_key_order_and_ancestor_role_accepts_valid_narrowing() -> None:
    """PS-M1-09 accept: ?path= puts path and ancestor_role between peers and tree."""
    env = envelope()
    env["tree"] = env["tree"]["children"][0]
    env["tree"]["provenance"] = prov()
    env = {k: env[k] for k in ("schema_version", "seq", "series_legend", "served_by", "peers")}
    env.update(path="ecu", ancestor_role=None, tree=node("ecu", values=[value()]))
    assert codes(env) == []
    env["ancestor_role"] = {"node": "car", "label": "Vehicle", "role": "role_a"}
    assert codes(env) == []
    env["ancestor_role"]["node"] = "ecu"
    assert "ancestor_role_in_tree" in codes(env)


def test_valid_role_accepted() -> None:
    """PS-M1-09 accept: role/roles_available come last in the node."""
    env = mutate(lambda e: ecu(e).update(role="r", roles_available=["r", "q"]))
    assert codes(env) == []


def test_no_identity_envelope_is_valid_when_consistent() -> None:
    """PS-M1-09 accept: unknown/no_identity/owns [] with null board in provenance."""
    env = envelope()
    _no_identity(env)
    ecu(env)["provenance"] = prov(board=None, node_id=None)
    env["tree"]["provenance"] = prov(board=None, node_id=None)
    assert codes(env) == []


# --- actions (5.4, 5.5), PS-M3-03 ------------------------------------------------------


def _act(fn):
    def apply(env):
        fn(act(env))

    return apply


def _env_with(action_):
    env = envelope()
    ecu(env)["actions"] = [action_]
    return env


@pytest.mark.parametrize("control", ["switch", "button", "select", "number"])
def test_each_control_entry_is_valid(control) -> None:
    """PS-M3-03 accept."""
    assert codes(_env_with(action(control))) == []


def test_action_without_extension_keys_is_valid() -> None:
    """5.4: an older producer without the extension keys stays valid."""
    a = {k: v for k, v in action("switch").items() if k in ("key", "label", "method", "path", "enabled", "why", "body")}
    assert codes(_env_with(a)) == []


def test_disabled_select_without_state_is_valid() -> None:
    """PS-M3-03 accept: select/number without a state -> disabled, entity_unavailable, value null."""
    for c in ("select", "number"):
        a = action(c, enabled=False, why=why("entity_unavailable"), value=None)
        assert codes(_env_with(a)) == [], c


@pytest.mark.parametrize("state,code,val", [("running", "queued", True), ("accepted", "applied", True),
                                            ("refused", "out_of_range", 99)])
def test_results_are_valid(state, code, val) -> None:
    """PS-M3-03 accept: 5.5 result shapes (number variant for refused)."""
    a = action("number" if state == "refused" else "switch", result=result(state, code, 5, val))
    if state == "refused":
        a["value"] = 2.5
    assert codes(_env_with(a)) == []


def test_refused_result_with_null_value_for_unparseable_body() -> None:
    """5.5: value is null for an unparseable body."""
    a = action("switch", result=result("refused", "invalid_body", 5, None))
    assert codes(_env_with(a)) == []


def test_button_result_value_null() -> None:
    """5.5: button result value null."""
    a = action("button", result=result("accepted", "applied", 5, None))
    assert codes(_env_with(a)) == []


ACTION_RULES = [
    ("unknown_control", _act(lambda a: a.update(control="slider")), "unknown_control"),
    ("method_post", _act(lambda a: a.update(method="GET")), "action_method_not_post"),
    ("path_shape", _act(lambda a: a.update(path="/api/v1/actions/other/act_switch")), "bad_action_path"),
    ("path_slash", _act(lambda a: a.update(path="x")), "bad_action_path"),
    ("body_for_control", _act(lambda a: a.update(body={"value": "number"})), "action_body_mismatch"),
    ("button_body", lambda e: (ecu(e)["actions"].__setitem__(0, action("button", body={"value": "bool"})), None)[1], "action_body_mismatch"),
    ("missing_ext_key", _act(lambda a: a.pop("result")), "missing_key"),
    ("ext_order", _act(_rev), "action_key_order"),
    ("ext_without_control", _act(lambda a: a.pop("control")), "action_extension_without_control"),
    ("why_enabled", _act(lambda a: a.update(why=why())), "action_why_mismatch"),
    ("why_disabled_missing", _act(lambda a: a.update(enabled=False)), "action_why_mismatch"),
    ("switch_disabled", _act(lambda a: a.update(enabled=False, why=why("entity_unavailable"))), "action_always_enabled"),
    ("switch_value_type", _act(lambda a: a.update(value="on")), "action_value_type"),
    ("options_on_switch", _act(lambda a: a.update(options=["a"])), "action_field_not_applicable"),
    ("min_on_switch", _act(lambda a: a.update(min=0)), "action_field_not_applicable"),
    ("unit_on_switch", _act(lambda a: a.update(unit="V")), "action_field_not_applicable"),
    ("button_value", lambda e: ecu(e)["actions"].__setitem__(0, action("button", value=True)), "action_value_type"),
    ("select_options_missing", lambda e: ecu(e)["actions"].__setitem__(0, action("select", options=None)), "wrong_type"),
    ("select_value_type", lambda e: ecu(e)["actions"].__setitem__(0, action("select", value=3)), "action_value_type"),
    ("number_min_type", lambda e: ecu(e)["actions"].__setitem__(0, action("number", min="low")), "wrong_type"),
    ("number_range_inverted", lambda e: ecu(e)["actions"].__setitem__(0, action("number", min=5, max=1)), "action_range_inverted"),
    ("number_step", lambda e: ecu(e)["actions"].__setitem__(0, action("number", step=0)), "action_step_not_positive"),
    ("number_unit_type", lambda e: ecu(e)["actions"].__setitem__(0, action("number", unit=3)), "wrong_type"),
    ("enabled_select_without_state", lambda e: ecu(e)["actions"].__setitem__(0, action("select", value=None)), "action_value_mismatch"),
    ("disabled_with_value", lambda e: ecu(e)["actions"].__setitem__(0, action("number", enabled=False, why=why("entity_unavailable"))), "action_value_mismatch"),
    ("disabled_wrong_code", lambda e: ecu(e)["actions"].__setitem__(0, action("number", enabled=False, why=why("other"), value=None)), "action_why_code"),
    ("result_type", _act(lambda a: a.update(result="x")), "wrong_type"),
    ("result_state", _act(lambda a: a.update(result=result("done"))), "unknown_result_state"),
    ("result_why_object", _act(lambda a: a.update(result=dict(result(), why=None))), "result_why_not_object"),
    ("result_running_code", _act(lambda a: a.update(result=result("running", "applied"))), "result_why_mismatch"),
    ("result_accepted_code", _act(lambda a: a.update(result=result("accepted", "queued"))), "result_why_mismatch"),
    ("result_refused_code", _act(lambda a: a.update(result=result("refused", "applied"))), "result_why_mismatch"),
    ("result_age", _act(lambda a: a.update(result=result(age=-1))), "wrong_type"),
    ("result_value_type", _act(lambda a: a.update(result=result(value="x"))), "result_value_type"),
    ("result_missing_key", _act(lambda a: a.update(result={"state": "accepted"})), "missing_key"),
    ("result_order", _act(lambda a: a.update(result=dict(list(result().items())[::-1]))), "action_key_order"),
    ("result_extra_key", _act(lambda a: a.update(result=dict(result(), extra=1))), "unknown_key"),
    ("actions_type", _set(["tree", "children", 0, "actions"], {}), "wrong_type"),
]


@pytest.mark.parametrize("name,fn,code", ACTION_RULES, ids=[r[0] for r in ACTION_RULES])
def test_action_rule_has_a_rejecting_case(name, fn, code) -> None:
    """PS-M3-03 / PS-M1-09: action entry (5.4) and result (5.5) rules."""
    env = copy.deepcopy(envelope())
    fn(env)
    assert code in codes(env), (name, codes(env))


@pytest.mark.parametrize("refusal", ["invalid_body", "wrong_type", "out_of_range", "unknown_option", "disabled", "body_too_large"])
def test_every_refusal_code_is_accepted(refusal) -> None:
    """5.5: the six refusal codes."""
    a = action("switch", result=result("refused", refusal, 1, None))
    assert codes(_env_with(a)) == []


# --- robustness and CLI ----------------------------------------------------------------


@pytest.mark.parametrize("bad", [None, 5, "x", [], [1], {}, {"schema_version": 2}, {"tree": 3}, {"tree": {"children": 3}},
                                 {"served_by": [], "peers": 1, "tree": []}])
def test_validate_never_raises_on_garbage(bad) -> None:
    """PS-M1-09: never raises on non-dict / missing keys."""
    assert portal_schema.validate_envelope(bad)


def test_validate_never_raises_on_mangled_valid_envelopes() -> None:
    """PS-M1-09: delete or retype each top-level and node-level key of a rich envelope."""
    base = envelope()
    ecu(base)["series"] = series()
    base["series_legend"] = dict(LEGEND)
    ecu(base)["actions"] = [action(c) for c in ("switch", "button", "select", "number")]
    ecu(base)["actions"][0]["result"] = result()

    def paths(obj, prefix=()):
        if isinstance(obj, dict):
            for k, v in obj.items():
                yield prefix + (k,)
                yield from paths(v, prefix + (k,))
        elif isinstance(obj, list):
            for i, v in enumerate(obj):
                yield prefix + (i,)
                yield from paths(v, prefix + (i,))

    for p in list(paths(base)):
        for repl in ("DELETE", None, 7, "s", [], {}):
            env = copy.deepcopy(base)
            t = env
            for k in p[:-1]:
                t = t[k]
            if repl == "DELETE":
                if isinstance(t, dict):
                    del t[p[-1]]
                else:
                    t.pop(p[-1])
            else:
                t[p[-1]] = repl
            portal_schema.validate_envelope(env)


def _run(*args):
    return subprocess.run([sys.executable, str(CLI), *args], capture_output=True, text=True)


def test_cli_exit_codes(tmp_path: Path) -> None:
    """PS-M1-09: exit 0 iff all files valid; 1 on a violation, unreadable or non-JSON file."""
    good = tmp_path / "good.json"
    good.write_text(json.dumps(envelope()))
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(mutate(_set(["seq"], -1))))
    junk = tmp_path / "junk.json"
    junk.write_text("{not json")
    assert _run(str(good)).returncode == 0
    assert _run(str(good), str(good)).returncode == 0
    r = _run(str(bad))
    assert r.returncode == 1 and "negative" in r.stdout
    assert _run(str(good), str(bad)).returncode == 1
    assert _run(str(junk)).returncode == 1
    assert _run(str(tmp_path / "missing.json")).returncode == 1


def test_cli_quiet_suppresses_ok_lines(tmp_path: Path) -> None:
    """PS-M1-09."""
    good = tmp_path / "good.json"
    good.write_text(json.dumps(envelope()))
    assert "OK" in _run(str(good)).stdout
    assert _run("-q", str(good)).stdout == ""


def test_golden_fixtures_validate() -> None:
    """PS-M1-08: every fictional golden fixture validates, via library and CLI."""
    files = sorted(ENVELOPES.glob("*.json"))
    assert len(files) >= 3, "golden fixtures missing"
    for f in files:
        assert portal_schema.validate_envelope(json.loads(f.read_text())) == [], f.name
    assert _run(*map(str, files)).returncode == 0


# --- links (5.1, v1.1), PS-M2-13 -------------------------------------------------------------

WIFI_LINK = {"rel": "wifi_setup", "label": "WiFi setup", "href": "/wifi"}


def _with_links(links):
    env = envelope()
    items = list(env.items())
    env.clear()
    for k, v in items:
        env[k] = v
        if k == "peers":
            env["links"] = links
    return env


def test_links_empty_and_wifi_entry_are_valid() -> None:
    """PS-M2-13 accept: [] without AP, the wifi_setup entry with AP; the key is optional."""
    assert codes(_with_links([])) == []
    assert codes(_with_links([dict(WIFI_LINK)])) == []
    assert codes(envelope()) == []


@pytest.mark.parametrize("name,links,code", [
    ("not_a_list", {}, "links_wrong_type"),
    ("entry_not_object", ["x"], "links_wrong_type"),
    ("missing_rel", [{"label": "L", "href": "/x"}], "link_missing_key"),
    ("missing_label", [{"rel": "r", "href": "/x"}], "link_missing_key"),
    ("missing_href", [{"rel": "r", "label": "L"}], "link_missing_key"),
    ("empty_rel", [dict(WIFI_LINK, rel="")], "link_empty_field"),
    ("empty_label", [dict(WIFI_LINK, label="")], "link_empty_field"),
    ("empty_href", [dict(WIFI_LINK, href="")], "link_empty_field"),
    ("non_string", [dict(WIFI_LINK, label=3)], "link_empty_field"),
    ("href_absolute", [dict(WIFI_LINK, href="http://x/")], "link_href_not_relative"),
    ("href_relative", [dict(WIFI_LINK, href="wifi")], "link_href_not_relative"),
    ("extra_key", [dict(WIFI_LINK, target="_blank")], "unknown_key"),
])
def test_links_rejecting_cases(name, links, code) -> None:
    """PS-M2-13 reject: missing/empty fields, href without leading /."""
    assert code in codes(_with_links(links)), (name, codes(_with_links(links)))


def test_links_position_is_after_peers_and_before_path() -> None:
    """PS-M2-13 / 5.1 key order."""
    env = _with_links([])
    keys = list(env)
    assert keys.index("peers") + 1 == keys.index("links")
    bad = {k: env[k] for k in ["schema_version", "seq", "series_legend", "served_by", "links", "peers", "tree"]}
    assert "key_order" in codes(bad)
