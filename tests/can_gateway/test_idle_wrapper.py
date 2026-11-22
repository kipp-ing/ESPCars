"""Compile the actual wrapper against a minimal FreeRTOS ABI on the host."""
from pathlib import Path
import subprocess

import pytest


@pytest.mark.parametrize("mode", ["active", "test-off", "dual-core", "other-idf", "no-flags"])
def test_production_idle_wrapper(tmp_path, mode):
    from esphome.components import can_gateway as module
    component = Path(module.__file__).parent
    sdk = tmp_path / "sdk"
    headers = {
        "esphome/core/defines.h": "",
        "sdkconfig.h": "",
        "esp_attr.h": "#define IRAM_ATTR\n",
        "esp_idf_version.h": "#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n",
        "freertos/FreeRTOS.h": '''#pragma once
#include <cstdint>
using UBaseType_t = unsigned;
using BaseType_t = int;
using EventGroupHandle_t = void *;
using TaskHandle_t = void *;
using PendedFunction_t = void (*)(void *, uint32_t);
constexpr BaseType_t pdPASS = 1;
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle();
extern "C" void vEventGroupSetBitsCallback(void *, uint32_t);
''',
    }
    for name in ("task.h", "event_groups.h", "timers.h", "idf_additions.h"):
        headers[f"freertos/{name}"] = '#include "FreeRTOS.h"\n'
    for name, contents in headers.items():
        target = sdk / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents)
    test = tmp_path / "wrapper.cpp"
    test.write_text('''#include "idle_notify.cpp"
#include <cassert>
#if defined(CAN_GATEWAY_IDLE_NOTIFY)
namespace {
int tasks[2], groups[8];
void *current_task = &tasks[0];
void *allocated_group = &groups[0];
unsigned real_calls = 0;
}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle() { return current_task; }
extern "C" void vEventGroupSetBitsCallback(void *, uint32_t) {}
void other_callback(void *, uint32_t) {}
extern "C" EventGroupHandle_t __real_xEventGroupCreateWithCaps(UBaseType_t) { return allocated_group; }
extern "C" BaseType_t __real_xTimerPendFunctionCallFromISR(PendedFunction_t, void *, uint32_t, BaseType_t *) {
  ++real_calls;
  return 7;
}
#endif
int main() {
#if defined(CAN_GATEWAY_IDLE_NOTIFY)
  BaseType_t woken = 9;
#ifdef CAN_GATEWAY_IDLE_NOTIFY_ACTIVE
  using namespace esphome::can_gateway;
  assert(!idle_notify_begin(2));
  assert(idle_notify_begin(0));
  assert(!idle_notify_begin(1));
  // Allocations by another task during the capture must remain unowned.
  current_task = &tasks[1];
  allocated_group = &groups[1];
  __wrap_xEventGroupCreateWithCaps(0);
  current_task = &tasks[0];
  allocated_group = &groups[0];
  assert(__wrap_xEventGroupCreateWithCaps(0) == &groups[0]);
  assert(idle_notify_end(true));
  assert(!idle_notify_begin(0));
  assert(__wrap_xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, &groups[0], 1, &woken) == pdPASS);
  assert(woken == 9 && real_calls == 0);
  assert(idle_notify_suppressed(0) == 1 && idle_notify_suppressed(2) == 0);
  assert(__wrap_xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, &groups[1], 1, &woken) == 7);
  assert(__wrap_xTimerPendFunctionCallFromISR(other_callback, &groups[0], 1, &woken) == 7);
  assert(__wrap_xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, &groups[0], 3, &woken) == 7);
  assert(__wrap_xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, nullptr, 1, &woken) == 7);
  assert(real_calls == 4);
  // Failed node allocation must not leave a handle that can later be reused.
  assert(idle_notify_begin(1));
  allocated_group = &groups[2];
  __wrap_xEventGroupCreateWithCaps(0);
  assert(!idle_notify_end(false));
  assert(__wrap_xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, &groups[2], 1, &woken) == 7);
  // Changed allocation contracts fail closed without publishing any group.
  assert(idle_notify_begin(1));
  assert(!idle_notify_end(true));
  assert(idle_notify_begin(1));
  __wrap_xEventGroupCreateWithCaps(0);
  allocated_group = &groups[3];
  __wrap_xEventGroupCreateWithCaps(0);
  assert(!idle_notify_end(true));
  assert(__wrap_xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, &groups[3], 1, &woken) == 7);
#else
  assert(__wrap_xEventGroupCreateWithCaps(0) == &groups[0]);
  assert(__wrap_xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, &groups[0], 1, &woken) == 7);
  assert(woken == 9 && real_calls == 1);
#endif
#endif
}
''')
    flags = ["-DUSE_ESP32", "-DCONFIG_FREERTOS_UNICORE=1", "-DESP_IDF_VERSION=50504"]
    if mode != "no-flags":
        flags += ["-DCAN_GATEWAY_IDLE_NOTIFY"]
    if mode == "test-off":
        flags += ["-DCAN_GATEWAY_TEST_DISABLE_IDLE_NOTIFY"]
    if mode == "dual-core":
        flags[1] = "-DCONFIG_FREERTOS_UNICORE=0"
    if mode == "other-idf":
        flags[2] = "-DESP_IDF_VERSION=50403"
    binary = tmp_path / "wrapper"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(sdk), "-I", str(component), str(test), "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True)
