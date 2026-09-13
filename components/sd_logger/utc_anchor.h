#pragma once

// The correspondence between ESP boot time and UTC.  This intentionally has no
// ESPHome or ESP-IDF dependency: a wrong conversion produces plausible-looking
// timestamps, so the arithmetic belongs in the host test suite.

#include <cstdint>
#include <limits>

namespace esphome {
namespace sd_logger {

struct UtcAnchor {
  uint64_t utc_us{0};
  uint64_t boot_us{0};
  bool valid{false};
};

/// Resolve a boot-relative timestamp through an anchor. Returns false before a
/// time sync has supplied one. The caller supplies the reconstructed 64-bit boot
/// time, never the wrapped uint32_t carried by a record.
inline bool resolve_utc_us(const UtcAnchor &anchor, uint64_t record_boot_us, uint64_t *record_utc_us) {
  if (!anchor.valid || record_utc_us == nullptr)
    return false;

  if (record_boot_us >= anchor.boot_us) {
    const uint64_t delta = record_boot_us - anchor.boot_us;
    *record_utc_us = delta > std::numeric_limits<uint64_t>::max() - anchor.utc_us ? std::numeric_limits<uint64_t>::max()
                                                                                  : anchor.utc_us + delta;
  } else {
    const uint64_t delta = anchor.boot_us - record_boot_us;
    *record_utc_us = delta > anchor.utc_us ? 0 : anchor.utc_us - delta;
  }
  return true;
}

}  // namespace sd_logger
}  // namespace esphome
