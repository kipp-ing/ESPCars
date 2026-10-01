// Format v2 of the sd_logger file (components/sd_logger/log_format.h).
//
// Why this is a host test and not a bench check: every failure mode here produces a file that is
// perfectly well-formed and quietly wrong. A hardware run sees `dropped == 0`, a plausible CSV and
// a green ring, and passes. The specific hazards:
//
//   * a timestamp reconstruction that underflows stamps every line ~584 000 years into the future
//     while the bench reports a healthy logger;
//   * a delta chain that anchors in the wrong places is worse still: every line parses, every
//     column is present, and the times are simply wrong — by an unsigned underflow of ~584 000
//     years on a backward step, or by however much a chain carried across a chunk boundary is out;
//   * one un-escaped newline inside a captured log message splits that line in two, and every
//     reader silently believes the halves;
//   * a block flush that is not a whole number of sectors is invisible until it shows up as write
//     latency, which the bench reads as "the card got slower".
//
// So the cases below are the only thing standing between a run and a card full of confident
// nonsense.

#include "harness.h"
#include "log_format.h"
#include "utc_anchor.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

using esphome::sd_logger::BlockBuffer;
using esphome::sd_logger::copy_log_payload;
using esphome::sd_logger::DeltaClock;
using esphome::sd_logger::format_close;
using esphome::sd_logger::format_drop;
using esphome::sd_logger::drop_marker_due;
using esphome::sd_logger::UnflushedRecordTracker;
using esphome::sd_logger::format_flags_legend;
using esphome::sd_logger::format_gap;
using esphome::sd_logger::format_header;
using esphome::sd_logger::format_log_name;
using esphome::sd_logger::format_origin;
using esphome::sd_logger::format_pad;
using esphome::sd_logger::format_rotate;
using esphome::sd_logger::format_src;
using esphome::sd_logger::format_src_num;
using esphome::sd_logger::format_types;
using esphome::sd_logger::format_utc;
using esphome::sd_logger::KIND_CAN;
using esphome::sd_logger::KIND_LIN;
using esphome::sd_logger::KIND_USER;
using esphome::sd_logger::level_letter;
using esphome::sd_logger::LogRecord;
using esphome::sd_logger::parse_log_seq;
using esphome::sd_logger::REC_FLAG_EXTENDED;
using esphome::sd_logger::REC_FLAG_RTR;
using esphome::sd_logger::REC_FLAG_SHED;
using esphome::sd_logger::REC_FLAG_TRUNCATED;
using esphome::sd_logger::REC_FLAG_TX;
using esphome::sd_logger::reconstruct_us;
using esphome::sd_logger::SD_LOG_ANCHOR_EVERY;
using esphome::sd_logger::SD_LOG_FORMAT_VERSION;
using esphome::sd_logger::SD_LOG_MAX_LINE;
using esphome::sd_logger::SD_LOG_RESYNC_US;
using esphome::sd_logger::SD_LOG_SECTOR;
using esphome::sd_logger::SD_LOG_TRUNCATED_MARK;
using esphome::sd_logger::SourceTable;
using esphome::sd_logger::UtcAnchor;
using esphome::sd_logger::resolve_utc_us;

namespace {

/// Most cases below format exactly one line, and one line is a chunk of one: a fresh chain always
/// anchors, which is why every golden line here carries `@`. The cases that are *about* the chain
/// hold their own `DeltaClock` and pass it explicitly to the real six-argument formatter.
size_t format_record(char *out, size_t room, const LogRecord &rec, uint64_t t_full, const SourceTable &sources) {
  DeltaClock clock;
  return esphome::sd_logger::format_record(out, room, clock, rec, t_full, sources);
}

size_t format_text(char *out, size_t room, uint64_t t_full, uint8_t level, const char *tag, size_t tag_len,
                   const char *msg, size_t msg_len, bool truncated = false) {
  DeltaClock clock;
  return esphome::sd_logger::format_text(out, room, clock, t_full, level, tag, tag_len, msg, msg_len, truncated);
}

/// Every formatter writes into a caller buffer and returns the length; this wraps that into a
/// std::string so the cases can compare whole golden lines rather than field fragments. A golden
/// line is the point: it catches a field that moved as well as a field that is wrong.
template<typename Fn> std::string line(Fn fn, size_t room = 4096) {
  char buf[4096];
  std::memset(buf, '\xEE', sizeof(buf));  // poison, so a short write shows up as garbage
  const size_t n = fn(buf, room);
  return std::string(buf, n);
}

SourceTable two_bus_table() {
  SourceTable table;
  table.add(1, KIND_CAN, "seg1");
  table.add(2, KIND_CAN, "seg2");
  table.add(5, KIND_LIN, "lin");
  return table;
}

LogRecord record_with(uint8_t source, uint32_t id, uint8_t flags, uint8_t len) {
  LogRecord rec{};
  rec.t_us = 412345;
  rec.id = id;
  rec.source = source;
  rec.flags = flags;
  rec.len = len;
  const uint8_t payload[8] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  std::memcpy(rec.data, payload, sizeof(payload));
  return rec;
}

/// The format's one hard invariant (F1e): a line contains exactly one newline, at the very end.
/// That is what makes a torn file recoverable — a reader drops one partial trailing line and
/// everything before it is sound.
void check_single_trailing_newline(const std::string &text, const char *what) {
  CHECK_MSG(!text.empty(), std::string("empty line from ") + what);
  if (text.empty())
    return;
  CHECK_MSG(text.back() == '\n', std::string(what) + ": line does not end with a newline");
  const size_t first = text.find('\n');
  CHECK_MSG(first == text.size() - 1, std::string(what) + ": bare newline at offset " + std::to_string(first) + " of " +
                                          std::to_string(text.size()));
}

}  // namespace

// ------------------------------------------------------------------ golden lines, one per type

TEST(record_line_is_the_documented_shape) {
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, REC_FLAG_EXTENDED, 8);
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 412345, table); });
  CHECK_EQ(out, std::string("C1x,@64AB9,1A2,0011223344556677\n"));
}

TEST(record_line_carries_the_shed_flag_that_this_file_exists_for) {
  // A frame that was on the wire and was not forwarded — the Issue #1 signature (F1b).
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(2, 0x1A2, REC_FLAG_EXTENDED | REC_FLAG_SHED, 8);
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 412388, table); });
  CHECK_EQ(out, std::string("C2xs,@64AE4,1A2,0011223344556677\n"));
}

TEST(lin_record_uses_the_same_layout) {
  const SourceTable table = two_bus_table();
  LogRecord rec = record_with(5, 0x3C, 0, 8);
  const uint8_t payload[8] = {0x55, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
  std::memcpy(rec.data, payload, sizeof(payload));
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 412610, table); });
  CHECK_EQ(out, std::string("L5,@64BC2,3C,55AA0000000000FF\n"));
}

TEST(text_line_is_the_documented_shape) {
  const std::string out = line([&](char *b, size_t r) {
    const char msg[] = "records=4120 dropped=0 bytes=198112";
    return format_text(b, r, 412502, /*level=*/3, "sd_logger", 9, msg, sizeof(msg) - 1);
  });
  CHECK_EQ(out, std::string("XI,@64B56,sd_logger,records=4120 dropped=0 bytes=198112\n"));
}

TEST(meta_lines_are_the_documented_shapes) {
  CHECK_EQ(line([](char *b, size_t r) { return format_header(b, r, 7, 412000, "2026.7.0"); }),
           std::string("#sdlog,2,7,@64960,2026.7.0\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_utc(b, r, 0x1'0000'0003ull, 0x6543210FEDCBAull); }),
           std::string("#utc,100000003,6543210FEDCBA\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_origin(b, r, "SEV Mr. Orange"); }),
           std::string("#origin,SEV Mr. Orange\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_src_num(b, r, KIND_CAN, "seg1", 1, "can_gateway", 500000); }),
           std::string("#src,C,seg1,1,can_gateway,500000\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_src(b, r, KIND_USER, "act", 0, "action", nullptr); }),
           std::string("#src,U,act,0,action,-\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_drop(b, r, 414002, "ring", nullptr, 17, 17); }),
           std::string("#drop,@65132,ring,17,17\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_drop(b, r, 414002, "tap", "seg1", 3, 20); }),
           std::string("#drop,@65132,tap:seg1,3,20\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_rotate(b, r, 500000, 8); }),
           std::string("#rotate,@7A120,L0000008.LOG\n"));
  CHECK_EQ(line([](char *b, size_t r) { return format_close(b, r, 999120, "emergency"); }),
           std::string("#close,@F3ED0,emergency\n"));
}

TEST(utc_anchor_resolution_is_optional_and_tracks_resyncs) {
  UtcAnchor anchor;
  uint64_t utc = 0;
  CHECK(!resolve_utc_us(anchor, 100, &utc));  // no sync is normal

  anchor = {1'700'000'000'000'000ull, 5'000'000ull, true};
  CHECK(resolve_utc_us(anchor, 5'123'456ull, &utc));
  CHECK_EQ(utc, 1'700'000'000'123'456ull);

  anchor = {1'700'000'100'000'000ull, 7'000'000ull, true};
  CHECK(resolve_utc_us(anchor, 7'000'100ull, &utc));
  CHECK_EQ(utc, 1'700'000'100'000'100ull);
}

TEST(utc_anchor_uses_the_full_boot_clock_across_the_32_bit_wrap) {
  UtcAnchor anchor{1'700'000'000'000'000ull, 0xFFFF'FFF0ull, true};
  uint64_t utc = 0;
  CHECK(resolve_utc_us(anchor, 0x1'0000'0010ull, &utc));
  CHECK_EQ(utc, 1'700'000'000'000'032ull);
}

TEST(the_legend_lines_name_every_type_and_flag_letter) {
  // A card found on a bench has to explain itself without the YAML that produced it, so anything
  // the formatter can emit must appear in the legend.
  const std::string types = line([](char *b, size_t r) { return format_types(b, r); });
  const std::string flags = line([](char *b, size_t r) { return format_flags_legend(b, r); });
  for (const char *needle : {"C=can", "L=lin", "U=user", "I=isotp", "X=esphome-log", "#=meta"})
    CHECK_MSG(types.find(needle) != std::string::npos, std::string("#types is missing ") + needle);
  for (const char *needle : {"x=extended", "r=rtr", "t=tx", "s=shed", "~=truncated", "none=absent"})
    CHECK_MSG(flags.find(needle) != std::string::npos, std::string("#flags is missing ") + needle);
}

// -------------------------------------------------------------------------- flags and unmapped

TEST(flag_letters_cover_every_bit_and_no_flags_costs_nothing) {
  const SourceTable table = two_bus_table();
  struct Row {
    uint8_t flags;
    const char *want;
  };
  const Row rows[] = {
      {0, ""},  // v1 spent a `-` and a comma here; v2 spends nothing, on most frames
      {REC_FLAG_EXTENDED, "x"},
      {REC_FLAG_RTR, "r"},
      {REC_FLAG_TX, "t"},
      {REC_FLAG_SHED, "s"},
      {REC_FLAG_TRUNCATED, "~"},
      {static_cast<uint8_t>(REC_FLAG_EXTENDED | REC_FLAG_RTR | REC_FLAG_TX | REC_FLAG_SHED | REC_FLAG_TRUNCATED),
       "xrts~"},
  };
  for (const Row &row : rows) {
    const LogRecord rec = record_with(1, 0x100, row.flags, 0);
    const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 1, table); });
    // "C1<flags>,@1,100,\n"
    const std::string want = std::string("C1") + row.want + ",@1,100,\n";
    CHECK_EQ_MSG(out, want, row.want);
  }
}

TEST(an_unmapped_tag_degrades_to_kind_U_and_its_decimal_value) {
  // A lambda calling log_frame(9, …) directly against a table that never declared 9 must keep
  // working — its records are ugly, not lost (D1).
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(9, 0x7FF, 0, 2);
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 77, table); });
  CHECK_EQ(out, std::string("U9,@4D,7FF,0011\n"));
}

TEST(a_zero_length_frame_emits_an_empty_data_field) {
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x0, REC_FLAG_RTR, 0);
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 1, table); });
  CHECK_EQ(out, std::string("C1r,@1,0,\n"));
}

TEST(an_over_long_dlc_is_clamped_to_the_eight_bytes_the_record_holds) {
  // tap_translate clamps too, but the formatter indexes data[8] and must not depend on that.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1, 0, 200);
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 1, table); });
  CHECK_EQ(out, std::string("C1,@1,1,0011223344556677\n"));
}

TEST(ids_are_uppercase_hex_without_leading_zeros) {
  const SourceTable table = two_bus_table();
  struct Row {
    uint32_t id;
    const char *want;
  };
  const Row rows[] = {{0x0, "0"}, {0x1A2, "1A2"}, {0x7FF, "7FF"}, {0x18DAF110, "18DAF110"}, {0xFFFFFFFF, "FFFFFFFF"}};
  for (const Row &row : rows) {
    const LogRecord rec = record_with(1, row.id, 0, 0);
    const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 1, table); });
    CHECK_EQ_MSG(out, std::string("C1,@1,") + row.want + ",\n", row.want);
  }
}

// ------------------------------------------------------------------------- the newline invariant

TEST(never_a_bare_newline_under_adversarial_payloads) {
  // Nothing on the bench will ever produce these, and nothing on the bench would ever catch them.
  struct Row {
    const char *msg;
    size_t len;
    const char *what;
  };
  const char embedded_nul[] = "before\0after";
  const Row rows[] = {
      {"plain", 5, "plain"},
      {"two\nlines", 9, "embedded LF"},
      {"cr\rreturn", 9, "embedded CR"},
      {"crlf\r\nboth", 10, "embedded CRLF"},
      {"tab\there", 8, "embedded TAB"},
      {"back\\slash", 10, "backslash"},
      {"esc\x1B[0mreset", 12, "bare ANSI reset"},
      {"del\x7F"
       "char",
       8, "DEL"},
      {"bell\x07here", 9, "BEL"},
      {embedded_nul, sizeof(embedded_nul) - 1, "embedded NUL"},
      {"comma,in,message", 16, "commas"},
      {"\n\n\n\n\n", 5, "nothing but newlines"},
      {"ümläut", 8, "UTF-8"},
  };
  for (const Row &row : rows) {
    const std::string out =
        line([&](char *b, size_t r) { return format_text(b, r, 1, 3, "tag", 3, row.msg, row.len); });
    check_single_trailing_newline(out, row.what);
  }
}

TEST(escaping_is_the_documented_mapping) {
  const std::string out = line([](char *b, size_t r) {
    const char msg[] = "a\\b\nc\rd\te\x01\x7F";
    return format_text(b, r, 1, 3, "tag", 3, msg, sizeof(msg) - 1);
  });
  CHECK_EQ(out, std::string("XI,@1,tag,a\\\\b\\nc\\rd\\te\\x01\\x7F\n"));
}

TEST(utf8_survives_but_control_bytes_do_not) {
  // Bytes >= 0x80 pass through so a message in German still reads as German on the card.
  const std::string out = line([](char *b, size_t r) {
    const char msg[] = "\xC3\xA4\xC3\xB6";  // "äö"
    return format_text(b, r, 1, 3, "tag", 3, msg, sizeof(msg) - 1);
  });
  CHECK_EQ(out, std::string("XI,@1,tag,\xC3\xA4\xC3\xB6\n"));
}

TEST(a_comma_in_a_tag_becomes_an_underscore_but_a_comma_in_a_message_passes) {
  // The tag is a middle field, so a comma there would shift every later field; the message is the
  // last field, so its commas are free (F1c).
  const std::string out = line([](char *b, size_t r) {
    const char tag[] = "we,ird";
    const char msg[] = "a,b,c";
    return format_text(b, r, 1, 3, tag, sizeof(tag) - 1, msg, sizeof(msg) - 1);
  });
  CHECK_EQ(out, std::string("XI,@1,we_ird,a,b,c\n"));
}

TEST(a_newline_cannot_reach_the_file_through_a_source_label) {
  // V13 rejects such a label at config time; this makes the invariant a property of the formatter
  // instead, because the file is the only artifact that survives the run.
  SourceTable table;
  table.add(1, KIND_CAN, "a\nb,c");
  const LogRecord rec = record_with(1, 0x1, 0, 0);
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 1, table); });
  check_single_trailing_newline(out, "hostile label");
  CHECK_EQ(out, std::string("C1,@1,1,\n"));
}

TEST(labels_are_truncated_to_the_byte_budget) {
  // Every character costs ~3.6 KB/s at production load (F1g), so the cap is real, not cosmetic.
  SourceTable table;
  table.add(1, KIND_CAN, "abcdefghijklmnop");
  const LogRecord rec = record_with(1, 0x1, 0, 0);
  const std::string out = line([&](char *b, size_t r) { return format_record(b, r, rec, 1, table); });
  CHECK_EQ(out, std::string("C1,@1,1,\n"));
}

// ------------------------------------------------------------------------- caps and truncation

TEST(a_long_message_is_capped_and_marked) {
  std::string huge(4000, 'x');
  const std::string out =
      line([&](char *b, size_t r) { return format_text(b, r, 1, 3, "tag", 3, huge.data(), huge.size()); });
  CHECK_EQ(out.size(), SD_LOG_MAX_LINE);
  // The mark, then the terminator — a reader can tell a cut line from a complete one.
  CHECK_EQ(out[out.size() - 2], '~');
  check_single_trailing_newline(out, "capped message");
}

TEST(truncation_never_splits_an_escape_sequence_across_the_cap) {
  // An escape is two to four bytes; cutting one in half would leave a trailing backslash that a
  // reader would apply to the newline.
  std::string huge(4000, '\n');
  const std::string out =
      line([&](char *b, size_t r) { return format_text(b, r, 1, 3, "tag", 3, huge.data(), huge.size()); });
  check_single_trailing_newline(out, "capped escapes");
  const std::string body = out.substr(0, out.size() - 2);  // drop "~\n"
  size_t backslashes = 0;
  for (size_t i = body.size(); i > 0 && body[i - 1] == '\\'; i--)
    backslashes++;
  CHECK_MSG(backslashes % 2 == 0, "line ends on an odd number of backslashes — an escape was cut in half");
}

TEST(no_alignment_of_the_cap_can_split_an_escape) {
  // The case above was passing on luck, and v2 is how that came out: widening the stamp column by
  // one byte for the `@` anchor mark moved the cut one place along and left the line ending on a
  // lone `\`. `ch()` writes one byte at a time and simply drops the one that does not fit, so with
  // a two-byte escape the odds were even. So sweep every alignment instead of trusting one: for a
  // 2-byte escape and a 4-byte one, at every tag width the cut can land on.
  const char *runs[] = {"\n", "\x01"};  // -> `\n` (2 bytes), `\x01` (4 bytes)
  for (const char *unit : runs) {
    std::string huge;
    while (huge.size() < 3000)
      huge += unit;
    for (size_t tag_len = 1; tag_len <= 16; tag_len++) {
      const std::string tag(tag_len, 't');
      const std::string out = line(
          [&](char *b, size_t r) { return format_text(b, r, 1, 3, tag.data(), tag.size(), huge.data(), huge.size()); });
      check_single_trailing_newline(out, "swept cap");
      const std::string body = out.substr(0, out.size() - 2);  // drop "~\n"
      size_t backslashes = 0;
      for (size_t i = body.size(); i > 0 && body[i - 1] == '\\'; i--)
        backslashes++;
      CHECK_MSG(backslashes == 0, "an escape was cut in half at tag width " + std::to_string(tag_len));
    }
  }
}

TEST(a_buffer_too_small_for_any_line_is_refused_rather_than_half_written) {
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 8);
  char buf[8];
  std::memset(buf, '\xEE', sizeof(buf));
  CHECK_EQ(format_record(buf, sizeof(buf), rec, 1, table), 0u);
  for (char c : buf)
    CHECK_EQ(static_cast<unsigned char>(c), 0xEEu);
}

TEST(a_tight_but_usable_buffer_still_ends_in_a_newline) {
  // The writer flushes when room runs low, but the boundary case must not produce a line that
  // never terminates.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, REC_FLAG_EXTENDED, 8);
  for (size_t room = 16; room <= 48; room++) {
    char buf[64];
    std::memset(buf, '\xEE', sizeof(buf));
    const size_t n = format_record(buf, room, rec, 412345, table);
    CHECK_MSG(n <= room, "formatter wrote past the room it was given");
    check_single_trailing_newline(std::string(buf, n), "tight buffer");
  }
}

// ---------------------------------------------------------------- timestamp reconstruction (D8)

TEST(reconstruct_us_is_exact_below_the_first_wrap) {
  CHECK_EQ(reconstruct_us(412345, 412345), 412345u);
  CHECK_EQ(reconstruct_us(1000000, 999000), 999000u);
}

TEST(reconstruct_us_crosses_the_2_to_the_32_boundary) {
  // now64 just past the wrap, record stamped just before it.
  const uint64_t now = 0x1'0000'0005ull;
  CHECK_EQ(reconstruct_us(now, 0xFFFFFFF0u), 0xFFFFFFF0ull);
  CHECK_EQ(reconstruct_us(now, 0x00000003u), 0x1'0000'0003ull);
  // And several wraps in: the high word must follow the sample, not a counter.
  const uint64_t later = 0x7'0000'0010ull;
  CHECK_EQ(reconstruct_us(later, 0xFFFFFFFFu), 0x6'FFFF'FFFFull);
  CHECK_EQ(reconstruct_us(later, 0x00000010u), 0x7'0000'0010ull);
}

TEST(reconstruct_us_handles_a_record_stamped_after_the_clock_sample) {
  // THE case the spec's original three-liner got wrong, and the reason this function is signed.
  // The writer samples the clock once and then drains, while the RX ISR keeps stamping records
  // into the tap rings for the whole pass — so records newer than the sample are the normal case
  // at load, not an edge. With `hi--` and hi == 0 (the first ~71 min after boot, i.e. every bench
  // run) the high word underflowed to 0xFFFFFFFF and the line was stamped ~584 000 years out.
  CHECK_EQ(reconstruct_us(1000, 1050), 1050u);
  CHECK_EQ(reconstruct_us(0, 40), 40u);
  CHECK_EQ(reconstruct_us(5, 9), 9u);
  // Same thing across the wrap: sample just before, record just after.
  CHECK_EQ(reconstruct_us(0xFFFFFFF0ull, 0x00000005u), 0x1'0000'0005ull);
}

TEST(reconstruct_us_never_reports_a_time_wildly_far_from_the_sample) {
  // The property that actually matters on a card: whatever the pair of numbers, the answer stays
  // within the ±~36 min the signed difference can express. A regression to the unsigned form fails
  // here by ~584 000 years — in both directions, which is why the bound is checked on both sides.
  const uint64_t samples[] = {0ull, 1000ull, 0xFFFFFFF0ull, 0x1'0000'0005ull, 0x7'0000'0010ull};
  const uint32_t stamps[] = {0u, 1u, 40u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFFu};
  const uint64_t bound = 1ull << 31;  // ~36 min in µs
  for (uint64_t now : samples) {
    for (uint32_t t32 : stamps) {
      const uint64_t got = reconstruct_us(now, t32);
      CHECK_MSG(got <= now + bound, "reconstruct_us(" + std::to_string(now) + ", " + std::to_string(t32) +
                                        ") = " + std::to_string(got) + ", which is absurdly far in the future");
      if (now < bound)
        continue;  // a stamp older than the clock itself is unreachable; see the saturation case
      if (got < now)
        CHECK_MSG(now - got <= bound, "reconstruct_us(" + std::to_string(now) + ", " + std::to_string(t32) +
                                          ") = " + std::to_string(got) + ", which is absurdly far in the past");
      CHECK_EQ_MSG(static_cast<uint32_t>(got), t32, "low word must always be the record's own stamp");
    }
  }
}

TEST(reconstruct_us_saturates_instead_of_underflowing) {
  // A stamp claiming to predate boot cannot be produced by a running board — it needs a clock
  // sample below the record's own age. But the last step of the reconstruction is an unsigned
  // subtraction, and an underflow there lands right back on the ~584 000-year timestamp this
  // function exists to prevent. So the arithmetic is total for every pair, reachable or not.
  CHECK_EQ(reconstruct_us(0, 0xFFFFFFF0u), 0u);
  CHECK_EQ(reconstruct_us(1000, 0xFFFFFFFFu), 0u);
}

// ------------------------------------------------------------- the delta timestamp chain (F1h)
//
// v2 delta-codes the stamp column: a bare number is the step from the previous stream line, `@n` is
// an absolute anchor. It is worth ~6 B of a 43 B line and, unlike the absolute it replaces, it does
// not get wider as uptime grows — the saving is largest on exactly the long soak where the log
// matters most.
//
// It is also the one change in this file that can produce a card nobody can tell is wrong. A wrong
// flag letter is visible; a chain that anchored in the wrong place yields a file where every line
// parses, every column is present, and the times are silently off — by an unsigned underflow of
// ~584 000 years on a backward step, or by whatever the previous chunk happened to end at. So each
// way time misbehaves on this bench gets its own case below, and then one case replays the lot
// through a decoder, because agreeing field-by-field is not the same as decoding back.

namespace {

/// Format one record onto an explicit chain — what the writer does, line after line, into one file.
std::string chained(DeltaClock &clock, const SourceTable &table, const LogRecord &rec, uint64_t t_full) {
  char buf[SD_LOG_MAX_LINE];
  const size_t n = esphome::sd_logger::format_record(buf, sizeof(buf), clock, rec, t_full, table);
  return std::string(buf, n);
}

/// The stamp column of a formatted line, `@` included when it is there.
std::string stamp_of(const std::string &text) {
  const size_t first = text.find(',');
  const size_t second = text.find(',', first + 1);
  return text.substr(first + 1, second - first - 1);
}

/// The reader half in the fewest lines that can be wrong: `@n` restarts the chain, a bare `n` steps
/// it, and a step before any anchor is undecodable. Everything a host-side parser has to do is here,
/// which is what makes the replay case a check of the *format* and not of one formatter.
struct Decoder {
  uint64_t prev{0};
  bool started{false};
  bool decodable{true};

  uint64_t feed(const std::string &stamp) {
    if (!stamp.empty() && stamp[0] == '@') {
      this->prev = std::strtoull(stamp.c_str() + 1, nullptr, 16);
      this->started = true;
      return this->prev;
    }
    if (!this->started)
      this->decodable = false;  // a step with nothing to step from
    this->prev += std::strtoull(stamp.c_str(), nullptr, 16);
    return this->prev;
  }
};

}  // namespace

TEST(the_first_line_of_a_chunk_anchors_and_the_next_one_steps) {
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, REC_FLAG_EXTENDED, 8);
  DeltaClock clock;
  CHECK_EQ(chained(clock, table, rec, 412345), std::string("C1x,@64AB9,1A2,0011223344556677\n"));
  CHECK_EQ(chained(clock, table, rec, 412388), std::string("C1x,2B,1A2,0011223344556677\n"));
  // ...and it keeps stepping: the second step is from the second line, not from the anchor.
  CHECK_EQ(stamp_of(chained(clock, table, rec, 412500)), std::string("70"));
}

TEST(a_backward_stamp_anchors_instead_of_underflowing) {
  // THE case an unsigned delta gets wrong, and it is not an edge: the writer samples the clock once
  // and then drains ring after ring, so a record out of the second ring is routinely older than the
  // last one out of the first (F1f) — the same non-monotonicity
  // `reconstruct_us_handles_a_record_stamped_after_the_clock_sample` exists for. `412300 - 412388`
  // as a uint64 is 18 446 744 073 709 551 528, which would parse cleanly and stamp the line ~584 000
  // years out while the bench reports a healthy logger.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  DeltaClock clock;
  chained(clock, table, rec, 412388);
  const std::string back = chained(clock, table, rec, 412300);
  CHECK_EQ(stamp_of(back), std::string("@64A8C"));
  // And the chain continues from the record that was actually written, not from the newer one.
  CHECK_EQ(stamp_of(chained(clock, table, rec, 412350)), std::string("32"));
}

TEST(the_chain_steps_straight_through_the_32_bit_wrap) {
  // The 32-bit stamp wraps every ~71.6 min, which on a soak happens many times. The chain never
  // sees it: `encode()` takes the reconstructed 64-bit value, so the pair below is 19 µs apart even
  // though the raw stamps are 0xFFFFFFF0 and 0x00000003. Feeding it `rec.t_us` instead would step
  // backwards by 4 294 967 277 and anchor on every wrap at best.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  const uint64_t now = 0x1'0000'0005ull;
  DeltaClock clock;
  CHECK_EQ(stamp_of(chained(clock, table, rec, reconstruct_us(now, 0xFFFFFFF0u))), std::string("@FFFFFFF0"));
  CHECK_EQ(stamp_of(chained(clock, table, rec, reconstruct_us(now, 0x00000003u))), std::string("13"));
}

TEST(a_card_stall_past_the_resync_cap_anchors_and_a_gap_below_it_steps) {
  // M6 measured a 1672 ms write() stall with records buffered straight through it, so a
  // multi-second step is legal traffic. It anchors anyway: a 7-digit step saves nothing against the
  // absolute, and re-anchoring where the stream is blocked costs nothing.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  DeltaClock clock;
  chained(clock, table, rec, 412345);
  CHECK_EQ(stamp_of(chained(clock, table, rec, 412345 + 1672000)), std::string("@1FCDF9"));
  // The cap itself is a step — the boundary is `>`, so it is stated rather than left to a reader.
  DeltaClock edge;
  chained(edge, table, rec, 1000);
  CHECK_EQ(stamp_of(chained(edge, table, rec, 1000 + SD_LOG_RESYNC_US)), std::string("F4240"));
  DeltaClock over;
  chained(over, table, rec, 1000);
  CHECK_EQ(stamp_of(chained(over, table, rec, 1000 + SD_LOG_RESYNC_US + 1)), std::string("@F4629"));
}

TEST(every_chunk_decodes_without_the_chunks_before_it) {
  // `reset()` is called from write_file_header_(), so the first stream line of every file anchors.
  // The collection server serves chunks individually and retention deletes old ones, so a chain
  // carried across a rotation would make a chunk undecodable the moment its predecessor is gone.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  DeltaClock clock;
  chained(clock, table, rec, 412345);
  chained(clock, table, rec, 412388);
  clock.reset();  // rotation: a new file, a new header block
  CHECK_EQ(stamp_of(chained(clock, table, rec, 412500)), std::string("@64B54"));
}

TEST(the_chain_re_anchors_on_a_line_count_so_one_bad_stamp_cannot_reach_the_end_of_a_file) {
  // A busy bus never trips the gap cap — ~4 ms between frames, for the whole 32 MB — so without a
  // line count the file would be one unbroken chain and a single garbled stamp would shift every
  // timestamp behind it to the end of the chunk. The count bounds the damage instead.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  DeltaClock clock;
  uint32_t anchors = 0;
  uint32_t run = 0;
  uint32_t longest_run = 0;
  uint32_t last_anchor = 0;
  for (uint32_t i = 0; i < 1000; i++) {
    // 4200 µs apart, the bench's own record spacing and far inside the resync cap, so nothing but
    // the line count can be producing these anchors.
    if (stamp_of(chained(clock, table, rec, 1000 + 4200ull * i))[0] == '@') {
      if (anchors == 0) {
        CHECK_EQ_MSG(i, 0u, "the first line of a chunk must anchor");
      } else {
        CHECK_EQ_MSG(i - last_anchor, SD_LOG_ANCHOR_EVERY + 1, "anchors are not evenly spaced");
      }
      anchors++;
      last_anchor = i;
      run = 0;
    } else {
      run++;
      longest_run = run > longest_run ? run : longest_run;
    }
  }
  CHECK_EQ(longest_run, SD_LOG_ANCHOR_EVERY);  // at most this many steps hang off one anchor
  CHECK_EQ(anchors, 4u);                       // lines 0, 257, 514, 771
}

TEST(text_lines_share_the_record_chain) {
  // One chain over every stream line, so a reader maintains it without telling a frame from a log
  // message — it only has to skip the `#` meta lines. A second chain for `X` would desync the first
  // one the moment the two interleave, which at load is every pass.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  DeltaClock clock;
  chained(clock, table, rec, 412345);
  chained(clock, table, rec, 412388);
  char buf[SD_LOG_MAX_LINE];
  const char msg[] = "records=4120 dropped=0 bytes=198112";
  const size_t n = esphome::sd_logger::format_text(buf, sizeof(buf), clock, 412502, /*level=*/3, "sd_logger", 9, msg,
                                                   sizeof(msg) - 1);
  CHECK_EQ(std::string(buf, n), std::string("XI,72,sd_logger,records=4120 dropped=0 bytes=198112\n"));
  // ...and the record after it steps from the text line, not from the record before it.
  CHECK_EQ(stamp_of(chained(clock, table, rec, 412610)), std::string("6C"));
}

TEST(a_line_the_formatter_refuses_does_not_advance_the_chain) {
  // The room check comes before `encode()` on purpose. A chain advanced for a line that was never
  // written is the quietest failure this format has: every later timestamp is out by the step of a
  // record that is not in the file, and nothing about the file says so.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  DeltaClock clock;
  chained(clock, table, rec, 412345);
  char tiny[8];
  CHECK_EQ(esphome::sd_logger::format_record(tiny, sizeof(tiny), clock, rec, 999999, table), 0u);
  CHECK_EQ(stamp_of(chained(clock, table, rec, 412388)), std::string("2B"));
}

TEST(a_replayed_chain_reproduces_every_absolute_timestamp) {
  // The property, rather than the encoding: whatever the writer emitted, a reader that knows the
  // two rules gets the original µs back. The sequence is deliberately the nastiest one this bench
  // produces — a normal step, a record older than the one before it, a 1672 ms card stall, a 32-bit
  // wrap, and a rotation in the middle of it.
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, 0, 0);
  const uint64_t times[] = {
      412345, 412388, 412300, 412300 + 1672000, 0xFFFFFFF0ull, 0x1'0000'0003ull, 0x1'0000'0007ull,
  };
  DeltaClock clock;
  Decoder decoder;
  for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
    if (i == 4)
      clock.reset();  // a rotation lands mid-sequence; the reader below is never told
    const uint64_t got = decoder.feed(stamp_of(chained(clock, table, rec, times[i])));
    CHECK_EQ_MSG(got, times[i], "replayed stamp " + std::to_string(i));
  }
  CHECK_MSG(decoder.decodable, "the chain stepped before it ever anchored");
}

TEST(the_format_version_says_a_v1_reader_must_not_try) {
  // The stamp column changed shape, so a v1 parser would call every record line malformed instead
  // of saying "newer format". The version is the only thing that can tell it apart, and
  // `script/sdlog.py` now branches on it rather than only printing it.
  CHECK_EQ(SD_LOG_FORMAT_VERSION, 2u);
}

TEST(a_record_line_is_thirty_bytes_where_v1_spent_forty_three) {
  // The measurement the whole format change exists for, taken off the real emitter rather than
  // estimated. The realistic steady state on this bench: ~1 h uptime (so v1's absolute stamp is at
  // its typical 10 digits) and ~238 rec/s (so the step is ~4200 µs).
  const SourceTable table = two_bus_table();
  const LogRecord rec = record_with(1, 0x1A2, REC_FLAG_EXTENDED, 8);
  DeltaClock clock;
  const std::string anchor = chained(clock, table, rec, 3600412345ull);
  const std::string step = chained(clock, table, rec, 3600412345ull + 4200);
  CHECK_EQ(anchor, std::string("C1x,@D699EEB9,1A2,0011223344556677\n"));
  CHECK_EQ(step, std::string("C1x,1068,1A2,0011223344556677\n"));

  // The same frame as v1 wrote it, spelled out because it is the baseline every figure in the
  // handover is quoted against and there is no longer any code that can produce it:
  //   `C` `,` decimal-absolute `,` label `,` id `,` flags `,` dlc `,` data
  const std::string v1 = "C,3600412345,seg1,1A2,x,8,0011223344556677\n";
  CHECK_EQ(v1.size(), 43u);
  CHECK_EQ(anchor.size(), 35u);
  CHECK_EQ(step.size(), 30u);
  // 13 B off the line that carries ~99.6 % of the file, and the step does not widen with uptime
  // the way the absolute it replaced does.
  CHECK_EQ(v1.size() - step.size(), 13u);
}

TEST(no_number_on_a_record_line_is_decimal) {
  // One radix for the whole line, and a case that fails if any single field quietly reverts.
  //
  // It is not a style rule: a decimal field is a divide-and-modulo per digit on a path that runs
  // ~3600 times a second, which is the cost v1 was still paying on its widest column. Every value
  // here is chosen so its hex and decimal spellings differ — 4200 is `1068`, tag 26 is `1A`, id
  // 0x2AF is `687` in decimal — so a reverted field cannot pass by looking the same in both.
  SourceTable table;
  table.add(26, KIND_CAN, "C26");
  LogRecord rec = record_with(26, 0x2AF, 0, 4);
  DeltaClock clock;
  chained(clock, table, rec, 1000000);
  const std::string out = chained(clock, table, rec, 1000000 + 4200);
  CHECK_EQ(out, std::string("C1A,1068,2AF,00112233\n"));
  // And the anchor, whose value is wide enough that a decimal spelling would be obvious.
  DeltaClock fresh;
  CHECK_EQ(stamp_of(chained(fresh, table, rec, 3600412345ull)), std::string("@D699EEB9"));
}

TEST(the_leading_token_is_self_delimiting_because_hex_and_flags_do_not_overlap) {
  // The token fuses kind, tag and flags with no separator, which is only safe because the tag's
  // alphabet (uppercase hex) and the flag alphabet (lowercase, plus `~`) are disjoint. A tag whose
  // hex spelling ends in a letter is the case that would break a naive split, so it is the case
  // pinned here: `C1Ax` is tag 0x1A with the extended flag, not tag 0x1 with flags `Ax`.
  SourceTable table;
  table.add(0x1A, KIND_CAN, "C26");
  const LogRecord rec = record_with(0x1A, 0x7FF, REC_FLAG_EXTENDED, 0);
  DeltaClock clock;
  const std::string out = chained(clock, table, rec, 1);
  CHECK_EQ(out, std::string("C1Ax,@1,7FF,\n"));
  // Every flag letter must stay outside the hex alphabet for that rule to hold, so check the
  // alphabets themselves rather than one example of them.
  for (const char flag : {'x', 'r', 't', 's', SD_LOG_TRUNCATED_MARK}) {
    const bool is_hex_digit = (flag >= '0' && flag <= '9') || (flag >= 'A' && flag <= 'F');
    CHECK_MSG(!is_hex_digit, std::string("flag letter '") + flag + "' is also a hex digit");
  }
}

// ------------------------------------------------------------------------------- level letters

TEST(level_letters_cover_every_esphome_level) {
  CHECK_EQ(level_letter(1), 'E');
  CHECK_EQ(level_letter(2), 'W');
  CHECK_EQ(level_letter(3), 'I');
  CHECK_EQ(level_letter(4), 'C');
  CHECK_EQ(level_letter(5), 'D');
  CHECK_EQ(level_letter(6), 'V');
  // VERY_VERBOSE collapses onto VERBOSE; the column stays one byte.
  CHECK_EQ(level_letter(7), 'V');
  // NONE and anything out of range must still be a single printable byte, not a gap in the line.
  CHECK_EQ(level_letter(0), '?');
  CHECK_EQ(level_letter(200), '?');
}

// --------------------------------------------------------------- ESPHome log prefix stripping

namespace {

std::string payload_of(const std::string &raw, size_t room = 256) {
  char out[256];
  bool truncated = false;
  const size_t n = copy_log_payload(out, room > sizeof(out) ? sizeof(out) : room, raw.data(), raw.size(), &truncated);
  return std::string(out, n);
}

}  // namespace

TEST(the_log_prefix_and_its_colour_are_stripped) {
  // What a callback actually receives from the pinned logger: colour, "[I][tag:line]: ", the text,
  // then the reset.
  CHECK_EQ(payload_of("\x1B[0;32m[I][sd_logger:093]: mounted\x1B[0m"), std::string("mounted"));
  CHECK_EQ(payload_of("[I][sd_logger:093]: mounted"), std::string("mounted"));
}

TEST(the_extra_bracketed_thread_name_is_stripped_too) {
  // Anything logged off the main task carries a thread name ahead of the level, which is exactly
  // the writer and VCC-monitor tasks — the messages a card most wants.
  CHECK_EQ(payload_of("\x1B[0;33m[W][sdlog_wr:212]: ring full\x1B[0m"), std::string("ring full"));
  CHECK_EQ(payload_of("[sdlog_wr][W][sd_logger:212]: ring full"), std::string("ring full"));
}

TEST(a_message_containing_its_own_bracket_keeps_everything_after_the_first_prefix) {
  // The first "]: " is always the end of the prefix; a later one belongs to the message.
  CHECK_EQ(payload_of("[I][gw:010]: route[0]: shed"), std::string("route[0]: shed"));
}

TEST(a_line_with_no_recognisable_prefix_is_kept_whole) {
  // ESP-IDF's own output arrives under tag "esp-idf" and does not always carry the ESPHome shape.
  // An ugly line beats a lost one.
  CHECK_EQ(payload_of("I (1234) sdmmc: card detected"), std::string("I (1234) sdmmc: card detected"));
  CHECK_EQ(payload_of(""), std::string(""));
}

TEST(colour_inside_the_message_body_is_dropped_as_well) {
  CHECK_EQ(payload_of("[I][x:1]: a\x1B[0;31mred\x1B[0mb"), std::string("aredb"));
}

TEST(a_payload_longer_than_the_slot_is_truncated_and_says_so) {
  const std::string raw = "[I][x:1]: " + std::string(300, 'y');
  char out[64];
  bool truncated = false;
  const size_t n = copy_log_payload(out, sizeof(out), raw.data(), raw.size(), &truncated);
  CHECK_EQ(n, sizeof(out));
  CHECK(truncated);
  CHECK_EQ(out[0], 'y');
}

TEST(a_payload_that_exactly_fills_the_slot_is_not_called_truncated) {
  const std::string raw = "[I][x:1]: abcd";
  char out[4];
  bool truncated = false;
  const size_t n = copy_log_payload(out, sizeof(out), raw.data(), raw.size(), &truncated);
  CHECK_EQ(n, 4u);
  CHECK(!truncated);
}

// ------------------------------------------------------------------------------- file names (F2)

TEST(log_names_are_8_3_and_zero_padded) {
  char name[esphome::sd_logger::SD_LOG_NAME_LEN];
  format_log_name(name, 0);
  CHECK_EQ(std::string(name), std::string("L0000000.LOG"));
  format_log_name(name, 8);
  CHECK_EQ(std::string(name), std::string("L0000008.LOG"));
  format_log_name(name, 1234567);
  CHECK_EQ(std::string(name), std::string("L1234567.LOG"));
  // F2b: the last representable name. Beyond it the counter would widen the name past 8.3, which
  // is written down rather than assumed safe.
  format_log_name(name, 9999999);
  CHECK_EQ(std::string(name), std::string("L9999999.LOG"));
}

TEST(the_boot_scan_matches_both_the_new_and_the_M1_era_extension) {
  // A card can hold files from both eras, and matching only .LOG would restart the sequence at 0 —
  // which is precisely the failure F2a documents (201 693 records lost across a 397-file walk).
  uint32_t seq = 0;
  CHECK(parse_log_seq("L0000007.LOG", &seq));
  CHECK_EQ(seq, 7u);
  CHECK(parse_log_seq("L0000042.CSV", &seq));
  CHECK_EQ(seq, 42u);
  CHECK(parse_log_seq("L9999999.LOG", &seq));
  CHECK_EQ(seq, 9999999u);
}

TEST(the_boot_scan_rejects_everything_that_is_not_a_log_file) {
  uint32_t seq = 0xDEADBEEF;
  const char *rejects[] = {"",
                           "L.LOG",
                           "L000000.LOG",    // six digits
                           "L00000007.LOG",  // eight digits
                           "L0000007.TXT",
                           "L0000007LOG",
                           "X0000007.LOG",
                           "L000000A.LOG",
                           "l0000007.log",  // FatFs without LFN reports upper case
                           "SYSTEM~1"};
  for (const char *name : rejects)
    CHECK_EQ_MSG(parse_log_seq(name, &seq), false, name);
  CHECK_EQ(seq, 0xDEADBEEFu);  // a reject must not touch the caller's counter
}

// ------------------------------------------------------------------------- pad line + alignment

TEST(the_pad_line_puts_the_stream_on_a_sector_boundary) {
  for (size_t offset = 0; offset < 3 * SD_LOG_SECTOR; offset++) {
    char buf[2 * SD_LOG_SECTOR];
    const size_t n = format_pad(buf, sizeof(buf), offset);
    CHECK_MSG(n >= 6, "pad line shorter than \"#pad,\\n\" at offset " + std::to_string(offset));
    CHECK_EQ_MSG((offset + n) % SD_LOG_SECTOR, 0u, "offset " + std::to_string(offset));
    CHECK_EQ_MSG(std::string(buf, 5), std::string("#pad,"), "offset " + std::to_string(offset));
    CHECK_EQ_MSG(buf[n - 1], '\n', "offset " + std::to_string(offset));
    for (size_t i = 5; i + 1 < n; i++)
      CHECK_EQ_MSG(buf[i], ' ', "pad body must be spaces at offset " + std::to_string(offset));
  }
}

TEST(the_pad_line_refuses_a_buffer_it_would_overrun) {
  char buf[8];
  std::memset(buf, '\xEE', sizeof(buf));
  CHECK_EQ(format_pad(buf, sizeof(buf), 100), 0u);
  for (char c : buf)
    CHECK_EQ(static_cast<unsigned char>(c), 0xEEu);
}

// ------------------------------------------------------------------------------- block buffer

TEST(the_block_buffer_only_ever_offers_whole_sectors) {
  // The invariant FATFS cares about: hand it a partial sector and it does a read-modify-write,
  // which shows up on the bench as "the card got slower" and nowhere else.
  char storage[4096];
  BlockBuffer buf;
  buf.attach(storage, sizeof(storage));
  CHECK(buf.valid());
  for (size_t n = 0; n <= sizeof(storage); n += 37) {
    buf.reset();
    buf.commit(n);
    const size_t flush = buf.flush_len();
    CHECK_EQ_MSG(flush % SD_LOG_SECTOR, 0u, "commit " + std::to_string(n));
    CHECK_MSG(flush <= n, "flush_len exceeds what was committed");
    CHECK_MSG(n - flush < SD_LOG_SECTOR, "a whole sector was left behind at commit " + std::to_string(n));
  }
}

TEST(consuming_a_flush_keeps_the_sub_sector_remainder) {
  char storage[4096];
  BlockBuffer buf;
  buf.attach(storage, sizeof(storage));
  std::memcpy(buf.cursor(), "HEAD", 4);
  buf.commit(700);
  // Poison the tail so the memmove is observable.
  std::memcpy(storage + 512, "TAILTAIL", 8);
  const size_t flush = buf.flush_len();
  CHECK_EQ(flush, 512u);
  buf.consume(flush);
  CHECK_EQ(buf.pending(), 188u);
  CHECK_EQ(std::string(buf.data(), 8), std::string("TAILTAIL"));
  CHECK_EQ(buf.room(), 4096u - 188u);
}

TEST(draining_everything_is_allowed_only_as_the_last_write_of_a_file) {
  // Closing a file writes the partial remainder — there is nothing left to align for.
  char storage[4096];
  BlockBuffer buf;
  buf.attach(storage, sizeof(storage));
  buf.commit(700);
  CHECK_EQ(buf.drain_all(), 700u);
  buf.consume(700);
  CHECK_EQ(buf.pending(), 0u);
  CHECK_EQ(buf.drain_all(), 0u);
}

TEST(a_full_pass_of_records_stays_sector_aligned_end_to_end) {
  // The property the writer actually depends on: header padded to a boundary, then every flush a
  // whole number of sectors, so the file offset is sector-aligned before every single write.
  char storage[4096];
  BlockBuffer buf;
  buf.attach(storage, sizeof(storage));
  const SourceTable table = two_bus_table();
  DeltaClock clock;

  uint64_t file_offset = 0;
  size_t n = format_header(buf.cursor(), buf.room(), 0, 1000, "2026.7.0");
  buf.commit(n);
  n = format_src_num(buf.cursor(), buf.room(), KIND_CAN, "seg1", 1, "can_gateway", 500000);
  buf.commit(n);
  n = format_types(buf.cursor(), buf.room());
  buf.commit(n);
  n = format_flags_legend(buf.cursor(), buf.room());
  buf.commit(n);
  n = format_pad(buf.cursor(), buf.room(), buf.pending());
  buf.commit(n);
  CHECK_EQ(buf.pending() % SD_LOG_SECTOR, 0u);

  for (uint32_t i = 0; i < 400; i++) {
    if (buf.room() < SD_LOG_MAX_LINE) {
      const size_t flush = buf.flush_len();
      CHECK_EQ_MSG(file_offset % SD_LOG_SECTOR, 0u, "write started off a sector boundary");
      CHECK_EQ_MSG(flush % SD_LOG_SECTOR, 0u, "write was not a whole number of sectors");
      file_offset += flush;
      buf.consume(flush);
    }
    const LogRecord rec = record_with(1, 0x100 + i, REC_FLAG_EXTENDED, 8);
    // One chain for the whole file, like the writer holds: 400 records is past SD_LOG_ANCHOR_EVERY,
    // so this pass also formats lines on both sides of a periodic re-anchor, which is where a line
    // whose width the alignment arithmetic did not expect would show up.
    const size_t written = esphome::sd_logger::format_record(buf.cursor(), buf.room(), clock, rec, 412345 + i, table);
    CHECK_MSG(written > 0, "a record did not fit a freshly flushed buffer");
    buf.commit(written);
  }
}

// --------------------------------------------------------------- retention gap marker (M6 §7)

// Retention is what makes "store everything all the time" bounded: at ~148 KB/s the card holds
// about 60 hours, so above a fill threshold the oldest chunks are deleted — CONFIRMED ones first,
// then SEALED ones that were never collected, which is real, permanent data loss.
//
// `#gap` is the in-band statement of that loss, on the same terms as `#drop`: emitted only when the
// discard counters move. It is the only trace the deletion leaves, and it is exactly the kind of
// line the bench cannot check — a card whose history quietly skips six hours still reports
// `dropped == 0` and still parses as valid CSV.

namespace {

/// The one gap line the cases below share, so the golden text and the truncation arithmetic cannot
/// drift apart: three 16 MB-ish chunks discarded, seq 412 through 414 inclusive.
const char GAP_GOLDEN[] = "#gap,@65132,3,50331648,412,414\n";

size_t gap_golden(char *out, size_t room) { return format_gap(out, room, 414002, 3, 50331648, 412, 414); }

}  // namespace

TEST(the_gap_line_is_the_documented_shape) {
  // `#gap,<t_us>,<chunks>,<bytes>,<first_seq>,<last_seq>` (design §7a, the arbiter). The seq pair
  // names the ENDPOINTS of the window retention emptied, in the order it emptied them — it is not a
  // promise that every seq between them is gone. Expanding the span into "L0000412 through
  // L0000414 are the ones you will not find" is precisely the over-report §7a forbids: when
  // `chunks` is short of the span the window is punctured, and the seqs inside it were collected
  // safely rather than lost.
  CHECK_EQ(line(gap_golden), std::string(GAP_GOLDEN));
  // A single discarded chunk repeats its seq rather than collapsing the range to one field — the
  // reader splits on commas, so the field count must never vary.
  CHECK_EQ(line([](char *b, size_t r) { return format_gap(b, r, 500000, 1, 4194304, 7, 7); }),
           std::string("#gap,@7A120,1,4194304,7,7\n"));
}

TEST(a_gap_of_zero_still_emits_every_field) {
  // The emitter only fires when a counter moves, so an all-zero line should not occur in a healthy
  // file. It is checked anyway because a field that vanishes at zero shifts every later column, and
  // that is precisely the failure this whole file exists to catch: a well-formed, quietly wrong
  // line that no bench run can see.
  CHECK_EQ(line([](char *b, size_t r) { return format_gap(b, r, 0, 0, 0, 0, 0); }), std::string("#gap,@0,0,0,0,0\n"));
}

TEST(the_gap_line_carries_its_counters_at_full_width) {
  // `bytes` is the counter that actually gets large: 12.4 GB/day continuous (§7) passes the 32-bit
  // ceiling in under six hours, and a silently wrapped total is worse than no total at all.
  CHECK_EQ(line([](char *b, size_t r) { return format_gap(b, r, 86400000000ull, 3200, 12787200000ull, 1, 3200); }),
           std::string("#gap,@141DD76000,3200,12787200000,1,3200\n"));
  // Every field at its type's maximum, which is also the 20-digit case for the decimal emitter.
  CHECK_EQ(line([](char *b, size_t r) {
             return format_gap(b, r, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFu,
                               0xFFFFFFFFu);
           }),
           std::string("#gap,@FFFFFFFFFFFFFFFF,4294967295,18446744073709551615,4294967295,4294967295\n"));
}

TEST(the_gap_line_ends_like_every_other_line_in_the_format) {
  // F1e's one hard invariant: exactly one newline, at the very end, whatever the numbers. A gap
  // marker that fits its buffer must also not wear the truncation mark.
  struct Row {
    uint64_t t;
    uint32_t chunks;
    uint64_t bytes;
    uint32_t first;
    uint32_t last;
    const char *what;
  };
  const Row rows[] = {
      {0, 0, 0, 0, 0, "all zero"},
      {414002, 3, 50331648, 412, 414, "typical"},
      {1, 1, 1, 9999999, 9999999, "last representable seq (F2b)"},
      {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFu, 0xFFFFFFFFu, "all maxima"},
  };
  for (const Row &row : rows) {
    const std::string out =
        line([&](char *b, size_t r) { return format_gap(b, r, row.t, row.chunks, row.bytes, row.first, row.last); });
    check_single_trailing_newline(out, row.what);
    CHECK_MSG(out.size() >= 5 && out.compare(0, 5, "#gap,") == 0,
              std::string(row.what) + ": line does not start with \"#gap,\"");
    CHECK_MSG(out.find(SD_LOG_TRUNCATED_MARK) == std::string::npos,
              std::string(row.what) + ": a line that fits its buffer must not be marked truncated");
  }
}

TEST(a_gap_line_one_byte_short_of_its_room_is_marked_not_overrun) {
  // The writer formats straight into the block buffer at the write cursor, so "one byte short" is
  // an ordinary end-of-buffer event, not an edge case. It must behave exactly like every sibling
  // emitter: cut the line, append the truncation mark and the terminator inside `room`, and never
  // touch a byte past it.
  const size_t full = sizeof(GAP_GOLDEN) - 1;
  CHECK_EQ(line(gap_golden).size(), full);

  char buf[64];
  std::memset(buf, '\xEE', sizeof(buf));
  const size_t room = full - 1;
  const size_t n = gap_golden(buf, room);
  CHECK_MSG(n > 0, "a room one byte short must still produce a marked line, not nothing");
  CHECK_MSG(n <= room, "formatter wrote past the room it was given");
  const std::string out(buf, n);
  check_single_trailing_newline(out, "gap one byte short");
  CHECK_MSG(out.size() >= 2 && out[out.size() - 2] == SD_LOG_TRUNCATED_MARK,
            "a cut gap line must carry the truncation mark before its terminator");
  CHECK_EQ(out, std::string("#gap,@65132,3,50331648,412,4~\n"));
  for (size_t i = room; i < sizeof(buf); i++)
    CHECK_EQ_MSG(static_cast<unsigned char>(buf[i]), 0xEEu,
                 "byte past `room` was written at index " + std::to_string(i));
}

TEST(a_tight_but_usable_buffer_still_yields_a_terminated_gap_line) {
  // Every room from the minimum the format will attempt up to comfortably past the full line: none
  // may overrun, and none may leave a line that never terminates.
  for (size_t room = 16; room <= 40; room++) {
    char buf[64];
    std::memset(buf, '\xEE', sizeof(buf));
    const size_t n = gap_golden(buf, room);
    CHECK_MSG(n <= room, "formatter wrote past the room it was given at room " + std::to_string(room));
    check_single_trailing_newline(std::string(buf, n), ("tight buffer, room " + std::to_string(room)).c_str());
    for (size_t i = room; i < sizeof(buf); i++)
      CHECK_EQ_MSG(static_cast<unsigned char>(buf[i]), 0xEEu,
                   "byte past `room` was written at room " + std::to_string(room));
  }
}

TEST(a_gap_line_refuses_a_buffer_too_small_for_any_line) {
  // Same contract as format_record and format_pad: below SD_LOG_MIN_ROOM the emitter reports 0 and
  // leaves the caller's buffer untouched, rather than half-writing a line the writer would commit.
  char buf[8];
  std::memset(buf, '\xEE', sizeof(buf));
  CHECK_EQ(gap_golden(buf, sizeof(buf)), 0u);
  for (char c : buf)
    CHECK_EQ(static_cast<unsigned char>(c), 0xEEu);
}

// ---------------------------------------------------------------------------
// Writer-failure accounting
// ---------------------------------------------------------------------------

TEST(unflushed_frame_records_survive_partial_write_accounting_until_their_line_finishes) {
  // Simulated write failure: the first line reached the card, the second only half did. The
  // tracker must retain that second record for enter_failed_() to count; resetting a plain counter
  // on the partial write is precisely how a torn record previously disappeared from accounting.
  UnflushedRecordTracker pending;
  pending.commit_record(20);
  pending.commit_record(45);
  pending.commit_record(70);
  pending.consume(20);
  CHECK_EQ(pending.count(), 2u);
  pending.consume(15);  // partial second line: it remains a loss if the next write fails
  CHECK_EQ(pending.count(), 2u);
  pending.consume(10);  // second line is now complete
  CHECK_EQ(pending.count(), 1u);
  pending.reset();  // enter_failed_() has counted the remaining record before resetting block_
  CHECK_EQ(pending.count(), 0u);
}

TEST(drop_marker_baseline_survives_a_simulated_outage) {
  // A recovered file must compare against the marker written before the outage, not reset to the
  // current value at header time. That makes the three losses incurred while no file was writable
  // appear as one `#drop` in the first recovered file.
  uint32_t delta = 0;
  CHECK_EQ(drop_marker_due(17, 14, &delta), true);
  CHECK_EQ(delta, 3u);
  CHECK_EQ(drop_marker_due(17, 17, &delta), false);
}
