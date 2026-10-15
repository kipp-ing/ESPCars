#!/usr/bin/env python3
"""Local dev harness for the built-in portal page (components/portal/index.html).

    .venv/bin/python tests/portal/page/serve.py [--port 8765]
    open http://127.0.0.1:8765/?s=problem          # or ?s=healthy (default)
         http://127.0.0.1:8765/?s=problem&lang=en  # override the hints' language
         http://127.0.0.1:8765/?ui=none            # no hints: one tab per root node

Serves the page with ui.json injected where the firmware's codegen would (the token
`/*PORTAL_UI*/null`), the fixture of the chosen scenario as GET /api/v1/state (with `?after=`
-> 204 while unchanged), and accepts POST /api/v1/actions/<node>/<key> like the device: 202
`running`, then `accepted` with the new value a moment later. Switching the switch a
`switched_off` node points at swaps that node between its problem and healthy version.
The scenario sticks per browser via a cookie, so the page's own fetches need no query.
"""

from __future__ import annotations

import argparse
import copy
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import time
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).parent
PAGE = HERE.parents[2] / "components" / "portal" / "index.html"
TOKEN = "/*PORTAL_UI*/null"
SCENARIOS = ("healthy", "problem")
START = time.monotonic()
LOCK = threading.Lock()


def load(name: str):
    return json.loads((HERE / name).read_text())


STATES = {s: load(f"{s}.json") for s in SCENARIOS}
PRISTINE = copy.deepcopy(STATES)


def walk(n):
    yield n
    for c in n.get("children") or []:
        yield from walk(c)


def find_node(tree, node_id):
    return next((n for n in walk(tree) if n.get("id") == node_id), None)


def page(query: dict) -> bytes:
    ui = None if query.get("ui") == ["none"] else load("ui.json")
    if ui is not None and query.get("lang"):
        ui["lang"] = query["lang"][0]
    text = PAGE.read_text()
    return text.replace(TOKEN, json.dumps(ui, ensure_ascii=False, separators=(",", ":")), 1).encode()


def check(action, body):
    """The device's body rules (spec 6.2), reduced to what the page can trigger."""
    control = action.get("control") or "button"
    if control == "button":
        return None, None
    if not isinstance(body, dict) or "value" not in body:
        return "invalid_body", 400
    v = body["value"]
    if control == "switch" and not isinstance(v, bool):
        return "wrong_type", 400
    if control == "select":
        if not isinstance(v, str):
            return "wrong_type", 400
        if v not in (action.get("options") or []):
            return "unknown_option", 422
    if control == "number":
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            return "wrong_type", 400
        if not action["min"] <= v <= action["max"]:
            return "out_of_range", 422
    return None, None


def result(state, code, value):
    return {"state": state, "why": {"code": code, "detail": None, "since_ms": None, "evidence": {}},
            "age_ms": 0, "value": value, "_t": time.monotonic()}


def apply(scen, node_id, key, value):
    """Main-loop pass: the entity takes the command."""
    with LOCK:
        env = STATES[scen]
        node = find_node(env["tree"], node_id)
        action = next(a for a in node["actions"] if a["key"] == key)
        if action.get("control") in ("switch", "select", "number"):
            action["value"] = value
        action["result"] = result("accepted", "applied", value)
        ref = f"{node_id}/{key}"
        if action.get("control") == "switch":
            for scenario in SCENARIOS:          # a switched_off node follows its switch
                for n in list(walk(env["tree"])):
                    other = find_node(PRISTINE[scenario]["tree"], n["id"])
                    off = (other or {}).get("why") or {}
                    if off.get("code") == "switched_off" and off.get("evidence", {}).get("action") == ref:
                        src = PRISTINE["healthy" if value else "problem"]["tree"]
                        repl = copy.deepcopy(find_node(src, n["id"]))
                        for a in repl["actions"]:
                            mine = next((b for b in n["actions"] if b["key"] == a["key"]), None)
                            if mine:
                                a.update(value=mine["value"], result=mine["result"])
                        n.clear()
                        n.update(repl)
        env["seq"] += 1


def snapshot(scen):
    with LOCK:
        env = copy.deepcopy(STATES[scen])
    env["served_by"]["uptime_ms"] += int((time.monotonic() - START) * 1000)
    now = time.monotonic()
    for n in walk(env["tree"]):
        for a in n.get("actions") or []:
            r = a.get("result")
            if r and "_t" in r:
                r["age_ms"] = int((now - r.pop("_t")) * 1000)
    return env


class Handler(BaseHTTPRequestHandler):
    def scenario(self, query):
        if query.get("s") and query["s"][0] in SCENARIOS:
            return query["s"][0]
        c = SimpleCookie(self.headers.get("Cookie", ""))
        return c["scen"].value if "scen" in c and c["scen"].value in SCENARIOS else "healthy"

    def send(self, code, body=b"", ctype="application/json", extra=()):
        self.send_response(code)
        self.send_header("Cache-Control", "no-store")
        if body:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        u = urlparse(self.path)
        q = parse_qs(u.query)
        scen = self.scenario(q)
        if u.path in ("/", "/index.html"):
            return self.send(200, page(q), "text/html; charset=utf-8", [("Set-Cookie", f"scen={scen}; Path=/")])
        if u.path == "/api/v1/state":
            env = snapshot(scen)
            if q.get("after") and q["after"][0] == str(env["seq"]):
                return self.send(204)
            return self.send(200, json.dumps(env, ensure_ascii=False).encode())
        if u.path == "/wifi":
            return self.send(200, b"<p>WiFi setup (harness stub)</p>", "text/html")
        return self.send(404)

    def do_POST(self):  # noqa: N802
        u = urlparse(self.path)
        scen = self.scenario({})
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        parts = u.path.split("/")
        if len(parts) != 6 or parts[1:4] != ["api", "v1", "actions"]:
            return self.send(404)
        node_id, key = parts[4], parts[5]
        with LOCK:
            node = find_node(STATES[scen]["tree"], node_id)
            action = next((a for a in (node or {}).get("actions") or [] if a["key"] == key), None)
            if action is None:
                return self.send(404)
            if (action.get("result") or {}).get("state") == "running":
                return self.send(409, b'{"error":"busy"}')
            try:
                body = json.loads(raw) if raw else None
            except ValueError:
                body = None
            why, status = check(action, body)
            value = body.get("value") if isinstance(body, dict) else None
            action["result"] = result("refused" if why else "running", why or "queued", value)
            STATES[scen]["seq"] += 1
            res = {k: v for k, v in action["result"].items() if k != "_t"}
            out = json.dumps({"seq": STATES[scen]["seq"], "node": node_id, "key": key, "result": res}).encode()
        if not why:
            threading.Timer(0.3, apply, (scen, node_id, key, value)).start()
        return self.send(status or 202, out)

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"portal page harness on http://{a.host}:{a.port}/?s=healthy  (?s=problem, &lang=en, ?ui=none)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
