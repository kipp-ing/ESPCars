#pragma once

/// Freestanding field extraction and scale evaluation for UDS responses, per
/// docs/uds-catalog-format.md §5.1 (the normative bit-extraction algorithm) and §6 (scale rows).
/// No ESPHome, no ESP-IDF, no allocation, no exceptions; everything here is exercised by
/// tests/host against hand-computed vectors.
///
/// The one rule that shapes this file: **a value the catalog does not explain is "not
/// available", never a number.** A sentinel row, a raw outside every declared range, a field the
/// response is too short to cover — each yields `valid == false`, because publishing a confident
/// number for a decode we do not understand is the failure mode the format exists to prevent.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "uds_catalog.h"

namespace esphome::uds {

// -------------------------------------------------------------------------------------------
// Bit extraction (§5.1)
// -------------------------------------------------------------------------------------------

/// Is element `element` of `f` fully inside a response of `resp_len` bytes?
///
/// The same arithmetic `extract_raw()` and `field_bytes()` apply before reading, exposed as a
/// question so the component can *report* an uncovered field instead of only failing to decode it.
/// It does not replace those guards: they still refuse to read outside the buffer whatever a caller
/// asked, because a coverage predicate that a caller may forget is not a bounds check.
///
/// A short response is ordinary, not malformed (format §4): an ECU that answers with fewer array
/// elements than its database declares covers the elements it sent and no more, and each one has
/// to be judged on its own.
inline bool field_covered(const FieldView &f, size_t resp_len, uint16_t element = 0) {
  if (f.bit_size == 0 || f.repeat_count == 0 || element >= f.repeat_count)
    return false;
  const uint32_t start = static_cast<uint32_t>(f.bit_pos) + static_cast<uint32_t>(element) * f.repeat_stride;
  const uint32_t end = start + f.bit_size;  // exclusive
  return static_cast<uint64_t>(end + 7u) / 8u <= resp_len;
}

/// Extract array element `element` (0-based, < repeat_count; 0 for a scalar) of a numeric field.
/// Bit numbering is MSB-first within each byte and runs across byte boundaries without gaps;
/// BYTESWAP then reverses the low ceil(bit_size/8) bytes of the accumulator; SIGNED sign-extends.
///
/// Returns false — and writes nothing — when the field's bits are not fully covered by
/// `resp_len`: a short response skips the field, it never reads past the buffer (format §4).
/// v1 compilers emit repeat_count == 1, but the stride is implemented here so a later catalog
/// works against unchanged firmware (§5).
///
/// `*out` is the sign-extended value as int64_t. The one width where that representation is
/// lossy: an *unsigned* 64-bit raw above INT64_MAX arrives as its two's-complement negative. Such
/// a value is outside every i32 scale bound either way, so it decodes to "not available" — only a
/// scale-less (raw passthrough) 64-bit unsigned field would show the difference, and no factory
/// database field is 64-bit unsigned raw.
inline bool extract_raw(const uint8_t *resp, size_t resp_len, const FieldView &f, uint16_t element, int64_t *out) {
  if (resp == nullptr || out == nullptr)
    return false;
  if (f.bit_size == 0 || f.bit_size > 64)
    return false;
  if (f.repeat_count == 0 || element >= f.repeat_count)
    return false;
  // element <= 254 and stride <= 65535, so the bit offset fits comfortably in 32 bits.
  const uint32_t start = static_cast<uint32_t>(f.bit_pos) + static_cast<uint32_t>(element) * f.repeat_stride;
  const uint32_t end = start + f.bit_size;  // exclusive
  if (static_cast<uint64_t>(end + 7u) / 8u > resp_len)
    return false;
  uint64_t raw = 0;
  for (uint32_t i = 0; i < f.bit_size; i++) {
    const uint32_t bit_index = start + i;
    raw = (raw << 1) | ((resp[bit_index >> 3] >> (7u - (bit_index & 7u))) & 1u);
  }
  if ((f.flags & FIELD_FLAG_BYTESWAP) != 0) {
    const uint32_t n = (static_cast<uint32_t>(f.bit_size) + 7u) / 8u;
    uint64_t swapped = 0;
    for (uint32_t j = 0; j < n; j++)
      swapped |= ((raw >> (8 * j)) & 0xFFu) << (8 * (n - 1 - j));
    raw = swapped;
  }
  if ((f.flags & FIELD_FLAG_SIGNED) != 0 && f.bit_size < 64 && ((raw >> (f.bit_size - 1)) & 1u) != 0) {
    // Two's-complement sign extension: the unsigned subtraction wraps, the cast below reads the
    // wrapped pattern back as the intended negative. bit_size == 64 needs no extension (and
    // 1 << 64 would be UB, which is why the guard is part of the algorithm, not an optimisation).
    raw -= (static_cast<uint64_t>(1) << f.bit_size);
  }
  *out = static_cast<int64_t>(raw);
  return true;
}

/// The response bytes of a byte-oriented field (ASCII, HEXDUMP), as a borrowed range.
///
/// Both flags require byte alignment — `bit_pos % 8 == 0` and `bit_size % 8 == 0` (§5.1): there is
/// no byte to copy "verbatim" from a position that is not a byte, so a misaligned field is a
/// mis-flagged record and is refused rather than guessed at. That rule lives here, once, so the
/// ASCII and HEXDUMP paths cannot drift apart on it. Returns false for a field the response does
/// not fully cover, so no caller can read past `resp_len`.
inline bool field_bytes(const uint8_t *resp, size_t resp_len, const FieldView &f, uint16_t element, const uint8_t **out,
                        size_t *len_out) {
  if (resp == nullptr || out == nullptr || len_out == nullptr)
    return false;
  if (f.bit_size == 0 || f.repeat_count == 0 || element >= f.repeat_count)
    return false;
  const uint32_t start = static_cast<uint32_t>(f.bit_pos) + static_cast<uint32_t>(element) * f.repeat_stride;
  if ((start & 7u) != 0 || (f.bit_size & 7u) != 0)
    return false;
  const uint32_t off = start >> 3;
  const uint32_t len = f.bit_size >> 3;
  if (static_cast<uint64_t>(off) + len > resp_len)
    return false;
  *out = resp + off;
  *len_out = len;
  return true;
}

/// ASCII fields are not numeric: copy bit_size/8 bytes verbatim and NUL-terminate (§5.1).
/// `out_cap` must hold the bytes plus the terminator, or nothing is written.
inline bool extract_ascii(const uint8_t *resp, size_t resp_len, const FieldView &f, uint16_t element, char *out,
                          size_t out_cap) {
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (out == nullptr || !field_bytes(resp, resp_len, f, element, &bytes, &len))
    return false;
  if (out_cap < len + 1)
    return false;
  std::memcpy(out, bytes, len);
  out[len] = '\0';
  return true;
}

/// Format raw bytes as uppercase hex pairs. Returns the number of characters written (2 per byte),
/// or 0 when `out_cap` cannot hold them plus the terminator.
inline size_t format_hexdump(const uint8_t *bytes, size_t len, char *out, size_t out_cap) {
  if (bytes == nullptr || out == nullptr || out_cap < len * 2 + 1)
    return 0;
  static const char DIGITS[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = DIGITS[bytes[i] >> 4];
    out[i * 2 + 1] = DIGITS[bytes[i] & 0x0F];
  }
  out[len * 2] = '\0';
  return len * 2;
}

/// HEXDUMP fields (§5.1): the same byte-alignment rule as ASCII, enforced by field_bytes().
/// Returns the number of characters written (2 per byte), or 0.
inline size_t extract_hexdump(const uint8_t *resp, size_t resp_len, const FieldView &f, uint16_t element, char *out,
                              size_t out_cap) {
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (!field_bytes(resp, resp_len, f, element, &bytes, &len))
    return 0;
  return format_hexdump(bytes, len, out, out_cap);
}

// -------------------------------------------------------------------------------------------
// Scale evaluation (§6)
// -------------------------------------------------------------------------------------------

/// The result of evaluating a raw value against a field's scale rows.
///   valid == false          → not available: a numeric sensor publishes NaN. `text` still points
///                             at the sentinel's text when the matching row had one.
///   is_text == true         → a TEXT row matched; `text` points into the catalog (flash) and
///                             lives as long as the mapping.
///   otherwise               → `value` is raw * factor + offset.
struct Decoded {
  float value;       ///< NaN whenever valid == false
  const char *text;  ///< nullptr when no TEXT row matched
  bool valid;
  bool is_text;
};

/// Try the field's scale rows in table order; the first row whose [low, high] contains `raw`
/// wins (§6). scale_count == 0 is the raw-passthrough case.
///
/// An INVALID row means "the value is not available" whether or not it carries text. The spec's
/// evaluation list only spells out TEXT|INVALID — the compiler derives INVALID from a row's text,
/// so the flag never appears alone in practice — but if it ever did, publishing a number from a
/// row the compiler marked invalid would be exactly the confident-wrong-value this table exists
/// to prevent, so INVALID is authoritative here.
inline Decoded evaluate_scales(const Catalog &cat, const FieldView &f, int64_t raw) {
  Decoded r{NAN, nullptr, false, false};
  if (f.scale_count == 0) {
    r.value = static_cast<float>(raw);
    r.valid = true;
    return r;
  }
  for (uint16_t i = 0; i < f.scale_count; i++) {
    ScaleView row;
    if (!cat.scale(static_cast<uint32_t>(f.scale_first) + i, &row))
      break;  // rows run off the scale table: nothing trustworthy matched
    if (raw < row.low || raw > row.high)
      continue;
    if ((row.flags & SCALE_FLAG_TEXT) != 0) {
      r.is_text = true;
      r.text = row.text;
    }
    if ((row.flags & SCALE_FLAG_INVALID) != 0)
      return r;  // sentinel: valid stays false, value stays NaN, text (if any) for text sensors
    r.value = static_cast<float>(raw) * row.factor + row.offset;
    r.valid = true;
    return r;
  }
  // No row matched: a raw outside every declared range is a decode we do not understand (§6).
  return r;
}

/// Extract + evaluate in one step, for the common numeric path. An uncovered field (response too
/// short) comes back as not-available, exactly like a sentinel — skipped and counted, never read.
inline Decoded decode_numeric(const Catalog &cat, const FieldView &f, const uint8_t *resp, size_t resp_len,
                              uint16_t element = 0) {
  int64_t raw;
  if (!extract_raw(resp, resp_len, f, element, &raw))
    return Decoded{NAN, nullptr, false, false};
  return evaluate_scales(cat, f, raw);
}

}  // namespace esphome::uds
