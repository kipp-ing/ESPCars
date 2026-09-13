#pragma once

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace esphome {
namespace sd_logger {

/// The complete, additive wire view of `GET /sdlog/status`.
///
/// Kept ESPHome- and IDF-free so the host tests exercise the production JSON
/// formatter, including its no-truncation boundary.
struct StatusFields {
  const char *device;
  uint32_t sealed;
  uint32_t confirmed;
  uint32_t card_percent;
  uint32_t discarded_chunks;
  uint64_t discarded_bytes;
  bool has_oldest_uncollected;
  uint32_t oldest_uncollected;
  uint32_t index_refused;
  bool mounted;
  bool degraded;
  uint32_t records;
  uint32_t dropped;
  uint32_t card_dropped;
  uint32_t write_lost;
  uint32_t tap_shutdown_lost;
  uint32_t tap_accepted;
  uint32_t tap_drained;
  uint32_t tap_record_ring_accepted;
};

/// Formats the status response and returns `snprintf`'s result. A card percent
/// above 100 means its fill is not known yet and is intentionally rendered null.
inline int format_status_json(char *out, size_t len, const StatusFields &fields) {
  char oldest_text[16] = "null";
  if (fields.has_oldest_uncollected)
    snprintf(oldest_text, sizeof(oldest_text), "%" PRIu32, fields.oldest_uncollected);
  char fill_text[16] = "null";
  if (fields.card_percent <= 100)
    snprintf(fill_text, sizeof(fill_text), "%" PRIu32, fields.card_percent);

  return snprintf(out, len,
                  "{\"device\":\"%s\",\"sealed\":%" PRIu32 ",\"confirmed\":%" PRIu32
                  ",\"card_percent\":%s,\"discarded_chunks\":%" PRIu32 ",\"discarded_bytes\":%" PRIu64
                  ",\"oldest_uncollected\":%s,\"index_refused\":%" PRIu32 ",\"mounted\":%s,\"degraded\":%s"
                  ",\"records\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"card_dropped\":%" PRIu32
                  ",\"write_lost\":%" PRIu32 ",\"tap_shutdown_lost\":%" PRIu32 ",\"tap_accepted\":%" PRIu32
                  ",\"tap_drained\":%" PRIu32 ",\"tap_record_ring_accepted\":%" PRIu32 "}",
                  fields.device, fields.sealed, fields.confirmed, fill_text, fields.discarded_chunks,
                  fields.discarded_bytes, oldest_text, fields.index_refused, fields.mounted ? "true" : "false",
                  fields.degraded ? "true" : "false", fields.records, fields.dropped, fields.card_dropped,
                  fields.write_lost, fields.tap_shutdown_lost, fields.tap_accepted, fields.tap_drained,
                  fields.tap_record_ring_accepted);
}

}  // namespace sd_logger
}  // namespace esphome
