"""Fictional valid envelopes for the wire-format tests (docs/portal-spec.md section 5)."""

from __future__ import annotations

import copy
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "script"))
import portal_schema  # noqa: E402

LEGEND = {"r": "real", "s": "SNA", "i": "implausible", "-": "not measured"}


def why(code="entities_unavailable", detail=None, since=None, evidence=None):
    return {"code": code, "detail": detail, "since_ms": since, "evidence": evidence or {}}


def prov(age=10, budget=1500, trust="confirmed", board="demo", node_id="0xA", seq=1):
    return {"origin": "local", "board": board, "node_id": node_id, "age_ms": age, "budget_ms": budget, "trust": trust, "seq": seq}


def value(key="vcc", label="VCC", unit="V", v=3.3, raw=3300.0):
    return {"key": key, "label": label, "unit": unit, "value": v, "raw": raw, "raw_hex": None,
            "classification": "real", "state": "ok", "why": None}


def node(id, kind="ecu", label=None, state="ok", w=None, provenance="own", values=None, series=None,
         children=None, actions=None, **extra):
    n = {"id": id, "kind": kind, "label": label or id, "state": state, "why": w,
         "provenance": prov() if provenance == "own" else provenance,
         "values": values or [], "series": series, "children": children or [], "actions": actions or []}
    n.update(extra)
    return n


def series(values=(3300, None, 3310), cls="r-r"):
    return {"key": "cells", "label": "Cells", "unit": "mV", "index_base": 1, "index": None,
            "index_frame": "submodule_cell", "values": list(values), "class": cls, "pack_index_base": None}


def action(control="switch", node_id="ecu", key=None, **over):
    key = key or f"act_{control}"
    body = {"switch": {"value": "bool"}, "button": None, "select": {"value": "string"}, "number": {"value": "number"}}[control]
    a = {"key": key, "label": key, "method": "POST", "path": f"/api/v1/actions/{node_id}/{key}",
         "enabled": True, "why": None, "body": body, "control": control,
         "value": {"switch": True, "button": None, "select": "one", "number": 2.5}[control],
         "options": ["one", "two"] if control == "select" else None,
         "min": 0 if control == "number" else None, "max": 10 if control == "number" else None,
         "step": 0.5 if control == "number" else None, "unit": "V" if control == "number" else None,
         "result": None}
    a.update(over)
    return a


def result(state="accepted", code="applied", age=100, value=True, detail=None):
    return {"state": state, "why": why(code, detail), "age_ms": age, "value": value}


def envelope(**over):
    env = {
        "schema_version": 2, "seq": 1, "series_legend": None,
        "served_by": {"board": "demo", "node_id": "0xA", "mac": "02:00:00:00:00:01", "state": "ok", "why": None,
                      "uptime_ms": 1000, "build": "demo-1", "owns": ["ecu"], "components": ["logger", "portal"]},
        "peers": [],
        "tree": node("car", "car", "Vehicle", provenance=None, children=[
            node("ecu", values=[value()], actions=[action("switch")])]),
    }
    env.update(over)
    return env


def codes(env):
    return sorted({v.code for v in portal_schema.validate_envelope(env)})


def mutate(fn):
    env = copy.deepcopy(envelope())
    fn(env)
    return env


def ecu(env):
    return env["tree"]["children"][0]


def act(env, i=0):
    return ecu(env)["actions"][i]
