#pragma once

#include <cstdint>

namespace esphome {
namespace sd_logger {

// One captured frame. Packed to ~20 bytes so the RAM ring stays small: a
// 500 ms SD stall at 60 % of 500 kbps (~1050 frames) fits in ~21 KB.
struct LogRecord {
  uint32_t t_us;   // esp_timer timestamp, wraps ~71 min (fine for deltas)
  uint32_t id;     // CAN or LIN identifier
  uint8_t source;  // producer tag (0 = user/action, room for CAN-port/LIN)
  uint8_t flags;   // bit0 extended, bit1 rtr, bit2 tx, bit3 shed, bit7 truncated
  uint8_t len;     // payload length, 0..8
  uint8_t data[8];
};

// Record flag bits.
static const uint8_t REC_FLAG_EXTENDED = 1 << 0;
static const uint8_t REC_FLAG_RTR = 1 << 1;
static const uint8_t REC_FLAG_TX = 1 << 2;
// "Was on the wire but the gateway did not forward it" — the frame Issue #1 is
// about, and the one a log must never lose. Deliberately NOT bit 2:
// can_gateway's TAP_FLAG_SHED is 0x04 and means this, while REC_FLAG_TX is 0x04
// and means something else entirely. See tap_translate.h.
static const uint8_t REC_FLAG_SHED = 1 << 3;
static const uint8_t REC_FLAG_TRUNCATED = 1 << 7;

}  // namespace sd_logger
}  // namespace esphome
