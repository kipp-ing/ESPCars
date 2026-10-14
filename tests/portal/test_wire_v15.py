"""script/portal_schema.py, spec v1.5 rules (5.3, 5.6): PS-M6-08.

Value `level`, node why-codes `limit`, `not_requested`, `switched_off`. Every rule has a
rejecting case with its violation code, and the shapes a v1.4 producer emits still pass.
Fictional envelopes only (tests/portal/wire.py).
"""

from __future__ import annotations

import pytest

from .wire import codes, ecu, envelope, mutate, node, value, why


def _limit_evidence(**over):
    ev = {"value": "vcc", "level": "bad", "reading": 3.3, "bound": {"below": 11.5}}
    ev.update(over)
    return ev


def _limited(env, level="bad", state="degraded", evidence=None):
    n = ecu(env)
    n["values"][0]["level"] = level
    n["state"] = state
    n["why"] = None if state == "ok" else why("limit", evidence=evidence or _limit_evidence(level=level))
    env["tree"]["state"] = "degraded"
    env["tree"]["why"] = why("child_degraded", evidence={"children": {"ecu": state}})


def test_an_envelope_without_level_still_passes() -> None:
    """5.6: a v1.4 producer (no `level` anywhere) is valid."""
    assert "level" not in envelope()["tree"]["children"][0]["values"][0]
    assert codes(envelope()) == []


@pytest.mark.parametrize("level", [None, "ok"])
def test_level_null_and_ok_pass(level) -> None:
    def fn(env):
        ecu(env)["values"][0]["level"] = level

    assert codes(mutate(fn)) == []


def test_level_warn_bad_with_a_limit_node_pass() -> None:
    for level in ("warn", "bad"):
        assert codes(mutate(lambda env, lv=level: _limited(env, lv))) == []


@pytest.mark.parametrize("bad", ["high", 1, True, ""])
def test_unknown_level_is_rejected(bad) -> None:
    def fn(env):
        ecu(env)["values"][0]["level"] = bad

    assert "unknown_level" in codes(mutate(fn))


def test_level_on_a_value_without_a_reading_is_rejected() -> None:
    def fn(env):
        v = ecu(env)["values"][0]
        v.update(value=None, raw=None, classification="sna", state="absent", why=why("value_not_available"), level="ok")
        ecu(env)["state"] = "degraded"
        ecu(env)["why"] = why("entities_unavailable")
        env["tree"]["state"] = "degraded"
        env["tree"]["why"] = why("child_degraded", evidence={"children": {"ecu": "degraded"}})

    assert "level_without_reading" in codes(mutate(fn))


def test_ok_node_with_a_value_at_warn_is_rejected() -> None:
    def fn(env):
        ecu(env)["values"][0]["level"] = "warn"

    assert "node_ok_with_limit" in codes(mutate(fn))


@pytest.mark.parametrize(
    "evidence",
    [
        {},
        _limit_evidence(value=None),
        _limit_evidence(level="ok"),
        {"value": "vcc", "level": "bad", "bound": {"below": 1}},
        _limit_evidence(bound=11.5),
    ],
)
def test_limit_with_incomplete_evidence_is_rejected(evidence) -> None:
    def fn(env):
        _limited(env)
        ecu(env)["why"]["evidence"] = evidence

    assert "limit_evidence_mismatch" in codes(mutate(fn))


def test_limit_evidence_naming_a_value_at_another_level_is_rejected() -> None:
    def fn(env):
        _limited(env, "warn", evidence=_limit_evidence(level="bad"))

    assert "limit_evidence_mismatch" in codes(mutate(fn))


def _not_requested(env, state="unknown", reading=False):
    n = node("ecu.a", values=[value(key="dtc")], w=why("not_requested"), state=state)
    if not reading:
        n["values"][0].update(value=None, raw=None, classification="sna", state="absent", why=why("value_not_available"))
    ecu(env)["children"].append(n)


def test_not_requested_unknown_without_reading_passes() -> None:
    assert codes(mutate(_not_requested)) == []


def test_not_requested_on_another_state_is_rejected() -> None:
    assert "not_requested_inconsistent" in codes(mutate(lambda env: _not_requested(env, state="degraded")))


def test_not_requested_with_a_reading_is_rejected() -> None:
    assert "not_requested_inconsistent" in codes(mutate(lambda env: _not_requested(env, reading=True)))


def _switched_off(env, state="absent"):
    n = node("ecu.bat", values=[value(key="soc")], w=why("switched_off", evidence={"action": "ecu/act_switch"}),
             state=state)
    ecu(env)["children"].append(n)
    if state != "ok":
        ecu(env)["state"] = "degraded"
        ecu(env)["why"] = why("child_degraded", evidence={"children": {"ecu.bat": state}})
        env["tree"]["state"] = "degraded"
        env["tree"]["why"] = why("child_degraded", evidence={"children": {"ecu": "degraded"}})


def test_switched_off_absent_with_values_passes() -> None:
    assert codes(mutate(_switched_off)) == []


def test_switched_off_on_another_state_is_rejected() -> None:
    assert "switched_off_inconsistent" in codes(mutate(lambda env: _switched_off(env, state="degraded")))
