"""D-101 defaults: wrappers only on supported IDF/single-core configurations."""
from pathlib import Path
from unittest.mock import patch

import pytest
from esphome.const import KEY_FRAMEWORK_VERSION, PlatformFramework
from esphome.components.esp32.const import KEY_VARIANT
import esphome.config_validation as cv


@pytest.mark.parametrize("variant,enabled", [
    ("ESP32C6", True), ("ESP32C3", True), ("ESP32H2", True),
    ("ESP32S2", True), ("ESP32", False), ("ESP32S3", False),
    ("ESP32P4", False), ("ESP32C5", False),
])
@pytest.mark.parametrize("version", [(5, 4, 3), (5, 5, 0), (5, 5, 4), (5, 6, 0)])
@pytest.mark.parametrize("framework", [PlatformFramework.ESP32_IDF, PlatformFramework.ESP32_ARDUINO])
def test_idle_flags(set_core_config, variant, enabled, version, framework):
    set_core_config(framework, core_data={KEY_FRAMEWORK_VERSION: cv.Version(*version)},
                    platform_data={KEY_VARIANT: variant})
    from esphome.components.can_gateway import idle_notify_build_flags
    flags = idle_notify_build_flags()
    if enabled and version[:2] == (5, 5) and framework == PlatformFramework.ESP32_IDF:
        assert flags == (
            "-DCAN_GATEWAY_IDLE_NOTIFY",
            "-Wl,--wrap=xEventGroupCreateWithCaps",
            "-Wl,--wrap=xTimerPendFunctionCallFromISR",
            "-Wl,-u,__wrap_xEventGroupCreateWithCaps",
            "-Wl,-u,__wrap_xTimerPendFunctionCallFromISR",
        )
    else:
        assert flags == ()


def test_codegen_emits_component_owned_flags(set_core_config):
    from .common import setup_c6, gateway, validate
    setup_c6(set_core_config)
    from esphome.components import can_gateway as module
    config = validate(gateway())
    # Run the real coroutine through its first await; flags precede all IDF
    # setup, so no mocked copy of the codegen logic is involved.
    async def halt(*args):
        raise RuntimeError("registered")
    with patch.object(module.cg, "add_build_flag") as add, \
         patch.object(module, "include_builtin_idf_component"), \
         patch.object(module, "add_idf_sdkconfig_option"), \
         patch.object(module.cg, "add_define"), \
         patch.object(module.cg, "new_Pvariable"), \
         patch.object(module.cg, "register_component", halt):
        coroutine = module.to_code(config)
        with pytest.raises(RuntimeError, match="registered"):
            coroutine.send(None)
        assert [call.args[0] for call in add.call_args_list] == list(module.idle_notify_build_flags())


def test_filter_guards_and_diagnostics(set_core_config):
    from esphome.components import can_gateway as module
    from .test_log_tap import _brace_block
    source = (Path(module.__file__).parent / "can_gateway.cpp").read_text()
    body = _brace_block(source, "void GatewayPort::try_hw_filter_offload_")
    assert "#ifndef USE_CAN_GATEWAY_STATS" in body
    assert body.index("rx_taps_active_()") < body.index("derive_allowlist_filter")
    assert body.index("route->observed_") < body.index("derive_allowlist_filter")
    header = (Path(module.__file__).parent / "can_gateway.h").read_text()
    taps = _brace_block(header, "bool rx_taps_active_")
    for consumer in ("snapshot_ring_", "observe_ring_", "log_tap_ring_", "isr_frame_hook_"):
        assert consumer in taps
    assert "hw_filter_reason_" in _brace_block(source, "void CanGateway::dump_config")
    assert "inject_refused" in _brace_block(source, "void GatewayPort::log_statistics_")
