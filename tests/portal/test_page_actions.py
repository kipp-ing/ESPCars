"""Run the page's actual action functions with deterministic fetches and timers in Node."""

from __future__ import annotations

import json
import shutil
import subprocess

import pytest

from .common import COMPONENT
from .test_page_core import _core

NODE = shutil.which("node")
pytestmark = pytest.mark.skipif(not NODE, reason="node not installed")


def _run(scenario: str) -> dict:
    page = (COMPONENT / "index.html").read_text()
    actions = page[page.index("function send(") : page.index("function holdStart(")]
    feedback = page[page.index("function fbHtml(") : page.index("function ctlHtml(")]
    html_helpers = "\n".join(
        line
        for line in page.splitlines()
        if line.startswith(("function esc(", "function E(", "function tag("))
    )
    js = (
        _core()
        + "\n"
        + actions
        + feedback
        + html_helpers
        + r"""
const assert = require("node:assert/strict");
var C = globalThis.PortalCore, L = C.texts(false), timers = [], posts = [], events = [], clock = 0, feedback = "";
var isObj = C.isObj;
var tree = {id:"demo", actions:[
  {key:"first", label:"First", path:"/first"},
  {key:"second", label:"Second", path:"/second"},
  {key:"main", label:"Main", path:"/main"}
], children:[]};
var app = {tree:tree, pend:{}, said:true};
function render() { feedback = fbHtml({ref:"demo/main", a:tree.actions[2]}); }
function poll() {}
function take(b) { app.tree = b.tree; }
function fill(s, o) { return C.fill(s, o); }
function wtxt(w) { return w && w.code || ""; }
function setTimeout(fn, ms) { var t = {fn:fn, at:clock + ms}; timers.push(t); timers.sort((a,b) => a.at-b.at); return t; }
function clearTimeout(t) { timers = timers.filter(x => x !== t); }
function state(result) {
  var b = JSON.parse(JSON.stringify({tree:tree}));
  b.tree.actions.forEach(a => { a.result = result; });
  return b;
}
function response(b, ok=true, status=200) {
  return Promise.resolve({ok:ok, status:status, json:() => Promise.resolve(b)});
}
async function flush() { for (var i=0; i<30; i++) await Promise.resolve(); }
async function tick() {
  assert.ok(timers.length, "expected another completion poll or timeout");
  var t = timers.shift(); clock = t.at; t.fn(); await flush();
}
var scenario = SCENARIO, late;
var fetch = (path, opts) => {
  if (opts && opts.method === "POST") {
    posts.push(path); events.push(path);
    return response({}, true, 202);
  }
  if (scenario === "network") return Promise.reject(new Error("offline"));
  if (scenario === "http") return response(state({state:"accepted"}), false, 503);
  if (scenario === "json") return Promise.resolve({ok:true, json:() => Promise.reject(new Error("bad JSON"))});
  if (scenario === "hung") return new Promise(() => {});
  if (scenario === "late") return new Promise(resolve => { late = resolve; });
  if (scenario === "missing") return response(state(null));
  if (scenario === "refused") return response(state({state:"refused", why:{code:"disabled"}}));
  if (scenario === "unexpected") return response(state({state:"other"}));
  return response(state({state:"running"}));
};
(async () => {
  doAction({ref:"demo/main", a:tree.actions[2], h:{before:[
    {action:"demo/first", value:true}, {action:"demo/second", value:2}
  ]}});
  await flush();
  if (scenario === "ordered") {
    assert.deepEqual(posts, ["/first"]);
    fetch = (path, opts) => {
      if (opts && opts.method === "POST") {
        posts.push(path); events.push(path); return response({}, true, 202);
      }
      events.push("complete:" + posts.at(-1));
      return response(state({state:"accepted"}));
    };
    for (var i=0; i<5 && posts.length<3; i++) await tick();
    assert.deepEqual(events, ["/first", "complete:/first", "/second", "complete:/second", "/main"]);
  } else {
    for (var i=0; i<50 && timers.length && posts.length === 1 && app.pend["demo/main"].s === "running"; i++) await tick();
    assert.deepEqual(posts, ["/first"], "a failed prerequisite must abort all dependent POSTs");
    assert.equal(app.pend["demo/main"].s, "refused");
    assert.match(app.pend["demo/main"].t, /Preset refused: First \(.+\)/);
    assert.match(feedback, /role="status"/);
    assert.ok(feedback.includes(app.pend["demo/main"].t), "the error is rendered next to the control");
    if (scenario === "late") {
      late({ok:true, json:() => Promise.resolve(state({state:"accepted"}))});
      await flush();
      assert.deepEqual(posts, ["/first"], "a late completion cannot resume an aborted sequence");
      assert.equal(app.pend["demo/main"].s, "refused");
    }
  }
  process.stdout.write(JSON.stringify({posts:posts, pending:app.pend["demo/main"]}));
})().catch(e => { console.error(e); process.exitCode=1; });
"""
    )
    js = js.replace("SCENARIO", json.dumps(scenario))
    result = subprocess.run(
        [NODE, "-e", js], capture_output=True, text=True, timeout=30, check=False
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@pytest.mark.parametrize(
    "scenario",
    [
        "network",
        "http",
        "json",
        "hung",
        "late",
        "missing",
        "refused",
        "unexpected",
        "timeout",
    ],
)
def test_failed_prerequisite_aborts_main_action_with_visible_error(
    scenario: str,
) -> None:
    _run(scenario)


def test_prerequisites_complete_in_order_before_main_action() -> None:
    _run("ordered")
