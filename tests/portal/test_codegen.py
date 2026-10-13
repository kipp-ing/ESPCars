"""Codegen checks via ``esphome compile --only-generate`` (seconds, no toolchain).

PS-M1-05 (root label reaches the firmware), PS-M1-10 (web_asset bytes), PS-M2-03 (port),
PS-M2-04 (auth defines), PS-M3-02 (no action storage without actions), sizing defines of 4.8,
``served_by.components`` / ``owns`` arrays of 4.2.
"""

from __future__ import annotations

import gzip
from pathlib import Path
import re

import pytest

from .common import (
    AUTH,
    COMPONENT,
    action_entry,
    generate,
    portal,
    portal_yaml,
    series_entry,
    tree_node,
    value_entry,
)


def _gen(tmp_path: Path, cfg=None, **kw):
    gen = generate(tmp_path, portal_yaml(cfg, **kw))
    assert gen.returncode == 0, gen.output
    return gen


def _define(defines_h: str, name: str):
    m = re.search(rf"^#define {name}(?: (\S+))?\s*$", defines_h, re.M)
    return m


def _byte_arrays(cpp: str) -> list[bytes]:
    out = []
    for m in re.finditer(r"\{((?:\s*(?:0x[0-9a-fA-F]+|\d+)\s*,?)+)\}", cpp):
        nums = re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1))
        if len(nums) >= 16:
            try:
                out.append(bytes(int(n, 0) for n in nums))
            except ValueError:
                pass
    return out


SIZING_TREE = [
    tree_node(
        segment="a",
        values=[value_entry(key="v1"), value_entry(key="v2", sensor="b_sensor")],
        series=series_entry(),
        actions=[action_entry("switch"), action_entry("button")],
        children=[
            tree_node(segment="b", values=[value_entry(key="v3")], actions=[action_entry("select")]),
            tree_node(segment="c", series=series_entry(key="more")),
        ],
    )
]


def test_feature_and_sizing_defines(tmp_path: Path) -> None:
    """PS-M2-04 / 4.8: USE_PORTAL and PORTAL_MAX_* (nodes+root, values, series, actions), PORTAL_TEXT_MAX."""
    gen = _gen(tmp_path, portal(tree=SIZING_TREE, auth=AUTH, text_max=64), extra="")
    d = gen.defines_h
    assert _define(d, "USE_PORTAL")
    expect = {
        "PORTAL_MAX_NODES": 4,  # a, b, c + synthetic root
        "PORTAL_MAX_VALUES": 3,
        "PORTAL_MAX_SERIES": 2,
        "PORTAL_MAX_ACTIONS": 3,
        "PORTAL_TEXT_MAX": 64,
    }
    for name, value in expect.items():
        m = _define(d, name)
        assert m and m.group(1) == str(value), f"{name}: {m and m.group(0)}\n{d}"


def test_sizing_defines_are_at_least_one(tmp_path: Path) -> None:
    """4.8: each sizing define is >= 1 even for a tree with no values/series/actions."""
    gen = _gen(tmp_path, portal(tree=[tree_node(segment="only")]))
    d = gen.defines_h
    for name in ("PORTAL_MAX_NODES", "PORTAL_MAX_VALUES", "PORTAL_MAX_SERIES", "PORTAL_MAX_ACTIONS", "PORTAL_TEXT_MAX"):
        m = _define(d, name)
        assert m and int(m.group(1)) >= 1, f"{name}: {m and m.group(0)}"


def test_auth_emits_both_webserver_auth_defines(tmp_path: Path) -> None:
    """PS-M2-04: auth: -> USE_WEBSERVER_AUTH and USE_WEBSERVER_AUTH_DIGEST."""
    d = _gen(tmp_path, portal(auth=AUTH)).defines_h
    assert _define(d, "USE_WEBSERVER_AUTH")
    assert _define(d, "USE_WEBSERVER_AUTH_DIGEST")


def test_no_auth_means_no_auth_defines(tmp_path: Path) -> None:
    """PS-M2-04 (v1.1): no `auth:` emits neither define."""
    d = _gen(tmp_path, portal()).defines_h
    assert not _define(d, "USE_WEBSERVER_AUTH")
    assert not _define(d, "USE_WEBSERVER_AUTH_DIGEST")


def test_actions_without_auth_generate_with_one_warning(tmp_path: Path) -> None:
    """PS-M2-04 (v1.1): valid, with a config-time warning that access control is the network."""
    tree = [tree_node(segment="dev", actions=[action_entry("switch")])]
    gen = generate(tmp_path, portal_yaml(portal(tree=tree)))
    assert gen.returncode == 0, gen.output
    assert gen.output.count("reachable without a login") == 1, gen.output


def test_actions_with_auth_generate_without_that_warning(tmp_path: Path) -> None:
    """PS-M2-04 (v1.1)."""
    tree = [tree_node(segment="dev", actions=[action_entry("switch")])]
    gen = generate(tmp_path, portal_yaml(portal(tree=tree, auth=AUTH)))
    assert gen.returncode == 0, gen.output
    assert "reachable without a login" not in gen.output


def test_no_actions_no_warning(tmp_path: Path) -> None:
    """PS-M2-04 (v1.1)."""
    gen = generate(tmp_path, portal_yaml(portal()))
    assert gen.returncode == 0 and "reachable without a login" not in gen.output


def test_port_reaches_web_server_base(tmp_path: Path) -> None:
    """PS-M2-03: `port:` is applied to web_server_base's port."""
    cpp = _gen(tmp_path, portal(port=8080)).main_cpp
    assert re.search(r"set_port\(\s*8080\s*\)", cpp), "set_port(8080) not emitted"


def test_default_port_is_80(tmp_path: Path) -> None:
    """PS-M2-03."""
    cpp = _gen(tmp_path, portal()).main_cpp
    assert re.search(r"set_port\(\s*80\s*\)", cpp)


def test_web_server_base_is_loaded_and_web_server_is_not(tmp_path: Path) -> None:
    """PS-M2-01 / 4.8: the portal auto-loads web_server_base; the generated code has no own httpd."""
    gen = _gen(tmp_path, portal())
    assert "web_server_base" in gen.main_cpp
    assert "httpd_start" not in gen.main_cpp


def test_root_label_reaches_the_firmware(tmp_path: Path) -> None:
    """PS-M1-05: default 'Vehicle' and an override both appear in the generated code."""
    assert '"Vehicle"' in _gen(tmp_path, portal()).main_cpp
    other = tmp_path / "other"
    other.mkdir()
    assert '"Demo car"' in _gen(other, portal(root_label="Demo car")).main_cpp


def test_default_asset_is_the_gzip_of_index_html(tmp_path: Path) -> None:
    """PS-M1-10 / PS-M1-11: embedded bytes are `gzip -9`, mtime 0, of components/portal/index.html."""
    expected = gzip.compress((COMPONENT / "index.html").read_bytes(), compresslevel=9, mtime=0)
    arrays = _byte_arrays(_gen(tmp_path, portal()).main_cpp)
    assert expected in arrays, "gzip(index.html) not found among the generated byte arrays"


def test_web_asset_replaces_the_embedded_bytes(tmp_path: Path) -> None:
    """PS-M1-10: the gzip of the given file is embedded and the built-in page is not."""
    page = b"<!doctype html><title>custom</title>" + b"<p>fictional custom page</p>" * 8
    (tmp_path / "my_page.html").write_bytes(page)
    gen = generate(tmp_path, portal_yaml(portal(web_asset="my_page.html")))
    assert gen.returncode == 0, gen.output
    arrays = _byte_arrays(gen.main_cpp)
    assert gzip.compress(page, compresslevel=9, mtime=0) in arrays
    assert gzip.compress((COMPONENT / "index.html").read_bytes(), compresslevel=9, mtime=0) not in arrays


def test_served_by_components_and_owns_arrays(tmp_path: Path) -> None:
    """4.2: components = sorted top-level keys minus substitutions/packages/esphome/external_components;
    owns = the YAML list."""
    cfg = portal(owns=["ecu.unit", "ecu"], board="demo", node_id=10, mac="02:00:00:00:00:01")
    cpp = _gen(tmp_path, cfg).main_cpp
    lists = [re.findall(r'"([^"]*)"', m.group(1)) for m in re.finditer(r"\{([^{}]*)\}", cpp)]
    owns = [x for x in lists if x == ["ecu.unit", "ecu"]]
    assert owns, "owns array not emitted verbatim"
    comps = [x for x in lists if "portal" in x and "sensor" in x]
    assert comps, "components array not found"
    got = comps[0]
    assert got == sorted(got)
    for excluded in ("substitutions", "packages", "esphome", "external_components"):
        assert excluded not in got
    for present in ("portal", "sensor", "wifi", "logger", "switch", "button", "select", "number"):
        assert present in got


@pytest.mark.parametrize("with_actions", [False, True])
def test_actions_storage_follows_declared_actions(tmp_path: Path, with_actions: bool) -> None:
    """PS-M3-02: PORTAL_MAX_ACTIONS reflects the declared actions (>=1 floor), so a build without
    actions carries no per-action storage beyond the floor."""
    tree = [tree_node(segment="dev", actions=[action_entry("button")])] if with_actions else [tree_node(segment="dev")]
    cfg = portal(tree=tree, auth=AUTH if with_actions else None)
    m = _define(_gen(tmp_path, cfg).defines_h, "PORTAL_MAX_ACTIONS")
    assert m and m.group(1) == "1"


def test_domains_without_entities_do_not_break_generation(tmp_path: Path) -> None:
    """4.7: control code compiles only when its domain exists (USE_SWITCH etc.); a config with
    only a button action and no other action domain must generate and compile cleanly."""
    text = portal_yaml(
        portal(tree=[tree_node(segment="dev", actions=[action_entry("button")])], auth=AUTH), with_entities=False
    )
    text = text.replace("portal:\n", "button:\n  - platform: template\n    id: demo_button\nportal:\n", 1)
    gen = generate(tmp_path, text)
    assert gen.returncode == 0, gen.output
