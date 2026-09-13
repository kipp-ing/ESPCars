// RecoveryBackoff: the bus-off recovery schedule.
//
// Constants are asserted against the header rather than assumed, then the
// schedule is exercised through the state machine the component's loop() drives.
//
// Contract under test (gateway_core.h RecoveryBackoff):
//   - delays double from INITIAL_DELAY_MS and saturate at MAX_DELAY_MS
//   - should_attempt() is true exactly once per scheduled attempt, when due
//   - on_recovered() KEEPS the escalated delay (resetting on every recovery
//     would make a permanently dead bus cycle recover->bus-off at ~5 Hz)
//   - the schedule only resets after STABLE_RESET_MS of uninterrupted health,
//     and a bus-off inside that window voids the measurement
//   - the due-time comparison is millis()-wrap safe

#include "gateway_core.h"
#include "harness.h"

#include <cstdint>

using namespace esphome::can_gateway;

TEST(backoff_constants_match_the_documented_schedule) {
  // If these change, every expectation below changes with them — this case
  // names the change instead of leaving the others mysteriously red.
  CHECK_EQ(RecoveryBackoff::INITIAL_DELAY_MS, 100u);
  CHECK_EQ(RecoveryBackoff::MAX_DELAY_MS, 3200u);
  CHECK_EQ(RecoveryBackoff::STABLE_RESET_MS, 10000u);

  RecoveryBackoff backoff;
  CHECK_EQ(backoff.current_delay_ms(), RecoveryBackoff::INITIAL_DELAY_MS);
  CHECK_EQ(backoff.pending(), false);
}

TEST(backoff_doubles_then_saturates_at_the_cap) {
  RecoveryBackoff backoff;
  const uint32_t expected[] = {100, 200, 400, 800, 1600, 3200, 3200, 3200, 3200};

  uint32_t now = 1000;
  for (uint32_t want : expected) {
    CHECK_EQ(backoff.current_delay_ms(), want);
    backoff.on_bus_off(now);
    CHECK_EQ(backoff.pending(), true);
    // Not due one tick early ...
    CHECK_EQ(backoff.should_attempt(now + want - 1), false);
    CHECK_EQ(backoff.pending(), true);
    // ... due exactly on time.
    CHECK_EQ(backoff.should_attempt(now + want), true);
    now += want + 1;
  }
  CHECK_EQ(backoff.current_delay_ms(), RecoveryBackoff::MAX_DELAY_MS);
}

TEST(backoff_should_attempt_fires_exactly_once) {
  RecoveryBackoff backoff;
  backoff.on_bus_off(0);
  CHECK_EQ(backoff.should_attempt(100), true);
  CHECK_EQ(backoff.pending(), false);
  // The caller polls loop() at ~16 ms; the attempt must not repeat.
  CHECK_EQ(backoff.should_attempt(100), false);
  CHECK_EQ(backoff.should_attempt(116), false);
  CHECK_EQ(backoff.should_attempt(5000), false);
}

TEST(backoff_should_attempt_is_false_without_a_bus_off) {
  RecoveryBackoff backoff;
  CHECK_EQ(backoff.should_attempt(0), false);
  CHECK_EQ(backoff.should_attempt(1000000), false);
  CHECK_EQ(backoff.current_delay_ms(), 100u);  // and nothing escalated
}

TEST(backoff_late_poll_still_fires) {
  // A loop() stalled past the due time must still get its single attempt.
  RecoveryBackoff backoff;
  backoff.on_bus_off(1000);
  CHECK_EQ(backoff.should_attempt(9999), true);
  CHECK_EQ(backoff.current_delay_ms(), 200u);
}

TEST(backoff_repeated_bus_off_reschedules_from_the_new_now) {
  RecoveryBackoff backoff;
  backoff.on_bus_off(1000);
  CHECK_EQ(backoff.should_attempt(1100), true);  // delay -> 200

  backoff.on_bus_off(5000);
  CHECK_EQ(backoff.should_attempt(5199), false);
  CHECK_EQ(backoff.should_attempt(5200), true);  // due at now + 200, not earlier
}

TEST(backoff_on_recovered_keeps_the_escalated_delay) {
  RecoveryBackoff backoff;
  uint32_t now = 0;
  for (int i = 0; i < 3; i++) {  // 100 -> 200 -> 400 -> 800
    backoff.on_bus_off(now);
    CHECK_EQ(backoff.should_attempt(now + backoff.current_delay_ms()), true);
    now += 5000;
  }
  CHECK_EQ(backoff.current_delay_ms(), 800u);

  backoff.on_recovered(now);
  CHECK_EQ(backoff.current_delay_ms(), 800u);  // NOT reset by the recovery
  CHECK_EQ(backoff.pending(), false);
}

TEST(backoff_on_recovered_cancels_a_pending_attempt) {
  RecoveryBackoff backoff;
  backoff.on_bus_off(1000);
  CHECK_EQ(backoff.pending(), true);
  backoff.on_recovered(1050);  // recovered before the attempt came due
  CHECK_EQ(backoff.pending(), false);
  CHECK_EQ(backoff.should_attempt(1100), false);
  CHECK_EQ(backoff.current_delay_ms(), 100u);  // never fired, never doubled
}

TEST(backoff_stable_tick_resets_only_after_the_full_window) {
  RecoveryBackoff backoff;
  backoff.on_bus_off(0);
  backoff.should_attempt(100);
  backoff.on_bus_off(1000);
  backoff.should_attempt(1200);
  CHECK_EQ(backoff.current_delay_ms(), 400u);

  const uint32_t recovered_at = 2000;
  backoff.on_recovered(recovered_at);

  backoff.on_stable_tick(recovered_at);
  CHECK_EQ(backoff.current_delay_ms(), 400u);
  backoff.on_stable_tick(recovered_at + RecoveryBackoff::STABLE_RESET_MS - 1);
  CHECK_EQ(backoff.current_delay_ms(), 400u);
  backoff.on_stable_tick(recovered_at + RecoveryBackoff::STABLE_RESET_MS);
  CHECK_EQ(backoff.current_delay_ms(), RecoveryBackoff::INITIAL_DELAY_MS);
}

TEST(backoff_stable_tick_without_a_recovery_never_resets) {
  // stable_since_valid_ is only armed by on_recovered(); ticking a port that
  // never recovered must not quietly rearm the fast schedule.
  RecoveryBackoff backoff;
  backoff.on_bus_off(0);
  backoff.should_attempt(100);
  backoff.on_bus_off(1000);
  backoff.should_attempt(1200);
  CHECK_EQ(backoff.current_delay_ms(), 400u);

  for (uint32_t t = 0; t < 200000; t += 5000)
    backoff.on_stable_tick(t);
  CHECK_EQ(backoff.current_delay_ms(), 400u);
}

TEST(backoff_bus_off_inside_the_window_voids_the_measurement) {
  RecoveryBackoff backoff;
  backoff.on_bus_off(0);
  backoff.should_attempt(100);  // delay -> 200
  backoff.on_recovered(200);

  backoff.on_stable_tick(5000);  // half-way through the stable window
  CHECK_EQ(backoff.current_delay_ms(), 200u);

  backoff.on_bus_off(6000);      // the bus was not stable after all
  backoff.should_attempt(6200);  // delay -> 400

  // Long past recovered_at + STABLE_RESET_MS, but the window was voided.
  backoff.on_stable_tick(60000);
  CHECK_EQ(backoff.current_delay_ms(), 400u);
}

TEST(backoff_resets_once_then_needs_a_new_recovery) {
  RecoveryBackoff backoff;
  backoff.on_bus_off(0);
  backoff.should_attempt(100);
  CHECK_EQ(backoff.current_delay_ms(), 200u);

  backoff.on_recovered(1000);
  backoff.on_stable_tick(1000 + RecoveryBackoff::STABLE_RESET_MS);
  CHECK_EQ(backoff.current_delay_ms(), 100u);

  // The reset disarms itself; escalating again must stick until the next
  // recovery + stable window.
  backoff.on_bus_off(20000);
  backoff.should_attempt(20100);
  CHECK_EQ(backoff.current_delay_ms(), 200u);
  backoff.on_stable_tick(50000);
  CHECK_EQ(backoff.current_delay_ms(), 200u);
}

TEST(backoff_full_dead_bus_settles_at_the_cap) {
  // The scenario the "keep the escalated delay" rule exists for: a bus that
  // recovers and immediately falls over again must converge on MAX_DELAY_MS
  // rather than hammering recover() at the initial delay forever.
  RecoveryBackoff backoff;
  uint32_t now = 0;
  for (int i = 0; i < 40; i++) {
    backoff.on_bus_off(now);
    uint32_t delay = backoff.current_delay_ms();
    now += delay;
    CHECK_EQ(backoff.should_attempt(now), true);
    now += 1;
    backoff.on_recovered(now);  // recovery "succeeds" ...
    now += 1;                   // ... and the bus dies again immediately
  }
  CHECK_EQ(backoff.current_delay_ms(), RecoveryBackoff::MAX_DELAY_MS);
}

TEST(backoff_due_time_is_millis_wrap_safe) {
  // millis() wraps every ~49.7 days; a due time computed just before the wrap
  // must not read as "already due" for the whole preceding half of the range.
  RecoveryBackoff backoff;
  const uint32_t now = 0xFFFFFFC0;  // 64 ms before the wrap
  backoff.on_bus_off(now);
  const uint32_t due = now + RecoveryBackoff::INITIAL_DELAY_MS;  // wraps to 0x24

  CHECK_EQ(backoff.should_attempt(now), false);
  CHECK_EQ(backoff.should_attempt(0xFFFFFFFF), false);
  CHECK_EQ(backoff.should_attempt(0x00000000), false);
  CHECK_EQ(backoff.should_attempt(due - 1), false);
  CHECK_EQ(backoff.should_attempt(due), true);
}

TEST(backoff_stable_window_is_millis_wrap_safe) {
  RecoveryBackoff backoff;
  backoff.on_bus_off(0xFFFF0000);
  backoff.should_attempt(0xFFFF0064);
  CHECK_EQ(backoff.current_delay_ms(), 200u);

  const uint32_t recovered_at = 0xFFFFFF00;
  backoff.on_recovered(recovered_at);
  backoff.on_stable_tick(recovered_at + RecoveryBackoff::STABLE_RESET_MS - 1);  // wrapped
  CHECK_EQ(backoff.current_delay_ms(), 200u);
  backoff.on_stable_tick(recovered_at + RecoveryBackoff::STABLE_RESET_MS);  // wrapped
  CHECK_EQ(backoff.current_delay_ms(), RecoveryBackoff::INITIAL_DELAY_MS);
}
