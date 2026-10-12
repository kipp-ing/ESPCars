"""PS-M3-10: the page's repaint hold and local clocks, run under node against the core block.

The core block (between the portal-core markers in index.html) is extracted from the built-in
page itself and evaluated as is, so there is no second copy to drift. Skipped without node.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest

from .common import COMPONENT

PAGE = COMPONENT / "index.html"
NODE = shutil.which("node")
pytestmark = pytest.mark.skipif(not NODE, reason="node not installed")


def _core() -> str:
    m = re.search(r"/\* portal-core:start.*?/\* portal-core:end \*/", PAGE.read_text(), re.S)
    assert m, "portal-core markers missing"
    return m.group(0)


def _run(expr: str):
    """Evaluate `expr` with the core loaded (as `C`) and return its JSON value."""
    js = _core() + "\nvar C = globalThis.PortalCore;\nprocess.stdout.write(JSON.stringify(" + expr + "));\n"
    r = subprocess.run([NODE, "-e", js], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def _hold(tag: str, typ: str, ago) -> bool:
    return _run(f"C.holdsRepaint({json.dumps({'tag': tag, 'type': typ, 'touched_ago_ms': ago})})")


@pytest.mark.parametrize("tag,typ", [("INPUT", "checkbox"), ("BUTTON", "submit"), ("A", "")])
def test_toggle_and_button_never_hold_the_repaint(tag, typ) -> None:
    assert _hold(tag, typ, 0) is False


@pytest.mark.parametrize("tag,typ", [("SELECT", "select-one"), ("INPUT", "number")])
def test_dropdown_and_number_hold_only_while_recently_touched(tag, typ) -> None:
    hold_ms = _run("C.HOLD_MS")
    assert _hold(tag, typ, 0) is True
    assert _hold(tag, typ, hold_ms - 1) is True
    assert _hold(tag, typ, hold_ms) is False
    assert _hold(tag, typ, None) is False


def test_nothing_focused_holds_nothing() -> None:
    assert _run("C.holdsRepaint(null)") is False


ENVELOPE = {
    "schema_version": 2, "seq": 7,
    "served_by": {"board": "demo", "uptime_ms": 15800},
    "tree": {"id": "car", "actions": [], "provenance": None, "children": [
        {"id": "car.dev", "provenance": {"age_ms": 572},
         "actions": [{"key": "go", "result": {"state": "accepted", "age_ms": 7600}},
                     {"key": "idle", "result": None}],
         "children": []}]},
}


def test_uptime_and_result_age_advance_while_held() -> None:
    out = _run(f"C.advanceClocks({json.dumps(ENVELOPE)}, 3000)")
    assert out["served_by"]["uptime_ms"] == 18800
    dev = out["tree"]["children"][0]
    assert dev["actions"][0]["result"]["age_ms"] == 10600
    assert dev["actions"][1]["result"] is None


def test_provenance_age_is_not_advanced_and_input_is_not_mutated() -> None:
    out = _run(f"(function(){{var e={json.dumps(ENVELOPE)}; var o=C.advanceClocks(e, 3000);"
               "return [o.tree.children[0].provenance.age_ms, e.served_by.uptime_ms];})()")
    assert out == [572, 15800]


@pytest.mark.parametrize("elapsed", [0, -5, None])
def test_no_elapsed_time_changes_nothing(elapsed) -> None:
    out = _run(f"C.advanceClocks({json.dumps(ENVELOPE)}, {json.dumps(elapsed)})")
    assert out == ENVELOPE


def test_page_refetches_in_full_and_repaints_on_navigation() -> None:
    """The DOM layer wires the core: full fetch every REFRESH_MS, blur on navigation and on a dropdown change."""
    t = PAGE.read_text()
    assert re.search(r"var REFRESH_MS = \d+;", t)
    assert "C.holdsRepaint(" in t and "C.advanceClocks(" in t
    assert re.search(r"function readHash\(\) \{[^}]*ae\.blur\(\)", t, re.S)
    assert "ctl.blur(); doAction(a, ctl.value)" in t
    assert "inp.blur(); setb.click();" in t, "Enter in a number field sends and releases the hold"
