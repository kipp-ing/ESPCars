#pragma once

// The capacity-confinement decisions are intentionally ESPHome- and IDF-free:
// the failure is in the card, not the framework, and the exact boundary and
// echo verdict need host coverage without a card attached.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace sd_logger {

static constexpr uint32_t SD_CONFINE_SECTOR_BYTES = 512;
static constexpr uint64_t SD_CONFINE_MIN_BYTES = 1ull * 1024 * 1024;
// A typo guard, not a policy. The safe size for a given card is whatever its capacity
// self-test proves at runtime, which this compile-time bound cannot know; pinning it to
// one card's measured region would silently refuse the next card.
static constexpr uint64_t SD_CONFINE_MAX_BYTES = 32ull * 1024 * 1024 * 1024;

/// Convert a requested confined volume size into FatFs' absolute-sector plist
/// value. f_fdisk treats 1..100 as percentages, so the minimum deliberately
/// remains well above that ambiguity.
inline bool confine_sector_count(uint64_t bytes, uint32_t *sectors_out) {
  if (sectors_out == nullptr || bytes < SD_CONFINE_MIN_BYTES || bytes > SD_CONFINE_MAX_BYTES ||
      bytes % SD_CONFINE_SECTOR_BYTES != 0)
    return false;
  const uint64_t sectors = bytes / SD_CONFINE_SECTOR_BYTES;
  if (sectors <= 100 || sectors > UINT32_MAX)
    return false;
  *sectors_out = static_cast<uint32_t>(sectors);
  return true;
}

inline bool confine_volume_fits(uint64_t volume_bytes, uint64_t confine_bytes) {
  return confine_bytes == 0 || volume_bytes <= confine_bytes;
}

enum class CapacitySelfTestVerdict : uint8_t {
  PASS,
  READ_ERROR,
  TARGET_CHANGED,
  TARGET_ECHOED_PRIMER,
};

/// Decide the four-read counterfeit test. The caller reads A, T, B, T in
/// precisely that order. A target read that repeats the low-sector buffer it
/// immediately follows is the documented silent no-op; two differing target
/// reads catch the less tidy variants of the same lie.
inline CapacitySelfTestVerdict capacity_self_test_verdict(bool reads_ok, const uint8_t *primer_a,
                                                          const uint8_t *target_first, const uint8_t *primer_b,
                                                          const uint8_t *target_second, size_t sector_bytes) {
  if (!reads_ok || primer_a == nullptr || target_first == nullptr || primer_b == nullptr || target_second == nullptr ||
      sector_bytes == 0)
    return CapacitySelfTestVerdict::READ_ERROR;
  if (std::memcmp(target_first, target_second, sector_bytes) != 0)
    return CapacitySelfTestVerdict::TARGET_CHANGED;
  if (std::memcmp(target_first, primer_a, sector_bytes) == 0 || std::memcmp(target_second, primer_b, sector_bytes) == 0)
    return CapacitySelfTestVerdict::TARGET_ECHOED_PRIMER;
  return CapacitySelfTestVerdict::PASS;
}

}  // namespace sd_logger
}  // namespace esphome
