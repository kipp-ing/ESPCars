// F2c — the next boot seals a torn file (components/sd_logger/torn_seal.h, docs/sd_logger-spec.md F2c).
//
// Host tier, because both ways to get this wrong produce a file that looks fine: a cut one byte off
// leaves a line the collector calls malformed (or keeps half a record that decodes as a real one),
// and a seal written into a file that already ended cleanly adds a second `#close` that no bench run
// would notice. IDs in the case names refer to the spec's R table.
//
// The file is a std::string here and the seal I/O a fake that can fail each step. The window and the
// read size follow the firmware: the last 64 KiB (clock spec C5), fed in pieces.

#include "harness.h"
#include "torn_seal.h"

#include <cstdint>
#include <string>

using esphome::sd_logger::format_recovered_close;
using esphome::sd_logger::SD_LOG_CLOCK_TAIL_BYTES;
using esphome::sd_logger::SD_LOG_SEAL_LINE_MAX;
using esphome::sd_logger::SD_LOG_SEAL_PROBE_BYTES;
using esphome::sd_logger::seal_torn_tail;
using esphome::sd_logger::SealResult;
using esphome::sd_logger::TailKind;
using esphome::sd_logger::TornTailScanner;

namespace {

// A fictional header block and records, in the shapes tests/host/test_log_format.cpp pins.
const std::string HEADER = "#sdlog,2,7,@64960,2026.7.0\n"
                           "#clock,@64960,65E2B1A3C4000,utc,sync\n"
                           "#src,C,C1,1,can_gateway,500000\n";
const std::string RECORDS = "C1x,@64AB9,1A2,0011223344556677\n"
                            "C1,2B,1A2,0011223344556677\n"
                            "#drop,@65132,ring,17,17\n"
                            "C1,72,1A2,0011223344556677\n";

enum class Fail : uint8_t { NONE, OPEN, TRUNCATE, WRITE, SHORT_WRITE, SYNC };

/// The card file, and the four calls seal_torn_tail() makes on it.
struct MemFile {
  std::string data;
  Fail fail{Fail::NONE};
  int opens{0};
  int truncates{0};
  int writes{0};
  int syncs{0};

  bool open() {
    this->opens++;
    return this->fail != Fail::OPEN;
  }
  bool truncate(uint64_t length) {
    this->truncates++;
    if (this->fail == Fail::TRUNCATE)
      return false;
    this->data.resize(static_cast<size_t>(length));
    return true;
  }
  bool write_at(uint64_t offset, const char *p, size_t n) {
    this->writes++;
    if (this->fail == Fail::WRITE)
      return false;
    // A short write is the nasty one: part of the line is on the card and the call still failed.
    const size_t take = this->fail == Fail::SHORT_WRITE ? n / 2 : n;
    this->data.replace(static_cast<size_t>(offset), std::string::npos, std::string(p, take));
    return take == n;
  }
  bool sync() {
    this->syncs++;
    return this->fail != Fail::SYNC;
  }
};

/// The firmware's read of one file: the last `window` bytes, in `piece`-byte reads; `fail_at`
/// stops the read at that file offset with an error, as a read() returning -1 would.
TornTailScanner scan(const std::string &file, uint64_t window = SD_LOG_CLOCK_TAIL_BYTES, size_t piece = 4096,
                     uint64_t fail_at = UINT64_MAX) {
  const uint64_t size = file.size();
  const uint64_t start = size > window ? size - window : 0;
  TornTailScanner tail(start);
  for (uint64_t at = start; at < size; at += piece) {
    if (at >= fail_at) {
      tail.read_failed();
      break;
    }
    const uint64_t n = size - at < piece ? size - at : piece;
    tail.feed(file.data() + at, static_cast<size_t>(n));
  }
  return tail;
}

/// One boot over one file: classify, then seal.
SealResult boot(MemFile &f, uint64_t window = SD_LOG_CLOCK_TAIL_BYTES) {
  return seal_torn_tail(f, scan(f.data, window));
}

std::string recovered(uint64_t t) {
  char buf[SD_LOG_SEAL_LINE_MAX];
  return std::string(buf, format_recovered_close(buf, sizeof(buf), t));
}

}  // namespace

TEST(torn_seal_line_is_the_documented_one) {
  CHECK_EQ(recovered(0x65132), std::string("#close,@65132,recovered\n"));
  CHECK_EQ(recovered(0), std::string("#close,@0,recovered\n"));
  // R4: the widest stamp still fits the one <= 64 B append.
  const std::string widest = recovered(UINT64_MAX);
  CHECK_EQ(widest, std::string("#close,@FFFFFFFFFFFFFFFF,recovered\n"));
  CHECK(widest.size() <= SD_LOG_SEAL_LINE_MAX);
}

TEST(torn_seal_R1_R2_a_clean_close_is_never_written) {
  for (const char *reason : {"clean", "rotate", "emergency", "recovered"}) {
    MemFile f;
    f.data = HEADER + RECORDS + "#close,@F3ED0," + reason + "\n";
    const std::string before = f.data;
    CHECK_EQ(scan(f.data).kind(), TailKind::CLOSED);
    CHECK_EQ(boot(f), SealResult::CLOSED);
    CHECK_EQ(f.data, before);
    CHECK_EQ(f.opens, 0);  // not even opened for writing
  }
}

TEST(torn_seal_R1_a_power_cut_between_lines_gets_a_close) {
  // The cut landed on a line boundary: nothing to drop, only the `#close` is missing.
  MemFile f;
  f.data = HEADER + RECORDS;
  CHECK_EQ(boot(f), SealResult::SEALED);
  // `@65132` is the newest timed meta line (`#drop`), not a record anchor (C4's projection).
  CHECK_EQ(f.data, HEADER + RECORDS + "#close,@65132,recovered\n");
  CHECK_EQ(f.truncates, 1);
  CHECK_EQ(f.writes, 1);
  CHECK_EQ(f.syncs, 1);
}

TEST(torn_seal_R1_a_torn_last_line_is_dropped) {
  MemFile f;
  f.data = HEADER + RECORDS + "C1,5A,1A2,00112";
  const TornTailScanner tail = scan(f.data);
  CHECK_EQ(tail.kind(), TailKind::TORN);
  CHECK_EQ(tail.cut(), static_cast<uint64_t>((HEADER + RECORDS).size()));
  CHECK_EQ(tail.end(), static_cast<uint64_t>(f.data.size()));
  CHECK_EQ(boot(f), SealResult::SEALED);
  CHECK_EQ(f.data, HEADER + RECORDS + "#close,@65132,recovered\n");
}

TEST(torn_seal_R1_a_nul_padded_tail_is_dropped) {
  // F1e: a cut can leave the size ahead of the data, read back as zeros.
  MemFile f;
  f.data = HEADER + RECORDS + std::string(700, '\0');
  CHECK_EQ(boot(f), SealResult::SEALED);
  CHECK_EQ(f.data, HEADER + RECORDS + "#close,@65132,recovered\n");
}

TEST(torn_seal_R1_a_torn_close_line_is_replaced) {
  // The writer's own `#close` was cut mid-line: that is a torn line like any other.
  MemFile f;
  f.data = HEADER + RECORDS + "#close,@F3E";
  CHECK_EQ(boot(f), SealResult::SEALED);
  CHECK_EQ(f.data, HEADER + RECORDS + "#close,@65132,recovered\n");
}

TEST(torn_seal_R1_no_timed_line_seals_at_zero) {
  MemFile f;
  f.data = "#sdlog,2,7,@64960,2026.7.0\n#src,C,C1,1,can_gateway,500000\nC1x,@64AB9,1A2,00\nC1,2B,1A";
  CHECK_EQ(boot(f), SealResult::SEALED);
  CHECK_EQ(f.data, std::string("#sdlog,2,7,@64960,2026.7.0\n#src,C,C1,1,can_gateway,500000\nC1x,@64AB9,1A2,00\n"
                               "#close,@0,recovered\n"));
}

TEST(torn_seal_R1_stamp_is_the_newest_meta_line_after_the_last_header) {
  // A seq that was ever appended to holds a second boot below a second header: nothing above it
  // projects into the new boot-time domain (as in ClockTailScanner).
  MemFile f;
  f.data = HEADER + RECORDS + "#rotate,@FFFFFF,L0000008.LOG\n" + HEADER + "C1x,@64AB9,1A2,00\n" +
           "#clock,@70000,65E2B1A3C4000,utc,sync\n#drop,@6FFFF,ring,1,18\nC1,10,1";
  CHECK_EQ(boot(f), SealResult::SEALED);
  const std::string tail_line = "#close,@70000,recovered\n";
  CHECK_EQ(f.data.substr(f.data.size() - tail_line.size()), tail_line);
}

TEST(torn_seal_R1_an_empty_file_is_sealed_from_offset_zero) {
  // Cut before the header reached the card. The seal is still the rule; the collector refuses the
  // result as headerless, which is the honest verdict for a file that holds nothing.
  MemFile f;
  CHECK_EQ(scan(f.data).kind(), TailKind::TORN);
  CHECK_EQ(boot(f), SealResult::SEALED);
  CHECK_EQ(f.data, std::string("#close,@0,recovered\n"));
  MemFile g;
  g.data = "#sdlog,2,7,@6";  // the header line itself torn
  CHECK_EQ(boot(g), SealResult::SEALED);
  CHECK_EQ(g.data, std::string("#close,@0,recovered\n"));
}

TEST(torn_seal_R1_a_mid_file_window_skips_its_partial_first_line) {
  // A 32 MB chunk is read from 64 KiB before its end. The window's first line is partial and is
  // never judged, but stamps after it count.
  std::string body = HEADER;
  while (body.size() < 3 * SD_LOG_CLOCK_TAIL_BYTES)
    body += "C1,2B,1A2,0011223344556677\n";
  body += "#drop,@ABCDE,ring,1,1\nC1,2B,1A2,0011223344556677\nC1,2B,1A";
  MemFile f;
  f.data = body;
  const TornTailScanner tail = scan(f.data);
  CHECK_EQ(tail.kind(), TailKind::TORN);
  CHECK_EQ(tail.stamp(), 0xABCDEull);
  CHECK_EQ(boot(f), SealResult::SEALED);
  const std::string tail_line = "#close,@ABCDE,recovered\n";
  CHECK_EQ(f.data.substr(f.data.size() - tail_line.size()), tail_line);
  CHECK_EQ(f.data.size(), body.size() - std::string("C1,2B,1A").size() + tail_line.size());
}

TEST(torn_seal_R3_a_window_with_no_line_boundary_is_left_alone) {
  // 64 KiB of zeros after the last line: the window starts mid-file and holds no `\n`, so there is
  // no cut point that is known to be right. Left as it is, and counted by the caller.
  MemFile f;
  f.data = HEADER + RECORDS + std::string(SD_LOG_CLOCK_TAIL_BYTES + 10, '\0');
  const std::string before = f.data;
  CHECK_EQ(scan(f.data).kind(), TailKind::NO_CUT);
  CHECK_EQ(boot(f), SealResult::NO_CUT);
  CHECK_EQ(f.data, before);
  CHECK_EQ(f.opens, 0);
  // Same when the only newline in the window ends the partial first line: that line was never seen
  // from its start, so whether it was `#close` is unknown.
  MemFile g;
  g.data = HEADER + "#close,@1,clean\n" + std::string(SD_LOG_CLOCK_TAIL_BYTES - 3, '\0');
  CHECK_EQ(scan(g.data).kind(), TailKind::NO_CUT);
}

TEST(torn_seal_R2_idempotent_a_second_boot_changes_nothing) {
  MemFile f;
  f.data = HEADER + RECORDS + "C1,5A,1A2,0";
  CHECK_EQ(boot(f), SealResult::SEALED);
  const std::string once = f.data;
  CHECK_EQ(boot(f), SealResult::CLOSED);
  CHECK_EQ(f.data, once);
  CHECK_EQ(f.truncates, 1);
  CHECK_EQ(f.writes, 1);
}

TEST(torn_seal_reads_split_anywhere_give_the_same_verdict) {
  // The firmware reads in 4 KiB pieces (256 B on its stack fallback), so a line, a stamp or the
  // `#close,` token can straddle any two reads.
  const std::string file = HEADER + RECORDS + "#rotate,@7A120,L0000008.LOG\nC1,5A,1A2,0011";
  const TornTailScanner whole = scan(file, SD_LOG_CLOCK_TAIL_BYTES, file.size());
  for (size_t piece = 1; piece <= 64; piece++) {
    const TornTailScanner split = scan(file, SD_LOG_CLOCK_TAIL_BYTES, piece);
    CHECK_EQ(split.kind(), whole.kind());
    CHECK_EQ(split.cut(), whole.cut());
    CHECK_EQ(split.stamp(), whole.stamp());
  }
  CHECK_EQ(whole.stamp(), 0x7A120ull);
  const std::string closed = HEADER + RECORDS + "#close,@F3ED0,clean\n";
  for (size_t piece = 1; piece <= 64; piece++)
    CHECK_EQ(scan(closed, SD_LOG_CLOCK_TAIL_BYTES, piece).kind(), TailKind::CLOSED);
}

TEST(torn_seal_the_probe_window_sees_a_clean_close_behind_a_nul_sector) {
  // The firmware reads only SD_LOG_SEAL_PROBE_BYTES of a file the clock scan does not need; a clean
  // file must be recognised from that alone, or every boot pays a 64 KiB read per file.
  std::string body = HEADER;
  while (body.size() < 8 * SD_LOG_SEAL_PROBE_BYTES)
    body += "C1,2B,1A2,0011223344556677\n";
  CHECK_EQ(scan(body + "#close,@F3ED0,rotate\n", SD_LOG_SEAL_PROBE_BYTES).kind(), TailKind::CLOSED);
  // Junk after a complete `#close` does not make the file torn: its last line is `#close` (R2).
  CHECK_EQ(scan(body + "#close,@F3ED0,rotate\n" + std::string(300, '\0'), SD_LOG_SEAL_PROBE_BYTES).kind(),
           TailKind::CLOSED);
  CHECK_EQ(scan(body + "C1,2B", SD_LOG_SEAL_PROBE_BYTES).kind(), TailKind::TORN);
}

TEST(torn_seal_a_long_line_never_yields_a_stamp_cut_mid_digit) {
  // Only the start of each line is kept. A meta line longer than that is parsed only if its stamp
  // field closed inside the kept part.
  MemFile f;
  f.data = HEADER + "#drop,@ABCDEF0,tap:" + std::string(200, 'x') + ",1,1\nC1,2B";
  CHECK_EQ(scan(f.data).stamp(), 0xABCDEF0ull);
  MemFile g;
  g.data = HEADER + "#x," + std::string(60, 'y') + "\nC1,2B";  // no stamp at all
  CHECK_EQ(scan(g.data).stamp(), 0x64960ull);                  // the header block's `#clock`
}

// R3: a failure at any step never stops logging — the caller indexes the file as it is and counts
// it. What matters here is what each failure leaves on the card, and that the next boot recovers.

TEST(torn_seal_R3_a_read_error_writes_nothing) {
  MemFile f;
  f.data = HEADER + RECORDS + "C1,5A";
  const std::string before = f.data;
  const TornTailScanner tail = scan(f.data, SD_LOG_CLOCK_TAIL_BYTES, 16, 32);
  CHECK_EQ(tail.kind(), TailKind::READ_ERR);
  CHECK_EQ(seal_torn_tail(f, tail), SealResult::READ_FAILED);
  CHECK_EQ(f.data, before);
  CHECK_EQ(f.opens, 0);
}

TEST(torn_seal_R3_each_failing_step_is_named_and_the_next_boot_recovers) {
  const std::string torn = HEADER + RECORDS + "C1,5A,1A2,0011";
  const std::string sealed = HEADER + RECORDS + "#close,@65132,recovered\n";
  struct Case {
    Fail fail;
    SealResult want;
  };
  const Case cases[] = {{Fail::OPEN, SealResult::OPEN_FAILED},
                        {Fail::TRUNCATE, SealResult::TRUNC_FAILED},
                        {Fail::WRITE, SealResult::WRITE_FAILED},
                        {Fail::SHORT_WRITE, SealResult::WRITE_FAILED},
                        {Fail::SYNC, SealResult::SYNC_FAILED}};
  for (const Case &c : cases) {
    MemFile f;
    f.data = torn;
    f.fail = c.fail;
    CHECK_EQ(boot(f), c.want);
    // Whatever the failure left — the file as it was, cut back, or a torn `#close` — is still a file
    // without a complete `#close` (or, after a failed fsync, one that may have it): never a second
    // line, never a cut into a complete line.
    CHECK(f.data.size() >= (HEADER + RECORDS).size());
    CHECK_EQ(f.data.compare(0, (HEADER + RECORDS).size(), HEADER + RECORDS), 0);
    // The next boot, with the card behaving, ends in the same sealed file.
    f.fail = Fail::NONE;
    const SealResult again = boot(f);
    CHECK(again == SealResult::SEALED || (c.fail == Fail::SYNC && again == SealResult::CLOSED));
    CHECK_EQ(f.data, sealed);
  }
}
