"""`log_tap:` — the datalogger tap must arm the receive path on its own.

Found on the bench 2026-07-29 (private/notes/HANDOVER-guest-bms.md §8): a port
with **only** `log_tap: true` — no routes, no decode sensor, no `on_frame` —
never received a single frame. The config validated, the card mounted, chunks
were sealed, and the port statistics read 0.0 % bus load against a live,
several-hundred-frames/s sender: a silent failure that looks exactly like an
idle bus. The mechanism was
the no-route branch of `GatewayPort::handle_rx_isr()`, which retrieved a frame
only for an observation ring; the tap ring was never consulted, so the driver
discarded every frame before any tap (including bus load) could see it.

Three properties hold the fix together, and each fails silently on its own:

- the tap-only *shape* must stay legal — the workaround era proved people will
  otherwise add an empty `on_frame:` and pay `observe_all` for nothing;
- codegen must emit `USE_CAN_GATEWAY_LOG_TAP` exactly when a port taps, since
  the whole tap path (ring, ISR push, the RX arming below) compiles away with
  the define;
- the ISR's no-route branch must consult the tap ring, which no pytest can run
  but this suite can pin in the source, the same way the sd_logger collection
  server pins its `#ifdef` spelling.
"""

from __future__ import annotations

import ast
import inspect
from pathlib import Path
import textwrap

from .common import PORT_A, PORT_B, gateway, port, setup_c6, validate

LOG_TAP_DEFINE = "USE_CAN_GATEWAY_LOG_TAP"


def _component_dir() -> Path:
    from esphome.components import can_gateway as module

    return Path(module.__file__).parent


def _brace_block(source: str, opener: str) -> str:
    """The `{...}` block following the first occurrence of `opener`."""
    start = source.index(opener)
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unbalanced braces after {opener!r}")


# ---------------------------------------------------------------- the shape


def test_tap_only_port_accepted(set_core_config) -> None:
    """The exact shape of tests/hil/soak-orange-guest.yaml: both ports tap,
    nothing routes, decodes or triggers. This is the rig that found the bug —
    it must never need a decoy consumer to validate."""
    setup_c6(set_core_config)
    config = validate(
        gateway(
            ports=[port(PORT_A, log_tap=True), port(PORT_B, log_tap=True)],
            routes=[],
        )
    )
    assert config["ports"][0]["log_tap"] is True
    assert config["ports"][1]["log_tap"] is True
    assert "routes" not in config


def test_tap_only_single_port_accepted(set_core_config) -> None:
    """One port, one tap, nothing else — the minimal SD-logging listener."""
    setup_c6(set_core_config)
    config = validate(gateway(ports=[port(PORT_A, log_tap=True)], routes=[]))
    assert config["ports"][0]["log_tap"] is True


def test_log_tap_defaults_off(set_core_config) -> None:
    """The ring costs RAM and an ISR push per frame; nobody pays it unasked."""
    setup_c6(set_core_config)
    config = validate(gateway())
    assert config["ports"][0]["log_tap"] is False


# ---------------------------------------------------------------- the define


def test_define_emitted_exactly_when_a_port_taps(set_core_config) -> None:
    """`to_code` must emit USE_CAN_GATEWAY_LOG_TAP from inside a guard that
    reads the ports' CONF_LOG_TAP, and nowhere else. Emitted unconditionally it
    builds the ring type and the ISR push into every config; never emitted it
    leaves `log_tap: true` validating while the entire tap path — including the
    RX arming this suite exists for — compiles away in silence."""
    setup_c6(set_core_config)
    from esphome.components import can_gateway as module

    tree = ast.parse(textwrap.dedent(inspect.getsource(module.to_code)))

    def names_in(node: ast.AST) -> set[str]:
        return {child.id for child in ast.walk(node) if isinstance(child, ast.Name)}

    def add_define_calls(node: ast.AST) -> list[str]:
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

    guarded = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.If)
        and "CONF_LOG_TAP" in names_in(node.test)
        and LOG_TAP_DEFINE in add_define_calls(node)
    ]
    assert guarded, (
        f"to_code() must emit {LOG_TAP_DEFINE} from inside a guard on the "
        f"ports' CONF_LOG_TAP flags"
    )
    assert add_define_calls(tree).count(LOG_TAP_DEFINE) == 1


# ---------------------------------------------------------------- the ISR arm


def test_rx_isr_no_route_branch_consults_the_tap_ring(set_core_config) -> None:
    """The regression itself, pinned in the source: inside handle_rx_isr()'s
    `route == nullptr` branch the tap ring must be consulted and the staging
    receive reachable, under the same define spelling codegen emits. Before the
    fix that branch received only for an observation ring, which is why a
    tap-only port stayed deaf while an empty `on_frame` — whose trigger builds
    an observation ring — woke it up (the A/B in HANDOVER-guest-bms.md §8)."""
    setup_c6(set_core_config)
    source = (_component_dir() / "can_gateway.cpp").read_text()
    isr = _brace_block(source, "GatewayPort::handle_rx_isr()")
    no_route = _brace_block(isr, "if (route == nullptr)")
    assert "log_tap_ring_" in no_route, (
        "handle_rx_isr()'s no-route branch no longer consults log_tap_ring_ — "
        "a tap-only port (log_tap: true, no routes, no decode entities, no "
        "on_frame) would leave every frame to the driver and log nothing"
    )
    assert "receive_into_staging_" in no_route
    assert f"#ifdef {LOG_TAP_DEFINE}" in no_route
