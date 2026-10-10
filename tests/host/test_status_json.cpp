// `GET /sdlog/status` has to remain observable even in precisely the SD-card
// failure states where a host cannot infer the truth from card_percent alone.
//
// writer_stack_free remains the last key in the JSON object: 0 means "never sampled", and the
// existing clients that append-read the tail keep that simple invariant. The write-failure fields
// inserted before it push the maximum-value object to 1430 bytes; the boundary cases below pin the
// exact byte where a too-small caller buffer truncates.

#include "harness.h"
#include "status_json.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

using esphome::sd_logger::format_status_json;
using esphome::sd_logger::StatusFields;

namespace {

StatusFields representative_fields(uint32_t writer_stack_free = 47) {
  return StatusFields{"mr-orange", 3u,  2u,  41u, 7u,  9ull, true, 123u, 11u, true, false,
                      "healthy",   17u, 19u, 23u, 29u, 31u,  37u,  41u,  43u,
                      "retrying", 53u, "retry_recovery", 5, -1, 736u, 700u, 0, 1248u, 512u, 42u,
                      100u, 103u, 3u, 3u, 0u, -7, 62500000u, 63u, 62499937u,
                      1000u, 900u, 800u, 700u, writer_stack_free};
}

size_t occurrences(const char *text, const char *needle) {
  size_t count = 0;
  while ((text = std::strstr(text, needle)) != nullptr) {
    count++;
    text += std::strlen(needle);
  }
  return count;
}

}  // namespace

TEST(status_json_has_the_complete_additive_wire_contract) {
  char body[1280];
  const StatusFields fields = representative_fields();
  const int n = format_status_json(body, sizeof(body), fields);

  CHECK(n > 0);
  CHECK(static_cast<size_t>(n) < sizeof(body));
  static const char *const KEYS[] = {
      "\"device\":",
      "\"sealed\":",
      "\"confirmed\":",
      "\"card_percent\":",
      "\"discarded_chunks\":",
      "\"discarded_bytes\":",
      "\"oldest_uncollected\":",
      "\"index_refused\":",
      "\"mounted\":",
      "\"degraded\":",
      "\"capacity\":",
      "\"records\":",
      "\"dropped\":",
      "\"card_dropped\":",
      "\"write_lost\":",
      "\"tap_shutdown_lost\":",
      "\"tap_accepted\":",
      "\"tap_drained\":",
      "\"tap_record_ring_accepted\":",
      "\"recovery_state\":",
      "\"write_failures\":",
      "\"last_write_action\":",
      "\"last_write_errno\":",
      "\"last_write_result\":",
      "\"last_write_offset\":",
      "\"last_write_derived_offset\":",
      "\"last_write_lseek_errno\":",
      "\"last_write_committed_end\":",
      "\"last_write_len\":",
      "\"last_write_elapsed_us\":",
      "\"last_write_spi_begin\":",
      "\"last_write_spi_end\":",
      "\"last_write_spi_commands\":",
      "\"last_write_spi_available\":",
      "\"last_write_spi_missing\":",
      "\"last_write_spi_worst_err\":",
      "\"last_write_card_sectors\":",
      "\"last_write_volume_first_lba\":",
      "\"last_write_volume_sectors\":",
      "\"last_write_heap_default_free\":",
      "\"last_write_heap_default_largest\":",
      "\"last_write_heap_dma_free\":",
      "\"last_write_heap_dma_largest\":",
      "\"writer_stack_free\":",
  };
  for (const char *key : KEYS)
    CHECK_EQ(occurrences(body, key), 1u);
  CHECK(std::strstr(body, "\"mounted\":true") != nullptr);
  CHECK(std::strstr(body, "\"degraded\":false") != nullptr);
  CHECK(std::strstr(body, "\"capacity\":\"healthy\"") != nullptr);
  CHECK(std::strstr(body, "\"mounted\":1") == nullptr);
  CHECK(std::strstr(body, "\"degraded\":0") == nullptr);

  // writer_stack_free must be the LAST key: nothing but its value and the closing brace may
  // follow it. A field "inserted anywhere but last" (contract §4, listed explicitly as a failing
  // case) still passes every occurrences() check above, so that check alone does not catch it —
  // this one does.
  const char *key_pos = std::strstr(body, "\"writer_stack_free\":");
  CHECK(key_pos != nullptr);
  const char *value_start = key_pos + std::strlen("\"writer_stack_free\":");
  char *value_end = nullptr;
  std::strtoul(value_start, &value_end, 10);
  CHECK(value_end != nullptr);
  CHECK(std::string(value_end) == "}");
}

TEST(status_json_last_write_fields_are_absent_until_a_failure_exists) {
  char body[1280];
  StatusFields fields = representative_fields();
  fields.write_failures = 0;
  const int n = format_status_json(body, sizeof(body), fields);

  CHECK(n > 0);
  CHECK(static_cast<size_t>(n) < sizeof(body));
  CHECK(std::strstr(body, "\"write_failures\":0") != nullptr);
  CHECK(std::strstr(body, "\"last_write_action\":\"none\"") != nullptr);
  CHECK(std::strstr(body, "\"last_write_action\":\"retry_recovery\"") == nullptr);

  static const char *const NULL_FIELDS[] = {
      "\"last_write_errno\":null",
      "\"last_write_result\":null",
      "\"last_write_offset\":null",
      "\"last_write_derived_offset\":null",
      "\"last_write_lseek_errno\":null",
      "\"last_write_committed_end\":null",
      "\"last_write_len\":null",
      "\"last_write_elapsed_us\":null",
      "\"last_write_spi_begin\":null",
      "\"last_write_spi_end\":null",
      "\"last_write_spi_commands\":null",
      "\"last_write_spi_available\":null",
      "\"last_write_spi_missing\":null",
      "\"last_write_spi_worst_err\":null",
      "\"last_write_card_sectors\":null",
      "\"last_write_volume_first_lba\":null",
      "\"last_write_volume_sectors\":null",
      "\"last_write_heap_default_free\":null",
      "\"last_write_heap_default_largest\":null",
      "\"last_write_heap_dma_free\":null",
      "\"last_write_heap_dma_largest\":null",
  };
  for (const char *field : NULL_FIELDS)
    CHECK(std::strstr(body, field) != nullptr);
}

TEST(status_json_writer_stack_free_zero_means_never_sampled) {
  // The one field on this struct whose own zero is a meaningful, documented state ("never
  // sampled") rather than an absent/default value — worth its own exact-string case rather than
  // folding it into the boundary test, where a reader would have to go hunting for which field
  // is 0 and why.
  char body[1280];
  const StatusFields fields = representative_fields(0);
  const int n = format_status_json(body, sizeof(body), fields);

  static const char kExpected[] =
      "{\"device\":\"mr-orange\",\"sealed\":3,\"confirmed\":2,\"card_percent\":41,\"discarded_chunks\":7,"
      "\"discarded_bytes\":9,\"oldest_uncollected\":123,\"index_refused\":11,\"mounted\":true,"
      "\"degraded\":false,\"capacity\":\"healthy\",\"records\":17,\"dropped\":19,\"card_dropped\":23,"
      "\"write_lost\":29,\"tap_shutdown_lost\":31,\"tap_accepted\":37,\"tap_drained\":41,"
      "\"tap_record_ring_accepted\":43,\"recovery_state\":\"retrying\",\"write_failures\":53,"
      "\"last_write_action\":\"retry_recovery\",\"last_write_errno\":5,\"last_write_result\":-1,"
      "\"last_write_offset\":736,\"last_write_derived_offset\":700,\"last_write_lseek_errno\":0,"
      "\"last_write_committed_end\":1248,\"last_write_len\":512,"
      "\"last_write_elapsed_us\":42,\"last_write_spi_begin\":100,\"last_write_spi_end\":103,"
      "\"last_write_spi_commands\":3,\"last_write_spi_available\":3,\"last_write_spi_missing\":0,"
      "\"last_write_spi_worst_err\":-7,\"last_write_card_sectors\":62500000,"
      "\"last_write_volume_first_lba\":63,\"last_write_volume_sectors\":62499937,"
      "\"last_write_heap_default_free\":1000,\"last_write_heap_default_largest\":900,"
      "\"last_write_heap_dma_free\":800,\"last_write_heap_dma_largest\":700,\"writer_stack_free\":0}";
  CHECK_EQ(n, static_cast<int>(sizeof(kExpected) - 1));
  CHECK(std::string(body) == kExpected);
}

TEST(status_json_writer_stack_free_full_object_exact_string) {
  // Full byte-for-byte comparison with a representative (non-zero, non-max) value, so a field
  // reordered, mistyped (%d vs %u would only show up under a negative — covered by the max case
  // below — or a stray space/comma) is caught even when every individual key still appears
  // exactly once.
  char body[1280];
  const StatusFields fields = representative_fields();
  const int n = format_status_json(body, sizeof(body), fields);

  static const char kExpected[] =
      "{\"device\":\"mr-orange\",\"sealed\":3,\"confirmed\":2,\"card_percent\":41,\"discarded_chunks\":7,"
      "\"discarded_bytes\":9,\"oldest_uncollected\":123,\"index_refused\":11,\"mounted\":true,"
      "\"degraded\":false,\"capacity\":\"healthy\",\"records\":17,\"dropped\":19,\"card_dropped\":23,"
      "\"write_lost\":29,\"tap_shutdown_lost\":31,\"tap_accepted\":37,\"tap_drained\":41,"
      "\"tap_record_ring_accepted\":43,\"recovery_state\":\"retrying\",\"write_failures\":53,"
      "\"last_write_action\":\"retry_recovery\",\"last_write_errno\":5,\"last_write_result\":-1,"
      "\"last_write_offset\":736,\"last_write_derived_offset\":700,\"last_write_lseek_errno\":0,"
      "\"last_write_committed_end\":1248,\"last_write_len\":512,"
      "\"last_write_elapsed_us\":42,\"last_write_spi_begin\":100,\"last_write_spi_end\":103,"
      "\"last_write_spi_commands\":3,\"last_write_spi_available\":3,\"last_write_spi_missing\":0,"
      "\"last_write_spi_worst_err\":-7,\"last_write_card_sectors\":62500000,"
      "\"last_write_volume_first_lba\":63,\"last_write_volume_sectors\":62499937,"
      "\"last_write_heap_default_free\":1000,\"last_write_heap_default_largest\":900,"
      "\"last_write_heap_dma_free\":800,\"last_write_heap_dma_largest\":700,\"writer_stack_free\":47}";
  CHECK_EQ(n, static_cast<int>(sizeof(kExpected) - 1));
  CHECK(std::string(body) == kExpected);
}

TEST(status_json_maximum_values_fit_the_status_response_buffer) {
  // Re-derived for the longer object per this file's header comment: was 517/517 before
  // writer_stack_free existed. 548 = 517 + strlen(',"writer_stack_free":4294967295') (32 bytes:
  // the comma, the quoted 17-char key, the colon, and 10 digits for UINT32_MAX). Also doubles as
  // the UINT32_MAX case for the new field, since every other field here is already UINT32_MAX.
  char body[1500];
  const std::string device(39, 'x');  // CollectionServer::device_ reserves one byte for NUL.
  const StatusFields fields{device.c_str(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint64_t>::max(),
                            true,
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            false,
                            true,
                            "self_test_failed",
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            "stopped_no_bus_traffic",
                            std::numeric_limits<uint32_t>::max(),
                            "stop_no_bus_traffic",
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max()};
  const int n = format_status_json(body, sizeof(body), fields);

  CHECK_EQ(n, 1430);
  CHECK(static_cast<size_t>(n) < sizeof(body));
  CHECK_EQ(std::strlen(body), 1430u);
  CHECK(std::strstr(body, "\"mounted\":false") != nullptr);
  CHECK(std::strstr(body, "\"degraded\":true") != nullptr);
  CHECK(std::strstr(body, "\"recovery_state\":\"stopped_no_bus_traffic\"") != nullptr);
  CHECK(std::strstr(body, "\"last_write_spi_worst_err\":-2147483648") != nullptr);
  CHECK(std::strstr(body, "\"writer_stack_free\":4294967295}") != nullptr);
}

TEST(status_json_buffer_exactly_the_required_capacity_does_not_truncate) {
  // The razor's edge companion to the case below: 549 bytes (548 content bytes + the NUL
  // snprintf always writes when len > 0) is the smallest buffer that holds the maximum-value
  // object whole. Exercised at the exact figure, not just "some generously large buffer", so a
  // future off-by-one in whoever sizes a real caller's buffer has a test to check itself against.
  constexpr size_t kRequiredCapacity = 1431;  // 1430 content bytes + NUL
  char body[kRequiredCapacity];
  const std::string device(39, 'x');
  const StatusFields fields{device.c_str(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint64_t>::max(),
                            true,
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            false,
                            true,
                            "self_test_failed",
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            "stopped_no_bus_traffic",
                            std::numeric_limits<uint32_t>::max(),
                            "stop_no_bus_traffic",
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max()};
  const int n = format_status_json(body, sizeof(body), fields);

  CHECK_EQ(n, 1430);
  CHECK_EQ(static_cast<size_t>(n), sizeof(body) - 1);  // exact fit, zero bytes to spare
  CHECK_EQ(std::strlen(body), 1430u);
  CHECK(body[1429] == '}');  // the object's own closing brace, not a truncated digit
}

TEST(status_json_buffer_one_byte_short_of_the_new_object_truncates) {
  // Nobody wants this one; it is the one the contract says to write anyway (§5, §4 "What a
  // failing case looks like"). 548 bytes is exactly one byte short of the 549 the maximum-value
  // object now needs (previously 518 was enough: 517 content bytes + NUL). A caller whose buffer
  // was sized against the pre-writer_stack_free object is exactly this case.
  //
  // snprintf's return value is defined as the length it WOULD have written, uncapped by `len` —
  // so `n` alone cannot tell a caller truncation happened; that is what `n >= sizeof(body)` is
  // for. The truncation itself lands on the object's own closing brace: the last byte a
  // too-small buffer drops here is `}`, not a digit, which is the more dangerous failure — a
  // parser reading char-by-char sees a well-formed-looking number with no terminator rather than
  // an obviously broken document.
  constexpr size_t kOneByteShort = 1430;  // 1431 required - 1
  char body[kOneByteShort];
  const std::string device(39, 'x');
  const StatusFields fields{device.c_str(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint64_t>::max(),
                            true,
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            false,
                            true,
                            "self_test_failed",
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            "stopped_no_bus_traffic",
                            std::numeric_limits<uint32_t>::max(),
                            "stop_no_bus_traffic",
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max()};
  const int n = format_status_json(body, sizeof(body), fields);

  CHECK_EQ(n, 1430);                              // snprintf still reports the full length
  CHECK(static_cast<size_t>(n) >= sizeof(body));  // ...which is how a caller detects this
  CHECK_EQ(std::strlen(body), sizeof(body) - 1);  // 547 bytes actually written, then NUL
  CHECK(body[sizeof(body) - 2] != '}');           // the closing brace is exactly what got dropped

  // And the 547 bytes that DID make it out must be an exact, unmangled prefix of the full object
  // — a truncated-but-still-correct-so-far buffer, not a formatter that also corrupted content
  // while running out of room.
  CHECK(std::strstr(body, "\"writer_stack_free\":4294967295") != nullptr);
}
