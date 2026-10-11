// The wall-clock basis of a file (components/sd_logger/clock_basis.h, docs/sd_logger-clock-spec.md).
//
// Host tier, because every failure here is a plausible timestamp: an estimate that passes for a
// reading, a floor a few seconds late, a torn line after a power cut read as the end of the run.
// None of them makes a bench run red. IDs in the case names refer to the spec's requirement table.

#include "clock_basis.h"
#include "harness.h"

#include <cstdint>
#include <string>

using esphome::sd_logger::BootClockDecision;
using esphome::sd_logger::BootClockInputs;
using esphome::sd_logger::clock_at;
using esphome::sd_logger::clock_contradicts_floor;
using esphome::sd_logger::clock_project;
using esphome::sd_logger::clock_restate_due;
using esphome::sd_logger::ClockBasis;
using esphome::sd_logger::ClockPoint;
using esphome::sd_logger::ClockSrc;
using esphome::sd_logger::ClockTailScanner;
using esphome::sd_logger::decide_boot_clock;
using esphome::sd_logger::format_clock;
using esphome::sd_logger::parse_clock_line;
using esphome::sd_logger::parse_utc_line;
using esphome::sd_logger::SD_LOG_CLOCK_MIN_EPOCH_US;
using esphome::sd_logger::SD_LOG_CLOCK_RESTATE_BYTES;
using esphome::sd_logger::SD_LOG_CLOCK_RESTATE_US;
using esphome::sd_logger::SD_LOG_MAX_LINE;
using esphome::sd_logger::WakeKind;

namespace {

// 2026-10-11T00:00:00Z and a build a week before it — fictional, round numbers.
const uint64_t T_2026 = 1791676800ull * 1000000ull;
const uint64_t BUILD = T_2026 - 7ull * 86400ull * 1000000ull;

ClockPoint point(uint64_t t, uint64_t utc, ClockBasis basis, ClockSrc src) {
  ClockPoint p;
  p.t_us = t;
  p.utc_us = utc;
  p.basis = basis;
  p.src = src;
  return p;
}

std::string clock_line(const ClockPoint &p) {
  char buf[SD_LOG_MAX_LINE];
  const size_t n = format_clock(buf, sizeof(buf), p);
  return std::string(buf, n);
}

bool parse(const std::string &line, ClockPoint *out) { return parse_clock_line(line.data(), line.size(), out); }

bool scan(const std::string &text, bool mid, ClockPoint *out) {
  ClockTailScanner scanner(mid);
  scanner.feed(text.data(), text.size());
  return scanner.end(out);
}

}  // namespace

TEST(clock_line_shapes_are_the_documented_ones) {
  CHECK_EQ(clock_line(point(0x64960, 0x65E2B1A3C4000ull, ClockBasis::UTC, ClockSrc::SYNC)),
           std::string("#clock,@64960,65E2B1A3C4000,utc,sync\n"));
  CHECK_EQ(clock_line(point(0x10, 0xABC, ClockBasis::UTC, ClockSrc::WARM)), std::string("#clock,@10,ABC,utc,warm\n"));
  ClockPoint card = point(0, 0xABC, ClockBasis::EST, ClockSrc::CARD);
  card.has_detail = true;
  card.detail = 41;
  CHECK_EQ(clock_line(card), std::string("#clock,@0,ABC,est,card,41\n"));
  CHECK_EQ(clock_line(point(0, 0xABC, ClockBasis::EST, ClockSrc::BUILD)), std::string("#clock,@0,ABC,est,build\n"));
  ClockPoint sleep = point(0x1F4, 0xABC, ClockBasis::DRIFT, ClockSrc::SLEEP);
  sleep.has_detail = true;
  sleep.detail = 600;
  CHECK_EQ(clock_line(sleep), std::string("#clock,@1F4,ABC,drift,sleep,600\n"));
  // Nothing to state is not a line: an empty `#clock` would read as a time of zero.
  CHECK_EQ(clock_line(ClockPoint{}), std::string(""));
}

TEST(clock_lines_round_trip_through_the_parser) {
  ClockPoint card = point(0x123, T_2026, ClockBasis::EST, ClockSrc::CARD);
  card.has_detail = true;
  card.detail = 4294967295u;
  ClockPoint sleep = point(0x1F4, T_2026, ClockBasis::DRIFT, ClockSrc::SLEEP);
  const ClockPoint cases[] = {point(0, T_2026, ClockBasis::UTC, ClockSrc::SYNC),
                              point(UINT64_MAX, UINT64_MAX, ClockBasis::UTC, ClockSrc::WARM), card, sleep,
                              point(7, BUILD, ClockBasis::EST, ClockSrc::BUILD)};
  for (const ClockPoint &want : cases) {
    std::string line = clock_line(want);
    line.pop_back();  // the parser takes a line without its terminator, like the scanner hands it
    ClockPoint got;
    CHECK(parse(line, &got));
    CHECK_EQ(got.t_us, want.t_us);
    CHECK_EQ(got.utc_us, want.utc_us);
    CHECK_EQ(got.basis, want.basis);
    CHECK_EQ(got.src, want.src);
    CHECK_EQ(got.has_detail, want.has_detail);
    CHECK_EQ(got.detail, want.detail);
  }
}

TEST(clock_parser_refuses_what_the_writer_cannot_have_written) {
  ClockPoint p;
  const char *bad[] = {
      "#clock,64960,ABC,utc,sync",           // stamp not anchor-marked
      "#clock,@64960,abc,utc,sync",          // lowercase hex
      "#clock,@64960,,utc,sync",             // empty value
      "#clock,@64960,11112222333344445,utc,sync",  // 17 digits: above 64 bits
      "#clock,@64960,ABC,UTC,sync",          // basis is lowercase
      "#clock,@64960,ABC,utc,card,4",        // a card floor is never a reading
      "#clock,@64960,ABC,est,sync",          // and a sync is never an estimate
      "#clock,@64960,ABC,utc,sleep",         // sleep is drift, not utc
      "#clock,@64960,ABC,utc,sync,4",        // detail on a source that has none
      "#clock,@64960,ABC,est,card",          // a card floor always names its file
      "#clock,@64960,ABC,est,card,4x",       // detail is decimal
      "#clock,@64960,ABC,est,card,4,5",      // a field too many
      "#clock,@64960,ABC,utc",               // a field too few
      "#clocks,@64960,ABC,utc,sync",         // a different line
      "#clock,@64960,ABC,est,card,99999999999999999999999",  // detail overflows
  };
  for (const char *line : bad)
    CHECK(!parse(line, &p));
}

TEST(utc_lines_of_older_files_read_as_synced_readings) {
  ClockPoint p;
  const std::string ok = "#utc,100000003,6543210FEDCBA";
  CHECK(parse_utc_line(ok.data(), ok.size(), &p));
  CHECK_EQ(p.t_us, 0x100000003ull);
  CHECK_EQ(p.utc_us, 0x6543210FEDCBAull);
  CHECK_EQ(p.basis, ClockBasis::UTC);
  CHECK_EQ(p.src, ClockSrc::SYNC);
  const std::string extra = "#utc,1,2,3";
  CHECK(!parse_utc_line(extra.data(), extra.size(), &p));
}

TEST(projection_adds_the_boot_time_difference_and_saturates) {
  const ClockPoint p = point(5000000, T_2026, ClockBasis::UTC, ClockSrc::SYNC);
  CHECK_EQ(clock_project(p, 5000000), T_2026);
  CHECK_EQ(clock_project(p, 7500000), T_2026 + 2500000);
  CHECK_EQ(clock_project(p, 0), T_2026 - 5000000);
  CHECK_EQ(clock_project(point(10, 3, ClockBasis::EST, ClockSrc::BUILD), 0), 0ull);
  CHECK_EQ(clock_project(point(0, UINT64_MAX - 1, ClockBasis::EST, ClockSrc::BUILD), 5), UINT64_MAX);
  const ClockPoint moved = clock_at(p, 9000000);
  CHECK_EQ(moved.t_us, 9000000ull);
  CHECK_EQ(moved.utc_us, T_2026 + 4000000);
  CHECK_EQ(moved.basis, ClockBasis::UTC);
}

// C4 ---------------------------------------------------------------------------------------------

TEST(c4_a_cleanly_closed_file_ends_at_its_close_line) {
  const std::string tail =
      "#sdlog,2,41,@100,2026.7.0\n"
      "#clock,@200,65E2B1A3C4000,utc,sync\n"
      "C1,@300,1A2,00\n"
      "C1,64,1A2,01\n"
      "#clock,@F4240,65E2B1A4B8240,utc,sync\n"
      "C1,@F4300,1A2,02\n"
      "#drop,@F4400,ring,1,1\n"
      "#close,@F4500,clean\n";
  ClockPoint end;
  CHECK(scan(tail, false, &end));
  CHECK_EQ(end.t_us, 0xF4500ull);
  CHECK_EQ(end.utc_us, 0x65E2B1A4B8240ull + (0xF4500 - 0xF4240));
  CHECK_EQ(end.basis, ClockBasis::UTC);
}

TEST(c4_a_power_cut_tail_is_never_read_as_a_time) {
  // The last line has no newline: torn. It would push the end four seconds later.
  const std::string tail =
      "#clock,@200,1000000,est,build\n"
      "#drop,@300,ring,1,1\n"
      "#close,@3D0A00";
  ClockPoint end;
  CHECK(scan(tail, false, &end));
  CHECK_EQ(end.t_us, 0x300ull);
  CHECK_EQ(end.utc_us, 0x1000000ull + 0x100);
  CHECK_EQ(end.basis, ClockBasis::EST);
  CHECK_EQ(end.src, ClockSrc::BUILD);
}

TEST(c4_a_window_that_starts_mid_line_skips_the_fragment) {
  // The window opened inside a `#clock` line: what it sees of it parses as a perfectly good line
  // with a far later time, which is exactly what must not be believed.
  const std::string tail =
      "lock,@999999,FFFFFFFFFFFF,utc,sync\n"
      "#clock,@200,1000000,utc,sync\n"
      "#close,@300,clean\n";
  ClockPoint end;
  CHECK(scan(tail, true, &end));
  CHECK_EQ(end.t_us, 0x300ull);
  CHECK_EQ(end.utc_us, 0x1000100ull);
  // The same bytes read as a whole file would take the first line at face value — it does not
  // start with '#', so nothing changes, which pins down that the flag is what does the work.
  CHECK(scan("#clock,@999999,FFFFFFFFFFFF,utc,sync\n#sdlog,2,1,@1,x\n", true, &end) == false);
}

TEST(c4_garbage_and_overlong_lines_change_nothing) {
  std::string tail = "#clock,@200,1000000,utc,sync\n";
  tail += "#clock,@300,zz,utc,sync\n";       // garbled clock: ignored, not a reset
  tail += "#close,@nothex,clean\n";          // garbled stamp: ignored
  tail += "#clock," + std::string(SD_LOG_MAX_LINE + 40, 'A') + "\n";  // longer than any line
  tail += "#rotate,@280,L0000042.LOG\n";
  ClockPoint end;
  CHECK(scan(tail, false, &end));
  CHECK_EQ(end.t_us, 0x280ull);
  CHECK_EQ(end.utc_us, 0x1000080ull);
}

TEST(c4_the_latest_timed_line_wins_not_the_last_one) {
  // Meta lines are not time-sorted (F1f): the floor is the furthest the run provably got.
  const std::string tail =
      "#clock,@200,1000000,est,card,40\n"
      "#drop,@900,ring,1,1\n"
      "#drop,@500,tap:seg1,1,1\n";
  ClockPoint end;
  CHECK(scan(tail, false, &end));
  CHECK_EQ(end.t_us, 0x900ull);
  CHECK_EQ(end.src, ClockSrc::CARD);
  CHECK_EQ(end.detail, 40ull);
}

TEST(c4_timed_lines_before_the_clock_do_not_count) {
  const std::string tail =
      "#drop,@FFFFFF,ring,1,1\n"
      "#clock,@200,1000000,utc,sync\n";
  ClockPoint end;
  CHECK(scan(tail, false, &end));
  CHECK_EQ(end.t_us, 0x200ull);
}

TEST(c4_an_old_file_with_only_utc_still_gives_an_end) {
  const std::string tail =
      "#sdlog,2,7,@64960,2026.7.0\n"
      "#utc,100,1000000\n"
      "#close,@500,clean\n";
  ClockPoint end;
  CHECK(scan(tail, false, &end));
  CHECK_EQ(end.utc_us, 0x1000400ull);
  CHECK_EQ(end.basis, ClockBasis::UTC);
}

TEST(c4_no_clock_at_all_is_no_answer) {
  ClockPoint end;
  CHECK(!scan("#sdlog,2,7,@1,x\nC1,@2,1A2,00\n#close,@3,clean\n", false, &end));
  CHECK(!scan("", false, &end));
}

TEST(c4_a_second_header_starts_a_new_boot_domain) {
  // A reused seq appended to by a later boot: the first boot's clock does not project into it.
  const std::string tail =
      "#clock,@200,1000000,utc,sync\n"
      "#sdlog,2,7,@5,2026.7.0\n"
      "#close,@500,clean\n";
  ClockPoint end;
  CHECK(!scan(tail, false, &end));
}

TEST(c4_chunked_feeding_gives_the_same_answer) {
  const std::string tail =
      "ent fragment\n"
      "#clock,@200,1000000,est,card,40\n"
      "C1,@300,1A2,00\n"
      "#clock,@F4240,2000000,drift,sleep,600\n"
      "#close,@F4500,clean\n"
      "C1,@F4600,1A2,0";
  ClockPoint whole;
  CHECK(scan(tail, true, &whole));
  for (size_t step = 1; step < 9; step++) {
    ClockTailScanner scanner(true);
    for (size_t at = 0; at < tail.size(); at += step)
      scanner.feed(tail.data() + at, tail.size() - at < step ? tail.size() - at : step);
    ClockPoint got;
    CHECK(scanner.end(&got));
    CHECK_EQ(got.t_us, whole.t_us);
    CHECK_EQ(got.utc_us, whole.utc_us);
    CHECK_EQ(got.basis, ClockBasis::DRIFT);
    CHECK_EQ(got.detail, 600ull);
  }
}

// C1 / C3 ----------------------------------------------------------------------------------------

namespace {
BootClockInputs boot(WakeKind wake, uint64_t now, uint64_t sys) {
  BootClockInputs in;
  in.wake = wake;
  in.now_boot_us = now;
  in.sys_utc_us = sys;
  in.build_utc_us = BUILD;
  return in;
}
}  // namespace

TEST(c3_a_cold_boot_states_the_card_floor_plus_uptime_and_nothing_else) {
  BootClockInputs in = boot(WakeKind::POWER_ON, 2000000, 2000000);  // 1970 + 2 s
  in.card_valid = true;
  in.card_end = point(0x500, T_2026, ClockBasis::UTC, ClockSrc::SYNC);
  in.card_seq = 41;
  const BootClockDecision d = decide_boot_clock(in);
  CHECK(!d.sys_rejected);  // a power-on reset is expected to have lost the clock
  CHECK_EQ(d.best.basis, ClockBasis::EST);
  CHECK_EQ(d.best.src, ClockSrc::CARD);
  CHECK_EQ(d.best.detail, 41ull);
  CHECK_EQ(d.best.t_us, 0ull);
  CHECK_EQ(d.best.utc_us, T_2026);
  // The file opens 3.5 s after power-up: its header says previous end + 3.5 s. That is the "+x".
  CHECK_EQ(clock_project(d.best, 3500000), T_2026 + 3500000);
}

TEST(c3_the_build_time_is_the_floor_under_an_older_card) {
  BootClockInputs in = boot(WakeKind::POWER_ON, 1, 1);
  in.card_valid = true;
  in.card_end = point(0, BUILD - 1, ClockBasis::EST, ClockSrc::BUILD);
  in.card_seq = 3;
  const BootClockDecision d = decide_boot_clock(in);
  CHECK_EQ(d.best.src, ClockSrc::BUILD);
  CHECK_EQ(d.best.utc_us, BUILD);
  CHECK(!d.best.has_detail);
}

TEST(c3_a_fresh_card_gets_the_build_time) {
  const BootClockDecision d = decide_boot_clock(boot(WakeKind::POWER_ON, 1, 1));
  CHECK_EQ(d.best.basis, ClockBasis::EST);
  CHECK_EQ(d.best.src, ClockSrc::BUILD);
  CHECK_EQ(d.best.utc_us, BUILD);
}

TEST(c3_no_build_time_and_no_card_states_nothing) {
  BootClockInputs in = boot(WakeKind::POWER_ON, 1, 1);
  in.build_utc_us = 0;
  CHECK(!decide_boot_clock(in).best.valid());
}

TEST(c1_a_warm_reset_keeps_a_plausible_clock_as_a_reading) {
  BootClockInputs in = boot(WakeKind::OTHER, 1500000, T_2026);
  in.card_valid = true;
  in.card_end = point(0x500, T_2026 - 60000000, ClockBasis::UTC, ClockSrc::SYNC);
  const BootClockDecision d = decide_boot_clock(in);
  CHECK_EQ(d.best.basis, ClockBasis::UTC);
  CHECK_EQ(d.best.src, ClockSrc::WARM);
  CHECK_EQ(d.best.t_us, 1500000ull);
  CHECK_EQ(d.best.utc_us, T_2026);
  CHECK(!d.best.has_detail);
  // The floor is still known, for C8.
  CHECK_EQ(d.floor.src, ClockSrc::CARD);
}

TEST(c1_a_clock_that_did_not_survive_falls_through_to_the_floor) {
  const BootClockDecision d = decide_boot_clock(boot(WakeKind::OTHER, 1500000, 1500000));
  CHECK(d.sys_rejected);
  CHECK_EQ(d.best.basis, ClockBasis::EST);
  CHECK_EQ(d.best.src, ClockSrc::BUILD);
}

TEST(c1_a_clock_before_the_build_is_not_a_clock) {
  const BootClockDecision d = decide_boot_clock(boot(WakeKind::OTHER, 1, BUILD - 1));
  CHECK(d.sys_rejected);
  CHECK_EQ(d.best.src, ClockSrc::BUILD);
}

TEST(c1_without_a_build_time_2024_is_the_minimum) {
  BootClockInputs in = boot(WakeKind::OTHER, 1, SD_LOG_CLOCK_MIN_EPOCH_US - 1);
  in.build_utc_us = 0;
  CHECK(decide_boot_clock(in).sys_rejected);
  in.sys_utc_us = SD_LOG_CLOCK_MIN_EPOCH_US;
  CHECK_EQ(decide_boot_clock(in).best.src, ClockSrc::WARM);
}

TEST(c1_a_clock_behind_the_card_floor_is_rejected_beyond_the_slack) {
  BootClockInputs in = boot(WakeKind::OTHER, 1000000, 0);
  in.card_valid = true;
  in.card_end = point(0, T_2026, ClockBasis::UTC, ClockSrc::SYNC);
  in.card_seq = 9;
  // floor at now = T_2026 + 1 s. Within 2 s slack: accepted. Beyond: rejected.
  in.sys_utc_us = T_2026 + 1000000 - 1999999;
  CHECK_EQ(decide_boot_clock(in).best.src, ClockSrc::WARM);
  in.sys_utc_us = T_2026 + 1000000 - 2000001;
  const BootClockDecision d = decide_boot_clock(in);
  CHECK(d.sys_rejected);
  CHECK_EQ(d.best.src, ClockSrc::CARD);
}

TEST(c1_a_deep_sleep_wake_is_drift_and_states_how_long_it_slept) {
  BootClockInputs in = boot(WakeKind::DEEP_SLEEP, 800000, T_2026 + 600800000);
  in.card_valid = true;
  in.card_end = point(0x900, T_2026, ClockBasis::UTC, ClockSrc::SYNC);
  in.card_seq = 41;
  const BootClockDecision d = decide_boot_clock(in);
  CHECK_EQ(d.best.basis, ClockBasis::DRIFT);
  CHECK_EQ(d.best.src, ClockSrc::SLEEP);
  CHECK(d.best.has_detail);
  CHECK_EQ(d.best.detail, 600ull);
  CHECK_EQ(clock_line(d.best), std::string("#clock,@C3500,") + [] {
             char b[32];
             std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(T_2026 + 600800000));
             return std::string(b);
           }() + ",drift,sleep,600\n");
}

TEST(c1_a_wake_without_a_card_clock_is_drift_without_a_duration) {
  const BootClockDecision d = decide_boot_clock(boot(WakeKind::DEEP_SLEEP, 1, T_2026));
  CHECK_EQ(d.best.basis, ClockBasis::DRIFT);
  CHECK(!d.best.has_detail);
}

TEST(c1_a_warm_reset_after_a_wake_keeps_the_drift_label) {
  // sync -> deep sleep -> wake (drift) -> crash: the clock that survives the crash is still the one
  // that ran on the RC through the sleep. Calling it `utc` would launder the drift.
  BootClockInputs in = boot(WakeKind::OTHER, 1, T_2026);
  in.sys_drifted = true;
  const BootClockDecision d = decide_boot_clock(in);
  CHECK_EQ(d.best.basis, ClockBasis::DRIFT);
  CHECK_EQ(d.best.src, ClockSrc::SLEEP);
  CHECK(!d.best.has_detail);
}

TEST(c1_a_power_on_never_trusts_the_system_clock) {
  // Whatever the RTC timer reads after a power-on, it was not kept.
  const BootClockDecision d = decide_boot_clock(boot(WakeKind::POWER_ON, 1, T_2026));
  CHECK_EQ(d.best.src, ClockSrc::BUILD);
  CHECK(!d.sys_rejected);
}

// C8 / restatement -------------------------------------------------------------------------------

TEST(c8_a_sync_behind_the_floor_is_flagged) {
  const ClockPoint floor = point(0, T_2026, ClockBasis::EST, ClockSrc::CARD);
  CHECK(!clock_contradicts_floor(floor, T_2026 + 5000000, 5000000));
  CHECK(!clock_contradicts_floor(floor, T_2026 + 5000000 - 2000000, 5000000));
  CHECK(clock_contradicts_floor(floor, T_2026 + 5000000 - 2000001, 5000000));
  CHECK(!clock_contradicts_floor(ClockPoint{}, 0, 5000000));
}

TEST(restatement_is_owed_by_bytes_or_by_time) {
  CHECK(!clock_restate_due(SD_LOG_CLOCK_RESTATE_BYTES - 1, 1000, 0));
  CHECK(clock_restate_due(SD_LOG_CLOCK_RESTATE_BYTES, 1000, 0));
  CHECK(clock_restate_due(0, SD_LOG_CLOCK_RESTATE_US, 0));
  CHECK(!clock_restate_due(0, SD_LOG_CLOCK_RESTATE_US - 1, 0));
  CHECK(!clock_restate_due(0, 5, 10));  // a clock that went backwards owes nothing
}
