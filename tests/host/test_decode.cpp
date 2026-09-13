// Signal decode: extract_bytes / extract_bits / sign_extend / raw_is_sna, plus
// the bus-load bit estimate.
//
// The vectors below are hand-computed against the documented model, not against
// a reimplementation of the code:
//   - extract_bytes: `byte_len` bytes from `byte_offset`, assembled LSB-first
//     (Intel) or MSB-first (Motorola); bytes at or beyond dlc read as 0
//   - extract_bits: the payload forms a 64-bit little-endian word and the field
//     is bits [bit_offset, bit_offset + bit_len); bytes beyond dlc contribute 0
//   - sign_extend: widen a `width`-bit two's-complement value to int32_t
//
// The permitted argument ranges come from validate_signal_position() in
// components/can_gateway/__init__.py plus the platform schemas:
//   byte form: offset 0-7, length 1-4, offset + length <= 8
//   bit form:  bit_offset 0-63, bit_length 1-32, bit_offset + bit_length <= 64,
//              little-endian only
// The sweep at the end walks every permitted combination so UBSan sees each
// shift at the edges of those bounds.

#include "gateway_core.h"
#include "harness.h"

#include <cstdint>
#include <string>

using namespace esphome::can_gateway;

namespace {

/// Payload used by every vector: little-endian word 0xEFCDAB8967452301.
const uint8_t PAYLOAD[MAX_FRAME_DATA_LEN] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

std::string tag(const char *what, int a, int b, int c) {
  return std::string(what) + "(dlc=" + std::to_string(a) + ", off=" + std::to_string(b) + ", len=" + std::to_string(c) +
         ")";
}

}  // namespace

// ---------------------------------------------------------------------------
// extract_bytes
// ---------------------------------------------------------------------------

TEST(decode_bytes_little_endian_vectors) {
  struct Row {
    uint8_t dlc, offset, length;
    uint32_t expected;
  };
  const Row rows[] = {
      {8, 0, 1, 0x00000001}, {8, 0, 2, 0x00002301}, {8, 0, 3, 0x00452301}, {8, 0, 4, 0x67452301}, {8, 1, 4, 0x89674523},
      {8, 4, 4, 0xEFCDAB89}, {8, 3, 2, 0x00008967}, {8, 6, 2, 0x0000EFCD}, {8, 7, 1, 0x000000EF}, {8, 4, 1, 0x00000089},
  };
  for (const Row &row : rows) {
    uint32_t got = extract_bytes(PAYLOAD, row.dlc, row.offset, row.length, /*big_endian=*/false);
    CHECK_EQ_MSG(got, row.expected, tag("extract_bytes LE", row.dlc, row.offset, row.length));
  }
}

TEST(decode_bytes_big_endian_vectors) {
  struct Row {
    uint8_t dlc, offset, length;
    uint32_t expected;
  };
  const Row rows[] = {
      {8, 0, 1, 0x00000001}, {8, 0, 2, 0x00000123}, {8, 0, 3, 0x00012345}, {8, 0, 4, 0x01234567}, {8, 1, 4, 0x23456789},
      {8, 4, 4, 0x89ABCDEF}, {8, 3, 2, 0x00006789}, {8, 6, 2, 0x0000CDEF}, {8, 7, 1, 0x000000EF}, {8, 4, 1, 0x00000089},
  };
  for (const Row &row : rows) {
    uint32_t got = extract_bytes(PAYLOAD, row.dlc, row.offset, row.length, /*big_endian=*/true);
    CHECK_EQ_MSG(got, row.expected, tag("extract_bytes BE", row.dlc, row.offset, row.length));
  }
}

TEST(decode_bytes_beyond_dlc_read_as_zero) {
  struct Row {
    uint8_t dlc, offset, length;
    bool big_endian;
    uint32_t expected;
  };
  const Row rows[] = {
      // A 4-byte signal in a 2-byte frame: bytes 2 and 3 are absent.
      {2, 0, 4, false, 0x00002301},
      {2, 0, 4, true, 0x01230000},
      // Signal that straddles the end of the payload.
      {3, 2, 2, false, 0x00000045},
      {3, 2, 2, true, 0x00004500},
      {5, 4, 4, false, 0x00000089},
      {5, 4, 4, true, 0x89000000},
      // Signal entirely past the end.
      {2, 4, 4, false, 0x00000000},
      {2, 4, 4, true, 0x00000000},
      {0, 0, 1, false, 0x00000000},
      {0, 0, 4, true, 0x00000000},
      {7, 7, 1, false, 0x00000000},
      // Exactly at the boundary: dlc 8 makes byte 7 present.
      {8, 7, 1, false, 0x000000EF},
  };
  for (const Row &row : rows) {
    uint32_t got = extract_bytes(PAYLOAD, row.dlc, row.offset, row.length, row.big_endian);
    CHECK_EQ_MSG(got, row.expected,
                 tag(row.big_endian ? "extract_bytes BE" : "extract_bytes LE", row.dlc, row.offset, row.length));
  }
}

TEST(decode_bytes_widest_field_at_the_highest_offset) {
  // offset 4 + length 4 is the widest signal the validator permits at the
  // highest offset it permits.
  CHECK_EQ(extract_bytes(PAYLOAD, 8, 4, 4, false), 0xEFCDAB89u);
  CHECK_EQ(extract_bytes(PAYLOAD, 8, 4, 4, true), 0x89ABCDEFu);
}

TEST(decode_bytes_dlc_above_eight_is_ignored_for_absent_bytes) {
  // Defensive: a driver reporting dlc > 8 must not make the reader walk past
  // the caller's 8-byte buffer. The heap allocation turns any overrun into an
  // ASan report instead of a silent read.
  uint8_t *payload = new uint8_t[MAX_FRAME_DATA_LEN];
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    payload[i] = PAYLOAD[i];
  CHECK_EQ(extract_bytes(payload, 15, 4, 4, false), 0xEFCDAB89u);
  CHECK_EQ(extract_bits(payload, 15, 32, 32), 0xEFCDAB89u);
  delete[] payload;
}

// ---------------------------------------------------------------------------
// extract_bits
// ---------------------------------------------------------------------------

TEST(decode_bits_vectors) {
  // Reference: the payload as a 64-bit little-endian word, 0xEFCDAB8967452301.
  struct Row {
    uint8_t dlc, bit_offset, bit_length;
    uint32_t expected;
  };
  const Row rows[] = {
      {8, 0, 1, 0x00000001},   // bit 0 of 0x01
      {8, 1, 1, 0x00000000},   // bit 1 of 0x01
      {8, 63, 1, 0x00000001},  // MSB of 0xEF
      {8, 0, 8, 0x00000001},   // byte 0
      {8, 8, 8, 0x00000023},   // byte 1
      {8, 56, 8, 0x000000EF},  // byte 7
      {8, 4, 8, 0x00000030},   // nibble-straddling
      {8, 12, 12, 0x00000452},
      {8, 24, 16, 0x00008967},
      {8, 40, 24, 0x00EFCDAB},
      {8, 0, 32, 0x67452301},
      {8, 32, 32, 0xEFCDAB89},  // widest field at the highest permitted offset
      {8, 31, 32, 0xDF9B5712},  // widest field straddling the 32-bit boundary
      {8, 1, 32, 0xB3A29180},
      {8, 33, 31, 0x77E6D5C4},
      // Bytes beyond dlc contribute 0.
      {2, 0, 32, 0x00002301},
      {2, 8, 8, 0x00000023},
      {2, 12, 8, 0x00000002},
      {2, 16, 16, 0x00000000},
      {0, 0, 8, 0x00000000},
      {0, 32, 32, 0x00000000},
      {4, 32, 32, 0x00000000},
  };
  for (const Row &row : rows) {
    uint32_t got = extract_bits(PAYLOAD, row.dlc, row.bit_offset, row.bit_length);
    CHECK_EQ_MSG(got, row.expected, tag("extract_bits", row.dlc, row.bit_offset, row.bit_length));
  }
}

TEST(decode_bits_result_never_exceeds_its_width) {
  for (uint8_t len = 1; len <= 32; len++) {
    for (uint8_t off = 0; off + len <= 64; off++) {
      uint32_t got = extract_bits(PAYLOAD, 8, off, len);
      uint64_t limit = 1ull << len;
      CHECK_EQ_MSG(static_cast<uint64_t>(got) < limit, true, tag("extract_bits width", 8, off, len));
    }
  }
}

TEST(decode_bits_boundary_sweep_matches_the_documented_word_model) {
  // Every (dlc, bit_offset, bit_length) the validator permits, checked against
  // the 64-bit little-endian word the header documents. The word here is built
  // MSB-first from the payload, i.e. by a different construction than the code
  // under test, and this also drives UBSan over every shift at the bounds.
  for (uint8_t dlc = 0; dlc <= MAX_FRAME_DATA_LEN; dlc++) {
    uint64_t word = 0;
    for (int i = MAX_FRAME_DATA_LEN - 1; i >= 0; i--)
      word = (word << 8) | (i < dlc ? PAYLOAD[i] : 0u);
    for (uint8_t len = 1; len <= 32; len++) {
      for (uint8_t off = 0; off + len <= 64; off++) {
        uint64_t mask = (1ull << len) - 1ull;
        uint32_t expected = static_cast<uint32_t>((word >> off) & mask);
        uint32_t got = extract_bits(PAYLOAD, dlc, off, len);
        CHECK_EQ_MSG(got, expected, tag("extract_bits sweep", dlc, off, len));
      }
    }
  }
}

TEST(decode_bytes_boundary_sweep_endianness_is_a_byte_reversal) {
  // Property check over every permitted (dlc, offset, length): the big-endian
  // assembly is the byte reversal of the little-endian one, and neither exceeds
  // its declared width.
  for (uint8_t dlc = 0; dlc <= MAX_FRAME_DATA_LEN; dlc++) {
    for (uint8_t len = 1; len <= 4; len++) {
      for (uint8_t off = 0; off + len <= MAX_FRAME_DATA_LEN; off++) {
        uint32_t le = extract_bytes(PAYLOAD, dlc, off, len, false);
        uint32_t be = extract_bytes(PAYLOAD, dlc, off, len, true);
        uint32_t reversed = 0;
        for (uint8_t i = 0; i < len; i++)
          reversed |= ((be >> (8 * i)) & 0xFFu) << (8 * (len - 1 - i));
        CHECK_EQ_MSG(le, reversed, tag("extract_bytes endianness", dlc, off, len));
        uint64_t limit = 1ull << (8 * len);
        CHECK_EQ_MSG(static_cast<uint64_t>(le) < limit, true, tag("extract_bytes width", dlc, off, len));
        if (off >= dlc)
          CHECK_EQ_MSG(le, 0u, tag("extract_bytes past dlc", dlc, off, len));
      }
    }
  }
}

TEST(decode_byte_aligned_and_bit_forms_agree_where_they_overlap) {
  // A byte-aligned little-endian signal is the same field as a bit-level one at
  // bit_offset = 8 * offset. Cross-checking the two implementations catches an
  // error in either.
  for (uint8_t dlc = 0; dlc <= MAX_FRAME_DATA_LEN; dlc++) {
    for (uint8_t len = 1; len <= 4; len++) {
      for (uint8_t off = 0; off + len <= MAX_FRAME_DATA_LEN; off++) {
        uint32_t by_bytes = extract_bytes(PAYLOAD, dlc, off, len, false);
        uint32_t by_bits = extract_bits(PAYLOAD, dlc, static_cast<uint8_t>(off * 8), static_cast<uint8_t>(len * 8));
        CHECK_EQ_MSG(by_bytes, by_bits, tag("byte/bit agreement", dlc, off, len));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// sign_extend
// ---------------------------------------------------------------------------

TEST(decode_sign_extend_vectors) {
  struct Row {
    uint32_t raw;
    uint8_t width;
    int32_t expected;
  };
  const Row rows[] = {
      // 1-bit fields (bit_length: 1 is permitted).
      {0x00000000, 1, 0},
      {0x00000001, 1, -1},
      // 2 and 4 bits.
      {0x00000001, 2, 1},
      {0x00000003, 2, -1},
      {0x00000002, 2, -2},
      {0x00000007, 4, 7},
      {0x00000008, 4, -8},
      {0x0000000F, 4, -1},
      // 8 bits (byte-aligned length 1).
      {0x00000000, 8, 0},
      {0x0000007F, 8, 127},
      {0x00000080, 8, -128},
      {0x000000FF, 8, -1},
      // 12 bits, a common packed signal width.
      {0x000007FF, 12, 2047},
      {0x00000800, 12, -2048},
      {0x00000FFF, 12, -1},
      // 16 bits (byte-aligned length 2).
      {0x00007FFF, 16, 32767},
      {0x00008000, 16, -32768},
      {0x0000FFFF, 16, -1},
      // 24 bits (byte-aligned length 3).
      {0x007FFFFF, 24, 8388607},
      {0x00800000, 24, -8388608},
      {0x00FFFFFF, 24, -1},
      // 31 bits: the widest field where the shift path still runs.
      {0x3FFFFFFF, 31, 1073741823},
      {0x40000000, 31, -1073741824},
      {0x7FFFFFFF, 31, -1},
      // 32 bits: the early-return path, a plain reinterpretation.
      {0x00000000, 32, 0},
      {0x7FFFFFFF, 32, 2147483647},
      {0x80000000, 32, INT32_MIN},
      {0xFFFFFFFF, 32, -1},
  };
  for (const Row &row : rows) {
    int32_t got = sign_extend(row.raw, row.width);
    CHECK_EQ_MSG(got, row.expected, std::string("sign_extend(width=") + std::to_string(row.width) + ")");
  }
}

TEST(decode_sign_extend_round_trips_every_width) {
  // For each permitted width, the largest positive value, the most negative
  // value and -1 must come back exactly.
  for (uint8_t width = 1; width <= 32; width++) {
    uint32_t all_ones = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
    uint32_t sign_bit = 1u << (width - 1);
    std::string where = std::string("width=") + std::to_string(width);

    CHECK_EQ_MSG(sign_extend(0, width), 0, where);
    CHECK_EQ_MSG(sign_extend(all_ones, width), -1, where);
    if (width > 1) {
      int64_t most_negative = -(static_cast<int64_t>(1) << (width - 1));
      int64_t most_positive = (static_cast<int64_t>(1) << (width - 1)) - 1;
      CHECK_EQ_MSG(static_cast<int64_t>(sign_extend(sign_bit, width)), most_negative, where);
      CHECK_EQ_MSG(static_cast<int64_t>(sign_extend(sign_bit - 1, width)), most_positive, where);
    }
  }
}

TEST(decode_sign_extend_composes_with_extraction) {
  // The path the decode sensor actually takes: extract then sign-extend, with
  // the width the sensor derives (bit_length, or 8 * byte length).
  const uint8_t payload[MAX_FRAME_DATA_LEN] = {0xFF, 0x80, 0x7F, 0xFF, 0x00, 0x00, 0x00, 0x00};

  // Byte-aligned, 1 byte at offset 0 -> 0xFF -> -1.
  CHECK_EQ(sign_extend(extract_bytes(payload, 8, 0, 1, false), 8), -1);
  // Byte-aligned, 2 bytes big-endian at offset 1 -> 0x807F -> -32641.
  CHECK_EQ(sign_extend(extract_bytes(payload, 8, 1, 2, true), 16), -32641);
  // Byte-aligned, 2 bytes little-endian at offset 1 -> 0x7F80 -> 32640.
  CHECK_EQ(sign_extend(extract_bytes(payload, 8, 1, 2, false), 16), 32640);
  // Bit-level, 4 bits at bit 4 -> high nibble of 0xFF -> 0xF -> -1.
  CHECK_EQ(sign_extend(extract_bits(payload, 8, 4, 4), 4), -1);
  // Bit-level, 12 bits at bit 0 -> 0x0FF | (0x0 << 8) ... = 0x0FF -> 255.
  CHECK_EQ(sign_extend(extract_bits(payload, 8, 0, 12), 12), 255);
}

// ---------------------------------------------------------------------------
// SNA — the reserved "signal not available" raw value
// ---------------------------------------------------------------------------
//
// Model, again hand-computed rather than re-derived from the code:
//   - width_all_ones(w) = 2^w - 1, for w in 1..32; w >= 32 answers 0xFFFFFFFF
//     without shifting (1u << 32 is UB, and UBSan here is fatal)
//   - raw_is_sna(raw, width, mode, sna_raw): ALL_ONES compares against
//     width_all_ones(width), EXPLICIT against sna_raw, NONE never matches
//   - the comparison is on the RAW value, before sign_extend()

TEST(decode_sna_all_ones_per_width) {
  struct Row {
    uint8_t width;
    uint32_t all_ones;
  };
  // Every width the sensor can configure that is worth naming: the 1-bit edge,
  // the DBC widths the evidence sampled (2, 10), the byte-aligned widths, and
  // the 32-bit edge where the shift would overflow.
  const Row rows[] = {
      {1, 0x00000001},  {2, 0x00000003},  {3, 0x00000007},  {4, 0x0000000F},  {8, 0x000000FF},
      {10, 0x000003FF}, {12, 0x00000FFF}, {16, 0x0000FFFF}, {24, 0x00FFFFFF}, {31, 0x7FFFFFFF},
      {32, 0xFFFFFFFF},
  };
  for (const Row &row : rows) {
    std::string where = std::string("width=") + std::to_string(row.width);
    CHECK_EQ_MSG(width_all_ones(row.width), row.all_ones, where);
    CHECK_EQ_MSG(raw_is_sna(row.all_ones, row.width, SnaMode::ALL_ONES, 0), true, where);
  }
}

TEST(decode_sna_one_below_all_ones_is_a_reading) {
  // The value immediately below the marker is the widest real reading and must
  // survive; so must 0 and a mid-range value.
  for (uint8_t width = 1; width <= 32; width++) {
    uint32_t all_ones = width_all_ones(width);
    std::string where = std::string("width=") + std::to_string(width);
    CHECK_EQ_MSG(raw_is_sna(all_ones - 1u, width, SnaMode::ALL_ONES, 0), false, where);
    CHECK_EQ_MSG(raw_is_sna(0, width, SnaMode::ALL_ONES, 0), false, where);
    // Half the range: all-ones with the top bit cleared, never the marker.
    CHECK_EQ_MSG(raw_is_sna(all_ones >> 1, width, SnaMode::ALL_ONES, 0), false, where);
  }
}

TEST(decode_sna_32_bit_width_does_not_shift_overflow) {
  // Explicit, because `1u << 32` is undefined behavior and the build runs UBSan
  // with -fno-sanitize-recover: this case fails the whole run if the guard goes.
  CHECK_EQ(width_all_ones(32), 0xFFFFFFFFu);
  CHECK_EQ(raw_is_sna(0xFFFFFFFFu, 32, SnaMode::ALL_ONES, 0), true);
  CHECK_EQ(raw_is_sna(0xFFFFFFFEu, 32, SnaMode::ALL_ONES, 0), false);
  // A 32-bit field extracted from a full 0xFF payload is the marker.
  const uint8_t payload[MAX_FRAME_DATA_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
  CHECK_EQ(raw_is_sna(extract_bytes(payload, 8, 0, 4, false), 32, SnaMode::ALL_ONES, 0), true);
  CHECK_EQ(raw_is_sna(extract_bits(payload, 8, 0, 32), 32, SnaMode::ALL_ONES, 0), true);
}

TEST(decode_sna_is_tested_before_sign_extension) {
  // A signed 8-bit signal at SNA: raw 0xFF, which sign-extends to -1. The test
  // has to happen on the raw value — after sign_extend() the marker and a real
  // -1 are the same number.
  const uint8_t payload[MAX_FRAME_DATA_LEN] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint32_t raw8 = extract_bytes(payload, 8, 0, 1, false);
  CHECK_EQ(raw8, 0x000000FFu);
  CHECK_EQ(sign_extend(raw8, 8), -1);
  CHECK_EQ(raw_is_sna(raw8, 8, SnaMode::ALL_ONES, 0), true);

  // Same bit pattern, a 16-bit signal: 0x00FF is an ordinary 255, not the
  // marker, because the width the marker is taken at is the signal's own.
  uint32_t raw16 = extract_bytes(payload, 8, 1, 2, false);
  CHECK_EQ(raw16, 0x000000FFu);
  CHECK_EQ(sign_extend(raw16, 16), 255);
  CHECK_EQ(raw_is_sna(raw16, 16, SnaMode::ALL_ONES, 0), false);

  // A genuine -1 in a 16-bit signed signal that declares an EXPLICIT marker
  // elsewhere stays -1: only the declared raw value is swallowed.
  CHECK_EQ(raw_is_sna(0x0000FFFFu, 16, SnaMode::EXPLICIT, 0x00008000u), false);
  CHECK_EQ(sign_extend(0x0000FFFFu, 16), -1);
  // ...whereas that DBC's actual marker, raw 0x8000, does match.
  CHECK_EQ(raw_is_sna(0x00008000u, 16, SnaMode::EXPLICIT, 0x00008000u), true);
}

TEST(decode_sna_explicit_matches_only_that_value) {
  // The escape hatch: a 10-bit signal reserving raw 1022 (not all-ones).
  const uint8_t width = 10;
  const uint32_t marker = 1022;
  for (uint32_t raw = 0; raw <= width_all_ones(width); raw++) {
    bool expected = raw == marker;
    CHECK_EQ_MSG(raw_is_sna(raw, width, SnaMode::EXPLICIT, marker), expected,
                 std::string("raw=") + std::to_string(raw));
  }
  // All-ones is NOT special in EXPLICIT mode — 1023 is a reading here.
  CHECK_EQ(raw_is_sna(1023, width, SnaMode::EXPLICIT, marker), false);
  // ...and the width is irrelevant in EXPLICIT mode: the same marker matches
  // whatever field it was configured on.
  CHECK_EQ(raw_is_sna(marker, 16, SnaMode::EXPLICIT, marker), true);
  // 0 is a legal explicit marker (a DBC may reserve it) and must not be read
  // as "no SNA configured".
  CHECK_EQ(raw_is_sna(0, 8, SnaMode::EXPLICIT, 0), true);
  CHECK_EQ(raw_is_sna(1, 8, SnaMode::EXPLICIT, 0), false);
}

TEST(decode_sna_disabled_never_matches) {
  // The default. Back-compat is the point: with no `sna:` key nothing is ever
  // swallowed, whatever the raw value or the leftover sna_raw argument.
  for (uint8_t width = 1; width <= 32; width++) {
    uint32_t all_ones = width_all_ones(width);
    std::string where = std::string("width=") + std::to_string(width);
    CHECK_EQ_MSG(raw_is_sna(all_ones, width, SnaMode::NONE, all_ones), false, where);
    CHECK_EQ_MSG(raw_is_sna(0, width, SnaMode::NONE, 0), false, where);
    CHECK_EQ_MSG(raw_is_sna(all_ones >> 1, width, SnaMode::NONE, 0xFFFFFFFFu), false, where);
  }
}

TEST(decode_sna_composes_with_extraction) {
  // The 10-bit state-of-charge field from the report that motivated the key:
  // at SNA it decodes to 1023, which a 0.1 %/bit filter renders as 102.3 %.
  // Payload: 0x03FF little-endian at bit 0, then the same field one count
  // lower in a second frame.
  const uint8_t sna_payload[MAX_FRAME_DATA_LEN] = {0xFF, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t live_payload[MAX_FRAME_DATA_LEN] = {0xFE, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint32_t sna_raw = extract_bits(sna_payload, 8, 0, 10);
  uint32_t live_raw = extract_bits(live_payload, 8, 0, 10);
  CHECK_EQ(sna_raw, 1023u);
  CHECK_EQ(live_raw, 1022u);
  CHECK_EQ(raw_is_sna(sna_raw, 10, SnaMode::ALL_ONES, 0), true);
  CHECK_EQ(raw_is_sna(live_raw, 10, SnaMode::ALL_ONES, 0), false);

  // A 2-bit status field packed above it: SNA is raw 3, per the same DBC.
  const uint8_t status_payload[MAX_FRAME_DATA_LEN] = {0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CHECK_EQ(extract_bits(status_payload, 8, 10, 2), 3u);
  CHECK_EQ(raw_is_sna(extract_bits(status_payload, 8, 10, 2), 2, SnaMode::ALL_ONES, 0), true);
  CHECK_EQ(raw_is_sna(extract_bits(status_payload, 8, 12, 2), 2, SnaMode::ALL_ONES, 0), false);
}

// ---------------------------------------------------------------------------
// estimate_frame_bits — the bus-load statistic
// ---------------------------------------------------------------------------

TEST(decode_frame_bit_estimate) {
  // Documented model: standard 47 bits + 8 * DLC, extended 67 bits + 8 * DLC,
  // RTR frames carry no data bits.
  CHECK_EQ(estimate_frame_bits(false, false, 0), 47u);
  CHECK_EQ(estimate_frame_bits(false, false, 1), 55u);
  CHECK_EQ(estimate_frame_bits(false, false, 8), 111u);
  CHECK_EQ(estimate_frame_bits(true, false, 0), 67u);
  CHECK_EQ(estimate_frame_bits(true, false, 8), 131u);
  CHECK_EQ(estimate_frame_bits(false, true, 8), 47u);
  CHECK_EQ(estimate_frame_bits(true, true, 8), 67u);
}
