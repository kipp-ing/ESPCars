#pragma once

#include <cstdint>

namespace esphome::linbus {

// Shared constants for LIN communication
static constexpr uint8_t LIN_MAX_DATA_SIZE = 8;
static constexpr uint8_t LIN_MAX_DISCOVERED_PIDS = 32;
static constexpr uint8_t LIN_MAX_ID_RQ_COUNT = 16;
static constexpr uint8_t LIN_MAX_RESPONSES = 16;

enum LinMode { LIN_MODE_COMMANDER, LIN_MODE_RESPONDER, LIN_MODE_LISTENER };

}  // namespace esphome::linbus
