"""Grep-level checks for the handler rules of spec 6.2 (v1.3) that no host/cfg tier can reach.

PS-M3-09 (rev support), PS-M2-08/M3-07 (503 on lock timeout), shutdown rule (B1), request body
state reset (N1). These are static checks on components/portal/portal.cpp: they prove the code
shape, the bench proves the behaviour (checklist in tests/portal/BENCH.md).
"""

from __future__ import annotations

import re

from .common import COMPONENT

CPP = (COMPONENT / "portal.cpp").read_text()
HDR = (COMPONENT / "portal.h").read_text()


def _bodies(name: str, text: str) -> list[str]:
    out = []
    for m in re.finditer(rf"\b{name}\s*\([^)]*\)\s*(?:const\s*)?(?:override\s*)?\{{", text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        out.append(text[m.end(): i])
    return out


def test_shutdown_destroys_nothing_the_httpd_task_uses() -> None:
    """Shutdown rule (v1.3 / B1): on_shutdown()/teardown()/destructor never delete the mutex or buffers."""
    banned = [r"vSemaphoreDelete", r"\bdelete\b", r"\bfree\s*\(", r"mutex_\s*=\s*nullptr"]
    for fn in ("on_shutdown", "teardown", "~Portal"):
        for body in _bodies(fn, CPP) + _bodies(fn, HDR):
            for pat in banned:
                assert not re.search(pat, body), f"{pat} inside {fn}"


def test_mutex_is_created_once_and_never_deleted_anywhere() -> None:
    """Shutdown rule (v1.3): the portal has no code path that deletes its mutex."""
    assert "vSemaphoreDelete" not in CPP + HDR


def test_post_body_state_is_reset_per_request() -> None:
    """N1: the streamed-body state is keyed to the request and restarted at index 0 / for a new request,
    and cleared when the POST has been served, so a body from request N can never be read by N+1."""
    assert re.search(r"index\s*==\s*0\s*\|\|\s*this->body_req_\s*!=\s*request", CPP)
    assert re.search(r"body_stored_\s*=\s*0", CPP) and re.search(r"body_req_\s*=\s*nullptr", CPP)
    # the evaluator only trusts the buffer when it belongs to this very request
    assert re.search(r"body_req_\s*==\s*request", CPP)


def test_action_route_answers_503_without_recording_on_lock_timeout() -> None:
    """PS-M3-07 / 6.2: the 200 ms lock timeout is answered 503 with an empty body, before anything is recorded."""
    m = re.search(r"xSemaphoreTake\(this->mutex_,\s*pdMS_TO_TICKS\(200\)\)[^\n]*\{\n(.*?)\n\s*return;", CPP, re.S)
    assert m, "200 ms lock timeout branch not found"
    assert "503" in m.group(1)
    assert not re.search(r"record|result|rt\.", m.group(1))


def test_state_route_503_when_the_snapshot_cannot_be_taken() -> None:
    """6.1: producer cannot snapshot itself -> 503."""
    assert CPP.count("503 Service Unavailable") >= 2
