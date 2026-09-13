// Per-group polling scheduler (components/uds/uds_sched.h) against docs/DESIGN-uds.md §4/§5:
// most-overdue selection under a wrapped millisecond counter, one request in flight, the exact
// backoff schedule (2x, 4x, ... the interval, capped at 60 s), reset on success, the
// publish-unavailable threshold, and NRC-0x31 suspension.

#include "uds_sched.h"
#include "harness.h"

#include <cstdint>
#include <string>

using namespace esphome::uds;

namespace {
/// A stand-in for the catalog's Ecu::p2_ms. Long enough that no case below trips the deadline by
/// accident; the deadline cases state their own timing explicitly.
constexpr uint32_t P2 = 500;
}  // namespace

TEST(uds_sched_add_capacity_and_horizon) {
  PollEntry table[3];
  GroupScheduler s;
  s.init(table, 3, 3);
  CHECK_EQ(s.capacity(), 3);
  CHECK(s.add(10, 1000, 0));
  CHECK(s.add(20, 2000, 0));
  CHECK(s.add(30, 3000, 0));
  CHECK_EQ(s.size(), 3);
  CHECK(!s.add(40, 4000, 0));  // table full
  // An interval at the wrap-safety horizon would make "overdue" ambiguous; refused up front.
  PollEntry table2[1];
  GroupScheduler s2;
  s2.init(table2, 1, 3);
  CHECK(!s2.add(1, 0x80000000u, 0));
  CHECK(s2.add(1, 0x7FFFFFFFu, 0));
}

TEST(uds_sched_due_ordering_across_the_counter_wrap) {
  PollEntry table[2];
  GroupScheduler s;
  s.init(table, 2, 3);
  const uint32_t t0 = 0xFFFFFF00u;  // 256 ms before the 49.7-day wrap
  CHECK(s.add(10, 1000, t0));
  CHECK(s.add(20, 400, t0));
  // Both due immediately on add; equal overdue resolves to the first slot.
  CHECK_EQ(s.next(t0, P2), 0);
  s.on_success(0, t0);  // next due 0x2E8, past the wrap
  CHECK_EQ(s.next(t0, P2), 1);
  s.on_success(1, t0);  // next due 0x90, past the wrap
  // 16 ms before the wrap nothing is due — a naive `now >= due` comparison would fire for both,
  // because both due times wrapped to small numbers.
  CHECK_EQ(s.next(0xFFFFFFF0u, P2), SCHED_NONE);
  // Exactly at its wrapped due time, the shorter interval fires.
  CHECK_EQ(s.next(0x90u, P2), 1);
  s.on_success(1, 0x90u);  // next due 0x220
  // Past both dues, the most overdue wins: slot 1 (due 0x220) over slot 0 (due 0x2E8).
  CHECK_EQ(s.next(0x800u, P2), 1);
}

TEST(uds_sched_most_overdue_wins) {
  PollEntry table[3];
  GroupScheduler s;
  s.init(table, 3, 3);
  CHECK(s.add(10, 100, 0));
  CHECK(s.add(20, 300, 0));
  CHECK(s.add(30, 200, 0));
  // Clear the initial immediately-due state.
  CHECK_EQ(s.next(0, P2), 0);
  s.on_success(0, 0);
  CHECK_EQ(s.next(0, P2), 1);
  s.on_success(1, 0);
  CHECK_EQ(s.next(0, P2), 2);
  s.on_success(2, 0);
  CHECK_EQ(s.next(0, P2), SCHED_NONE);
  // Dues now 100, 300, 200. At t=350 the overdues are 250, 50, 150: slot 0 first, not merely
  // "the first slot found due".
  CHECK_EQ(s.next(350, P2), 0);
  s.on_success(0, 350);  // slot 0 due again at 450
  CHECK_EQ(s.next(350, P2), 2);
  s.on_success(2, 350);
  CHECK_EQ(s.next(350, P2), 1);
  s.on_success(1, 350);
}

TEST(uds_sched_one_in_flight) {
  PollEntry table[2];
  GroupScheduler s;
  s.init(table, 2, 3);
  CHECK(s.add(10, 100, 0));
  CHECK(s.add(20, 100, 0));
  const int16_t first = s.next(0, P2);
  CHECK_EQ(first, 0);
  CHECK_EQ(s.in_flight(), 0);
  // The other group is due too, but ISO-TP has no multiplexing: nothing until the outcome lands.
  CHECK_EQ(s.next(0, P2), SCHED_NONE);
  CHECK_EQ(s.next(1000, P2), SCHED_NONE);
  s.on_failure(0, 1000);
  CHECK_EQ(s.in_flight(), SCHED_NONE);
  CHECK_EQ(s.next(1000, P2), 1);
  s.on_success(1, 1000);
  CHECK_EQ(s.in_flight(), SCHED_NONE);
}

TEST(uds_sched_exact_backoff_schedule) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 0xFF);  // threshold out of the way; this case is about the delays
  CHECK(s.add(7, 1000, 0));
  CHECK_EQ(s.next(0, P2), 0);
  // Doubling from the group interval, capped at 60 s: 2000, 4000, ..., 32000, then the cap.
  const uint32_t want[] = {2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
  uint32_t t = 0;
  for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
    s.on_failure(0, t);
    CHECK_EQ_MSG(s.entry(0).backoff_ms, want[i], "failure " + std::to_string(i + 1));
    CHECK_EQ_MSG(s.entry(0).next_due_ms, t + want[i], "failure " + std::to_string(i + 1));
    // Not due one tick early, due exactly on time (and marked in flight again for the next round).
    CHECK_EQ_MSG(s.next(t + want[i] - 1, P2), SCHED_NONE, "failure " + std::to_string(i + 1));
    t += want[i];
    CHECK_EQ_MSG(s.next(t, P2), 0, "failure " + std::to_string(i + 1));
  }
}

TEST(uds_sched_backoff_resets_on_success) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 0xFF);
  CHECK(s.add(7, 1000, 0));
  CHECK_EQ(s.next(0, P2), 0);
  s.on_failure(0, 0);
  CHECK_EQ(s.next(2000, P2), 0);
  s.on_failure(0, 2000);
  CHECK_EQ(s.entry(0).backoff_ms, 4000u);
  CHECK_EQ(s.next(6000, P2), 0);
  s.on_success(0, 6000);
  CHECK_EQ(s.entry(0).backoff_ms, 0u);
  CHECK_EQ(s.entry(0).consecutive_failures, 0);
  CHECK_EQ(s.entry(0).next_due_ms, 7000u);  // one plain interval out, not a decayed backoff
  // The next failure starts the schedule over at 2x interval.
  CHECK_EQ(s.next(7000, P2), 0);
  s.on_failure(0, 7000);
  CHECK_EQ(s.entry(0).backoff_ms, 2000u);
}

TEST(uds_sched_failure_threshold_publishes_unavailable_once) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 3);
  CHECK(s.add(7, 1000, 0));
  CHECK(!s.is_unavailable(0));
  CHECK(!s.on_failure(0, 0));  // 1st
  CHECK(!s.on_failure(0, 0));  // 2nd
  CHECK(!s.is_unavailable(0));
  CHECK(s.on_failure(0, 0));  // 3rd crosses the threshold: publish NaN now, exactly once
  CHECK(s.is_unavailable(0));
  CHECK(!s.on_failure(0, 0));  // already unavailable: no second publish
  CHECK(!s.on_failure(0, 0));
  // Polling never stopped; the first success recovers and says so, exactly once.
  const uint32_t due = s.entry(0).next_due_ms;
  CHECK_EQ(s.next(due, P2), 0);
  CHECK(s.on_success(0, due));
  CHECK(!s.is_unavailable(0));
  CHECK(!s.on_success(0, due));
}

TEST(uds_sched_suspend_stops_polling_permanently) {
  PollEntry table[2];
  GroupScheduler s;
  s.init(table, 2, 3);
  CHECK(s.add(10, 100, 0));
  CHECK(s.add(20, 100, 0));
  // NRC 0x31 arrives for slot 0 while it is in flight.
  CHECK_EQ(s.next(0, P2), 0);
  s.suspend(0);
  CHECK_EQ(s.in_flight(), SCHED_NONE);
  CHECK(s.is_suspended(0));
  // However overdue slot 0 becomes, only slot 1 is ever polled again.
  CHECK_EQ(s.next(0, P2), 1);
  s.on_success(1, 0);
  CHECK_EQ(s.next(1000000, P2), 1);
  s.on_success(1, 1000000);
}

TEST(uds_sched_zero_interval_backoff_still_escapes) {
  // interval 0 means "every loop"; the backoff seed is floored at 1 ms so failures still slow it
  // down instead of retrying at full rate forever.
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 0xFF);
  CHECK(s.add(1, 0, 0));
  CHECK_EQ(s.next(0, P2), 0);
  s.on_failure(0, 0);
  CHECK_EQ(s.entry(0).backoff_ms, 2u);
  CHECK_EQ(s.next(1, P2), SCHED_NONE);
  CHECK_EQ(s.next(2, P2), 0);
  s.on_failure(0, 2);
  CHECK_EQ(s.entry(0).backoff_ms, 4u);
}

TEST(uds_sched_out_of_range_slots_are_harmless) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 3);
  CHECK(s.add(1, 100, 0));
  CHECK(!s.on_success(7, 0));
  CHECK(!s.on_failure(7, 0));
  s.suspend(7);
  CHECK(!s.is_suspended(7));
  CHECK(!s.is_unavailable(7));
  CHECK_EQ(s.next(0, P2), 0);  // the real entry is untouched
}

// ---------------------------------------------------------------------------
// Releasing an in-flight slot, and the response deadline the caller cannot forget
// ---------------------------------------------------------------------------

TEST(uds_sched_cancel_in_flight_is_not_a_failure) {
  // A send the transport refused is backpressure, not an answer the ECU failed to give: the same
  // group must come back on the next pass with its backoff and failure count untouched.
  PollEntry table[2];
  GroupScheduler s;
  s.init(table, 2, 3);
  CHECK(s.add(10, 1000, 0));
  CHECK(s.add(20, 5000, 0));
  CHECK_EQ(s.next(0, P2), 0);
  const uint32_t due_before = s.entry(0).next_due_ms;
  s.cancel_in_flight();
  CHECK_EQ(s.in_flight(), SCHED_NONE);
  CHECK_EQ(s.next(0, P2), 0);  // the same slot, immediately
  CHECK_EQ(s.entry(0).consecutive_failures, 0);
  CHECK_EQ(s.entry(0).backoff_ms, 0u);
  CHECK_EQ(s.entry(0).next_due_ms, due_before);
  CHECK(!s.is_unavailable(0));
  // And a cancelled slot does not carry a deadline into the next selection.
  s.cancel_in_flight();
  CHECK(!s.deadline_pending());
  CHECK_EQ(s.poll(1000000).slot, SCHED_NONE);
}

TEST(uds_sched_cancel_with_nothing_in_flight_is_harmless) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 3);
  CHECK(s.add(10, 1000, 0));
  s.cancel_in_flight();
  CHECK_EQ(s.in_flight(), SCHED_NONE);
  CHECK_EQ(s.next(0, P2), 0);
}

TEST(uds_sched_silent_ecu_fails_on_its_own_deadline) {
  // The case that produces no IsoTpError at all: isotp's timers only run during a transfer, so an
  // ECU that never answers yields silence. poll() must fail it, or in_flight_ stays set forever
  // and every group stops being polled.
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 3);
  CHECK(s.add(10, 1000, 0));
  CHECK_EQ(s.next(100, P2), 0);
  CHECK(s.deadline_pending());
  CHECK_EQ(s.deadline_ms(), 600u);
  CHECK_EQ(s.poll(599).slot, SCHED_NONE);  // not yet
  const SchedTimeout t = s.poll(600);
  CHECK_EQ(t.slot, 0);
  CHECK(!t.publish_unavailable);  // 1 of 3 failures
  CHECK_EQ(s.in_flight(), SCHED_NONE);
  CHECK_EQ(s.entry(0).consecutive_failures, 1);
  CHECK_EQ(s.entry(0).backoff_ms, 2000u);  // the ordinary failure path, not a special case
  CHECK_EQ(s.poll(600).slot, SCHED_NONE);  // nothing in flight: reported once
}

TEST(uds_sched_deadline_expiry_reaches_the_unavailable_threshold) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 2);
  CHECK(s.add(10, 1000, 0));
  uint32_t t = 0;
  CHECK_EQ(s.next(t, P2), 0);
  CHECK(!s.poll(t + P2).publish_unavailable);
  t = s.entry(0).next_due_ms;
  CHECK_EQ(s.next(t, P2), 0);
  const SchedTimeout second = s.poll(t + P2);
  CHECK_EQ(second.slot, 0);
  CHECK(second.publish_unavailable);  // the threshold crossing, signalled exactly once
  CHECK(s.is_unavailable(0));
}

TEST(uds_sched_one_silent_group_does_not_stop_the_others) {
  // The failure mode the deadline exists to prevent: with in-flight global, a wedged slot would
  // stop polling for every group. Here the silent one backs off and the healthy one keeps going.
  PollEntry table[2];
  GroupScheduler s;
  s.init(table, 2, 3);
  CHECK(s.add(10, 1000, 0));  // the silent ECU
  CHECK(s.add(20, 1000, 0));  // the one that answers
  CHECK_EQ(s.next(0, P2), 0);
  CHECK_EQ(s.next(0, P2), SCHED_NONE);  // blocked while slot 0 is in flight
  CHECK_EQ(s.poll(P2).slot, 0);
  CHECK_EQ(s.next(P2, P2), 1);  // the healthy group gets its turn as soon as the deadline lands
  CHECK(!s.on_success(1, P2));
  CHECK_EQ(s.entry(1).consecutive_failures, 0);
}

TEST(uds_sched_extend_deadline_on_response_pending) {
  // NRC 0x78: the ECU is working on it, so extend to p2_ext_ms and keep waiting — the request must
  // not be counted as failed while the answer is still coming.
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 3);
  CHECK(s.add(10, 1000, 0));
  CHECK_EQ(s.next(0, P2), 0);
  CHECK_EQ(s.deadline_ms(), 500u);
  s.extend_deadline(400, 5000);  // 0x78 arrived at t=400
  CHECK_EQ(s.deadline_ms(), 5400u);
  CHECK_EQ(s.poll(500).slot, SCHED_NONE);  // the original deadline no longer applies
  CHECK_EQ(s.poll(5399).slot, SCHED_NONE);
  CHECK_EQ(s.in_flight(), 0);  // still ours, still waiting
  CHECK_EQ(s.poll(5400).slot, 0);
}

TEST(uds_sched_arm_deadline_rebases_from_end_of_transmission) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 3);
  CHECK(s.add(10, 1000, 0));
  CHECK_EQ(s.next(0, P2), 0);
  s.arm_deadline(32, P2);  // two loop passes later, the request is actually on the wire
  CHECK_EQ(s.deadline_ms(), 532u);
  CHECK_EQ(s.poll(531).slot, SCHED_NONE);
  CHECK_EQ(s.poll(532).slot, 0);
  // With nothing in flight, arming is a no-op rather than a deadline nobody owns.
  s.arm_deadline(1000, P2);
  CHECK(!s.deadline_pending());
  CHECK_EQ(s.poll(2000).slot, SCHED_NONE);
}

TEST(uds_sched_deadline_survives_the_counter_wrap) {
  PollEntry table[1];
  GroupScheduler s;
  s.init(table, 1, 3);
  const uint32_t t0 = 0xFFFFFF00u;  // 256 ms before the wrap, P2 lands past it
  CHECK(s.add(10, 1000, t0));
  CHECK_EQ(s.next(t0, P2), 0);
  CHECK_EQ(s.deadline_ms(), 0xF4u);  // wrapped
  CHECK_EQ(s.poll(0xFFFFFFF0u).slot, SCHED_NONE);
  CHECK_EQ(s.poll(0xF3u).slot, SCHED_NONE);
  CHECK_EQ(s.poll(0xF4u).slot, 0);
}

TEST(uds_sched_success_and_suspend_abandon_the_deadline) {
  PollEntry table[2];
  GroupScheduler s;
  s.init(table, 2, 3);
  CHECK(s.add(10, 1000, 0));
  CHECK(s.add(20, 1000, 0));
  CHECK_EQ(s.next(0, P2), 0);
  s.on_success(0, 10);
  CHECK(!s.deadline_pending());
  CHECK_EQ(s.poll(1000000).slot, SCHED_NONE);  // no stale deadline to fire
  CHECK_EQ(s.next(10, P2), 1);
  s.suspend(1);
  CHECK(!s.deadline_pending());
  CHECK_EQ(s.poll(1000000).slot, SCHED_NONE);
}
