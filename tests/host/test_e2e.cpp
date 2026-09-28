// E2E CRC repair: crc8_linear / e2e_fix_crc / process_frame with modify.e2e.
//
// The claim under test (gateway_core.h): for a fixed covered length, any
// MSB-first CRC-8 is affine, crc(x) = L(x) ^ K(init, xor-out, leading data-ID),
// so after a patch crc(new) = crc(old) ^ L(old ^ new). The reference below is an
// independent full CRC with init, xor-out and an optional leading data-ID; the
// repair must agree with it for every parameter set without knowing any of them.

#include "gateway_core.h"
#include "harness.h"

#include <cstdint>
#include <string>

using namespace esphome::can_gateway;

namespace {

/// Independent reference: MSB-first CRC-8 with init, xor-out and a data-ID byte
/// processed before the data (the common "data-ID first" convention).
uint8_t crc8_full(const uint8_t *data, uint8_t len, uint8_t poly, uint8_t init, uint8_t xor_out, int data_id) {
  uint8_t crc = init;
  auto feed = [&](uint8_t byte) {
    crc ^= byte;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ poly) : static_cast<uint8_t>(crc << 1);
  };
  if (data_id >= 0)
    feed(static_cast<uint8_t>(data_id));
  for (uint8_t i = 0; i < len; i++)
    feed(data[i]);
  return static_cast<uint8_t>(crc ^ xor_out);
}

uint32_t lcg(uint32_t &s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

}  // namespace

TEST(e2e_crc8_linear_check_value) {
  // CRC-8/GSM-A (poly 0x1D, init 0, no reflection, xor-out 0) check value.
  const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK_EQ(crc8_linear(check, 9, 0x1D), 0x37);
  // CRC-8/SAE-J1850 (init 0xFF, xor-out 0xFF) check value, via the reference.
  CHECK_EQ(crc8_full(check, 9, 0x1D, 0xFF, 0xFF, -1), 0x4B);
}

TEST(e2e_repair_matches_full_recompute_for_any_parameters) {
  struct Params {
    uint8_t init, xor_out;
    int data_id;
  };
  const Params sets[] = {{0x00, 0x00, -1}, {0xFF, 0xFF, -1}, {0xFF, 0x00, 0x5A}, {0x3C, 0xA7, 0x01}};
  E2eCrc e2e{};
  e2e.crc_index = 7;
  e2e.first = 0;
  e2e.last = 6;
  e2e.poly = 0x1D;
  uint32_t seed = 12345;
  for (const Params &p : sets) {
    for (int round = 0; round < 500; round++) {
      uint8_t frame[8];
      for (uint8_t i = 0; i < 7; i++)
        frame[i] = static_cast<uint8_t>(lcg(seed));
      frame[7] = crc8_full(frame, 7, 0x1D, p.init, p.xor_out, p.data_id);
      uint8_t before[8];
      for (uint8_t i = 0; i < 8; i++)
        before[i] = frame[i];
      // Random patch over bytes 0..6 (never the CRC byte), like a masked rule.
      for (uint8_t i = 0; i < 7; i++) {
        uint8_t mask = static_cast<uint8_t>(lcg(seed));
        uint8_t val = static_cast<uint8_t>(lcg(seed));
        frame[i] = static_cast<uint8_t>((frame[i] & ~mask) | (val & mask));
      }
      e2e_fix_crc(e2e, before, frame, 8);
      uint8_t expected = crc8_full(frame, 7, 0x1D, p.init, p.xor_out, p.data_id);
      CHECK_EQ_MSG(frame[7], expected, std::string("round ") + std::to_string(round));
    }
  }
}

TEST(e2e_unchanged_payload_keeps_even_a_wrong_crc) {
  E2eCrc e2e{};
  e2e.crc_index = 7;
  e2e.last = 6;
  uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 0x00};  // 0x00 is not the right CRC
  uint8_t before[8] = {1, 2, 3, 4, 5, 6, 7, 0x00};
  e2e_fix_crc(e2e, before, frame, 8);
  CHECK_EQ(frame[7], 0x00);
}

TEST(e2e_corrupt_frame_stays_corrupt) {
  // A frame that arrived with CRC error e keeps exactly that error after repair:
  // the gateway never launders a corrupt frame into a valid one.
  E2eCrc e2e{};
  e2e.crc_index = 7;
  e2e.last = 6;
  uint8_t frame[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0};
  uint8_t good = crc8_full(frame, 7, 0x1D, 0xFF, 0xFF, -1);
  frame[7] = static_cast<uint8_t>(good ^ 0x5A);
  uint8_t before[8];
  for (int i = 0; i < 8; i++)
    before[i] = frame[i];
  frame[2] = 0x99;
  e2e_fix_crc(e2e, before, frame, 8);
  uint8_t now_good = crc8_full(frame, 7, 0x1D, 0xFF, 0xFF, -1);
  CHECK_EQ(static_cast<uint8_t>(frame[7] ^ now_good), 0x5A);
}

TEST(e2e_short_frame_is_left_alone) {
  E2eCrc e2e{};
  e2e.crc_index = 7;
  e2e.last = 6;
  uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t before[8] = {9, 2, 3, 4, 5, 6, 7, 8};
  e2e_fix_crc(e2e, before, frame, 7);  // dlc 7: the CRC byte is not there
  CHECK_EQ(frame[7], 8);
}

TEST(e2e_disabled_by_default) {
  E2eCrc e2e{};
  CHECK(!e2e.enabled());
  uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t before[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  e2e_fix_crc(e2e, before, frame, 8);
  CHECK_EQ(frame[7], 8);
}

TEST(e2e_process_frame_patches_and_repairs) {
  // A rule the way codegen builds it: Motorola 13-bit field (start 4) stamped
  // to 0x1234, CRC in byte 7 over 0..6, J1850 poly.
  RuleEntry rule{};
  rule.match_id = 0x1A0;
  rule.match_mask = MAX_STANDARD_ID;
  rule.flags = RULE_FLAG_HAS_PATCH;
  uint8_t mask[8]{}, bits[8]{};
  signal_encode(4, 13, true, 0x1234, mask, bits);
  for (uint8_t i = 0; i < 8; i++) {
    rule.static_patch.and_mask[i] = static_cast<uint8_t>(~mask[i]);
    rule.static_patch.or_value[i] = bits[i];
  }
  rule.e2e.crc_index = 7;
  rule.e2e.first = 0;
  rule.e2e.last = 6;
  rule.e2e.poly = 0x1D;
  RouteTable table{};
  table.rules = &rule;
  table.rule_count = 1;

  uint8_t data[8] = {0xE5, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0};
  data[7] = crc8_full(data, 7, 0x1D, 0xFF, 0xFF, -1);
  uint32_t can_id = 0x1A0;
  bool extended = false;
  CHECK(process_frame(table, can_id, extended, false, data, 8) == FrameAction::FORWARD);
  CHECK_EQ(data[0], 0xE0 | 0x12);
  CHECK_EQ(data[1], 0x34);
  CHECK_EQ(data[7], crc8_full(data, 7, 0x1D, 0xFF, 0xFF, -1));
}
