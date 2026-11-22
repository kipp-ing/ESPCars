#pragma once

#include "esphome/core/defines.h"
#include <cstdint>

#if defined(USE_ESP32) && defined(CAN_GATEWAY_IDLE_NOTIFY)
#include "sdkconfig.h"
#include "esp_idf_version.h"
#if CONFIG_FREERTOS_UNICORE && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0) && \
    ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 6, 0) && !defined(CAN_GATEWAY_TEST_DISABLE_IDLE_NOTIFY)
#define CAN_GATEWAY_IDLE_NOTIFY_ACTIVE
#endif

namespace esphome::can_gateway {
// Only the gateway's own node allocation is captured. These nodes must never
// use twai_node_transmit_wait_all_done(): their BIT0 notification is omitted.
bool idle_notify_begin(unsigned port);
bool idle_notify_end(bool node_created);
uint32_t idle_notify_suppressed(unsigned port);
}  // namespace esphome::can_gateway
#endif
