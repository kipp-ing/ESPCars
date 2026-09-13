// TapRecord -> LogRecord translation (components/sd_logger/tap_translate.h).
//
// Why this is a host test and not a bench check: a wrong flag bit here produces
// a log file that is perfectly well-formed and quietly wrong. A hardware run
// sees `dropped == 0` and a plausible CSV and passes. The specific hazard:
//
//     can_gateway::TAP_FLAG_SHED == 0x04       "received but not forwarded"
//     sd_logger::REC_FLAG_TX     == 0x04       "this node transmitted it"
//
// Same bit, opposite meanings — which is why the two records are translated
// field by field and never memcpy'd, and why the first case below asserts the
// collision still exists rather than assuming someone will remember it.

#include "harness.h"
#include "tap_translate.h"

#include <cstdint>

using esphome::can_gateway::MAX_FRAME_DATA_LEN;
using esphome::can_gateway::TAP_FLAG_EXTENDED;
using esphome::can_gateway::TAP_FLAG_RTR;
using esphome::can_gateway::TAP_FLAG_SHED;
using esphome::can_gateway::TapRecord;
using esphome::sd_logger::LogRecord;
using esphome::sd_logger::REC_FLAG_EXTENDED;
using esphome::sd_logger::REC_FLAG_RTR;
using esphome::sd_logger::REC_FLAG_SHED;
using esphome::sd_logger::REC_FLAG_TRUNCATED;
using esphome::sd_logger::REC_FLAG_TX;
using esphome::sd_logger::tap_to_log_record;

namespace {

TapRecord tap_with(uint8_t flags, uint8_t dlc = 8) {
  TapRecord tap{};
  tap.t_us = 0x12345678u;
  tap.can_id = 0x1ABCDEFu;
  tap.dlc = dlc;
  tap.flags = flags;
  tap.source = 7;  // the gateway's port index — must NOT reach the log record
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    tap.data[i] = static_cast<uint8_t>(0xF0u + i);
  return tap;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(tap_and_record_flags_still_collide_on_bit_two) {
  // Not a tautology: this is the whole reason the translation exists. If a
  // future edit ever makes these equal in meaning, the memcpy shortcut becomes
  // tempting again — and this case is what says no.
  CHECK_EQ(TAP_FLAG_SHED, 0x04);
  CHECK_EQ(REC_FLAG_TX, 0x04);
  CHECK_EQ(REC_FLAG_SHED, 0x08);
}

TEST(translate_copies_the_plain_fields) {
  LogRecord rec{};
  tap_to_log_record(tap_with(0), 3, rec);
  CHECK_EQ(rec.t_us, 0x12345678u);
  CHECK_EQ(rec.id, 0x1ABCDEFu);
  CHECK_EQ(rec.len, 8);
  for (uint8_t i = 0; i < 8; i++)
    CHECK_EQ(rec.data[i], static_cast<uint8_t>(0xF0u + i));
}

TEST(translate_uses_the_loggers_source_tag_not_the_port_index) {
  // The tap carries the gateway's port index; the log carries whatever tag the
  // user gave that port in `can_ports:`. Conflating them makes two segments
  // indistinguishable in a file that looks fine.
  LogRecord rec{};
  tap_to_log_record(tap_with(0), 3, rec);
  CHECK_EQ(rec.source, 3);
}

TEST(translate_maps_shed_to_its_own_bit_never_to_tx) {
  LogRecord rec{};
  tap_to_log_record(tap_with(TAP_FLAG_SHED), 1, rec);
  CHECK((rec.flags & REC_FLAG_SHED) != 0);
  // The bug this whole header exists to prevent: a shed frame arriving in the
  // log labelled "transmitted by this node".
  CHECK((rec.flags & REC_FLAG_TX) == 0);
}

TEST(translate_maps_extended_and_rtr) {
  LogRecord rec{};
  tap_to_log_record(tap_with(TAP_FLAG_EXTENDED), 1, rec);
  CHECK((rec.flags & REC_FLAG_EXTENDED) != 0);
  CHECK((rec.flags & REC_FLAG_RTR) == 0);

  tap_to_log_record(tap_with(TAP_FLAG_RTR), 1, rec);
  CHECK((rec.flags & REC_FLAG_RTR) != 0);
  CHECK((rec.flags & REC_FLAG_EXTENDED) == 0);
}

TEST(translate_maps_all_three_flags_at_once) {
  LogRecord rec{};
  tap_to_log_record(tap_with(TAP_FLAG_EXTENDED | TAP_FLAG_RTR | TAP_FLAG_SHED), 1, rec);
  CHECK_EQ(rec.flags, static_cast<uint8_t>(REC_FLAG_EXTENDED | REC_FLAG_RTR | REC_FLAG_SHED));
}

TEST(translate_never_invents_tx_or_truncated) {
  // Neither can be true of a received frame, whatever the tap's flag byte says.
  LogRecord rec{};
  tap_to_log_record(tap_with(0xFF), 1, rec);
  CHECK((rec.flags & REC_FLAG_TX) == 0);
  CHECK((rec.flags & REC_FLAG_TRUNCATED) == 0);
}

TEST(translate_clears_stale_flags_from_a_reused_record) {
  // drain_can_taps_() reuses one LogRecord for a whole ring, so a translation
  // that OR'd into `flags` would smear one frame's flags across the rest.
  LogRecord rec{};
  tap_to_log_record(tap_with(TAP_FLAG_SHED | TAP_FLAG_EXTENDED), 1, rec);
  tap_to_log_record(tap_with(0), 1, rec);
  CHECK_EQ(rec.flags, 0);
}

TEST(translate_clamps_an_out_of_range_dlc) {
  // The ISR should never produce dlc > 8, but the writer indexes data[] with
  // this and the record's own array is only eight bytes.
  LogRecord rec{};
  tap_to_log_record(tap_with(0, /*dlc=*/15), 1, rec);
  CHECK_EQ(rec.len, 8);
}

TEST(translate_keeps_a_zero_length_frame_zero_length) {
  LogRecord rec{};
  tap_to_log_record(tap_with(TAP_FLAG_RTR, /*dlc=*/0), 1, rec);
  CHECK_EQ(rec.len, 0);
}
