// Writer task stack sizing + the headroom classifier (CONTRACT-sdlog-writer-stack.md, session 41).
//
// Contract under test (components/sd_logger/sd_diagnostics.h):
//   - SD_WRITER_STACK_BYTES (8192) and SD_WRITER_STACK_MEASURED_NEED_BYTES (4816) are not tuning
//     knobs: they are the entire justification for raising the stack out of the crash loop
//     (CONTRACT §0-1). A silent edit to either number must fail loudly here, not sail through
//     because nothing pinned it.
//   - classify_stack_headroom() turns a uxTaskGetStackHighWaterMark() reading (bytes, on this
//     port's StackType_t == uint8_t) into OK/LOW/CRITICAL. Every boundary the contract pins
//     (§2.1: warn=2048, critical=1024) is checked exactly, both sides.
//   - the static_assert in the header proves the budget clears its own worst case at compile
//     time; the same inequality is re-asserted here at runtime so a violation names itself in
//     test output instead of only breaking the build (contract §5).
//
// What this file deliberately does NOT test: SdLogger::sample_writer_stack_() itself. It calls
// uxTaskGetStackHighWaterMark(), which is FreeRTOS-only and has no host-reachable equivalent;
// sd_diagnostics.h is the freestanding half of this contract and the only half a host binary can
// exercise. The call-site cadence rule (§2.4: exactly two call sites, never every writer pass) is
// a grep-shaped property of sd_logger.cpp, not of this header, and is out of scope for this file.

#include "harness.h"
#include "sd_diagnostics.h"

#include <cstdint>
#include <cstring>

using esphome::sd_logger::classify_stack_headroom;
using esphome::sd_logger::stack_headroom_str;
using esphome::sd_logger::StackHeadroom;
using esphome::sd_logger::SD_WRITER_STACK_BYTES;
using esphome::sd_logger::SD_WRITER_STACK_MEASURED_NEED_BYTES;
using esphome::sd_logger::SD_WRITER_STACK_WARN_FREE_BYTES;

// Re-affirmed locally (not just via the header's own static_assert) so that if the header's copy
// is ever deleted, this translation unit still refuses to build rather than silently losing the
// guarantee. This is a compile-time echo of the runtime CHECK in
// writer_stack_budget_clears_measured_need_plus_warn_margin below, not a replacement for it: the
// contract explicitly wants the failure to name itself in test *output*, which only the runtime
// form does.
static_assert(SD_WRITER_STACK_BYTES >= SD_WRITER_STACK_MEASURED_NEED_BYTES + SD_WRITER_STACK_WARN_FREE_BYTES,
              "writer stack must clear its measured worst case by the warn margin (test-local echo)");

// --------------------------------------------------------------------------- the pinned numbers

TEST(writer_stack_pinned_constants_are_the_measured_ones) {
  // Deliberate change-detector: these two numbers ARE the argument in CONTRACT §1. A silent edit
  // to either — a "round it to 4096" or a "bump to 5000 to be safe" slipped in beside unrelated
  // work — must fail loudly and send whoever did it back to §1 rather than passing quietly.
  CHECK_EQ(SD_WRITER_STACK_BYTES, 8192u);
  CHECK_EQ(SD_WRITER_STACK_MEASURED_NEED_BYTES, 4816u);
  // Not explicitly demanded by the brief, but it is the third number the inequality below is
  // built from, and the boundary table already pins its value twice more (via
  // classify_stack_headroom(2047)==LOW vs (2048)==OK) — naming it directly here means a failure
  // there points straight at the constant instead of forcing a re-derivation from the boundary.
  CHECK_EQ(SD_WRITER_STACK_WARN_FREE_BYTES, 2048u);
}

TEST(writer_stack_budget_clears_measured_need_plus_warn_margin) {
  // The same inequality the header's static_assert enforces at compile time, run again at
  // runtime so a violation names itself in `make -C tests/host` output (contract §5) instead of
  // only breaking the build — useful the day someone reads a compile error about a static_assert
  // deep in a header they didn't touch and cannot immediately tell which of the three constants
  // moved.
  CHECK(SD_WRITER_STACK_BYTES >= SD_WRITER_STACK_MEASURED_NEED_BYTES + SD_WRITER_STACK_WARN_FREE_BYTES);
  // The margin claimed in CONTRACT §1.1: 8192 - 4816 = 3376 B / ~41% over the measured worst case.
  CHECK_EQ(SD_WRITER_STACK_BYTES - SD_WRITER_STACK_MEASURED_NEED_BYTES, 3376u);
}

// -------------------------------------------------------------------- classify_stack_headroom()

TEST(classify_stack_headroom_every_pinned_boundary) {
  // Exact boundaries per CONTRACT §2.1's own callout, both sides of each edge:
  //   critical/low edge at WARN_FREE_BYTES/2 == 1024 (1023 CRITICAL, 1024 LOW)
  //   low/ok edge at WARN_FREE_BYTES == 2048 (2047 LOW, 2048 OK)
  // plus 0 (must be CRITICAL, never OK — a reading that hasn't happened yet is not "fine") and
  // UINT32_MAX (the top of the type, must be OK).
  CHECK_EQ_MSG(classify_stack_headroom(0), StackHeadroom::CRITICAL, "free=0");
  CHECK_EQ_MSG(classify_stack_headroom(1023), StackHeadroom::CRITICAL, "free=1023");
  CHECK_EQ_MSG(classify_stack_headroom(1024), StackHeadroom::LOW, "free=1024");
  CHECK_EQ_MSG(classify_stack_headroom(2047), StackHeadroom::LOW, "free=2047");
  CHECK_EQ_MSG(classify_stack_headroom(2048), StackHeadroom::OK, "free=2048");
  CHECK_EQ_MSG(classify_stack_headroom(0xFFFFFFFFu), StackHeadroom::OK, "free=UINT32_MAX");
}

TEST(classify_stack_headroom_midrange_is_not_a_lookup_table) {
  // Not in the contract's boundary list, but cheap and worth having: an implementation that
  // special-cases exactly {0, 1023, 1024, 2047, 2048} and gets everything else wrong would still
  // pass the boundary test above. These sit clear of every edge on purpose.
  CHECK_EQ_MSG(classify_stack_headroom(1), StackHeadroom::CRITICAL, "free=1");
  CHECK_EQ_MSG(classify_stack_headroom(500), StackHeadroom::CRITICAL, "free=500");
  CHECK_EQ_MSG(classify_stack_headroom(1500), StackHeadroom::LOW, "free=1500");
  CHECK_EQ_MSG(classify_stack_headroom(4096), StackHeadroom::OK, "free=4096");
  CHECK_EQ_MSG(classify_stack_headroom(8192), StackHeadroom::OK, "free=8192");
}

TEST(classify_stack_headroom_enum_values_are_stable) {
  // The header pins OK=0, LOW=1, CRITICAL=2 explicitly (not just "some three-way enum"). Anything
  // that logs or serialises the numeric value — and StackHeadroom is uint8_t-backed specifically
  // so it CAN be — depends on these not moving under a refactor that reorders the enumerators.
  CHECK_EQ(static_cast<uint8_t>(StackHeadroom::OK), 0u);
  CHECK_EQ(static_cast<uint8_t>(StackHeadroom::LOW), 1u);
  CHECK_EQ(static_cast<uint8_t>(StackHeadroom::CRITICAL), 2u);
}

// ------------------------------------------------------------------------------ stack_headroom_str()

TEST(stack_headroom_str_all_named_values) {
  CHECK(std::strcmp(stack_headroom_str(StackHeadroom::OK), "ok") == 0);
  CHECK(std::strcmp(stack_headroom_str(StackHeadroom::LOW), "low") == 0);
  CHECK(std::strcmp(stack_headroom_str(StackHeadroom::CRITICAL), "critical") == 0);
}

TEST(stack_headroom_str_out_of_range_value_falls_back_to_ok) {
  // StackHeadroom is uint8_t-backed with no UB in constructing an out-of-range value (every value
  // 0-255 is representable in the underlying type), so this exercises the switch's `default:`
  // arm exactly as ESP_LOGD/ESP_LOGW in sample_writer_stack_() would see it if the enum were ever
  // widened without updating this function. The header falls default through to the OK string
  // rather than returning nullptr or an "unknown" placeholder — pin that choice, since a caller
  // that printf-%s's the result would otherwise crash on nullptr.
  const StackHeadroom bogus = static_cast<StackHeadroom>(200);
  CHECK(std::strcmp(stack_headroom_str(bogus), "ok") == 0);
}
