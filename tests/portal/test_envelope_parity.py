"""Host emitter vs fictional golden fixtures vs script/portal_schema.py (PS-M1-07, PS-M1-08, PS-M3-03).

The golden files in fixtures/envelopes/ are compact JSON, byte for byte what a schema_version 2
producer emits for the snapshots built in tests/host/emit_portal_envelope.cpp (basic, narrowed,
no_identity, series, stale: produced by the pre-portal v2 core, so they pin wire compatibility, G2;
since spec v1.5 each value additionally carries `"level":null`, and nothing else changed).
`actions.json` is the portal's own extension (5.4/5.5) and was reviewed against the spec by hand,
`daumen.json` the v1.5 additions (PS-M6-05..07). `legacy_v14_basic.json` is `basic` as a v1.4
producer emitted it (no `level`): the validator must keep accepting it (5.6). Everything is fictional.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess

import pytest

from .wire import REPO, portal_schema

EMITTER = REPO / "tests" / "host" / "emit_portal_envelope.cpp"
GOLDEN = Path(__file__).parent / "fixtures" / "envelopes"
CORE_DIR = REPO / "components" / "portal"

BYTE_IDENTICAL = ["basic", "narrowed", "no_identity", "series", "stale", "daumen"]


@pytest.fixture(scope="module")
def emit(tmp_path_factory):
    exe = tmp_path_factory.mktemp("emit") / "emit_portal_envelope"
    r = subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-DEMIT_SERIES_SCRATCH", "-fsanitize=address,undefined",
         f"-I{CORE_DIR}", str(EMITTER), "-o", str(exe)],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, "emitter did not build (no skip: a missing toolchain is a failure)\n" + r.stderr

    def run(scenario: str, *args: str) -> bytes:
        p = subprocess.run([str(exe), scenario, *args], capture_output=True)
        assert p.returncode == 0, p.stderr.decode()
        return p.stdout

    return run


@pytest.mark.parametrize("name", BYTE_IDENTICAL)
def test_core_output_is_byte_identical_to_the_golden_fixture(emit, name) -> None:
    """PS-M1-08: wire compatibility with schema_version 2, byte for byte."""
    expected = (GOLDEN / f"{name}.json").read_bytes()
    assert emit(name) == expected


@pytest.mark.parametrize("name", BYTE_IDENTICAL + ["large", "actions"])
def test_every_emitted_scenario_validates(emit, name) -> None:
    """PS-M1-07 / PS-M3-03: whatever the core emits passes the schema checker (library)."""
    env = json.loads(emit(name))
    assert portal_schema.validate_envelope(env) == []


def test_large_scenario_crosses_the_4096_byte_frame_boundary(emit) -> None:
    """PS-M1-07: a multi-frame body is still one valid JSON document."""
    raw = emit("large")
    assert len(raw) > 4096 * 3
    env = json.loads(raw)
    assert portal_schema.validate_envelope(env) == []
    ids = []

    def walk(n):
        ids.append(n["id"])
        for c in n["children"]:
            walk(c)

    walk(env["tree"])
    assert len(ids) == len(set(ids)) > 50


def test_actions_scenario_matches_the_reviewed_golden(emit) -> None:
    """PS-M3-03: the action extension is stable (regression golden, reviewed against 5.4/5.5)."""
    assert emit("actions") == (GOLDEN / "actions.json").read_bytes()


def test_actions_fixture_has_the_spec_key_order_and_values() -> None:
    """PS-M3-03: 5.4 key order and per-control values, independent of the byte comparison above."""
    env = json.loads((GOLDEN / "actions.json").read_text())
    acts = env["tree"]["children"][0]["actions"]
    order = ["key", "label", "method", "path", "enabled", "why", "body", "control", "value", "options",
             "min", "max", "step", "unit", "result"]
    for a in acts:
        assert list(a) == order
        assert a["method"] == "POST" and a["path"] == f"/api/v1/actions/ecu/{a['key']}"
    sw, btn, sel, num = acts
    assert sw["control"] == "switch" and sw["value"] is True and sw["body"] == {"value": "bool"}
    assert sw["result"]["state"] == "accepted" and sw["result"]["why"]["code"] == "applied"
    assert btn["body"] is None and btn["value"] is None and btn["result"]["state"] == "running"
    assert sel["options"] == ["one", "two"] and sel["value"] == "two" and sel["result"] is None
    assert num["enabled"] is False and num["why"]["code"] == "entity_unavailable" and num["value"] is None


def test_envelope_wire_basics_in_every_fixture() -> None:
    """PS-M1-08 / 5.1: peers is [], schema_version 2, series_legend canonical or null, root is car."""
    for f in sorted(GOLDEN.glob("*.json")):
        env = json.loads(f.read_text())
        assert env["schema_version"] == 2 and env["peers"] == [], f.name
        assert env["series_legend"] in (None, portal_schema.SERIES_LEGEND), f.name
        root = env["tree"]
        if "path" not in env:
            assert root["id"] == "car" and root["kind"] == "car" and root["label"] == "Vehicle", f.name
        keys = list(env)
        assert keys[:5] == ["schema_version", "seq", "series_legend", "served_by", "peers"], f.name
        assert keys[-1] == "tree", f.name


def test_narrowed_fixture_carries_path_and_ancestor_role_before_tree() -> None:
    """PS-M1-08 / 5.1."""
    env = json.loads((GOLDEN / "narrowed.json").read_text())
    assert list(env)[-3:] == ["path", "ancestor_role", "tree"]
    assert env["path"] == "ecu.unit" and env["tree"]["id"] == "ecu.unit"


def test_no_identity_fixture_follows_5_2() -> None:
    """PS-M1-08 / 5.2: incomplete identity -> unknown / no_identity / owns []."""
    sb = json.loads((GOLDEN / "no_identity.json").read_text())["served_by"]
    assert sb["state"] == "unknown" and sb["why"]["code"] == "no_identity" and sb["owns"] == []
    assert sb["board"] is None and sb["node_id"] is None and sb["mac"] is None


def test_fixtures_are_fictional() -> None:
    """PS-G-02: only locally administered MACs and fictional names in the fixtures."""
    import re

    for f in GOLDEN.glob("*.json"):
        for mac in re.findall(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", f.read_text()):
            assert mac.startswith("02:00:00:00:00:"), (f.name, mac)


def test_cli_validates_all_fixtures() -> None:
    """PS-M1-08: the CLI exit code on the golden set."""
    r = subprocess.run(["python3", str(REPO / "script" / "portal_schema.py"), *map(str, sorted(GOLDEN.glob("*.json")))])
    assert r.returncode == 0


def test_v15_goldens_differ_from_v14_only_by_level_null(emit) -> None:
    """5.6 / G2: the v1.5 producer adds `level` and changes nothing else for a tree without limits."""
    legacy = (GOLDEN / "legacy_v14_basic.json").read_bytes()
    assert emit("basic").replace(b',"level":null', b"") == legacy
    assert portal_schema.validate_envelope(json.loads(legacy)) == []
    assert b'"level"' not in legacy


def test_daumen_golden_carries_limit_not_requested_and_switched_off() -> None:
    """PS-M6-05..07 on the wire: limit evidence, a quiet not_requested child, switched_off evidence."""
    env = json.loads((GOLDEN / "daumen.json").read_text())
    by_id = {n["id"]: n for _, n in portal_schema.iter_nodes(env["tree"])}
    bus = by_id["bus"]
    assert bus["state"] == "degraded" and bus["why"]["code"] == "limit"
    assert bus["why"]["evidence"] == {"value": "rec", "level": "warn", "reading": 100, "bound": {"above": 95}}
    assert [v["level"] for v in bus["values"]] == ["warn", "ok"]
    a = by_id["ecus.a"]
    assert a["state"] == "unknown" and a["why"]["code"] == "not_requested" and a["why"]["evidence"] == {}
    assert by_id["ecus"]["state"] == "ok"
    assert "ecus" not in env["tree"]["why"]["evidence"]["children"]
    bat = by_id["bat"]
    assert bat["state"] == "absent" and bat["why"]["code"] == "switched_off"
    assert bat["why"]["evidence"] == {"action": "bat/polling"}
    assert bat["values"][0]["value"] == 55
