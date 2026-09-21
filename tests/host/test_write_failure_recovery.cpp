// The write-failure split in sd_logger is deliberately tiny and host-tested:
// recovery may retry a card that was actually asked something, but it must stop
// when write() failed before the SD layer saw a single command.

#include "harness.h"
#include "sd_diagnostics.h"

using esphome::sd_logger::classify_write_failure;
using esphome::sd_logger::SdSpiTraceEntry;
using esphome::sd_logger::SdSpiTraceRing;
using esphome::sd_logger::WriteFailureAction;

TEST(write_failure_without_spi_traffic_stops_recovery) {
  CHECK(classify_write_failure(0) == WriteFailureAction::STOP_NO_BUS_TRAFFIC);
}

TEST(write_failure_with_any_spi_traffic_keeps_the_ladder) {
  CHECK(classify_write_failure(1) == WriteFailureAction::RETRY_RECOVERY);
  CHECK(classify_write_failure(3) == WriteFailureAction::RETRY_RECOVERY);
}

TEST(write_failure_trace_summary_pins_the_failing_span) {
  SdSpiTraceRing trace;

  SdSpiTraceEntry cmd13{};
  cmd13.opcode = 13;
  cmd13.err = 0;
  cmd13.response_raw = 0x00000101u;
  cmd13.response = 0x00000100u;
  trace.append(cmd13);

  SdSpiTraceEntry cmd24{};
  cmd24.opcode = 24;
  cmd24.err = -123;
  trace.append(cmd24);

  const auto summary = trace.summarize(0, trace.write_index());
  CHECK_EQ(summary.commands, 2u);
  CHECK_EQ(summary.available, 2u);
  CHECK_EQ(summary.missing, 0u);
  CHECK_EQ(summary.worst_err, -123);

  const SdSpiTraceEntry first = trace.read(0);
  CHECK_EQ(first.response_raw, 0x00000101u);
  CHECK_EQ(first.response, 0x00000100u);
}
