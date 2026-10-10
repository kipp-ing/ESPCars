#include "harness.h"
#include "sd_confine_policy.h"

#include <array>
#include <cstdint>

using esphome::sd_logger::capacity_self_test_verdict;
using esphome::sd_logger::confine_sector_count;
using esphome::sd_logger::confine_volume_fits;
using esphome::sd_logger::CapacitySelfTestVerdict;

TEST(confine_bytes_becomes_an_absolute_fatfs_sector_count) {
  uint32_t sectors = 0;
  CHECK(confine_sector_count(400ull * 1024 * 1024, &sectors));
  CHECK_EQ(sectors, 819200u);
  CHECK(!confine_sector_count(400ull * 1024 * 1024 + 1, &sectors));
  CHECK(!confine_sector_count(0, &sectors));
  // 450 MiB is what the bench card is confined to: its measured last real sector is
  // LBA 974843, and this leaves ~5% margin below that.
  CHECK(confine_sector_count(450ull * 1024 * 1024, &sectors));
  CHECK_EQ(sectors, 921600u);
  CHECK(sectors < 974844u);
  // The cap is a typo guard, so a plainly absurd request is still refused.
  CHECK(!confine_sector_count(5ull * 1024 * 1024 * 1024 * 1024, &sectors));
}

TEST(confine_guard_only_allows_a_volume_at_or_below_the_cap) {
  constexpr uint64_t CAP = 400ull * 1024 * 1024;
  CHECK(confine_volume_fits(CAP, CAP));
  CHECK(confine_volume_fits(CAP - 512, CAP));
  CHECK(!confine_volume_fits(CAP + 512, CAP));
  CHECK(confine_volume_fits(CAP + 512, 0));
}

TEST(capacity_self_test_accepts_stable_non_primer_target) {
  std::array<uint8_t, 512> a{};
  std::array<uint8_t, 512> target{};
  std::array<uint8_t, 512> b{};
  a[0] = 0xA1;
  target[0] = 0x54;
  b[0] = 0xB2;
  CHECK_EQ(capacity_self_test_verdict(true, a.data(), target.data(), b.data(), target.data(), target.size()),
           CapacitySelfTestVerdict::PASS);
}

TEST(capacity_self_test_rejects_a_changing_high_sector) {
  std::array<uint8_t, 512> a{};
  std::array<uint8_t, 512> first{};
  std::array<uint8_t, 512> b{};
  std::array<uint8_t, 512> second{};
  first[0] = 1;
  second[0] = 2;
  CHECK_EQ(capacity_self_test_verdict(true, a.data(), first.data(), b.data(), second.data(), first.size()),
           CapacitySelfTestVerdict::TARGET_CHANGED);
}

TEST(capacity_self_test_rejects_either_preceding_primer_echo) {
  std::array<uint8_t, 512> a{};
  std::array<uint8_t, 512> b{};
  a[0] = 0x11;
  b[0] = 0x22;
  CHECK_EQ(capacity_self_test_verdict(true, a.data(), a.data(), b.data(), a.data(), a.size()),
           CapacitySelfTestVerdict::TARGET_ECHOED_PRIMER);
  CHECK_EQ(capacity_self_test_verdict(true, a.data(), b.data(), b.data(), b.data(), b.size()),
           CapacitySelfTestVerdict::TARGET_ECHOED_PRIMER);
  CHECK_EQ(capacity_self_test_verdict(false, a.data(), b.data(), b.data(), b.data(), b.size()),
           CapacitySelfTestVerdict::READ_ERROR);
}
