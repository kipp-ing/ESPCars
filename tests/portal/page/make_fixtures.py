#!/usr/bin/env python3
"""Writes the page fixtures: healthy.json, problem.json (GET /api/v1/state) and ui.json (PORTAL_UI).

Everything is fictional: a made-up vehicle "demo" with a traction battery, one bus ("Bus A"),
six control units and the device itself. Shape: docs/portal-spec.md section 5 plus the Daumen
additions (value `level`, why codes `limit` / `not_requested` / `switched_off`).
Run: `.venv/bin/python tests/portal/page/make_fixtures.py` (rewrites the three files).
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import random

HERE = Path(__file__).parent
BOARD, NODE_ID, MAC = "demo-board", "0xA", "02:00:00:00:00:01"
CELLS = 40


def why(code, evidence=None, detail=None):
    return {"code": code, "detail": detail, "since_ms": None, "evidence": evidence or {}}


def val(key, label, value, unit=None, state="ok", w=None, level=None, raw_hex=None, cls="real"):
    if cls == "sna":
        value = None
    return {"key": key, "label": label, "unit": unit, "value": value,
            "raw": None if isinstance(value, bool) else value, "raw_hex": raw_hex,
            "classification": cls, "state": state, "why": w, "level": level}


def prov(age_ms=420, budget_ms=None):
    return {"origin": "local", "board": BOARD, "node_id": NODE_ID, "age_ms": age_ms,
            "budget_ms": budget_ms, "trust": "assumed", "seq": 41}


def act(node, key, label, control, value=None, options=None, rng=None, unit=None, result=None):
    body = {"switch": {"value": "bool"}, "select": {"value": "string"}, "number": {"value": "number"}}.get(control)
    lo, hi, st = rng if rng else (None, None, None)
    return {"key": key, "label": label, "method": "POST", "path": f"/api/v1/actions/{node}/{key}",
            "enabled": True, "why": None, "body": body, "control": control, "value": value,
            "options": options, "min": lo, "max": hi, "step": st, "unit": unit, "result": result}


def accepted(value, age_ms):
    return {"state": "accepted", "why": why("applied"), "age_ms": age_ms, "value": value}


def node(id_, kind, label, state="ok", w=None, values=(), children=(), actions=(), series=None, p=True):
    return {"id": id_, "kind": kind, "label": label, "state": state, "why": w,
            "provenance": prov() if p else None, "values": list(values), "series": series,
            "children": list(children), "actions": list(actions)}


def cells():
    rnd = random.Random(7)
    v = [round(3.668 + rnd.random() * 0.012, 3) for _ in range(CELLS)]
    v[11], v[29] = 3.684, 3.661
    return v


ECUS = [("a", "Engine control"), ("b", "Charger"), ("c", "Inverter"),
        ("d", "Climate"), ("e", "Instrument cluster"), ("f", "Body controller")]


def ecu(key, label, answered):
    if answered:
        return node(f"ecus.{key}", "ecu", label,
                    values=[val("reply", "Last reply", "Ident DEMO-" + key.upper() + "1", raw_hex="62F190444D4F")])
    v = val("reply", "Last reply", None, state="absent", w=why("value_not_available"), cls="sna")
    return node(f"ecus.{key}", "ecu", label, "unknown", why("not_requested"), values=[v])


def tree(problem: bool):
    cv = cells()
    lo, hi = min(cv), max(cv)
    spread = round((hi - lo) * 1000)
    # --- battery ---------------------------------------------------------------------------
    main_vals = [
        val("soc", "State of charge", 62.4, "%"), val("power", "Power", -12.9, "kW"),
        val("voltage", "Voltage", 341.8, "V"), val("current", "Current", -37.8, "A"),
        val("temp_min", "Temperature min", 18.5, "°C"), val("temp_max", "Temperature max", 21.0, "°C"),
        val("iso", "Isolation", 3000, "kΩ", level="ok"), val("contactor", "Contactor", "closed"),
    ]
    polling = act("bat.main", "polling", "Battery polling", "switch", value=not problem,
                  result=accepted(False, 95000) if problem else None)
    if problem:
        main_vals = [dict(v, state="stale", why=why("stale_data")) for v in main_vals]
        main = node("bat.main", "battery", "Pack", "absent", why("switched_off", {"action": "bat.main/polling"}),
                    main_vals, actions=[polling])
        main["provenance"] = prov(age_ms=310000, budget_ms=180000)
    else:
        main = node("bat.main", "battery", "Pack", values=main_vals, actions=[polling])
        main["provenance"] = prov(age_ms=380, budget_ms=180000)
    cell_node = node("bat.cells", "module", "Cell monitor", values=[
        val("spread", "Cell spread", spread, "mV", level="ok"),
        val("min", "Cell min", lo, "V"), val("max", "Cell max", hi, "V"),
        val("hex", "Raw response", "62 02 08 0E 54 0E 55 0E 53 …"),
        val("rtt", "Response time", 42, "ms"),
    ], actions=[act("bat.cells", "monitor", "Cell monitor", "switch", value=True),
                act("bat.cells", "period", "Period", "number", value=500.0, rng=(250.0, 10000.0, 50.0), unit="ms")],
        series={"key": "cells", "label": "Cell voltages", "unit": "V", "index_base": 1, "index": None,
                "index_frame": "pack_cell", "values": cv, "class": "r" * CELLS, "pack_index_base": None})
    if problem:
        ident = node("bat.ident", "battery", "Battery identity", "stale", why("stale_data"), values=[
            val("serial", "Serial number", "DEMO-PACK-0001", state="stale", w=why("stale_data")),
            val("cap_loss", "Capacity loss", 11.8, "%", state="stale", w=why("stale_data"))])
        ident["provenance"] = prov(age_ms=200000, budget_ms=180000)
    else:
        ident = node("bat.ident", "battery", "Battery identity", values=[
            val("serial", "Serial number", "DEMO-PACK-0001"), val("cap_loss", "Capacity loss", 11.8, "%")])
    bat_kids = [main, cell_node, ident]
    bat_bad = [k for k in bat_kids if k["state"] != "ok"]
    bat = node("bat", "battery", "Traction battery",
               "degraded" if bat_bad else "ok",
               why("child_degraded", {"children": {k["id"]: k["state"] for k in bat_bad}}) if bat_bad else None,
               children=bat_kids, p=False)
    # --- bus ---------------------------------------------------------------------------------
    rec = 129 if problem else 0
    can0 = node("can0", "bus", "Bus A", values=[
        val("awake", "Bus awake", True), val("load", "Bus load", 0.4 if problem else 23.4, "%"),
        val("rec", "RX error counter", rec, None, level="bad" if problem else "ok"),
        val("tec", "TX error counter", 0, None, level="ok"),
        val("bus_off", "Bus off", False, level="ok"),
        val("frames", "Frames received", 18233 if problem else 9811042),
    ], actions=[act("can0", "wake_mode", "Wake method", "select", value="Tester present",
                    options=["Tester present", "Wake frame", "Network management"]),
                act("can0", "wake", "Wake bus", "button")])
    if problem:
        can0["state"] = "degraded"
        can0["why"] = why("limit", {"value": "rec", "level": "bad", "reading": 129, "bound": {"above": 127}})
    # --- control units -----------------------------------------------------------------------
    answered = {"a", "b", "c", "d"} if not problem else {"a"}
    kids = [ecu(k, l, k in answered) for k, l in ECUS]
    ecus = node("ecus", "component", "Control units", children=kids, actions=[
        act("ecus", "scan", "Query all", "button",
            result=None if problem else accepted(None, 4200)),
        act("ecus", "target", "Target", "select", value="a", options=[k for k, _ in ECUS]),
        act("ecus", "service", "Service", "select", value="Identify",
            options=["Identify", "Software version", "Read faults", "Clear faults"]),
        act("ecus", "send", "Send", "button")])
    # --- device ------------------------------------------------------------------------------
    supply = 2.62 if problem else 13.9
    device = node("device", "device", "Device", values=[
        val("supply", "Supply", supply, "V", level="bad" if problem else "ok"),
        val("firmware", "Firmware", "demo-1"),
        val("heap_free", "Heap free", 142312, "B"),
        val("heap_block", "Heap largest block", 61440, "B", level="ok"),
    ], actions=[
        act("device", "deep_sleep", "Deep sleep", "switch", value=True),
        act("device", "stay_awake", "Stay awake", "switch", value=False),
        act("device", "sleep_rail", "Rail in sleep", "select", value="off", options=["off", "held"]),
        act("device", "sleep_idle", "Sleep after bus idle", "number", value=120.0, rng=(30.0, 3600.0, 30.0), unit="s"),
        act("device", "sleep_now", "Sleep now", "button"),
        act("device", "log_tx", "Log own TX", "switch", value=False),
        act("device", "upload_wait", "Upload wait max", "number", value=30.0, rng=(0.0, 240.0, 5.0), unit="min"),
        act("device", "status_led", "Status LED", "switch", value=True),
        act("device", "restart", "Restart", "button"),
    ])
    if problem:
        device["state"] = "degraded"
        device["why"] = why("limit", {"value": "supply", "level": "bad", "reading": 2.62, "bound": {"below": 11.0}})
    top = [bat, can0, ecus, device]
    bad = {k["id"]: k["state"] for k in top if k["state"] != "ok"}
    # not_requested children do not upgrade a parent (contract B2)
    root = node("car", "car", "Vehicle", "degraded" if bad else "ok",
                why("child_degraded", {"children": bad}) if bad else None, children=top, p=False)
    return root


def envelope(problem: bool):
    return {"schema_version": 2, "seq": 41 if problem else 17, "series_legend": {"r": "real", "s": "SNA", "i": "implausible", "-": "not measured"},
            "served_by": {"board": BOARD, "node_id": NODE_ID, "mac": MAC, "state": "ok", "why": None,
                          "uptime_ms": 6817390, "build": "demo-1", "owns": ["bat.main"],
                          "components": ["portal", "sensor", "switch"]},
            "peers": [], "links": [{"rel": "wifi_setup", "label": "WiFi setup", "href": "/wifi"}] if problem else [],
            "tree": tree(problem)}


UI = {
    "lang": "de",
    "tabs": [
        {"label": "Drive", "glance": ["bat.main/power", "bat.main/soc", "device/supply"], "nodes": []},
        {"label": "Battery", "glance": ["bat.main/soc", "bat.cells/spread"], "nodes": ["bat.main", "bat.cells", "bat.ident"]},
        {"label": "Bus", "glance": ["can0/awake"], "nodes": ["can0", "ecus"]},
        {"label": "Device", "glance": ["device/supply"], "nodes": ["device"]},
    ],
    "values": {
        "can0/rec": {"expert": True, "warn": {"above": 95}, "bad": {"above": 127}},
        "can0/tec": {"expert": True, "warn": {"above": 95}, "bad": {"above": 127}},
        "can0/bus_off": {"expert": True, "bad_when": True},
        "can0/frames": {"expert": True},
        "bat.cells/spread": {"warn": {"above": 30}, "bad": {"above": 50}},
        "bat.cells/hex": {"expert": True}, "bat.cells/rtt": {"expert": True},
        "bat.main/iso": {"warn": {"below": 1000}, "bad": {"below": 500}},
        "device/supply": {"warn": {"below": 12.0, "above": 15.0}, "bad": {"below": 11.0}},
        "device/heap_free": {"expert": True},
        "device/heap_block": {"expert": True, "warn": {"below": 20000}, "bad": {"below": 12000}},
    },
    "actions": {
        "can0/wake": {"before": [{"action": "can0/wake_mode", "value": "Tester present"}]},
        "bat.cells/period": {"group": "Cell monitor"},
        "device/deep_sleep": {"group": "Sleep"}, "device/stay_awake": {"group": "Sleep"},
        "device/sleep_rail": {"group": "Sleep"}, "device/sleep_idle": {"group": "Sleep"},
        "device/sleep_now": {"group": "Sleep", "confirm": "hold"},
        "device/log_tx": {"group": "Logger"}, "device/upload_wait": {"group": "Logger"},
        "device/status_led": {"group": "Display"},
        "device/restart": {"confirm": "hold"},
        "ecus/target": {"group": "Manual request"}, "ecus/service": {"group": "Manual request"},
        "ecus/send": {"group": "Manual request"},
    },
    "borrow": {f"ecus.{k}": [{"action": "ecus/service"},
                             {"action": "ecus/send", "before": [{"action": "ecus/target", "value": k}]}]
               for k, _ in ECUS},
}


def main() -> None:
    for name, obj in (("healthy.json", envelope(False)), ("problem.json", envelope(True)), ("ui.json", UI)):
        (HERE / name).write_text(json.dumps(obj, indent=1, ensure_ascii=False) + "\n")
        print("wrote", name)


if __name__ == "__main__":
    main()
