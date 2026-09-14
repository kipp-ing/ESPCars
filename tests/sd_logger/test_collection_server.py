"""M6 Phase B — the collection server, held against the client that already exists.

`script/sdlog_collect.py` and the `FakeDevice`/`_Handler` pair in `test_collect.py`
are the wire contract, and `test_collect.py` proves *the client*. Its 66 tests stay
green whatever the firmware does, which is the whole hazard: a device that spells a
route differently, or that never builds a server at all, looks exactly like a
healthy boot from every angle except an operator's failed pull.

`components/sd_logger/collection_server.cpp` is IDF-bound — `esp_http_server`,
FATFS, FreeRTOS — so it cannot run in `tests/host/` the way `collection_policy.h`
does, and nothing in CI executes it. What CI *can* do is check the handful of facts
that are stated in two places at once and would otherwise be free to drift apart:
the routes, the build gate, and the status codes the contract calls traps. Those
are the tests below. They are deliberately narrow; behaviour is proved on the bench
against the real collector (`docs/sdlog-collection-design.md` §12).
"""

from __future__ import annotations

import ast
import inspect
from pathlib import Path
import textwrap

import pytest

from .common import setup_c6


def _component_dir() -> Path:
    from esphome.components import sd_logger as module

    return Path(module.__file__).parent


def _server_source() -> str:
    return (_component_dir() / "collection_server.cpp").read_text()


def _collector_source() -> str:
    # script/ sits two levels above tests/sd_logger/.
    return (Path(__file__).resolve().parents[2] / "script" / "sdlog_collect.py").read_text()


# ------------------------------------------------------------------- the routes


@pytest.mark.parametrize(
    ("client_path", "firmware_literal"),
    [
        ("/sdlog/index", '"/sdlog/index"'),
        ("/sdlog/f/", '"/sdlog/f/"'),
        ("/sdlog/done/", '"/sdlog/done/"'),
    ],
)
def test_every_path_the_collector_builds_is_a_route_the_firmware_serves(
    client_path: str, firmware_literal: str
) -> None:
    """The three URLs `sdlog_collect.py` can emit, spelled the same on both sides.

    A route typo is the cheapest possible way to ship a device nobody can collect
    from: the client's 4xx is "not retried, fatal", so a misspelled `/sdlog/done/`
    turns into "every chunk fails to confirm" with a perfectly healthy index and no
    error anywhere on the device. Both spellings are literals in files this repo
    owns, so nothing stops them being compared.
    """
    collector = _collector_source()
    assert client_path in collector, (
        f"{client_path!r} is not in script/sdlog_collect.py any more — the client "
        f"moved and this test is now pinning the wrong string."
    )
    server = _server_source()
    assert firmware_literal in server, (
        f"the collector requests {client_path!r} but "
        f"components/sd_logger/collection_server.cpp has no {firmware_literal} "
        f"route literal. A route the device does not serve answers 404, which the "
        f"client treats as fatal for that chunk and never retries."
    )


def test_the_status_route_is_served_even_though_no_client_calls_it() -> None:
    """`GET /sdlog/status` is the operator's only view of the index.

    The collector never calls it — grep `sdlog_collect.py` — so nothing else would
    ever notice it going missing, and it is the only thing that distinguishes "this
    device has nothing to give" from "this device's chunk index overflowed and it
    can no longer see its own files" (`index_refused`, design §11 V26).
    """
    server = _server_source()
    assert '"/sdlog/status"' in server
    assert "index_refused" in server, (
        "GET /sdlog/status must report `index_refused`: a device serving an empty "
        "index because the index was full looks exactly like a device with nothing "
        "to give, and that state cost a 12-minute bench soak on 2026-07-28."
    )


def test_fsync_failure_enters_the_single_failure_path_and_counts_buffered_records() -> None:
    """An fsync error must not leave a writable-looking logger with an erased RAM tail."""
    source = _logger_source()
    for signature in ("bool SdLogger::close_file_", "void SdLogger::sync_file_"):
        body = _function_body(source, signature)
        assert "if (fsync(this->fd_) != 0)" in body
        assert 'this->enter_failed_("fsync")' in body
    failed = _function_body(source, "void SdLogger::enter_failed_")
    assert "this->write_lost_.fetch_add(this->unflushed_records_.count()" in failed
    assert failed.index("write_lost_.fetch_add") < failed.index("this->block_.reset()")


def test_outage_marker_baselines_are_not_reset_by_a_new_file_header() -> None:
    """A recovered file must publish ring/tap losses accrued while no prior file could."""
    header = _function_body(_logger_source(), "void SdLogger::write_file_header_")
    assert "marked_dropped_ =" not in header
    assert "marked_dropped =" not in header


def test_static_tap_drain_stack_passes_freertos_a_word_depth_not_a_byte_count() -> None:
    """A static FreeRTOS task must advertise the length of its actual backing array.

    `xTaskCreateStatic` takes a count of `StackType_t` entries.  Passing the 3072-byte budget
    there while allocating only `3072 / sizeof(StackType_t)` entries makes FreeRTOS write up to
    12 KiB into a 3 KiB array on the C6.  The resulting overrun is load-sensitive and can scribble
    a logger buffer without a malformed input record, so keep the allocation and call tied to one
    words constant.
    """
    source = _logger_source()
    assert "TAP_DRAIN_STACK_WORDS = TAP_DRAIN_STACK_BYTES / sizeof(StackType_t)" in source
    assert "static StackType_t tap_drain_stack[TAP_DRAIN_STACK_WORDS]" in source
    call = _function_body(source, "void SdLogger::setup")
    assert '"sdlog_tap", TAP_DRAIN_STACK_WORDS, this' in call
    assert '"sdlog_tap", TAP_DRAIN_STACK_BYTES, this' not in call


# --------------------------------------------------------------- the build gate


def _to_code_tree() -> ast.Module:
    from esphome.components import sd_logger as module

    return ast.parse(textwrap.dedent(inspect.getsource(module.to_code)))


def _names_in(node: ast.AST) -> set[str]:
    return {child.id for child in ast.walk(node) if isinstance(child, ast.Name)}


def _add_define_calls(node: ast.AST) -> list[str]:
    """Every `cg.add_define("...")` literal reachable from `node`."""
    found: list[str] = []
    for child in ast.walk(node):
        if not isinstance(child, ast.Call):
            continue
        func = child.func
        if not isinstance(func, ast.Attribute) or func.attr != "add_define":
            continue
        if child.args and isinstance(child.args[0], ast.Constant):
            found.append(child.args[0].value)
    return found


SERVER_DEFINE = "USE_SD_LOGGER_COLLECTION_SERVER"


def test_the_server_is_built_exactly_when_serve_is_true(set_core_config) -> None:
    """`serve:` is the network half of `collection:` and it has to gate the code.

    Both directions fail silently and both have already been paid for once by this
    component. A define emitted unconditionally builds an `esp_http_server` into the
    bench rigs that run `serve: false` on purpose — WiFi brings tasks above the LIN
    task's priority and its effect on bus timing is unmeasured (design §9.1), which
    is the entire reason that shape exists. A define never emitted leaves `serve:
    true` validating, `dump_config` reporting a port, and nothing listening on it.

    Asserted on the AST rather than on the text so a reformat, a renamed local or a
    comment mentioning the define cannot make it pass or fail.
    """
    setup_c6(set_core_config)
    tree = _to_code_tree()

    guarded = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.If)
        and "CONF_SERVE" in _names_in(node.test)
        and SERVER_DEFINE in _add_define_calls(node)
    ]
    assert guarded, (
        f"sd_logger's to_code() must emit {SERVER_DEFINE} from inside an "
        f"`if ...[CONF_SERVE]:` branch. Emitted unconditionally it builds a server "
        f"into every `serve: false` config, including the bench rigs that are "
        f"deliberately WiFi-free; not emitted at all it leaves `serve: true` "
        f"accepted and nothing listening."
    )

    # ...and nowhere else. One emission site, inside the guard.
    assert _add_define_calls(tree).count(SERVER_DEFINE) == 1


def test_the_firmware_guards_the_server_on_the_same_define(set_core_config) -> None:
    """The C++ side has to spell the gate the way codegen emits it.

    An `#ifdef` on a define nothing emits compiles away in silence — no warning, no
    error, a build that is exactly the size it would be without the feature — and
    the only symptom is a port nobody answers on. `USE_*` defines come from the
    component's own Python (`cg.add_define`) and never from esphome core's
    `defines.h`, so the two spellings have nothing but this test holding them
    together.
    """
    setup_c6(set_core_config)
    component = _component_dir()
    assert f"#ifdef {SERVER_DEFINE}" in (component / "collection_server.h").read_text()
    assert f"#ifdef {SERVER_DEFINE}" in (component / "sd_logger.cpp").read_text()
    assert f"#ifdef {SERVER_DEFINE}" in (component / "sd_logger.h").read_text()


# ------------------------------------------------------------ the status traps


def test_a_successful_confirm_is_exactly_200_and_never_204() -> None:
    """`POST /sdlog/done/<name>` -> `200`, and 2xx is not good enough.

    The client's confirm loop is `if response.status == 200`, not "any 2xx": a
    `204 No Content` — the tempting choice for an empty ack — is reported as
    `confirm: HTTP 204`, the chunk is never marked collected, and it comes back on
    every run forever while the card fills behind it. Nothing on the device would
    say so.
    """
    server = _server_source()
    assert '"200 OK"' in server
    # Matched as a *status literal* — a leading quote, or the raw status line the
    # streamed chunk body writes by hand — so the prose above the handler, which has
    # to name 204 to explain why it is wrong, does not trip its own test.
    for spelling in ('"204', "HTTP/1.1 204"):
        assert spelling not in server, (
            f"collection_server.cpp emits a {spelling!r} status. A confirm answered "
            f"204 is read by sdlog_collect.py as a failure "
            f"(`if response.status == 200`), so the chunk is never collected and "
            f"never deleted."
        )


def test_the_range_answers_are_206_and_416_and_never_400() -> None:
    """The two status codes a resume rests on, and the one that must not appear.

    `206` is a *promise* that the body begins at the requested offset, so it is only
    ever sent alongside a `Content-Range` and a `Content-Length` measuring that body.
    `416` is what tells the client its local partial is longer than the chunk on
    offer, and it special-cases it: drop the partial, retry inside the same run.

    `400` is the fake device's answer to a Range it cannot parse, and it is the one
    answer the firmware must *not* copy: the client treats every non-416 4xx as
    fatal for that chunk, while simply ignoring an unparseable Range and sending the
    whole file as `200` costs it nothing but bandwidth (contract §3.7, §7).
    """
    server = _server_source()
    # The 206 is part of a hand-written status line rather than a bare literal: it is
    # the one response whose `Content-Length` must measure the body and not the file,
    # which rules out both of the IDF's own send helpers.
    assert "HTTP/1.1 206 Partial Content\\r\\n" in server
    assert '"416 Range Not Satisfiable"' in server
    assert "Content-Range: bytes " in server
    assert "400 Bad Request" not in server, (
        "an unparseable Range must be ignored and answered 200 with the whole "
        "file, not 400: the client treats any non-416 4xx as fatal for the chunk."
    )


# --------------------------------------------------------- when the server starts


def _logger_source() -> str:
    return (_component_dir() / "sd_logger.cpp").read_text()


def _function_body(source: str, signature: str) -> str:
    """One top-level function's text, signature to the `}` in column 0.

    Safe because the file is clang-formatted to ESPHome style, where a top-level
    definition is the only thing that closes on a brace in column 0.
    """
    start = source.index(signature)
    return source[start : source.index("\n}\n", start)]


def test_the_server_is_started_from_loop_and_never_from_setup() -> None:
    """`httpd_start()` must not run at `setup_priority::DATA`. That is a boot loop.

    sd_logger is `setup_priority::DATA` (600) and esphome's wifi component is
    `setup_priority::WIFI` (250) — higher runs first, so `SdLogger::setup()` returns
    *before* lwip is initialised. `httpd_start()` opens its control socket straight
    away, so the first lwip call locks a TCPIP core mutex that is still null and
    FreeRTOS aborts:

        assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))

    on every boot, about 1.3 s in, forever. Measured on Mr. Orange 2026-07-29 — the
    first time Phase B was flashed on hardware at all, and it never reached the point
    of serving a byte.

    Nothing else in this repo can catch it. `tests/build/sd_logger/common.yaml`
    compiles `serve: true` but CI never runs the binary; `test_collect.py`'s 66 cases
    drive a Python `FakeDevice`; `tests/host/` is IDF-free by construction. The fix is
    ordering, not error handling — `httpd_start()` never gets to return a code — so
    the call site is the thing worth pinning, and that is what this asserts.

    Note the requirement this does NOT introduce: binding without an association is
    still deliberate and still works (the socket takes 0.0.0.0, and a board whose card
    failed is exactly the one an operator wants to reach). *Initialisation* is needed,
    *association* is not; only the first was ever in question.
    """
    source = _logger_source()
    start_call = "collection_server_.start("

    assert start_call in _function_body(source, "void SdLogger::loop() {"), (
        "SdLogger::loop() must be what starts the collection server: esphome runs "
        "loop() only once every component's setup() has returned, which is the "
        "earliest point at which lwip is guaranteed to exist."
    )
    assert start_call not in _function_body(source, "void SdLogger::setup() {"), (
        "SdLogger::setup() reaches httpd_start() through collection_server_.start(). "
        "At setup_priority::DATA that runs before esphome's wifi component has "
        "initialised lwip, and the board dies inside FreeRTOS with "
        "'assert failed: xQueueSemaphoreTake queue.c:1709' on every boot."
    )
