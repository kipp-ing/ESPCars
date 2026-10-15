"""The built-in Daumen page against its fixtures: size budget, parse, hint refs, core verdicts, harness.

Fixtures (fictional vehicle "demo"): healthy.json / problem.json are GET /api/v1/state, ui.json the
UI hints codegen injects at /*PORTAL_UI*/null (docs/portal-spec.md 4.9). Regenerate with
make_fixtures.py. The node-based tests are skipped without node.
"""

from __future__ import annotations

import gzip
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

import pytest

HERE = Path(__file__).parent
REPO = HERE.parents[2]
PAGE = REPO / "components" / "portal" / "index.html"
TOKEN = "/*PORTAL_UI*/null"
NODE = shutil.which("node")
needs_node = pytest.mark.skipif(not NODE, reason="node not installed")

RAW_MAX, GZ_MAX = 45 * 1024, 15 * 1024   # P14: <= 45 KB raw, <= 15 KB gzip -9


def fixture(name):
    return json.loads((HERE / name).read_text())


def walk(n):
    yield n
    for c in n.get("children") or []:
        yield from walk(c)


def injected(ui) -> str:
    return PAGE.read_text().replace(TOKEN, json.dumps(ui, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c"))


# --- budget and parse -----------------------------------------------------------------------------


def test_page_fits_the_daumen_budget() -> None:
    raw = PAGE.read_bytes()
    packed = gzip.compress(raw, compresslevel=9, mtime=0)
    assert len(raw) <= RAW_MAX, len(raw)
    assert len(packed) <= GZ_MAX, len(packed)


def test_page_carries_the_hint_token_once() -> None:
    assert PAGE.read_text().count(TOKEN) == 1
    assert "const UI = " + TOKEN + ";" in PAGE.read_text()


def test_page_is_night_only() -> None:
    t = PAGE.read_text()
    assert "prefers-color-scheme" not in t and "data-theme" not in t
    assert re.search(r"body\{[^}]*background:#000", t)
    assert 'name="color-scheme" content="dark"' in t


@needs_node
@pytest.mark.parametrize("ui", [None, "ui.json"])
def test_scripts_parse_with_the_hints_injected(ui, tmp_path) -> None:
    text = injected(fixture(ui) if ui else None)
    scripts = re.findall(r"<script>(.*?)</script>", text, re.S)
    assert len(scripts) == 2
    for i, js in enumerate(scripts):
        f = tmp_path / f"s{i}.js"
        f.write_text(js)
        r = subprocess.run([NODE, "--check", str(f)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr


# --- fixtures -------------------------------------------------------------------------------------


@pytest.mark.parametrize("name", ["healthy.json", "problem.json"])
def test_fixtures_validate_against_the_wire_schema(name) -> None:
    r = subprocess.run([sys.executable, str(REPO / "script" / "portal_schema.py"), str(HERE / name)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


@pytest.mark.parametrize("name", ["healthy.json", "problem.json"])
def test_every_hint_ref_resolves_in_the_fixture(name) -> None:
    tree = fixture(name)["tree"]
    nodes = {n["id"]: n for n in walk(tree)}
    values = {f'{n["id"]}/{v["key"]}' for n in nodes.values() for v in n["values"]}
    actions = {f'{n["id"]}/{a["key"]}' for n in nodes.values() for a in n["actions"]}
    ui = fixture("ui.json")
    assert set(ui) == {"lang", "tabs", "values", "actions", "borrow"}
    assert len(ui["tabs"]) == 4
    for tab in ui["tabs"]:
        assert set(tab["glance"]) <= values and set(tab["nodes"]) <= set(nodes)
    assert set(ui["values"]) <= values and set(ui["actions"]) <= actions
    befores = [b for h in ui["actions"].values() for b in h.get("before", [])]
    for node_id, entries in ui["borrow"].items():
        assert node_id in nodes
        for e in entries:
            assert e["action"] in actions
            befores += e.get("before", [])
    assert befores and all(b["action"] in actions for b in befores)
    assert any(h.get("confirm") == "hold" for h in ui["actions"].values())
    assert any(h.get("expert") for h in ui["values"].values())


def test_problem_fixture_has_the_cases_the_page_must_show() -> None:
    nodes = {n["id"]: n for n in walk(fixture("problem.json")["tree"])}
    rec = next(v for v in nodes["can0"]["values"] if v["key"] == "rec")
    assert rec["value"] == 129 and rec["level"] == "bad" and nodes["can0"]["why"]["code"] == "limit"
    assert nodes["device"]["why"]["evidence"] == {"value": "supply", "level": "bad", "reading": 2.62, "bound": {"below": 11.0}}
    assert nodes["bat.main"]["state"] == "absent" and nodes["bat.main"]["why"]["evidence"] == {"action": "bat.main/polling"}
    assert sum(1 for n in nodes.values() if (n["why"] or {}).get("code") == "not_requested") == 5
    assert nodes["bat.ident"]["state"] == "stale"
    assert len(nodes["bat.cells"]["series"]["values"]) == 40


# --- the core's verdicts on the fixtures (node) ---------------------------------------------------


def core(expr: str, **data):
    m = re.search(r"/\* portal-core:start.*?/\* portal-core:end \*/", PAGE.read_text(), re.S)
    js = m.group(0) + "\nvar C = globalThis.PortalCore;\n" + "".join(
        f"var {k} = {json.dumps(v)};\n" for k, v in data.items()) + f"process.stdout.write(JSON.stringify({expr}));\n"
    r = subprocess.run([NODE, "-e", js], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


@needs_node
def test_problem_tab_status_counts_limits_red_and_not_requested_neutral() -> None:
    out = core("(function(){var t=C.tally([E.tree]);return [t.bad,t.warn,t.info,t.neutral,t.list.map(function(n){return n.id;}),"
               "C.statusText(t,C.texts(true)),C.statusText(t,C.texts(false))];})()", E=fixture("problem.json"))
    assert out == [2, 0, 2, 5, ["can0", "device", "bat.main", "bat.ident"],
                   "2 Probleme · 2 Hinweise", "2 problems · 2 notes"]


@needs_node
def test_healthy_tree_is_in_order_and_parents_only_count() -> None:
    out = core("(function(){var e=C.findNode(E.tree,'ecus'),t=C.tally([E.tree]);return [C.statusText(t,C.texts(true)),"
               "C.countText(C.tally(C.kids(e)),C.texts(true))];})()", E=fixture("healthy.json"))
    assert out == ["alles in Ordnung", "4 ok · 2 nicht abgefragt"]


@needs_node
def test_tabs_default_to_one_per_root_node() -> None:
    out = core("C.tabsOf(E.tree,{lang:'en',tabs:[],values:{},actions:{},borrow:{}}).map(function(t){return t.label;})", E=fixture("healthy.json"))
    assert out == ["Traction battery", "Bus A", "Control units", "Device"]


@needs_node
@pytest.mark.parametrize("value,hint,level", [
    ({"value": 129}, {"bad": {"above": 127}, "warn": {"above": 95}}, "bad"),
    ({"value": 96}, {"bad": {"above": 127}, "warn": {"above": 95}}, "warn"),
    ({"value": 127}, {"bad": {"above": 127}}, "ok"),
    ({"value": 2.62}, {"bad": {"below": 11}}, "bad"),
    ({"value": True}, {"bad_when": True}, "bad"),
    ({"value": None}, {"bad": {"below": 11}}, None),
    ({"value": 5}, None, None),
    ({"value": 129, "level": "ok"}, {"bad": {"above": 127}}, "ok"),     # the device's verdict wins
])
def test_level_falls_back_to_the_hints_only_for_an_older_producer(value, hint, level) -> None:
    assert core("C.levelOf(V,H)", V=value, H=hint) == level


# --- harness --------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def harness():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    p = subprocess.Popen([sys.executable, str(HERE / "serve.py"), "--port", str(port)], stdout=subprocess.DEVNULL)
    base = f"http://127.0.0.1:{port}"
    for _ in range(50):
        try:
            urllib.request.urlopen(base + "/api/v1/state", timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    yield base
    p.terminate()
    p.wait(5)


def get(url, cookie="problem"):
    req = urllib.request.Request(url, headers={"Cookie": f"scen={cookie}"})
    return urllib.request.urlopen(req, timeout=5)


def post(url, body, cookie="problem"):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST",
                                 headers={"Cookie": f"scen={cookie}", "Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=5)
    except urllib.error.HTTPError as e:
        return e


def test_harness_injects_the_hints_and_serves_204_while_unchanged(harness) -> None:
    html = get(harness + "/?s=problem").read().decode()
    assert TOKEN not in html and '"tabs":[{"label":"Drive"' in html
    env = json.loads(get(harness + "/api/v1/state").read())
    assert env["seq"] == fixture("problem.json")["seq"]
    assert get(harness + f"/api/v1/state?after={env['seq']}").status == 204


def test_harness_switch_brings_a_switched_off_node_back(harness) -> None:
    r = post(harness + "/api/v1/actions/bat.main/polling", {"value": True})
    assert r.status == 202 and json.loads(r.read())["result"]["state"] == "running"
    assert post(harness + "/api/v1/actions/ecus/target", {"value": "zz"}).status == 422
    time.sleep(0.6)
    main = next(n for n in walk(json.loads(get(harness + "/api/v1/state").read())["tree"]) if n["id"] == "bat.main")
    assert main["state"] == "ok" and main["actions"][0]["value"] is True
    assert main["actions"][0]["result"]["state"] == "accepted"
    post(harness + "/api/v1/actions/bat.main/polling", {"value": False})
