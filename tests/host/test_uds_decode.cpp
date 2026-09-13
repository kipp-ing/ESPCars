// Field extraction and scale evaluation (components/uds/uds_decode.h) against
// docs/uds-catalog-format.md §5.1 and §6.
//
// Every extraction vector below is hand-computed from the documented algorithm — MSB-first bit
// numbering, bits running across byte boundaries without gaps, BYTESWAP over ceil(bit_size/8)
// bytes, two's-complement sign extension — not replayed from the code. The scale cases assert
// the one rule that matters: a raw the catalog does not explain is "not available", never a
// number.

#include "uds_blob_builder.h"
#include "uds_decode.h"
#include "harness.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::uds;
using udstest::BlobBuilder;

namespace {

/// Response used by most numeric vectors, as one 0x22 positive response:
/// header 62 02 07, then payload 12 34 AB CD FF 80 01.
const uint8_t RESP[] = {0x62, 0x02, 0x07, 0x12, 0x34, 0xAB, 0xCD, 0xFF, 0x80, 0x01};

FieldView make_field(uint16_t bit_pos, uint8_t bit_size, uint8_t flags, uint8_t repeat_count = 1,
                     uint16_t repeat_stride = 0) {
  FieldView f{};
  f.name = "f";
  f.unit = "";
  f.bit_pos = bit_pos;
  f.bit_size = bit_size;
  f.flags = flags;
  f.scale_first = 0;
  f.scale_count = 0;
  f.repeat_count = repeat_count;
  f.repeat_stride = repeat_stride;
  f.group_index = 0;
  return f;
}

std::string tag(uint16_t bit_pos, uint8_t bit_size, uint8_t flags) {
  return "bit_pos=" + std::to_string(bit_pos) + " bit_size=" + std::to_string(bit_size) +
         " flags=" + std::to_string(flags);
}

bool approx(float got, float want, float tol = 1e-3f) { return std::fabs(got - want) <= tol; }

}  // namespace

// ---------------------------------------------------------------------------
// Extraction truth table (§5.1)
// ---------------------------------------------------------------------------

TEST(uds_extract_unsigned_vectors) {
  struct Row {
    uint16_t bit_pos;
    uint8_t bit_size;
    uint8_t flags;
    int64_t expected;
  };
  const Row rows[] = {
      // byte-aligned
      {24, 8, 0, 0x12},
      {32, 8, 0, 0x34},
      {24, 16, 0, 0x1234},
      {24, 32, 0, 0x1234ABCD},
      {0, 24, 0, 0x620207},  // the response header itself: bit_pos 0 is resp[0]'s MSB
      // sub-byte and cross-byte
      {24, 4, 0, 0x1},
      {28, 4, 0, 0x2},
      {28, 8, 0, 0x23},  // low nibble of 0x12, high nibble of 0x34
      {24, 12, 0, 0x123},
      {44, 12, 0, 0xBCD},
      {25, 1, 0, 0},  // 0x12 = 0001 0010: bit 1 (MSB-first) is 0
      {27, 1, 0, 1},  //                    bit 3 is 1
      {56, 1, 0, 1},  // MSB of 0xFF
      // BYTESWAP reverses ceil(bit_size/8) bytes of the accumulator
      {24, 16, FIELD_FLAG_BYTESWAP, 0x3412},
      {24, 32, FIELD_FLAG_BYTESWAP, static_cast<int64_t>(0xCDAB3412u)},
      {24, 24, FIELD_FLAG_BYTESWAP, 0xAB3412},
      {24, 8, FIELD_FLAG_BYTESWAP, 0x12},  // one byte: a no-op, not an accident
  };
  for (const Row &row : rows) {
    int64_t got = -1;
    const FieldView f = make_field(row.bit_pos, row.bit_size, row.flags);
    CHECK_MSG(extract_raw(RESP, sizeof(RESP), f, 0, &got), tag(row.bit_pos, row.bit_size, row.flags));
    CHECK_EQ_MSG(got, row.expected, tag(row.bit_pos, row.bit_size, row.flags));
  }
}

TEST(uds_extract_signed_vectors) {
  struct Row {
    uint16_t bit_pos;
    uint8_t bit_size;
    uint8_t flags;
    int64_t expected;
  };
  const Row rows[] = {
      {40, 8, FIELD_FLAG_SIGNED, -85},      // 0xAB
      {40, 16, FIELD_FLAG_SIGNED, -21555},  // 0xABCD
      {40, 4, FIELD_FLAG_SIGNED, -6},       // 0xA
      {44, 12, FIELD_FLAG_SIGNED, -1075},   // 0xBCD
      {24, 16, FIELD_FLAG_SIGNED, 4660},    // 0x1234: positive stays positive
      {24, 4, FIELD_FLAG_SIGNED, 1},
      {40, 16, FIELD_FLAG_SIGNED | FIELD_FLAG_BYTESWAP, -12885},  // 0xABCD -> 0xCDAB, then sign
  };
  for (const Row &row : rows) {
    int64_t got = 0;
    const FieldView f = make_field(row.bit_pos, row.bit_size, row.flags);
    CHECK_MSG(extract_raw(RESP, sizeof(RESP), f, 0, &got), tag(row.bit_pos, row.bit_size, row.flags));
    CHECK_EQ_MSG(got, row.expected, tag(row.bit_pos, row.bit_size, row.flags));
  }
}

TEST(uds_extract_64_bit) {
  int64_t got = 0;
  // Bytes 2..9 of RESP: 07 12 34 AB CD FF 80 01.
  CHECK(extract_raw(RESP, sizeof(RESP), make_field(16, 64, 0), 0, &got));
  CHECK_EQ(got, static_cast<int64_t>(0x071234ABCDFF8001LL));

  const uint8_t ones[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(extract_raw(ones, sizeof(ones), make_field(0, 64, FIELD_FLAG_SIGNED), 0, &got));
  CHECK_EQ(got, static_cast<int64_t>(-1));

  uint8_t min64[8] = {0x80, 0, 0, 0, 0, 0, 0, 0};
  CHECK(extract_raw(min64, sizeof(min64), make_field(0, 64, FIELD_FLAG_SIGNED), 0, &got));
  CHECK_EQ(got, INT64_MIN);

  // Signed just past the 32-bit line: bits 0..32 read 1 followed by 32 zeros (byte 4's MSB is
  // bit 32 and must stay clear), raw 2^32, sign bit set -> 2^32 - 2^33.
  uint8_t w33[5] = {0x80, 0, 0, 0, 0x00};
  CHECK(extract_raw(w33, sizeof(w33), make_field(0, 33, FIELD_FLAG_SIGNED), 0, &got));
  CHECK_EQ(got, static_cast<int64_t>(-4294967296LL));
}

TEST(uds_extract_every_width_1_through_64) {
  // All-ones input: unsigned width w reads (1 << w) - 1, signed width w reads -1 — at every
  // width, so UBSan sees the accumulator shifts and the sign extension at each edge.
  const uint8_t ones[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  for (uint8_t w = 1; w <= 63; w++) {
    int64_t got = 0;
    CHECK_MSG(extract_raw(ones, sizeof(ones), make_field(0, w, 0), 0, &got), "unsigned width " + std::to_string(w));
    CHECK_EQ_MSG(got, static_cast<int64_t>((uint64_t{1} << w) - 1), "unsigned width " + std::to_string(w));
    CHECK_MSG(extract_raw(ones, sizeof(ones), make_field(0, w, FIELD_FLAG_SIGNED), 0, &got),
              "signed width " + std::to_string(w));
    CHECK_EQ_MSG(got, static_cast<int64_t>(-1), "signed width " + std::to_string(w));
  }
}

TEST(uds_extract_repeat_array_with_stride) {
  // The format requires readers to implement the stride even though v1 compilers emit
  // repeat_count == 1 (§5): elements at bit_pos + k*stride.
  const FieldView f = make_field(24, 16, 0, /*repeat_count=*/3, /*repeat_stride=*/16);
  int64_t got = 0;
  CHECK(extract_raw(RESP, sizeof(RESP), f, 0, &got));
  CHECK_EQ(got, 0x1234);
  CHECK(extract_raw(RESP, sizeof(RESP), f, 1, &got));
  CHECK_EQ(got, 0xABCD);
  CHECK(extract_raw(RESP, sizeof(RESP), f, 2, &got));
  CHECK_EQ(got, 0xFF80);
  CHECK(!extract_raw(RESP, sizeof(RESP), f, 3, &got));  // past repeat_count

  // A stride wider than the element leaves a gap byte between elements.
  const FieldView gap = make_field(24, 8, 0, 2, 24);
  CHECK(extract_raw(RESP, sizeof(RESP), gap, 0, &got));
  CHECK_EQ(got, 0x12);
  CHECK(extract_raw(RESP, sizeof(RESP), gap, 1, &got));
  CHECK_EQ(got, 0xCD);
}

TEST(uds_extract_skips_fields_the_response_does_not_cover) {
  // Exact-size heap buffer: a read past resp_len is an ASan report, not just a wrong value.
  uint8_t *resp = new uint8_t[5];
  std::memcpy(resp, RESP, 5);
  int64_t got = 0;
  CHECK(extract_raw(resp, 5, make_field(24, 16, 0), 0, &got));  // ends exactly at byte 4
  CHECK_EQ(got, 0x1234);
  CHECK(!extract_raw(resp, 5, make_field(24, 17, 0), 0, &got));  // one bit into byte 5
  CHECK(!extract_raw(resp, 5, make_field(32, 16, 0), 0, &got));
  CHECK(!extract_raw(resp, 5, make_field(40, 8, 0), 0, &got));  // entirely past the end
  // An array whose later elements run off the end: covered elements extract, the rest skip.
  const FieldView arr = make_field(24, 16, 0, 3, 16);
  CHECK(!extract_raw(resp, 5, arr, 1, &got));
  delete[] resp;
}

TEST(uds_extract_rejects_degenerate_geometry) {
  int64_t got = 0;
  CHECK(!extract_raw(RESP, sizeof(RESP), make_field(24, 0, 0), 0, &got));   // bit_size 0
  CHECK(!extract_raw(RESP, sizeof(RESP), make_field(24, 65, 0), 0, &got));  // above the accumulator
  FieldView f = make_field(24, 8, 0);
  f.repeat_count = 0;  // v1 compilers must emit 1; 0 is a malformed record, not "scalar"
  CHECK(!extract_raw(RESP, sizeof(RESP), f, 0, &got));
  CHECK(!extract_raw(nullptr, 8, make_field(0, 8, 0), 0, &got));
  CHECK(!extract_raw(RESP, sizeof(RESP), make_field(0, 8, 0), 0, nullptr));
}

TEST(uds_extract_ascii) {
  const uint8_t resp[] = {0x5A, 0x90, 'W', 'M', 'E', '4', '5', '1'};
  const FieldView f = make_field(16, 48, FIELD_FLAG_ASCII);
  char out[8];
  CHECK(extract_ascii(resp, sizeof(resp), f, 0, out, sizeof(out)));
  CHECK(std::strcmp(out, "WME451") == 0);
  // The terminator needs its byte: 6 characters do not fit a 6-byte buffer.
  CHECK(!extract_ascii(resp, sizeof(resp), f, 0, out, 6));
  CHECK(extract_ascii(resp, sizeof(resp), f, 0, out, 7));
  // Not byte-aligned: there is no byte to copy "verbatim" from bit 17 (see the header comment;
  // reported as a spec under-specification).
  CHECK(!extract_ascii(resp, sizeof(resp), make_field(17, 48, FIELD_FLAG_ASCII), 0, out, sizeof(out)));
  CHECK(!extract_ascii(resp, sizeof(resp), make_field(16, 44, FIELD_FLAG_ASCII), 0, out, sizeof(out)));
  // Longer than the response.
  CHECK(!extract_ascii(resp, sizeof(resp), make_field(16, 56, FIELD_FLAG_ASCII), 0, out, sizeof(out)));
}

TEST(uds_extract_hexdump_requires_byte_alignment) {
  // §5.1: ASCII and HEXDUMP both require bit_pos % 8 == 0 and bit_size % 8 == 0. The rule lives in
  // field_bytes(), so both paths are pinned here against the same geometries.
  const uint8_t resp[] = {0x62, 0xD0, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
  char out[32];
  CHECK_EQ(extract_hexdump(resp, sizeof(resp), make_field(24, 32, FIELD_FLAG_HEXDUMP), 0, out, sizeof(out)),
           static_cast<size_t>(8));
  CHECK(std::strcmp(out, "DEADBEEF") == 0);
  // Misaligned start, misaligned size, and a field the response does not cover: each refused.
  CHECK_EQ(extract_hexdump(resp, sizeof(resp), make_field(25, 32, FIELD_FLAG_HEXDUMP), 0, out, sizeof(out)),
           static_cast<size_t>(0));
  CHECK_EQ(extract_hexdump(resp, sizeof(resp), make_field(24, 28, FIELD_FLAG_HEXDUMP), 0, out, sizeof(out)),
           static_cast<size_t>(0));
  CHECK_EQ(extract_hexdump(resp, sizeof(resp), make_field(24, 40, FIELD_FLAG_HEXDUMP), 0, out, sizeof(out)),
           static_cast<size_t>(0));
  // A stride array of byte-oriented elements works the same way.
  const FieldView arr = make_field(24, 16, FIELD_FLAG_HEXDUMP, 2, 16);
  CHECK_EQ(extract_hexdump(resp, sizeof(resp), arr, 1, out, sizeof(out)), static_cast<size_t>(4));
  CHECK(std::strcmp(out, "BEEF") == 0);
  // The borrowed range is the same one ASCII copies from.
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  CHECK(field_bytes(resp, sizeof(resp), make_field(24, 32, FIELD_FLAG_HEXDUMP), 0, &bytes, &len));
  CHECK_EQ(len, static_cast<size_t>(4));
  CHECK(bytes == resp + 3);
  CHECK(!field_bytes(resp, sizeof(resp), make_field(25, 32, 0), 0, &bytes, &len));
}

TEST(uds_format_hexdump_uppercase_pairs) {
  const uint8_t bytes[] = {0xDE, 0xAD, 0x01, 0xEF};
  char out[16];
  CHECK_EQ(format_hexdump(bytes, sizeof(bytes), out, sizeof(out)), static_cast<size_t>(8));
  CHECK(std::strcmp(out, "DEAD01EF") == 0);
  CHECK_EQ(format_hexdump(bytes, sizeof(bytes), out, 8), static_cast<size_t>(0));  // no room for the NUL
  CHECK_EQ(format_hexdump(bytes, 0, out, sizeof(out)), static_cast<size_t>(0));
  CHECK(std::strcmp(out, "") == 0);  // len 0 still terminates
}

// ---------------------------------------------------------------------------
// Scale evaluation (§6)
// ---------------------------------------------------------------------------

namespace {

/// One catalog carrying every scale geometry the cases below need. Field indices:
///   0 linear+sentinel, 1 enum+sentinel, 2 overlapping rows, 3 raw passthrough, 4 negative bounds
std::vector<uint8_t> make_scale_blob() {
  BlobBuilder b;
  b.add_field({"lin", "V", 24, 16, 0, 0, 2, 1, 0, 0});
  b.add_field({"state", "", 24, 8, FIELD_FLAG_ENUM, 2, 3, 1, 0, 0});
  b.add_field({"overlap", "", 24, 8, 0, 5, 2, 1, 0, 0});
  b.add_field({"raw", "", 24, 16, 0, 0, 0, 1, 0, 0});
  b.add_field({"neg", "", 24, 8, FIELD_FLAG_SIGNED, 7, 1, 1, 0, 0});
  b.add_field({"invalid_no_text", "", 24, 8, 0, 8, 2, 1, 0, 0});
  b.add_scale({0, 64999, 0.001f, 0.0f, "", 0});                                                // 0
  b.add_scale({65535, 65535, 0.0f, 0.0f, "SNA", SCALE_FLAG_TEXT | SCALE_FLAG_INVALID});        // 1
  b.add_scale({0, 0, 1.0f, 0.0f, "OPEN", SCALE_FLAG_TEXT});                                    // 2
  b.add_scale({1, 1, 1.0f, 0.0f, "CLOSED", SCALE_FLAG_TEXT});                                  // 3
  b.add_scale({255, 255, 0.0f, 0.0f, "not available", SCALE_FLAG_TEXT | SCALE_FLAG_INVALID});  // 4
  b.add_scale({0, 100, 1.0f, 0.0f, "", 0});                                                    // 5
  b.add_scale({50, 150, 2.0f, 100.0f, "", 0});                                                 // 6
  b.add_scale({-100, -1, 0.5f, 0.0f, "", 0});                                                  // 7
  b.add_scale({254, 254, 1.0f, 0.0f, "", SCALE_FLAG_INVALID});                                 // 8
  b.add_scale({0, 253, 1.0f, 0.0f, "", 0});                                                    // 9
  return b.build();
}

FieldView fetch_field(const Catalog &cat, uint32_t i) {
  FieldView f{};
  CHECK(cat.field(i, &f));
  return f;
}

}  // namespace

TEST(uds_scale_linear) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const Decoded d = evaluate_scales(cat, fetch_field(cat, 0), 12345);
  CHECK(d.valid);
  CHECK(!d.is_text);
  CHECK(d.text == nullptr);
  CHECK(approx(d.value, 12.345f));
}

TEST(uds_scale_sentinel_is_not_available) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const Decoded d = evaluate_scales(cat, fetch_field(cat, 0), 65535);
  CHECK(!d.valid);
  CHECK(std::isnan(d.value));  // a numeric sensor publishes NaN, never a number
  CHECK(d.is_text);
  CHECK(d.text != nullptr && std::strcmp(d.text, "SNA") == 0);  // a text sensor publishes the text
}

TEST(uds_scale_enum_text) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const FieldView f = fetch_field(cat, 1);
  Decoded d = evaluate_scales(cat, f, 0);
  CHECK(d.valid);
  CHECK(d.is_text);
  CHECK(d.text != nullptr && std::strcmp(d.text, "OPEN") == 0);
  CHECK(approx(d.value, 0.0f));  // a numeric sensor gets raw * factor + offset
  d = evaluate_scales(cat, f, 1);
  CHECK(d.valid);
  CHECK(d.text != nullptr && std::strcmp(d.text, "CLOSED") == 0);
  CHECK(approx(d.value, 1.0f));
  d = evaluate_scales(cat, f, 255);  // the enum's own sentinel row
  CHECK(!d.valid);
  CHECK(std::isnan(d.value));
  CHECK(d.text != nullptr && std::strcmp(d.text, "not available") == 0);
}

TEST(uds_scale_first_match_wins) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const FieldView f = fetch_field(cat, 2);
  // 60 is inside both [0,100] and [50,150]; table order decides, not best fit.
  Decoded d = evaluate_scales(cat, f, 60);
  CHECK(d.valid);
  CHECK(approx(d.value, 60.0f));
  // 120 only matches the second row.
  d = evaluate_scales(cat, f, 120);
  CHECK(d.valid);
  CHECK(approx(d.value, 340.0f));
}

TEST(uds_scale_no_row_matches_is_not_available) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  // 200 is outside every declared range: a decode we do not understand, so NaN — publishing a
  // confident number here is the failure mode §6 exists to prevent.
  const Decoded d = evaluate_scales(cat, fetch_field(cat, 2), 200);
  CHECK(!d.valid);
  CHECK(std::isnan(d.value));
  CHECK(!d.is_text);
  CHECK(d.text == nullptr);
}

TEST(uds_scale_invalid_without_text_is_still_not_available) {
  // §6: INVALID is authoritative on its own. The flag says what the value *is*; TEXT only says
  // whether there is a string to show for it. So this row yields NaN and no text — publishing
  // raw * factor + offset from a row the compiler marked invalid is exactly the confident wrong
  // number the table exists to prevent.
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const FieldView f = fetch_field(cat, 5);
  Decoded d = evaluate_scales(cat, f, 254);
  CHECK(!d.valid);
  CHECK(std::isnan(d.value));
  CHECK(!d.is_text);
  CHECK(d.text == nullptr);
  // The row after it still scales normally, so the sentinel is the row's doing, not the field's.
  d = evaluate_scales(cat, f, 12);
  CHECK(d.valid);
  CHECK(approx(d.value, 12.0f));
}

TEST(uds_scale_count_zero_is_raw_passthrough) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const Decoded d = evaluate_scales(cat, fetch_field(cat, 3), 42);
  CHECK(d.valid);
  CHECK(!d.is_text);
  CHECK(approx(d.value, 42.0f));
}

TEST(uds_scale_negative_bounds) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const FieldView f = fetch_field(cat, 4);
  Decoded d = evaluate_scales(cat, f, -50);
  CHECK(d.valid);
  CHECK(approx(d.value, -25.0f));
  d = evaluate_scales(cat, f, 5);  // above the only row
  CHECK(!d.valid);
  CHECK(std::isnan(d.value));
}

TEST(uds_decode_numeric_end_to_end) {
  const std::vector<uint8_t> blob = make_scale_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  const FieldView f = fetch_field(cat, 0);
  const uint8_t resp[] = {0x62, 0x02, 0x07, 0x30, 0x39};  // raw 0x3039 = 12345 at bit 24
  Decoded d = decode_numeric(cat, f, resp, sizeof(resp));
  CHECK(d.valid);
  CHECK(approx(d.value, 12.345f));
  // A response too short to cover the field is not-available — skipped and counted, never read.
  d = decode_numeric(cat, f, resp, 4);
  CHECK(!d.valid);
  CHECK(std::isnan(d.value));
}

// ---------------------------------------------------------------------------
// field_covered — the question a PARTIAL response has to ask, per field
// ---------------------------------------------------------------------------

TEST(uds_decode_field_covered_scalar) {
  // A 16-bit field at bit 24 needs 5 bytes. Exactly 5 covers it; 4 does not.
  const FieldView f = make_field(24, 16, 0);
  CHECK(field_covered(f, 5));
  CHECK(field_covered(f, 9));
  CHECK(!field_covered(f, 4));
  CHECK(!field_covered(f, 0));
  // The predicate and the extractor must never disagree — that is the whole point of sharing it.
  int64_t raw = 0;
  CHECK_EQ(field_covered(f, 5), extract_raw(RESP, 5, f, 0, &raw));
  CHECK_EQ(field_covered(f, 4), extract_raw(RESP, 4, f, 0, &raw));
}

TEST(uds_decode_field_covered_array_element_by_element) {
  // The 0x0208 shape in miniature: a 4-element array of 16-bit words at bit 24, against a response
  // carrying only the first two. The declared length is not the answer for any single element —
  // each is judged on its own, which is what makes a pack narrower than its database entry readable
  // from a field the database sizes for more elements than are actually wired.
  const FieldView f = make_field(24, 16, 0, 4, 16);
  // A buffer that really is long enough for element 3, because the cross-check below passes these
  // lengths to extract_raw() — and ASan is watching. (It caught the first version of this test
  // claiming 11 bytes of the 10-byte RESP, which is exactly the mistake the predicate exists to
  // stop a caller making against a real response.)
  const uint8_t wide[11] = {0x62, 0x02, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  const size_t needed[] = {5, 7, 9, 11};
  for (uint16_t k = 0; k < 4; k++) {
    CHECK_MSG(field_covered(f, needed[k], k), "element " + std::to_string(k) + " at its exact length");
    CHECK_MSG(!field_covered(f, needed[k] - 1, k), "element " + std::to_string(k) + " one byte short");
    int64_t raw = 0;
    CHECK_EQ(field_covered(f, needed[k], k), extract_raw(wide, needed[k], f, k, &raw));
    CHECK_EQ(field_covered(f, needed[k] - 1, k), extract_raw(wide, needed[k] - 1, f, k, &raw));
  }
  // A 7-byte response covers elements 0 and 1 and neither of the rest.
  CHECK(field_covered(f, 7, 0));
  CHECK(field_covered(f, 7, 1));
  CHECK(!field_covered(f, 7, 2));
  CHECK(!field_covered(f, 7, 3));
}

TEST(uds_decode_field_covered_rejects_impossible_requests) {
  const FieldView f = make_field(24, 16, 0, 4, 16);
  CHECK(!field_covered(f, 1000, 4));    // past the array, however long the response
  CHECK(!field_covered(f, 1000, 255));  // and far past it
  CHECK(!field_covered(make_field(24, 0, 0), 1000));
  FieldView no_elements = f;
  no_elements.repeat_count = 0;
  CHECK(!field_covered(no_elements, 1000));
}

TEST(uds_decode_field_covered_matches_field_bytes_for_ascii) {
  // The byte-oriented path has to agree too, or ASCII and HEXDUMP bindings would report coverage
  // differently from numeric ones.
  const FieldView f = make_field(24, 32, FIELD_FLAG_ASCII);
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  CHECK_EQ(field_covered(f, 7), field_bytes(RESP, 7, f, 0, &bytes, &len));
  CHECK_EQ(field_covered(f, 6), field_bytes(RESP, 6, f, 0, &bytes, &len));
  CHECK(field_covered(f, 7));
  CHECK(!field_covered(f, 6));
}
