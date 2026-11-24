#pragma once
#include "hal.h"
namespace esphome {
struct Application {
  uint32_t get_loop_component_start_time() { return millis(); }
};
inline Application App;
}  // namespace esphome
