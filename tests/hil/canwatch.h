// Raw CAN frame catcher — who is on this wire, by id, rate and payload.
//
// Bench diagnostics for tests/hil/, sibling of topology_live.h. The topology
// catcher decodes only our own MAC beacons, so anything that is *not ours* is
// invisible to it. This one records EVERY frame it is handed: built for the
// day an unknown device appears on a segment and the question is "what is it
// sending?". It never transmits — the config decides whether the port ACKs.
//
// First sight of an id logs immediately (that is the "a guest just spoke"
// moment). After that the id is summarised only by the periodic report —
// count, rate, latest payload, which bytes moved since last report — so a
// chatty guest cannot flood the console off the USB CDC and the cadence
// stays readable.
#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

using esphome::millis;

namespace canwatch {

static const char *const TAG = "watch";

// 64 distinct (port, id) streams is far beyond any single guest; past that the
// overflow counter keeps the loss honest instead of silent.
static const uint8_t MAX_IDS = 64;

struct Entry {
  const char *port;  ///< nullptr = free slot
  uint32_t id;
  bool ext;
  bool rtr;
  uint8_t dlc;
  uint8_t data[8];
  uint8_t moved;  ///< bit i = data[i] changed since the last report
  uint32_t count;
  uint32_t reported;  ///< count at the last report, for the delta
  uint32_t first_ms;
  uint32_t last_ms;
};

inline Entry g_entries[MAX_IDS];
inline uint32_t g_overflow = 0;
inline uint32_t g_last_rx_ms = 0;

/// True if any frame arrived within the last `window_ms` — drives the LED.
inline bool rx_recent(uint32_t window_ms) {
  return g_last_rx_ms != 0 && (millis() - g_last_rx_ms) <= window_ms;
}

/// Hex payload with a '*' after every byte that moved since the last report:
/// "01 5A* 00 ..." — the moving bytes are usually the counter/signal and the
/// static ones the identity, which is exactly the split you read first.
inline void format_data(const Entry &e, char *out, size_t cap) {
  size_t n = 0;
  if (e.rtr) {
    std::snprintf(out, cap, "(rtr)");
    return;
  }
  out[0] = '\0';
  for (uint8_t i = 0; i < e.dlc && i < 8; i++) {
    const int w = std::snprintf(out + n, cap - n, "%02X%c ", e.data[i], (e.moved >> i) & 1 ? '*' : ' ');
    if (w < 0 || n + static_cast<size_t>(w) >= cap)
      break;
    n += static_cast<size_t>(w);
  }
}

/// Feed one received frame in. Bounded scan, no allocation — cheap enough to
/// sit directly on on_frame.
inline void observe(const char *port, uint32_t can_id, const uint8_t *data, uint8_t dlc, bool extended, bool rtr) {
  const uint32_t now = millis();
  g_last_rx_ms = now;
  const uint8_t len = dlc > 8 ? 8 : dlc;

  Entry *free_slot = nullptr;
  for (auto &e : g_entries) {
    if (e.port == nullptr) {
      if (free_slot == nullptr)
        free_slot = &e;
      continue;
    }
    if (e.id != can_id || e.ext != extended || e.rtr != rtr || std::strcmp(e.port, port) != 0)
      continue;
    if (!rtr) {
      for (uint8_t i = 0; i < len; i++)
        if (e.data[i] != data[i])
          e.moved |= static_cast<uint8_t>(1u << i);
      std::memcpy(e.data, data, len);
    }
    e.dlc = dlc;
    e.count++;
    e.last_ms = now;
    return;
  }

  if (free_slot == nullptr) {
    g_overflow++;
    return;
  }
  Entry &e = *free_slot;
  e.port = port;
  e.id = can_id;
  e.ext = extended;
  e.rtr = rtr;
  e.dlc = dlc;
  e.moved = 0;
  if (!rtr)
    std::memcpy(e.data, data, len);
  e.count = 1;
  e.reported = 0;
  e.first_ms = e.last_ms = now;

  char hex[64];
  format_data(e, hex, sizeof(hex));
  ESP_LOGI(TAG, "%s NEW id=0x%0*X%s dlc=%u data=%s", port, extended ? 8 : 3, static_cast<unsigned>(can_id),
           extended ? " ext" : "", dlc, hex);
}

/// Periodic summary: one line per live (port, id) stream with the delta since
/// the last call, so both a steady 100 Hz sender and a one-shot are legible.
/// `window_ms` is the caller's interval — the rate divisor.
inline void report(uint32_t window_ms) {
  const uint32_t now = millis();
  uint8_t live = 0;
  for (auto &e : g_entries) {
    if (e.port == nullptr)
      continue;
    live++;
    const uint32_t delta = e.count - e.reported;
    e.reported = e.count;
    char hex[64];
    format_data(e, hex, sizeof(hex));
    if (delta == 0) {
      ESP_LOGW(TAG, "%s id=0x%0*X%s n=%" PRIu32 " QUIET %.1fs (last data=%s)", e.port, e.ext ? 8 : 3,
               static_cast<unsigned>(e.id), e.ext ? " ext" : "", e.count, (now - e.last_ms) / 1000.0f, hex);
    } else {
      ESP_LOGI(TAG, "%s id=0x%0*X%s n=%" PRIu32 " (+%" PRIu32 ", %.1f/s) dlc=%u data=%s", e.port, e.ext ? 8 : 3,
               static_cast<unsigned>(e.id), e.ext ? " ext" : "", e.count, delta, delta * 1000.0f / window_ms, e.dlc,
               hex);
    }
    e.moved = 0;
  }
  if (g_overflow > 0)
    ESP_LOGW(TAG, "id table full - %" PRIu32 " frame(s) from further ids uncounted", g_overflow);
  if (live == 0)
    ESP_LOGI(TAG, "nothing heard yet on any port");
}

}  // namespace canwatch
