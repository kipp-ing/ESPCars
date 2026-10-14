"""Spec v1.5 YAML surface, ref validation, UI-hint injection and codegen (PS-M6-01..04, PS-M6-07).

Schema cases go through CONFIG_SCHEMA; the rules that need the other components' configs (a
select's options, a number's range) through ESPHome's loader; injection and the generated C++
through `esphome compile --only-generate`. Everything is fictional.
"""

from __future__ import annotations

import gzip
import json
from pathlib import Path
import re

import pytest

from esphome import config_validation as cv

from .common import (
    action_entry,
    errors_of,
    generate,
    portal,
    portal_yaml,
    setup_c6,
    tree_node,
    validate,
    value_entry,
)

TOKEN = "/*PORTAL_UI*/null"


@pytest.fixture
def core(set_core_config):
    setup_c6(set_core_config)


def _tree(**over):
    """bus (limits) + dev (actions) + ecus with an on_demand child that borrows."""
    tree = [
        tree_node(
            segment="bus",
            kind="bus",
            label="Bus",
            values=[
                value_entry(key="rec", sensor="b_sensor", expert=True, warn={"above": 95}, bad={"above": 127}),
                value_entry(key="off", binary_sensor="link_up", sensor=None, bad_when=True, unit=None, decimals=None),
            ],
        ),
        tree_node(
            segment="dev",
            kind="device",
            label="Device",
            actions=[
                action_entry("button", key="restart", confirm="hold", group="Device"),
                action_entry("select", key="target"),
                action_entry("number", key="level"),
                action_entry("switch", key="polling"),
            ],
        ),
        tree_node(
            segment="ecus",
            kind="ecu",
            label="Units",
            children=[
                tree_node(
                    segment="a",
                    kind="ecu",
                    label="Unit A",
                    on_demand=True,
                    values=[value_entry(key="dtc")],
                    borrow=[
                        {"action": "dev/target"},
                        {"action": "dev/restart", "before": [{"action": "dev/target", "value": "one"}]},
                    ],
                )
            ],
        ),
    ]
    return tree


def _cfg(**over):
    return portal(tree=_tree(), **over)


def _err(cfg) -> str:
    with pytest.raises(cv.Invalid) as e:
        validate(cfg)
    return str(e.value)


# --- 4.3 limits / expert (PS-M6-01) ---------------------------------------------------------


def test_full_v15_config_validates(core) -> None:
    out = validate(_cfg(lang="de", layout={"tabs": [{"label": "Bus", "glance": ["bus/rec"], "nodes": ["bus", "ecus"]}]}))
    assert out["lang"] == "de"
    bus = out["tree"][0]
    assert bus["values"][0]["warn"] == {"above": 95.0} and bus["values"][0]["expert"] is True
    assert bus["values"][1]["bad_when"] is True
    assert out["tree"][2]["children"][0]["on_demand"] is True


def test_defaults_lang_en_expert_false_on_demand_false(core) -> None:
    out = validate(portal())
    assert out["lang"] == "en"
    assert "layout" not in out
    unit = out["tree"][0]["children"][0]
    assert unit["on_demand"] is False and unit["values"][0]["expert"] is False


@pytest.mark.parametrize(
    "over, needle",
    [
        ({"warn": {}}, "at least one"),
        ({"warn": {"over": 3}}, "over"),
        ({"bad": {"above": "x"}}, "above"),
        ({"warn_when": True}, "boolean limits"),
        ({"bad_when": False}, "boolean limits"),
    ],
)
def test_sensor_value_limit_rejections(core, over, needle) -> None:
    cfg = portal(tree=[tree_node(segment="x", values=[value_entry(**over)])])
    assert needle in _err(cfg)


def test_binary_value_rejects_numeric_limits_and_text_rejects_all(core) -> None:
    b = value_entry(binary_sensor="link_up", sensor=None, unit=None, warn={"above": 1})
    assert "numeric limits" in _err(portal(tree=[tree_node(segment="x", values=[b])]))
    for lim in ({"warn": {"above": 1}}, {"bad_when": True}):
        t = value_entry(text_sensor="state_text", sensor=None, **lim)
        assert "no limits" in _err(portal(tree=[tree_node(segment="x", values=[t])]))


def test_binary_value_accepts_warn_when_and_bad_when(core) -> None:
    b = value_entry(binary_sensor="link_up", sensor=None, unit=None, warn_when=False, bad_when=True)
    assert validate(portal(tree=[tree_node(segment="x", values=[b])]))["tree"][0]["values"][0]["warn_when"] is False


# --- 4.7 / 4.9 actions, before, borrow, layout (PS-M6-02, PS-M6-03) --------------------------


def test_confirm_only_hold_and_group_non_empty(core) -> None:
    def cfg(**a):
        return portal(tree=[tree_node(segment="d", actions=[action_entry("button", **a)])])

    assert validate(cfg(confirm="hold", group="G"))["tree"][0]["actions"][0]["confirm"] == "hold"
    assert "hold" in _err(cfg(confirm="tap"))
    assert "empty" in _err(cfg(group=""))


def _with_before(entries, control="button"):
    tree = _tree()
    tree[1]["actions"][0] = action_entry(control, key="restart", before=entries)
    if control != "button":
        tree[1]["actions"].append(action_entry("button", key="press"))
    return portal(tree=tree)


@pytest.mark.parametrize(
    "entry, typed",
    [
        ({"action": "dev/target", "value": "two"}, "two"),
        ({"action": "dev/level", "value": 3}, 3.0),
        ({"action": "dev/polling", "value": "on"}, True),
    ],
)
def test_before_accepts_and_types_select_number_switch(core, entry, typed) -> None:
    out = validate(_with_before([entry]))
    assert out["tree"][1]["actions"][0]["before"][0]["value"] == typed


@pytest.mark.parametrize(
    "entry, needle",
    [
        ({"action": "dev/nope", "value": 1}, "names nothing"),
        ({"action": "nope/target", "value": 1}, "names nothing"),
        ({"action": "dev/restart", "value": 1}, "is a button"),
        ({"action": "dev/level", "value": "lots"}, "number"),
        ({"action": "dev/polling", "value": "maybe"}, "switch"),
        ({"action": "dev/target", "value": True}, "bool value fits only a switch"),
        ({"action": "dev", "value": 1}, "<node id>/<key>"),
    ],
)
def test_before_rejections(core, entry, needle) -> None:
    assert needle in _err(_with_before([entry]))


def test_before_on_itself_is_rejected(core) -> None:
    tree = _tree()
    tree[1]["actions"][1] = action_entry("select", key="target", before=[{"action": "dev/target", "value": "one"}])
    assert "the action itself" in _err(portal(tree=tree))


def test_unknown_ref_error_lists_the_valid_ones(core) -> None:
    msg = _err(_with_before([{"action": "dev/nope", "value": 1}]))
    assert "dev/target" in msg and "dev/level" in msg and "dev/polling" in msg


def test_borrow_rejects_unknown_action_and_checks_its_before(core) -> None:
    tree = _tree()
    tree[2]["children"][0]["borrow"] = [{"action": "dev/gone"}]
    assert "borrow.action" in _err(portal(tree=tree))
    tree[2]["children"][0]["borrow"] = [{"action": "dev/restart", "before": [{"action": "dev/restart", "value": 1}]}]
    assert "is a button" in _err(portal(tree=tree))


def test_on_demand_needs_values(core) -> None:
    assert "on_demand" in _err(portal(tree=[tree_node(segment="x", on_demand=True)]))


@pytest.mark.parametrize(
    "layout, needle",
    [
        ({"tabs": [{"label": f"t{i}"} for i in range(6)]}, "at most 5"),
        ({"tabs": [{"label": "t", "glance": ["bus/rec"] * 5}]}, "at most 4"),
        ({"tabs": [{"label": "t", "glance": ["bus/nope"]}]}, "names nothing"),
        ({"tabs": [{"label": "t", "nodes": ["ecus.b"]}]}, "names nothing"),
        ({"tabs": [{"label": ""}]}, "empty"),
        ({"tabs": [{"glance": []}]}, "label"),
    ],
)
def test_layout_rejections(core, layout, needle) -> None:
    assert needle in _err(_cfg(layout=layout))


def test_layout_accepts_empty_tabs_and_dotted_nodes(core) -> None:
    out = validate(_cfg(layout={"tabs": [{"label": "A"}, {"label": "B", "nodes": ["ecus.a"], "glance": ["ecus.a/dtc"]}]}))
    assert out["layout"]["tabs"][0] == {"label": "A", "glance": [], "nodes": []}


def test_lang_only_en_or_de(core) -> None:
    assert "lang" in _err(_cfg(lang="fr")) or "one of" in _err(_cfg(lang="fr"))


# --- switch_off (4.6) ------------------------------------------------------------------------


def test_switch_off_defaults_absent_to_switched_off_and_keeps_values(core) -> None:
    node = tree_node(
        segment="bat", values=[value_entry()], absent_when={"provider": "switch_off", "source": "demo_switch"}
    )
    out = validate(portal(tree=[node]))
    assert out["tree"][0]["absent"] == "switched_off"
    assert out["tree"][0]["values"]


def test_switch_off_rejects_extra_args_and_needs_a_source(core) -> None:
    node = tree_node(segment="bat", absent_when={"provider": "switch_off", "source": "demo_switch", "n": 1})
    assert "does not accept" in _err(portal(tree=[node]))
    node = tree_node(segment="bat", absent_when={"provider": "switch_off"})
    assert "source" in _err(portal(tree=[node]))


def test_a_provider_without_default_why_still_needs_absent(core) -> None:
    """4.1: the default-why relaxation is per provider."""
    from esphome.components import sensor
    from esphome.components.portal import ABSENT_PROVIDERS, register_absent_provider

    from .common import restore_registry, snapshot_registry

    assert ABSENT_PROVIDERS["switch_off"]["default_why"] == "switched_off"
    saved = snapshot_registry()
    try:
        register_absent_provider("p_plain", args={}, source_type=sensor.Sensor, body="return false;")
        node = tree_node(segment="x", absent_when={"provider": "p_plain", "source": "vcc_measured_sensor"})
        assert "requires 'absent'" in _err(portal(tree=[node]))
    finally:
        restore_registry(saved)


# --- entity-aware before checks (final validation, PS-M6-02) ----------------------------------


def test_before_select_value_must_be_an_option(tmp_path: Path) -> None:
    assert errors_of(tmp_path, portal_yaml(_with_before([{"action": "dev/target", "value": "two"}]))) == []
    errs = "\n".join(errors_of(tmp_path, portal_yaml(_with_before([{"action": "dev/target", "value": "three"}]))))
    assert "not an option" in errs and "['one', 'two']" in errs


def test_borrow_before_select_value_must_be_an_option(tmp_path: Path) -> None:
    tree = _tree()
    tree[2]["children"][0]["borrow"][1]["before"][0]["value"] = "zero"
    errs = "\n".join(errors_of(tmp_path, portal_yaml(portal(tree=tree))))
    assert "not an option" in errs


def test_before_number_value_must_be_in_range(tmp_path: Path) -> None:
    assert errors_of(tmp_path, portal_yaml(_with_before([{"action": "dev/level", "value": 10}]))) == []
    errs = "\n".join(errors_of(tmp_path, portal_yaml(_with_before([{"action": "dev/level", "value": 11}]))))
    assert "outside" in errs


# --- hints JSON + injection (PS-M6-04) --------------------------------------------------------


def test_ui_hints_json_shape(core) -> None:
    from esphome.components.portal import build_ui_hints

    cfg = validate(_cfg(lang="de", layout={"tabs": [{"label": "Bus", "glance": ["bus/rec"], "nodes": ["bus"]}]}))
    hints = build_ui_hints(cfg)
    assert hints == {
        "lang": "de",
        "tabs": [{"label": "Bus", "glance": ["bus/rec"], "nodes": ["bus"]}],
        "values": {"bus/rec": {"expert": True, "warn": {"above": 95}, "bad": {"above": 127}}, "bus/off": {"bad_when": True}},
        "actions": {"dev/restart": {"confirm": "hold", "group": "Device"}},
        "borrow": {
            "ecus.a": [
                {"action": "dev/target"},
                {"action": "dev/restart", "before": [{"action": "dev/target", "value": "one"}]},
            ]
        },
    }


def test_ui_hints_defaults_are_empty(core) -> None:
    from esphome.components.portal import build_ui_hints

    assert build_ui_hints(validate(portal())) == {"lang": "en", "tabs": [], "values": {}, "actions": {}, "borrow": {}}


def test_inject_replaces_every_token_escapes_lt_and_leaves_a_tokenless_page_alone() -> None:
    from esphome.components.portal import UI_TOKEN, inject_ui

    assert UI_TOKEN == TOKEN
    hints = {"lang": "en", "tabs": [{"label": "</script><b>ä", "glance": [], "nodes": []}], "values": {}, "actions": {},
             "borrow": {}}
    page = f"<script>const UI = {TOKEN}; const B = {TOKEN};</script>".encode()
    out = inject_ui(page, hints).decode()
    assert TOKEN not in out and out.count('"lang":"en"') == 2
    assert "</script><b>" not in out and "\\u003c/script>\\u003cb>ä" in out
    payload = out.split("const UI = ", 1)[1].split("; const B", 1)[0]
    assert json.loads(payload) == hints
    plain = b"<!doctype html><script>const UI = null;</script>"
    assert inject_ui(plain, hints) is plain or inject_ui(plain, hints) == plain


def _byte_arrays(cpp: str) -> list[bytes]:
    out = []
    for m in re.finditer(r"\{((?:\s*(?:0x[0-9a-fA-F]+|\d+)\s*,?)+)\}", cpp):
        nums = re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1))
        if len(nums) >= 16:
            out.append(bytes(int(n, 0) for n in nums))
    return out


def _pages(cpp: str) -> list[bytes]:
    pages = []
    for raw in _byte_arrays(cpp):
        try:
            pages.append(gzip.decompress(raw))
        except OSError:
            continue
    return pages


def test_codegen_injects_hints_into_a_web_asset_with_the_token(tmp_path: Path) -> None:
    page = f"<!doctype html><title>t</title><script>const UI = {TOKEN};</script>".encode() + b"<p>pad</p>" * 4
    (tmp_path / "ui_page.html").write_bytes(page)
    cfg = _cfg(web_asset="ui_page.html", layout={"tabs": [{"label": "Bus", "glance": ["bus/rec"]}]})
    gen = generate(tmp_path, portal_yaml(cfg))
    assert gen.returncode == 0, gen.output
    pages = [p for p in _pages(gen.main_cpp) if b"const UI = " in p]
    assert len(pages) == 1
    text = pages[0].decode()
    assert TOKEN not in text
    hints = json.loads(text.split("const UI = ", 1)[1].split(";</script>", 1)[0])
    assert hints["tabs"] == [{"label": "Bus", "glance": ["bus/rec"], "nodes": []}]
    assert hints["borrow"]["ecus.a"][0] == {"action": "dev/target"}


def test_codegen_embeds_a_tokenless_web_asset_unchanged(tmp_path: Path) -> None:
    page = b"<!doctype html><title>old</title>" + b"<p>an older custom page</p>" * 6
    (tmp_path / "old.html").write_bytes(page)
    gen = generate(tmp_path, portal_yaml(_cfg(web_asset="old.html")))
    assert gen.returncode == 0, gen.output
    assert gzip.compress(page, compresslevel=9, mtime=0) in _byte_arrays(gen.main_cpp)


def test_codegen_builtin_page_gets_the_hints_iff_it_carries_the_token(tmp_path: Path) -> None:
    from esphome.components.portal import BUILTIN_ASSET

    raw = BUILTIN_ASSET.read_bytes()
    gen = generate(tmp_path, portal_yaml(portal()))
    assert gen.returncode == 0, gen.output
    hints = {"lang": "en", "tabs": [], "values": {}, "actions": {}, "borrow": {}}
    expected = raw.replace(TOKEN.encode(), json.dumps(hints, separators=(",", ":")).encode())
    assert gzip.compress(expected, compresslevel=9, mtime=0) in _byte_arrays(gen.main_cpp)


# --- generated C++ (limits as const data, on_demand, switch_off) ------------------------------


def test_codegen_limits_are_static_const_and_switch_off_carries_the_action_evidence(tmp_path: Path) -> None:
    tree = _tree()
    tree.append(
        tree_node(
            segment="bat",
            kind="battery",
            label="Battery",
            values=[value_entry(key="soc")],
            absent_when={"provider": "switch_off", "source": "demo_switch"},
        )
    )
    tree.append(
        tree_node(
            segment="spare",
            values=[value_entry(key="v")],
            absent_when={"provider": "switch_off", "source": "other_switch"},
        )
    )
    extra = "  - platform: template\n    id: other_switch\n    optimistic: true\n"
    text = portal_yaml(portal(tree=tree)).replace("    id: demo_switch\n    optimistic: true\n",
                                                   "    id: demo_switch\n    optimistic: true\n" + extra, 1)
    gen = generate(tmp_path, text)
    assert gen.returncode == 0, gen.output
    cpp = gen.main_cpp
    limits = re.findall(r"static const esphome::portal::core::ValueLimit (portal_limit_\d+) = (\{.*?\});", cpp)
    assert [n for n, _ in limits] == ["portal_limit_0", "portal_limit_1"]
    assert limits[0][1] == "{{false, true, 0.0f, 95.0f}, {false, true, 0.0f, 127.0f}, false, false, false, false}"
    assert limits[1][1] == "{{false, false, 0.0f, 0.0f}, {false, false, 0.0f, 0.0f}, false, false, true, true}"
    assert re.search(r"add_value_sensor\([^;]*&portal_limit_0\)", cpp)
    assert re.search(r"add_value_binary_sensor\([^;]*&portal_limit_1\)", cpp)
    # one thunk for both nodes, a switch's state read; evidence = the first action on that switch
    assert cpp.count("inline bool portal_prov_switch_off(void *ctx)") == 1
    assert "return source == nullptr || !source->state;" in cpp
    bat = cpp[cpp.find('add_node("bat"'):].split(";", 1)[0]
    # esphome writes the C string with octal-escaped quotes: "action":"dev/polling"
    assert '"switched_off"' in bat and "portal_prov_switch_off, demo_switch," in bat
    assert '"\\042action\\042:\\042dev/polling\\042"' in bat, bat
    spare = cpp[cpp.find('add_node("spare"'):].split(";", 1)[0]
    assert "switched_off" in spare and "nullptr, false)" in spare
    # on_demand reaches add_node as its last argument
    a = cpp[cpp.find('add_node("ecus.a"'):].split(";", 1)[0]
    assert a.rstrip(")").rstrip().endswith("true")
