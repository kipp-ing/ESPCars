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
  const char *capacity;
  uint32_t records;
  uint32_t dropped;
  uint32_t card_dropped;
  uint32_t write_lost;
  uint32_t tap_shutdown_lost;
  uint32_t tap_accepted;
  uint32_t tap_drained;
  uint32_t tap_record_ring_accepted;
  const char *recovery_state;
  uint32_t write_failures;
  const char *last_write_action;
  int32_t last_write_errno;
  int32_t last_write_result;
  uint32_t last_write_offset;
  uint32_t last_write_derived_offset;
  int32_t last_write_lseek_errno;
  uint32_t last_write_committed_end;
  uint32_t last_write_len;
  uint32_t last_write_elapsed_us;
  uint32_t last_write_spi_begin;
  uint32_t last_write_spi_end;
  uint32_t last_write_spi_commands;
  uint32_t last_write_spi_available;
  uint32_t last_write_spi_missing;
  int32_t last_write_spi_worst_err;
  uint32_t last_write_card_sectors;
  uint32_t last_write_volume_first_lba;
  uint32_t last_write_volume_sectors;
  uint32_t last_write_heap_default_free;
  uint32_t last_write_heap_default_largest;
  uint32_t last_write_heap_dma_free;
  uint32_t last_write_heap_dma_largest;
  uint32_t writer_stack_free;  // bytes; 0 == never sampled
};

/// Formats the status response and returns `snprintf`'s result. A card percent
/// above 100 means its fill is not known yet and is intentionally rendered null.
/// The last_write_* facts are absent until write_failures > 0: the action is
/// "none" and the per-failure numeric fields are null rather than sentinel
/// numbers that look like a real offset, errno or heap reading.
inline int format_status_json(char *out, size_t len, const StatusFields &fields) {
  char oldest_text[16] = "null";
  if (fields.has_oldest_uncollected)
    snprintf(oldest_text, sizeof(oldest_text), "%" PRIu32, fields.oldest_uncollected);
  char fill_text[16] = "null";
  if (fields.card_percent <= 100)
    snprintf(fill_text, sizeof(fill_text), "%" PRIu32, fields.card_percent);

  if (fields.write_failures == 0) {
    return snprintf(
        out, len,
        "{\"device\":\"%s\",\"sealed\":%" PRIu32 ",\"confirmed\":%" PRIu32
        ",\"card_percent\":%s,\"discarded_chunks\":%" PRIu32 ",\"discarded_bytes\":%" PRIu64
        ",\"oldest_uncollected\":%s,\"index_refused\":%" PRIu32 ",\"mounted\":%s,\"degraded\":%s"
        ",\"capacity\":\"%s\""
        ",\"records\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"card_dropped\":%" PRIu32 ",\"write_lost\":%" PRIu32
        ",\"tap_shutdown_lost\":%" PRIu32 ",\"tap_accepted\":%" PRIu32 ",\"tap_drained\":%" PRIu32
        ",\"tap_record_ring_accepted\":%" PRIu32
        ",\"recovery_state\":\"%s\",\"write_failures\":0,\"last_write_action\":\"none\""
        ",\"last_write_errno\":null,\"last_write_result\":null,\"last_write_offset\":null"
        ",\"last_write_derived_offset\":null,\"last_write_lseek_errno\":null,\"last_write_committed_end\":null"
        ",\"last_write_len\":null,\"last_write_elapsed_us\":null,\"last_write_spi_begin\":null"
        ",\"last_write_spi_end\":null,\"last_write_spi_commands\":null,\"last_write_spi_available\":null"
        ",\"last_write_spi_missing\":null,\"last_write_spi_worst_err\":null,\"last_write_card_sectors\":null"
        ",\"last_write_volume_first_lba\":null,\"last_write_volume_sectors\":null"
        ",\"last_write_heap_default_free\":null,\"last_write_heap_default_largest\":null"
        ",\"last_write_heap_dma_free\":null,\"last_write_heap_dma_largest\":null"
        ",\"writer_stack_free\":%" PRIu32 "}",
        fields.device, fields.sealed, fields.confirmed, fill_text, fields.discarded_chunks, fields.discarded_bytes,
        oldest_text, fields.index_refused, fields.mounted ? "true" : "false", fields.degraded ? "true" : "false",
        fields.capacity, fields.records, fields.dropped, fields.card_dropped, fields.write_lost,
        fields.tap_shutdown_lost, fields.tap_accepted, fields.tap_drained, fields.tap_record_ring_accepted,
        fields.recovery_state, fields.writer_stack_free);
  }

  return snprintf(
      out, len,
      "{\"device\":\"%s\",\"sealed\":%" PRIu32 ",\"confirmed\":%" PRIu32
      ",\"card_percent\":%s,\"discarded_chunks\":%" PRIu32 ",\"discarded_bytes\":%" PRIu64
      ",\"oldest_uncollected\":%s,\"index_refused\":%" PRIu32 ",\"mounted\":%s,\"degraded\":%s"
      ",\"capacity\":\"%s\""
      ",\"records\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"card_dropped\":%" PRIu32 ",\"write_lost\":%" PRIu32
      ",\"tap_shutdown_lost\":%" PRIu32 ",\"tap_accepted\":%" PRIu32 ",\"tap_drained\":%" PRIu32
      ",\"tap_record_ring_accepted\":%" PRIu32
      ",\"recovery_state\":\"%s\",\"write_failures\":%" PRIu32 ",\"last_write_action\":\"%s\""
      ",\"last_write_errno\":%" PRId32 ",\"last_write_result\":%" PRId32
      ",\"last_write_offset\":%" PRIu32 ",\"last_write_derived_offset\":%" PRIu32
      ",\"last_write_lseek_errno\":%" PRId32 ",\"last_write_committed_end\":%" PRIu32
      ",\"last_write_len\":%" PRIu32 ",\"last_write_elapsed_us\":%" PRIu32
      ",\"last_write_spi_begin\":%" PRIu32 ",\"last_write_spi_end\":%" PRIu32
      ",\"last_write_spi_commands\":%" PRIu32 ",\"last_write_spi_available\":%" PRIu32
      ",\"last_write_spi_missing\":%" PRIu32 ",\"last_write_spi_worst_err\":%" PRId32
      ",\"last_write_card_sectors\":%" PRIu32 ",\"last_write_volume_first_lba\":%" PRIu32
      ",\"last_write_volume_sectors\":%" PRIu32 ",\"last_write_heap_default_free\":%" PRIu32
      ",\"last_write_heap_default_largest\":%" PRIu32 ",\"last_write_heap_dma_free\":%" PRIu32
      ",\"last_write_heap_dma_largest\":%" PRIu32 ",\"writer_stack_free\":%" PRIu32 "}",
      fields.device, fields.sealed, fields.confirmed, fill_text, fields.discarded_chunks, fields.discarded_bytes,
      oldest_text, fields.index_refused, fields.mounted ? "true" : "false", fields.degraded ? "true" : "false",
      fields.capacity, fields.records, fields.dropped, fields.card_dropped, fields.write_lost, fields.tap_shutdown_lost,
      fields.tap_accepted, fields.tap_drained, fields.tap_record_ring_accepted, fields.recovery_state,
      fields.write_failures, fields.last_write_action, fields.last_write_errno, fields.last_write_result,
      fields.last_write_offset, fields.last_write_derived_offset, fields.last_write_lseek_errno,
      fields.last_write_committed_end, fields.last_write_len, fields.last_write_elapsed_us, fields.last_write_spi_begin,
      fields.last_write_spi_end, fields.last_write_spi_commands, fields.last_write_spi_available,
      fields.last_write_spi_missing, fields.last_write_spi_worst_err, fields.last_write_card_sectors,
      fields.last_write_volume_first_lba, fields.last_write_volume_sectors, fields.last_write_heap_default_free,
      fields.last_write_heap_default_largest, fields.last_write_heap_dma_free, fields.last_write_heap_dma_largest,
      fields.writer_stack_free);
}

}  // namespace sd_logger
}  // namespace esphome
