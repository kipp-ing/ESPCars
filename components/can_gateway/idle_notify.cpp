#include "esphome/core/defines.h"
#if defined(USE_ESP32) && defined(CAN_GATEWAY_IDLE_NOTIFY)
#include "idle_notify.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/idf_additions.h"
#include "esp_attr.h"
#include "esp_idf_version.h"
#include <atomic>

#ifdef CAN_GATEWAY_IDLE_NOTIFY_ACTIVE
#include "gateway_core.h"

namespace {
// Capture only allocations made by the setup task inside twai_new_node_onchip.
// No private IDF struct layout, no allocation in the ISR, no core patches.
struct Entry {
  std::atomic<EventGroupHandle_t> group{nullptr};
  std::atomic<uint32_t> suppressed{0};
} entries[2];
std::atomic<TaskHandle_t> capture_task{nullptr};
unsigned capture_port;
unsigned captured;
EventGroupHandle_t captured_group;
}  // namespace

namespace esphome::can_gateway {
bool idle_notify_begin(unsigned port) {
  if (port >= 2 || entries[port].group.load() != nullptr || capture_task.load() != nullptr)
    return false;
  capture_port = port;
  captured = 0;
  captured_group = nullptr;
  capture_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
  return true;
}
bool idle_notify_end(bool node_created) {
  capture_task.store(nullptr, std::memory_order_release);
  // A changed IDF allocation contract must fail before the node is enabled.
  if (!node_created || captured != 1 || captured_group == nullptr)
    return false;
  // Publish only after successful allocation. A failed driver allocation may
  // already have deleted the group; never retain its reusable handle.
  entries[capture_port].group.store(captured_group, std::memory_order_release);
  return true;
}
uint32_t idle_notify_suppressed(unsigned port) {
  return port < 2 ? entries[port].suppressed.load(std::memory_order_relaxed) : 0;
}
}  // namespace esphome::can_gateway

#endif  // CAN_GATEWAY_IDLE_NOTIFY_ACTIVE

extern "C" EventGroupHandle_t __real_xEventGroupCreateWithCaps(UBaseType_t caps);
extern "C" BaseType_t __real_xTimerPendFunctionCallFromISR(PendedFunction_t fn, void *arg, uint32_t bits,
                                                           BaseType_t *woken);
extern "C" EventGroupHandle_t __wrap_xEventGroupCreateWithCaps(UBaseType_t caps) {
  auto group = __real_xEventGroupCreateWithCaps(caps);
#ifdef CAN_GATEWAY_IDLE_NOTIFY_ACTIVE
  if (capture_task.load(std::memory_order_acquire) == xTaskGetCurrentTaskHandle()) {
    captured_group = group;
    ++captured;
  }
#endif
  return group;
}
extern "C" BaseType_t IRAM_ATTR __wrap_xTimerPendFunctionCallFromISR(PendedFunction_t fn, void *arg, uint32_t bits,
                                                                     BaseType_t *woken) {
#ifdef CAN_GATEWAY_IDLE_NOTIFY_ACTIVE
  for (auto &e : entries) {
    if (esphome::can_gateway::matches_idle_notify(fn, vEventGroupSetBitsCallback, arg,
                                                  static_cast<void *>(e.group.load(std::memory_order_acquire)), bits)) {
      e.suppressed.fetch_add(1, std::memory_order_relaxed);
      // Preserve an already requested yield; suppress only this unused job.
      return pdPASS;
    }
  }
#endif
  // Other event groups, callbacks and bit values keep the FreeRTOS contract.
  return __real_xTimerPendFunctionCallFromISR(fn, arg, bits, woken);
}
#endif
