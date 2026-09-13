#pragma once

/// Format v2 of the sd_logger file (docs/sd_logger-spec.md §6): the emitters, the escape pass,
/// the source table, the timestamp reconstruction and delta chain, and the sector-aligned block
/// buffer.
///
/// ESPHome-free and IDF-free, like `log_record.h` and `tap_translate.h`, and for the same reason:
/// this is where a wrong byte produces a perfectly well-formed, quietly wrong file. A bench run
/// sees `dropped == 0` and a plausible CSV and passes it. `tests/host/test_log_format.cpp` is the
/// only thing that can catch it.
///
/// **No `snprintf` anywhere.** The M1 writer spent one format-string parse on the header fields
/// plus one *per payload byte* — ~9 per record — which is where the 85 % CPU at the measured
/// 5300-5700 rec/s ceiling went (§6 F1g). Everything here is table- and shift-driven.
///
/// **v2 finishes that job: a stream line now contains no division at all.** v1 still spent one
/// divide-and-modulo per decimal digit on the timestamp — ~10 per record, the widest number on the
/// line — and another on `dlc`. v2 writes the stamp in hex (shift, mask, table lookup) and drops
/// `dlc` entirely, so the only arithmetic left on the hot path is shifts. Shorter and faster are
/// the same change here, which is why the stamp column was worth breaking the format for.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "log_record.h"

namespace esphome {
namespace sd_logger {

/// Written into the `#sdlog` header line. Bump it when a reader would have to change.
///
/// v2 (2026-07-29) delta-codes the timestamp column of record and text lines (F1h). A v1 reader
/// handed a v2 file would call every one of those lines malformed, so `script/sdlog.py` refuses a
/// version it does not know instead of parsing it — the gap that made this bump worth spelling out.
static const uint8_t SD_LOG_FORMAT_VERSION = 2;

/// Hard cap on one formatted line, terminator included (F1e). Record lines run ~42 B; only a
/// captured log message can approach this.
static const size_t SD_LOG_MAX_LINE = 512;

/// FAT sector size. The block buffer only ever hands whole multiples of this to `write()`, so
/// FATFS never does a read-modify-write on a partial sector (D6).
static const size_t SD_LOG_SECTOR = 512;

/// Labels are capped because every character costs ~3.6 KB/s at production load (F1g).
static const size_t SD_LOG_MAX_LABEL = 8;

/// Marks a truncated line, and the truncated flag on a record (F1e).
static const char SD_LOG_TRUNCATED_MARK = '~';

/// Prefixes an **absolute** timestamp in the stamp column (F1h). A bare number there is a delta
/// from the previous stream line of the same file.
static const char SD_LOG_ANCHOR_MARK = '@';

/// Gaps wider than this anchor instead of stepping. One second is the point where a delta stops
/// paying: a 7-digit delta is barely narrower than the absolute it replaces, and a stream that has
/// been quiet or blocked that long is a stall or an idle bus, so the anchor costs nothing where it
/// lands. It also bounds how far a garbled stamp can drag the timestamps behind it.
static const uint64_t SD_LOG_RESYNC_US = 1000000;

/// ...and so does this: at most this many delta lines follow one anchor. A busy bus never trips the
/// gap cap — 4 ms between frames, forever — so without a line count a 32 MB chunk would be one
/// unbroken chain, and a single corrupt stamp inside it would shift every timestamp to the end of
/// the file. 256 lines costs ~0.04 B/record and bounds the damage to one sector's worth of lines.
static const uint32_t SD_LOG_ANCHOR_EVERY = 256;

/// Type letters (F1a). `I` is reserved for isotp (§4 S5) so adding it later is not a format change.
static const char KIND_CAN = 'C';
static const char KIND_LIN = 'L';
static const char KIND_USER = 'U';
static const char KIND_ISOTP = 'I';
static const char KIND_LOG = 'X';

/// Source-table capacity: tapped CAN ports plus declared `sources:` entries. Overridable from the
/// build if a config ever needs more; a tag that does not fit degrades to a `U<tag>` token rather
/// than being lost (D1).
#ifndef SD_LOG_MAX_SOURCES
#define SD_LOG_MAX_SOURCES 16
#endif

// ---------------------------------------------------------------------------------------------
// Timestamps (D8)
// ---------------------------------------------------------------------------------------------

/// Rebuild the full 64-bit capture time from the record's 32-bit `t_us` and a 64-bit clock read
/// taken once per drain pass.
///
/// Deliberately not a wrap counter: a counter desyncs across a quiet period and is fooled by the
/// few-ms out-of-order arrivals between two rings (F1f), while this is a pure function of two
/// numbers — which is why it can be tested across the boundary.
///
/// **The signed difference is the whole trick, and the spec's original three-liner was wrong.**
/// That version read `if ((uint32_t) now64 < t32) hi--`, which assumes every record predates the
/// clock sample. They do not: the writer samples `now64` once and *then* drains, while the RX ISR
/// keeps stamping new records into the tap rings throughout the pass. So a record a few µs newer
/// than the sample is the normal case at load — and with `hi == 0`, which is the first ~71 min
/// after boot and therefore every bench run, the decrement underflowed to 0xFFFFFFFF and stamped
/// the line at ~584 000 years. A signed delta reads that as the small negative age it is.
///
/// Exact for any record within ±~36 min of the sample, in either direction. Records are
/// milliseconds old, so that is not a constraint anyone can reach.
///
/// It saturates at 0 rather than wrapping. A stamp claiming to predate boot cannot happen — it
/// would need a `now64` below the record's own age — but the last step is an unsigned subtraction,
/// and an underflow there lands right back on the ~584 000-year timestamp this function was
/// rewritten to remove. A clamp keeps the arithmetic total for every possible pair.
inline uint64_t reconstruct_us(uint64_t now64, uint32_t t32) {
  const int32_t age = static_cast<int32_t>(static_cast<uint32_t>(now64) - t32);
  const int64_t full = static_cast<int64_t>(now64) - age;
  return full < 0 ? 0 : static_cast<uint64_t>(full);
}

/// The stamp column of every record and text line (F1h, format v2): a bare number is the µs since
/// the previous stream line of the same file, a number prefixed `@` is an absolute anchor.
///
/// At the bench's ~238 rec/s a delta is 4 digits where the absolute is 10, and — the part that
/// actually matters — a delta does not get wider as uptime grows. An absolute stamp is at its widest
/// exactly during a long soak, which is when the log matters most.
///
/// **The chain belongs to the writer, so it is advanced where the writer can guarantee it**: inside
/// the line formatters, after they have decided a line will be written. A chain advanced for a line
/// that never reached the buffer desyncs every timestamp behind it, which is a file that parses
/// perfectly and is quietly wrong — the failure mode this whole header is arranged against.
///
/// Every way time misbehaves here is real on this bench, and each one anchors rather than being
/// arithmetic:
///
///   * **The 32-bit stamp wraps every ~71.6 min.** `encode()` takes the reconstructed 64-bit value
///     and there is deliberately no 32-bit path into it, so the wrap is `reconstruct_us()`'s problem
///     and never becomes a 4.29e9 µs step.
///   * **A record can carry a time before the line ahead of it.** The writer samples the clock once
///     and then drains ring after ring, so a record out of the second ring is routinely older than
///     the last one out of the first (F1f) — the same non-monotonicity
///     `reconstruct_us_handles_a_record_stamped_after_the_clock_sample` pins. An unsigned delta
///     would underflow into a ~1.8e19 µs jump; a backward step anchors instead.
///   * **A card stall.** M6 measured a 1672 ms `write()` stall with records buffered straight
///     through it, so a multi-second step is legal traffic and not corruption. Past
///     `SD_LOG_RESYNC_US` it anchors, because by then the delta is as wide as the absolute anyway.
///   * **A chunk boundary.** `reset()` at file open is what keeps every sealed chunk decodable on
///     its own. The collection server serves chunks individually, so a chain that only anchored at
///     the first file would make chunk 400 depend on the 399 nobody kept.
class DeltaClock {
 public:
  /// Start a fresh chain. Called at file open, so the first stream line of every chunk anchors.
  void reset() {
    this->prev_ = 0;
    this->run_ = 0;
    this->started_ = false;
  }

  /// Encode one stamp and advance the chain. Returns true when the line must carry an anchor, and
  /// then `*out` is the absolute time; otherwise `*out` is the step from the previous line.
  bool encode(uint64_t t_full, uint64_t *out) {
    const bool anchor = !this->started_ || t_full < this->prev_ || (t_full - this->prev_) > SD_LOG_RESYNC_US ||
                        this->run_ >= SD_LOG_ANCHOR_EVERY;
    *out = anchor ? t_full : t_full - this->prev_;
    this->prev_ = t_full;
    this->started_ = true;
    this->run_ = anchor ? 0 : this->run_ + 1;
    return anchor;
  }

 protected:
  uint64_t prev_{0};
  uint32_t run_{0};  ///< delta lines emitted since the last anchor
  bool started_{false};
};

// ---------------------------------------------------------------------------------------------
// Log level letters (F1c)
// ---------------------------------------------------------------------------------------------

/// ESPHome level (ESPHOME_LOG_LEVEL_*: 1 ERROR .. 7 VERY_VERBOSE) to its letter. VERY_VERBOSE
/// collapses onto VERBOSE — the distinction has never mattered on a card, and a single letter
/// keeps the column one byte wide.
inline char level_letter(uint8_t level) {
  switch (level) {
    case 1:
      return 'E';
    case 2:
      return 'W';
    case 3:
      return 'I';
    case 4:
      return 'C';
    case 5:
      return 'D';
    case 6:
    case 7:
      return 'V';
    default:
      return '?';
  }
}

// ---------------------------------------------------------------------------------------------
// Source table (D1, F1d)
// ---------------------------------------------------------------------------------------------

/// One declared source: the producer stamps a 1-byte tag, the writer resolves it here to a type
/// letter and a label. The resolve is pure formatting, so it belongs on the consumer side — the
/// producer side is ISR budget.
struct SourceEntry {
  uint8_t tag;
  char kind;                         ///< KIND_CAN / KIND_LIN / KIND_USER / KIND_ISOTP
  char label[SD_LOG_MAX_LABEL + 1];  ///< NUL-terminated, sanitised at add() time
};

/// Tag -> (kind, label). Small and linear on purpose: there are a handful of sources and the
/// lookup is one cache line, so a map would cost more than it saves.
class SourceTable {
 public:
  /// Declare a source. The label is truncated to SD_LOG_MAX_LABEL and sanitised: anything outside
  /// [A-Za-z0-9_-] becomes '_'. That sanitising is not paranoia about the schema (V13 already
  /// rejects such labels) — it is what makes the "never a bare newline, never a stray comma"
  /// invariant of F1e a property of the formatter rather than of the validator.
  /// Returns false when the table is full or the label is empty; the tag then stays unmapped.
  bool add(uint8_t tag, char kind, const char *label) {
    if (this->count_ >= SD_LOG_MAX_SOURCES || label == nullptr || label[0] == '\0')
      return false;
    SourceEntry &entry = this->entries_[this->count_];
    entry.tag = tag;
    entry.kind = kind;
    size_t i = 0;
    for (; i < SD_LOG_MAX_LABEL && label[i] != '\0'; i++) {
      const char c = label[i];
      const bool ok =
          (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
      entry.label[i] = ok ? c : '_';
    }
    entry.label[i] = '\0';
    this->count_++;
    return true;
  }

  /// nullptr when the tag was never declared — a lambda calling `log_frame(1, …)` directly keeps
  /// working, its records just read `U,…,1,…` (D1).
  const SourceEntry *find(uint8_t tag) const {
    for (uint8_t i = 0; i < this->count_; i++) {
      if (this->entries_[i].tag == tag)
        return &this->entries_[i];
    }
    return nullptr;
  }

  uint8_t size() const { return this->count_; }
  const SourceEntry &at(uint8_t index) const { return this->entries_[index]; }
  bool full() const { return this->count_ >= SD_LOG_MAX_SOURCES; }

 protected:
  SourceEntry entries_[SD_LOG_MAX_SOURCES]{};
  uint8_t count_{0};
};

// ---------------------------------------------------------------------------------------------
// The line writer
// ---------------------------------------------------------------------------------------------

/// A bounded cursor over the caller's buffer. Every field emitter writes through this, so a line
/// that would not fit is *marked* rather than silently cut mid-field: `finish()` appends the
/// truncation mark and the terminator, both of which it always has room for because the
/// constructor reserves two bytes up front.
///
/// This is the type that carries the format's one hard invariant: **no emitter here ever writes a
/// bare newline.** `escaped()` turns LF into the two characters `\n`, and every other emitter
/// writes only from a fixed alphabet. That is what makes a torn file recoverable — a reader
/// discards one partial trailing line and everything before it is sound (F1e).
class LineWriter {
 public:
  /// `cap` is the total room available; two bytes of it are held back for the terminator (and the
  /// truncation mark, which occupies the same reserve because a truncated line needs both).
  LineWriter(char *buf, size_t cap) : buf_(buf), limit_(cap >= 2 ? cap - 2 : 0) {}

  void ch(char c) {
    if (this->pos_ >= this->limit_) {
      this->overflow_ = true;
      return;
    }
    this->buf_[this->pos_++] = c;
  }

  void comma() { this->ch(','); }

  /// All of a multi-byte escape sequence, or none of it.
  ///
  /// `ch()` drops one byte at a time, so an escape written through it can be cut in half by the
  /// line cap and leave the line ending on a lone `\` — which a reader then applies to whatever
  /// follows. v1 never showed it because `X,<10-digit t_us>,I,tag,` happened to leave an even
  /// number of bytes for a run of `\n` escapes; widening the stamp column by one byte for the `@`
  /// anchor mark landed it on an odd one, and the case that had been guarding this the whole time
  /// (`truncation_never_splits_an_escape_sequence_across_the_cap`) failed. The alignment was luck,
  /// so this makes it a property.
  void seq(const char *s, size_t n) {
    if (this->pos_ + n > this->limit_) {
      this->overflow_ = true;
      return;
    }
    for (size_t i = 0; i < n; i++)
      this->buf_[this->pos_++] = s[i];
  }

  /// Raw bytes, no escaping. Only for fixed alphabets the format controls (labels sanitised by
  /// SourceTable, compile-time constants). Untrusted bytes go through escaped().
  void raw(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
      this->ch(s[i]);
  }

  void str(const char *s) {
    if (s == nullptr)
      return;
    for (size_t i = 0; s[i] != '\0'; i++)
      this->ch(s[i]);
  }

  /// Unsigned decimal, no padding. Digits are generated backwards into a stack buffer and copied,
  /// which is one pass and no division loop over the output.
  ///
  /// **Meta lines only.** No stream line spends a division per digit any more: every number on a
  /// record or text line is hex (F1a), which is both narrower and shift-and-lookup rather than
  /// divide-and-modulo. What is left here is `seq`, the drop counters and the `#gap` byte total —
  /// a handful of numbers per file, where matching the decimal in a filename and in a stats line
  /// is worth more than the cycles.
  void u64(uint64_t v) {
    char tmp[20];
    size_t n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + (v % 10));
      v /= 10;
    } while (v != 0);
    while (n > 0)
      this->ch(tmp[--n]);
  }

  /// Uppercase hex, no leading zeros (F1a: `1A2`, `18DAF110`).
  void u32_hex(uint32_t v) {
    if (v == 0) {
      this->ch('0');
      return;
    }
    char tmp[8];
    size_t n = 0;
    while (v != 0) {
      tmp[n++] = HEX_DIGITS[v & 0xF];
      v >>= 4;
    }
    while (n > 0)
      this->ch(tmp[--n]);
  }

  /// Same, 64 bits wide. Only the absolute anchor needs this — a step is bounded (see `stamp()`).
  void u64_hex(uint64_t v) {
    if (v == 0) {
      this->ch('0');
      return;
    }
    char tmp[16];
    size_t n = 0;
    while (v != 0) {
      tmp[n++] = HEX_DIGITS[v & 0xF];
      v >>= 4;
    }
    while (n > 0)
      this->ch(tmp[--n]);
  }

  /// The stamp column (F1h): `@` then the absolute µs for an anchor, the bare step otherwise, both
  /// in hex. One field either way, so the column count never varies with the encoding.
  ///
  /// **The step goes out through the 32-bit emitter, and `DeltaClock` is what makes that sound**:
  /// it anchors any step above `SD_LOG_RESYNC_US` (1 s), so a step can never approach 2^32 µs
  /// (~71.6 min). That is worth the coupling on a 32-bit MCU — a 64-bit shift loop costs two
  /// registers and a carry per digit for a number that is four digits wide.
  void stamp(bool anchor, uint64_t v) {
    if (anchor) {
      this->ch(SD_LOG_ANCHOR_MARK);
      this->u64_hex(v);
      return;
    }
    this->u32_hex(static_cast<uint32_t>(v));
  }

  /// Two uppercase chars per byte, no separator. This is the emitter that replaces one snprintf
  /// per payload byte.
  void hex_bytes(const uint8_t *data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
      this->ch(HEX_DIGITS[data[i] >> 4]);
      this->ch(HEX_DIGITS[data[i] & 0xF]);
    }
  }

  /// Flag letters from the REC_FLAG_* bits (F1b). Letters rather than a number because the column
  /// is read far more often than it is parsed, and because the TAP_FLAG_SHED/REC_FLAG_TX bit
  /// collision is invisible in a decimal column.
  ///
  /// **No flags emits nothing at all** — v1's `-` placeholder is gone with the column it padded.
  /// The letters are the tail of the leading token (F1a), not a field of their own, so an unflagged
  /// record costs zero bytes here instead of a `-` and a comma. Most frames on this bench are
  /// unflagged, so this is the one saving that lands on the majority of lines.
  ///
  /// **Every letter is lowercase, or `~`, and never a hex digit.** That is what makes the token
  /// self-delimiting: a reader takes uppercase hex characters for the tag until one is not, and
  /// everything left is flags. This is load-bearing, not incidental — see `format_record`.
  void flags(uint8_t f) {
    if (f & REC_FLAG_EXTENDED)
      this->ch('x');
    if (f & REC_FLAG_RTR)
      this->ch('r');
    if (f & REC_FLAG_TX)
      this->ch('t');
    if (f & REC_FLAG_SHED)
      this->ch('s');
    if (f & REC_FLAG_TRUNCATED)
      this->ch(SD_LOG_TRUNCATED_MARK);
  }

  /// The escape pass of F1e, and the only path untrusted bytes may take.
  /// `\`->`\\`, LF->`\n`, CR->`\r`, TAB->`\t`, other bytes < 0x20 or 0x7F -> `\xNN`; bytes >= 0x80
  /// pass through so UTF-8 survives. With `comma_to_underscore` a ',' becomes '_' — for tags,
  /// which are identifiers; commas in a message pass through because it is the last field.
  void escaped(const char *s, size_t n, bool comma_to_underscore) {
    for (size_t i = 0; i < n; i++) {
      // Stop at the first byte that did not fit rather than letting a later, shorter one slip in
      // past an escape that was refused: what follows the truncation mark is lost either way, and a
      // field that silently drops bytes out of its middle is worse than one that ends early.
      if (this->overflow_)
        return;
      const unsigned char c = static_cast<unsigned char>(s[i]);
      switch (c) {
        case '\\':
          this->seq("\\\\", 2);
          continue;
        case '\n':
          this->seq("\\n", 2);
          continue;
        case '\r':
          this->seq("\\r", 2);
          continue;
        case '\t':
          this->seq("\\t", 2);
          continue;
        case ',':
          this->ch(comma_to_underscore ? '_' : ',');
          continue;
        default:
          break;
      }
      if (c < 0x20 || c == 0x7F) {
        const char esc[4] = {'\\', 'x', HEX_DIGITS[c >> 4], HEX_DIGITS[c & 0xF]};
        this->seq(esc, 4);
        continue;
      }
      this->ch(static_cast<char>(c));
    }
  }

  /// Close the line: the truncation mark if anything was dropped, then the terminator. Returns the
  /// total line length. Always succeeds — the two bytes were reserved by the constructor.
  size_t finish() {
    if (this->overflow_)
      this->buf_[this->pos_++] = SD_LOG_TRUNCATED_MARK;
    this->buf_[this->pos_++] = '\n';
    return this->pos_;
  }

  bool overflowed() const { return this->overflow_; }
  size_t size() const { return this->pos_; }

 protected:
  static constexpr const char *HEX_DIGITS = "0123456789ABCDEF";

  char *buf_;
  size_t limit_;
  size_t pos_{0};
  bool overflow_{false};
};

// ---------------------------------------------------------------------------------------------
// Line formatters
// ---------------------------------------------------------------------------------------------

/// The smallest line any formatter can produce, terminator included. Below this there is no point
/// starting: `room()` is checked once per line instead of per field.
static const size_t SD_LOG_MIN_ROOM = 16;

/// `<K><tag><flags>,<t>,<id>,<data>` (F1a) — four fields, three commas. One layout for every bus,
/// so one parser covers all of them. Returns the bytes written, 0 if `room` was too small to try.
///
///     C1x,1068,1A2,0011223344556677     CAN source 1, extended, 4200 µs after the line above
///     C1,1068,1A2,0011223344556677      the same frame with no flags — the common case, and it
///                                       costs nothing where v1 spent `-` and a comma
///
/// **The leading token fuses what used to be three columns**, because a comma only earns its place
/// between fields whose width is not self-evident:
///
///   * `<K>` is the type letter, one byte, and still the only thing a reader needs to dispatch the
///     line — which is why it stays even though `#src` implies it.
///   * `<tag>` is the producer's 1-byte source tag in hex, where v1 wrote the label string.
///     `#src` maps tag to label and kind in every file's header, so the name is not lost; it moves
///     from ~3600 lines a second to once per file. With the default label being the interface
///     number (`C1`, `C2`), `<K><tag>` and the label are *the same characters* for any config that
///     does not override `source:` — the line still reads `C1` and means it.
///   * `<flags>` is the letters, or nothing.
///
/// **The token is self-delimiting because hex is uppercase and flags are not.** `C1Ax` is tag 0x1A
/// with the extended flag, unambiguously: take uppercase hex until a character is not one, and the
/// rest is flags. `LineWriter::flags()` owns that half of the invariant and says so.
///
/// **This also took the last branch and the last string copy off the hot path.** v1 chose between
/// `str(label)` — up to eight bounds-checked byte writes — and a decimal fallback for an unmapped
/// tag. Now the tag is written the same way whether or not it resolves, and `find()` is consulted
/// only for the kind letter, so the D1 degrade path is not a special case any more: an unmapped tag
/// simply reads `U<tag>`.
///
/// `t_full` is the reconstructed 64-bit capture time; `clock` decides whether the line carries it
/// whole (`@D699EEB9`) or as a step from the line before it (`1068`). The room check comes first on
/// purpose: a refused line must not advance the chain (F1h).
///
/// **There is no `dlc` column — the data field's own width is the length**, two hex characters per
/// byte, and a field restating that ~3600 times a second was paying for nothing. No exceptions and
/// no branch: every record kind is measured the same way.
///
/// The one thing that costs is an **RTR frame's requested length, which v2 does not preserve**. A
/// remote frame carries a length and no payload, so there is no data field for its width to be
/// read off. This is deliberate: RTR is not used on these buses, and a conditional last field would
/// buy a frame type that never appears a branch on the hot path plus a special case in every
/// reader. The `r` flag stays, so such a frame is still visible as one — only its requested length
/// is gone. Spec §6 F1a says so out loud, because a silent limitation is the kind someone meets at
/// two in the morning.
///
/// The payload itself is only ever bytes: `hex_bytes()` walks them one at a time through a nibble
/// table. It is never widened into an integer, never byte-swapped, and there is no endianness on
/// this path to get wrong.
inline size_t format_record(char *out, size_t room, DeltaClock &clock, const LogRecord &rec, uint64_t t_full,
                            const SourceTable &sources) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  const SourceEntry *src = sources.find(rec.source);
  w.ch(src != nullptr ? src->kind : KIND_USER);
  w.u32_hex(rec.source);
  w.flags(rec.flags);
  w.comma();
  uint64_t step = 0;
  const bool anchor = clock.encode(t_full, &step);
  w.stamp(anchor, step);
  w.comma();
  w.u32_hex(rec.id);
  w.comma();
  // The clamp is here rather than in the caller because the formatter indexes data[8] and must not
  // depend on tap_translate having done it (it does, but this is the last line of defence).
  w.hex_bytes(rec.data, rec.len > 8 ? 8 : rec.len);
  return w.finish();
}

/// `X<L>,<t>,<tag>,<message>` (F1c) — the level letter joins the type letter in the leading token,
/// for the same reason the record's flags do: it is one fixed byte, so the comma after it was
/// paying for nothing. `message` is the last field, so its commas need no escaping; `tag` is an
/// identifier, so a comma in one becomes '_'.
///
/// It shares the record lines' delta chain rather than keeping one of its own: the two are
/// interleaved in the file in write order, and one chain over every stream line means a reader
/// maintains it without having to tell a record from a log message — it only has to skip the `#`
/// meta lines, which carry absolute times and never touch the chain.
///
/// `truncated` marks a message the *text ring slot* could not hold — a different cut from the one
/// `finish()` marks, which is the line cap, but the same thing to a reader: what follows the
/// marker was lost. One mark for both, so there is one rule to know.
inline size_t format_text(char *out, size_t room, DeltaClock &clock, uint64_t t_full, uint8_t level, const char *tag,
                          size_t tag_len, const char *msg, size_t msg_len, bool truncated = false) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.ch(KIND_LOG);
  w.ch(level_letter(level));
  w.comma();
  uint64_t step = 0;
  const bool anchor = clock.encode(t_full, &step);
  w.stamp(anchor, step);
  w.comma();
  w.escaped(tag, tag_len, /*comma_to_underscore=*/true);
  w.comma();
  w.escaped(msg, msg_len, /*comma_to_underscore=*/false);
  if (truncated && !w.overflowed())
    w.ch(SD_LOG_TRUNCATED_MARK);
  return w.finish();
}

/// `#sdlog,<fmtver>,<seq>,<t_us>,<esphome_ver>` — first line of every file (F1d).
///
/// Every `#` meta line below carries an **anchor-marked absolute** `@<hex>` time and stays outside
/// the delta chain. That is not an inconsistency, it is what lets a reader skip meta lines it does
/// not care
/// about: a marker that advanced the chain would make a reader that ignores `#drop` decode every
/// record after one wrongly. The type letter already dispatches the line, so there is no ambiguity
/// between an absolute here and a step there.
inline size_t format_header(char *out, size_t room, uint32_t seq, uint64_t t_full, const char *esphome_version) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#sdlog,");
  w.u64(SD_LOG_FORMAT_VERSION);
  w.comma();
  w.u64(seq);
  w.comma();
  w.stamp(/*anchor=*/true, t_full);
  w.comma();
  w.escaped(esphome_version == nullptr ? "" : esphome_version,
            esphome_version == nullptr ? 0 : std::strlen(esphome_version), /*comma_to_underscore=*/true);
  return w.finish();
}

/// `#src,<K>,<label>,<tag>,<origin>,<detail>` — one per declared source, re-emitted into every
/// file so a card found on a bench explains itself without the YAML that produced it (F1d).
/// `detail` may be nullptr, which emits '-'.
inline size_t format_src(char *out, size_t room, char kind, const char *label, uint8_t tag, const char *origin,
                         const char *detail) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#src,");
  w.ch(kind);
  w.comma();
  w.str(label);
  w.comma();
  // Hex, because this is the same tag the record token carries and the two must read alike: a
  // decimal `10` here against a token of `CA` there is exactly the kind of mismatch nobody
  // notices until they are chasing a source that seems to have no records.
  w.u32_hex(tag);
  w.comma();
  w.str(origin);
  w.comma();
  if (detail == nullptr || detail[0] == '\0') {
    w.ch('-');
  } else {
    w.str(detail);
  }
  return w.finish();
}

/// Same, with a decimal `detail` (a bit rate). Saves the caller a scratch buffer and a conversion.
inline size_t format_src_num(char *out, size_t room, char kind, const char *label, uint8_t tag, const char *origin,
                             uint64_t detail) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#src,");
  w.ch(kind);
  w.comma();
  w.str(label);
  w.comma();
  // Hex, because this is the same tag the record token carries and the two must read alike: a
  // decimal `10` here against a token of `CA` there is exactly the kind of mismatch nobody
  // notices until they are chasing a source that seems to have no records.
  w.u32_hex(tag);
  w.comma();
  w.str(origin);
  w.comma();
  w.u64(detail);
  return w.finish();
}

/// The `#types` and `#flags` legend lines (F1d). Constant, but emitted through the same path so
/// the line-length accounting and the terminator rule have no exceptions.
inline size_t format_types(char *out, size_t room) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#types,C=can,L=lin,U=user,I=isotp,X=esphome-log,#=meta");
  return w.finish();
}

inline size_t format_flags_legend(char *out, size_t room) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#flags,x=extended,r=rtr,t=tx,s=shed,~=truncated,none=absent");
  return w.finish();
}

/// `#layout` — the column list and the two rules that are not guessable from looking at a line
/// (F1d). v2 is compact enough that it stopped being self-evident: `C,1068,C1,1A2,x,0011...` does
/// not say that 1068 is a hex step rather than a decimal microsecond, and there is no longer a
/// `dlc` column to make the payload width obvious.
///
/// **It is free.** The header block is padded to a 512-byte boundary either way, so this line comes
/// out of `#pad` and costs the card nothing — which is the whole reason to spend bytes on
/// self-description here rather than in the record lines, where they are paid ~3600 times a second.
inline size_t format_layout(char *out, size_t room) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#layout,rec=<K><tag><flags>:t:id:data,log=X<level>:t:tag:msg"
        ",t=@abs|step us HEX,tag/id/data HEX,dlc=len(data)/2");
  return w.finish();
}

/// `#pad,<spaces>` — the last header line, sized so the stream sits on a sector boundary from the
/// first record onwards (D6). `offset` is the file offset the pad line will start at.
///
/// Every later flush is a whole number of sectors, so this one line is what keeps FATFS off the
/// read-modify-write path for the whole file.
inline size_t format_pad(char *out, size_t room, uint64_t offset) {
  // "#pad," + "\n" is the shortest possible pad line, so a gap smaller than that is closed by
  // padding a whole extra sector instead. An already-aligned header lands on len == SD_LOG_SECTOR
  // and pads a full sector rather than emitting nothing: the line is part of the format, and 512
  // bytes against a 32 MB file is not a trade worth thinking about.
  const size_t min_len = 6;
  size_t len = SD_LOG_SECTOR - static_cast<size_t>(offset % SD_LOG_SECTOR);
  while (len < min_len)
    len += SD_LOG_SECTOR;
  if (room < len)
    return 0;
  std::memcpy(out, "#pad,", 5);
  std::memset(out + 5, ' ', len - min_len);
  out[len - 1] = '\n';
  return len;
}

/// `#drop,<t_us>,<what>,<delta>,<total>` — D4's overflow marker, emitted only when a counter
/// moves, so a healthy run costs nothing (F1d). `what_label` composes `tap:<label>`; pass nullptr
/// for the bare forms `ring` and `text`.
///
/// The three counters stay separate for the same reason the stats line refuses to sum them: they
/// name different bottlenecks with different fixes.
inline size_t format_drop(char *out, size_t room, uint64_t t_full, const char *what, const char *what_label,
                          uint32_t delta, uint32_t total) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#drop,");
  w.stamp(/*anchor=*/true, t_full);
  w.comma();
  w.str(what);
  if (what_label != nullptr) {
    w.ch(':');
    w.str(what_label);
  }
  w.comma();
  w.u64(delta);
  w.comma();
  w.u64(total);
  return w.finish();
}

/// `#gap,<t_us>,<chunks>,<bytes>,<first_seq>,<last_seq>` — retention's loss marker
/// (docs/sdlog-collection-design.md §7), emitted by the same marker pass as `#drop` and on the same
/// terms: only when the discard counters move, so a healthy run costs nothing.
///
/// Retention makes "store everything all the time" bounded by deleting oldest-first once the card
/// fills — CONFIRMED chunks preferentially, then SEALED ones that were never collected, which is
/// real data loss. **The gap must be stated in-band**: a log that silently skips six hours is worse
/// than one that says it skipped them.
///
/// `chunks` and `bytes` are the delta since the last marker, matching `#drop`'s delta column.
///
/// `first_seq`/`last_seq` are the **endpoints of the window retention emptied**, in the order it
/// emptied it (`CollectionPolicy::take_pending_discards()`; design §7a is where this is settled,
/// and it is the arbiter if this comment and that one ever drift). They are *not* a promise that
/// every seq between them went away. `chunks` is the loss; the span between the endpoints is only
/// how wide the window was, and the two are different numbers whenever a chunk inside the window
/// was collected before retention reached it. Seal 10, 11, 12; discard 10; the puller confirms 11;
/// discard 12 — and the line reads `chunks=2` over the window `10..12` while `L0000011.LOG` is
/// safely in the archive. When `chunks != span` the window is **punctured** and a reader must say
/// so: state the loss and the window as two separate numbers, and never expand the span into a
/// list of files to go looking for.
///
/// **Over-reporting loss is the one thing this marker must never do.** `#gap` exists because a log
/// that silently skips six hours is worse than one that says it skipped them — but a marker that
/// claims six hours it did not lose sends someone hunting for files that are not missing, and the
/// next honest `#gap` is then the one nobody believes.
///
/// The endpoints are deliberately emitted unsorted, because `seq` wraps: sorting them numerically
/// turns a two-chunk window straddling the ceiling into `#gap,…,0,9999999`, which claims the whole
/// card. A reader computes the span modularly. A single discarded chunk repeats its seq in both
/// fields — the field count never varies, because the reader splits on commas.
///
inline size_t format_gap(char *out, size_t room, uint64_t t_full, uint32_t chunks, uint64_t bytes, uint32_t first_seq,
                         uint32_t last_seq) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#gap,");
  w.stamp(/*anchor=*/true, t_full);
  w.comma();
  w.u64(chunks);
  w.comma();
  w.u64(bytes);
  w.comma();
  w.u64(first_seq);
  w.comma();
  w.u64(last_seq);
  return w.finish();
}

/// `#rotate,<t_us>,<next-file>` — immediately before rotation (F1d).
inline size_t format_rotate(char *out, size_t room, uint64_t t_full, uint32_t next_seq);

/// `#close,<t_us>,<clean|rotate|emergency>` — the last line of a file. **A missing `#close` is the
/// power-cut signature** (F1d): it costs one line and turns "parses as valid CSV up to the last
/// line" into something a script can decide.
inline size_t format_close(char *out, size_t room, uint64_t t_full, const char *reason) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#close,");
  w.stamp(/*anchor=*/true, t_full);
  w.comma();
  w.str(reason);
  return w.finish();
}

// ---------------------------------------------------------------------------------------------
// File names (F2)
// ---------------------------------------------------------------------------------------------

/// Bytes a log file name occupies, NUL included: "L1234567.LOG".
static const size_t SD_LOG_NAME_LEN = 13;

/// `L<7-digit seq>.LOG` — an 8.3 name, because ESPHome builds FatFs with long names disabled
/// (`CONFIG_FATFS_LFN_NONE`). `out` needs SD_LOG_NAME_LEN bytes.
inline void format_log_name(char *out, uint32_t seq) {
  out[0] = 'L';
  for (int i = 7; i >= 1; i--) {
    out[i] = static_cast<char>('0' + (seq % 10));
    seq /= 10;
  }
  std::memcpy(out + 8, ".LOG", 4);
  out[12] = '\0';
}

/// The boot scan's name matcher: `L#######.LOG` *or* `L#######.CSV`.
///
/// Both extensions on purpose (F2). `.CSV` is the only version marker an M1-era file carries —
/// those have no `#sdlog` header line — and matching only the new one would restart the sequence
/// at 0 on a card holding both eras, which is exactly the failure F2a documents: every already
/// full file trips the rotate check the instant it is opened, and the writer walks the whole file
/// set one open at a time, dropping every record produced meanwhile.
inline bool parse_log_seq(const char *name, uint32_t *seq_out) {
  if (name == nullptr || name[0] != 'L')
    return false;
  uint32_t seq = 0;
  for (int i = 1; i <= 7; i++) {
    if (name[i] < '0' || name[i] > '9')
      return false;
    seq = seq * 10 + static_cast<uint32_t>(name[i] - '0');
  }
  if (name[8] != '.')
    return false;
  const bool is_log = std::strcmp(name + 9, "LOG") == 0;
  const bool is_csv = std::strcmp(name + 9, "CSV") == 0;
  if (!is_log && !is_csv)
    return false;
  if (seq_out != nullptr)
    *seq_out = seq;
  return true;
}

inline size_t format_rotate(char *out, size_t room, uint64_t t_full, uint32_t next_seq) {
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  char name[SD_LOG_NAME_LEN];
  format_log_name(name, next_seq);
  w.str("#rotate,");
  w.stamp(/*anchor=*/true, t_full);
  w.comma();
  w.str(name);
  return w.finish();
}

// ---------------------------------------------------------------------------------------------
// ESPHome log capture (S4, §4a)
// ---------------------------------------------------------------------------------------------

/// Skip one ANSI escape sequence starting at `i`, if there is one. Returns the index of the first
/// byte after it, or `i` unchanged when `s[i]` is not an ESC.
inline size_t skip_ansi(const char *s, size_t len, size_t i) {
  if (i >= len || s[i] != '\x1B')
    return i;
  i++;
  if (i < len && s[i] == '[') {
    i++;
    // CSI: parameter and intermediate bytes, then one final byte in 0x40..0x7E.
    while (i < len && (static_cast<unsigned char>(s[i]) < 0x40 || static_cast<unsigned char>(s[i]) > 0x7E))
      i++;
    if (i < len)
      i++;
  }
  return i;
}

/// Copy the payload out of a fully formatted ESPHome log line.
///
/// The `message` a log callback receives is the *rendered* line: ANSI colour, then `[I][tag:123]: `,
/// plus an extra bracketed thread name (with its own escapes) for anything logged off the main
/// task, then the text, then the colour reset (§4a). `level` and `tag` arrive as separate
/// arguments, so all of that prefix is redundant — and it points into the logger's shared
/// `tx_buffer_`, reused by the very next line, so the copy has to happen synchronously anyway.
///
/// Stripping here rather than in the writer is deliberate: the copy is happening regardless, and
/// spending the text ring's fixed 184-byte slots on ANSI bytes and a prefix we already have would
/// cost real message text. Escaping stays in the writer, where formatting belongs.
///
/// Returns the bytes copied; sets `*truncated` when the payload did not fit.
inline size_t copy_log_payload(char *out, size_t room, const char *msg, size_t len, bool *truncated) {
  if (truncated != nullptr)
    *truncated = false;
  if (out == nullptr || msg == nullptr || room == 0)
    return 0;

  // Find the end of the bracketed prefix: the first "]: " outside an escape sequence. ESPHome
  // emits "[level][tag:line]: " and, off the main task, "[thread][level][tag:line]: " — the
  // *first* occurrence is always the end of the prefix, whichever shape it took.
  size_t start = 0;
  for (size_t i = 0; i < len;) {
    const size_t after = skip_ansi(msg, len, i);
    if (after != i) {
      i = after;
      continue;
    }
    if (msg[i] == ']' && i + 2 < len && msg[i + 1] == ':' && msg[i + 2] == ' ') {
      start = i + 3;
      break;
    }
    i++;
  }
  // No prefix found (an ESP-IDF line, or a format that changed under us): keep the whole message
  // rather than dropping it. A log line with its prefix left on is ugly; a lost one is a bug.

  size_t written = 0;
  for (size_t i = start; i < len;) {
    const size_t after = skip_ansi(msg, len, i);
    if (after != i) {
      i = after;  // drops the trailing colour reset too
      continue;
    }
    if (written >= room) {
      if (truncated != nullptr)
        *truncated = true;
      break;
    }
    out[written++] = msg[i++];
  }
  return written;
}

// ---------------------------------------------------------------------------------------------
// Block buffer (D6)
// ---------------------------------------------------------------------------------------------

/// The writer's own output buffer: records are formatted **directly into it at the write cursor**
/// — no intermediate line buffer, no per-line memcpy, no stdio lock, no `char line[80]` cap.
///
/// It hands `write()` only whole multiples of SD_LOG_SECTOR and keeps the sub-sector remainder,
/// so FATFS never does a read-modify-write on a partial sector. The `#pad` header line puts the
/// stream on a boundary at file open, so the invariant holds from the first record.
///
/// The one deliberate exception is `drain_all()`, used when a file is being closed: the final
/// write of a file may be partial, because there is nothing left to align for.
class BlockBuffer {
 public:
  void attach(char *buf, size_t cap) {
    this->buf_ = buf;
    this->cap_ = cap;
    this->pos_ = 0;
  }

  void reset() { this->pos_ = 0; }

  bool valid() const { return this->buf_ != nullptr && this->cap_ >= SD_LOG_SECTOR; }

  /// Where the next line should be formatted, and how much room it has.
  char *cursor() { return this->buf_ + this->pos_; }
  size_t room() const { return this->cap_ - this->pos_; }
  size_t pending() const { return this->pos_; }
  const char *data() const { return this->buf_; }

  void commit(size_t n) { this->pos_ += n; }

  /// Bytes that may be written right now: whole sectors only.
  size_t flush_len() const { return (this->pos_ / SD_LOG_SECTOR) * SD_LOG_SECTOR; }

  /// Everything buffered, sector-aligned or not. Only for the last write of a file.
  size_t drain_all() const { return this->pos_; }

  /// Drop the first `n` bytes (they reached the card) and move the remainder to the front.
  void consume(size_t n) {
    if (n >= this->pos_) {
      this->pos_ = 0;
      return;
    }
    std::memmove(this->buf_, this->buf_ + n, this->pos_ - n);
    this->pos_ -= n;
  }

 protected:
  char *buf_{nullptr};
  size_t cap_{0};
  size_t pos_{0};
};

}  // namespace sd_logger
}  // namespace esphome
