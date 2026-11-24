#pragma once
#include "esp_timer.h"
namespace esphome {
inline uint32_t millis() { return esp_timer_get_time() / 1000; }
}  // namespace esphome
