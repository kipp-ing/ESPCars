// `GET /sdlog/status` has to remain observable even in precisely the SD-card
// failure states where a host cannot infer the truth from card_percent alone.

#include "harness.h"
#include "status_json.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using esphome::sd_logger::format_status_json;
using esphome::sd_logger::StatusFields;

namespace {

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
  char body[512];
  const StatusFields fields{"mr-orange", 3, 2, 41, 7, 9, true, 123, 11, true, false, 17, 19, 23, 29, 31, 37, 41, 43};
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
      "\"records\":",
      "\"dropped\":",
      "\"card_dropped\":",
      "\"write_lost\":",
      "\"tap_shutdown_lost\":",
      "\"tap_accepted\":",
      "\"tap_drained\":",
      "\"tap_record_ring_accepted\":",
  };
  for (const char *key : KEYS)
    CHECK_EQ(occurrences(body, key), 1u);
  CHECK(std::strstr(body, "\"mounted\":true") != nullptr);
  CHECK(std::strstr(body, "\"degraded\":false") != nullptr);
  CHECK(std::strstr(body, "\"mounted\":1") == nullptr);
  CHECK(std::strstr(body, "\"degraded\":0") == nullptr);
}

TEST(status_json_maximum_values_fit_the_status_response_buffer) {
  char body[512];
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
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max()};
  const int n = format_status_json(body, sizeof(body), fields);

  CHECK_EQ(n, 487);
  CHECK(static_cast<size_t>(n) < sizeof(body));
  CHECK_EQ(std::strlen(body), 487u);
  CHECK(std::strstr(body, "\"mounted\":false") != nullptr);
  CHECK(std::strstr(body, "\"degraded\":true") != nullptr);
}
