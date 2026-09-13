"""Direct RX-ISR hook source contracts.

The hook is deliberately consumer-owned: its Python code emits the define and
registers the function before can_gateway setup. This suite pins the component
side so a future diagnostics change cannot silently move the deadline-sensitive
call behind a ring or compile it into ordinary configurations.
"""

from __future__ import annotations

from pathlib import Path

from .test_log_tap import _brace_block


HOOK_DEFINE = "USE_CAN_GATEWAY_ISR_HOOK"


def _component_dir() -> Path:
    from esphome.components import can_gateway as module

    return Path(module.__file__).parent


def test_isr_hook_is_fully_conditional_and_first_rx_tap(set_core_config) -> None:
    source = (_component_dir() / "can_gateway.h").read_text()
    assert f"#ifdef {HOOK_DEFINE}" in source
    assert "using IsrFrameHook = void (*)(void *ctx, uint32_t t_us" in source
    assert "void set_isr_frame_hook(IsrFrameHook fn, void *ctx)" in source
    assert "std::function" not in _brace_block(source, "void set_isr_frame_hook")

    taps = _brace_block(source, "void run_rx_taps_")
    hook = taps.index("isr_frame_hook_(")
    stats = taps.index("#ifdef USE_CAN_GATEWAY_STATS")
    assert hook < stats
    assert "esp_timer_get_time()" in taps[:stats]

    # The absent configuration must not be enabled by can_gateway's own Python:
    # only the external consumer is allowed to emit this define.
    python = (_component_dir() / "__init__.py").read_text()
    assert HOOK_DEFINE not in python


def test_hook_arms_a_route_less_port_and_inject_is_task_safe(set_core_config) -> None:
    header = (_component_dir() / "can_gateway.h").read_text()
    source = (_component_dir() / "can_gateway.cpp").read_text()
    isr = _brace_block(source, "GatewayPort::handle_rx_isr()")
    no_route = _brace_block(isr, "if (route == nullptr)")
    assert f"#ifdef {HOOK_DEFINE}" in no_route
    assert "isr_frame_hook_ != nullptr" in no_route
    assert "receive_into_staging_" in no_route
    assert "active = active || this->isr_frame_hook_ != nullptr;" in header

    inject = _brace_block(source, "bool GatewayPort::inject")
    assert "portENTER_CRITICAL(&this->mux_)" in inject
    assert "twai_node_transmit" in inject
    assert "outstanding_.push" in inject
    assert "compare_exchange_strong" in inject
    assert "Safe from ESPHome loop context or a FreeRTOS task" in header
