// The card-recovery retry ladder (components/sd_logger/recovery_policy.h).
//
// Why this is a host test and not a bench check: staging one case costs a real card failure —
// pulling the card mid-write, or shorting its supply — and then *waiting out* the ladder to see
// whether the delays are right. The failure modes are all quiet:
//
//   * a `now >= deadline` comparison instead of a signed difference works perfectly for 49.7 days
//     and then either retries every 20 ms pass or never retries again, depending on which side of
//     the wrap the failure landed on;
//   * a ladder that doubles before clamping overflows and schedules the next attempt in the past,
//     which reads on the bench as "recovery hammers the card";
//   * an attempt budget that is spent one attempt early, or never, looks identical to a card that
//     genuinely did not come back.
//
// None of those leave evidence on a card that, by definition, was not writable at the time.

#include "harness.h"
#include "recovery_policy.h"

#include <cstdint>

using esphome::sd_logger::RecoveryPolicy;
using esphome::sd_logger::SD_LOG_RECOVERY_UNLIMITED;

namespace {

/// The component's own defaults, so the cases below read as the shipped behaviour.
RecoveryPolicy make_policy(uint32_t initial = 1000, uint32_t max_delay = 30000,
                           uint32_t max_attempts = SD_LOG_RECOVERY_UNLIMITED) {
  RecoveryPolicy policy;
  policy.configure(initial, max_delay, max_attempts);
  return policy;
}

}  // namespace

// --------------------------------------------------------------------------------- construction

TEST(recovery_default_constructed_is_disabled) {
  // A logger whose Python never called set_recovery() must behave like every release before
  // recovery existed: a card failure is a one-way trip.
  RecoveryPolicy policy;
  CHECK(!policy.enabled());
  policy.arm(1000);
  CHECK(!policy.armed());
  CHECK(!policy.due(1000000));
}

TEST(recovery_configure_enables) {
  RecoveryPolicy policy = make_policy();
  CHECK(policy.enabled());
  CHECK(!policy.armed());
  CHECK_EQ(policy.attempts(), 0u);
}

TEST(recovery_disable_disarms) {
  RecoveryPolicy policy = make_policy();
  policy.arm(0);
  CHECK(policy.armed());
  policy.disable();
  CHECK(!policy.enabled());
  CHECK(!policy.armed());
  policy.arm(0);
  CHECK(!policy.armed());
}

TEST(recovery_configure_clamps_max_below_initial) {
  // V20 rejects this in YAML, but the class is also reachable from hand-written code, and a
  // max_delay below the initial delay would make the ladder step *down*.
  RecoveryPolicy policy = make_policy(5000, 1000);
  policy.arm(0);
  CHECK_EQ(policy.delay_ms(), 5000u);
  policy.note_attempt(0);
  CHECK_EQ(policy.delay_ms(), 5000u);
}

TEST(recovery_configure_clamps_zero_initial) {
  // A zero initial delay would make due() true on the same pass that armed it, so the writer
  // would retry inside the failure handler rather than on the next poll.
  RecoveryPolicy policy = make_policy(0, 1000);
  policy.arm(100);
  CHECK(!policy.due(100));
  CHECK(policy.due(101));
}

// ------------------------------------------------------------------------------------- arming

TEST(recovery_arm_schedules_first_retry) {
  RecoveryPolicy policy = make_policy();
  policy.arm(5000);
  CHECK(policy.armed());
  CHECK_EQ(policy.delay_ms(), 1000u);
  CHECK(!policy.due(5999));
  CHECK(policy.due(6000));
  CHECK(policy.due(6001));
}

TEST(recovery_due_is_exact_at_the_deadline) {
  RecoveryPolicy policy = make_policy(250, 30000);
  policy.arm(1000);
  CHECK(!policy.due(1249));
  CHECK(policy.due(1250));
}

TEST(recovery_arm_while_waiting_does_not_restart_the_ladder) {
  // The writer can discover the same dead card twice — a failed write, then a failed rotate —
  // before the first retry is due. Restarting the ladder there would pin the delay at its
  // minimum and hammer a card that is already in trouble.
  RecoveryPolicy policy = make_policy();
  policy.arm(0);
  policy.note_attempt(0);
  CHECK_EQ(policy.delay_ms(), 2000u);
  policy.arm(0);
  CHECK_EQ(policy.delay_ms(), 2000u);
  CHECK_EQ(policy.attempts(), 1u);
}

TEST(recovery_arm_is_a_noop_when_idle_after_reset) {
  RecoveryPolicy policy = make_policy();
  CHECK(!policy.armed());
  CHECK(!policy.due(1000000));
}

// -------------------------------------------------------------------------------------- ladder

TEST(recovery_ladder_doubles_to_the_cap) {
  RecoveryPolicy policy = make_policy(1000, 30000);
  policy.arm(0);
  const uint32_t expected[] = {2000, 4000, 8000, 16000, 30000, 30000, 30000};
  uint32_t now = 0;
  for (uint32_t want : expected) {
    policy.note_attempt(now);
    CHECK_EQ(policy.delay_ms(), want);
    now += want;
  }
}

TEST(recovery_ladder_deadline_follows_the_delay) {
  RecoveryPolicy policy = make_policy(1000, 8000);
  policy.arm(0);
  policy.note_attempt(1000);  // next delay 2000 -> due at 3000
  CHECK(!policy.due(2999));
  CHECK(policy.due(3000));
  policy.note_attempt(3000);  // next delay 4000 -> due at 7000
  CHECK(!policy.due(6999));
  CHECK(policy.due(7000));
}

TEST(recovery_ladder_constant_when_initial_equals_max) {
  RecoveryPolicy policy = make_policy(5000, 5000);
  policy.arm(0);
  for (int i = 0; i < 5; i++) {
    policy.note_attempt(0);
    CHECK_EQ(policy.delay_ms(), 5000u);
  }
}

TEST(recovery_ladder_does_not_overflow_at_a_huge_cap) {
  // Clamping after the multiply instead of before would wrap here and schedule the next attempt
  // in the past — recovery would then hammer the card every writer pass. The ladder must stay
  // monotonic all the way to the cap, which is the property that actually matters; the exact
  // rung count is incidental.
  RecoveryPolicy policy = make_policy(0x40000000u, 0xFFFFFFFFu);
  policy.arm(0);
  uint32_t previous = policy.delay_ms();
  for (int i = 0; i < 8; i++) {
    policy.note_attempt(0);
    CHECK(policy.delay_ms() >= previous);  // never wrapped
    CHECK(policy.delay_ms() <= 0xFFFFFFFFu);
    previous = policy.delay_ms();
  }
  CHECK_EQ(policy.delay_ms(), 0xFFFFFFFFu);  // and it does reach the cap
}

// ------------------------------------------------------------------------------------- budget

TEST(recovery_unlimited_never_exhausts) {
  RecoveryPolicy policy = make_policy(1000, 2000, SD_LOG_RECOVERY_UNLIMITED);
  policy.arm(0);
  uint32_t now = 0;
  for (int i = 0; i < 1000; i++) {
    policy.note_attempt(now);
    now += policy.delay_ms();
    CHECK(!policy.exhausted());
  }
  CHECK(policy.armed());
  CHECK_EQ(policy.attempts(), 1000u);
}

TEST(recovery_budget_spends_exactly_max_attempts) {
  RecoveryPolicy policy = make_policy(1000, 30000, 3);
  policy.arm(0);
  policy.note_attempt(0);
  CHECK_EQ(policy.attempts(), 1u);
  CHECK(!policy.exhausted());
  policy.note_attempt(1000);
  CHECK_EQ(policy.attempts(), 2u);
  CHECK(!policy.exhausted());
  policy.note_attempt(3000);
  CHECK_EQ(policy.attempts(), 3u);
  CHECK(policy.exhausted());
}

TEST(recovery_exhausted_is_never_due_again) {
  RecoveryPolicy policy = make_policy(1000, 30000, 1);
  policy.arm(0);
  policy.note_attempt(0);
  CHECK(policy.exhausted());
  CHECK(!policy.armed());
  CHECK(!policy.due(1000000));
  // and a further failure cannot revive it
  policy.arm(2000000);
  CHECK(!policy.due(3000000));
}

TEST(recovery_note_attempt_is_a_noop_when_not_waiting) {
  RecoveryPolicy policy = make_policy(1000, 30000, 3);
  policy.note_attempt(0);  // idle
  CHECK_EQ(policy.attempts(), 0u);
  CHECK(!policy.exhausted());
}

// -------------------------------------------------------------------------------------- reset

TEST(recovery_reset_clears_the_ladder_and_the_budget) {
  RecoveryPolicy policy = make_policy(1000, 30000, 5);
  policy.arm(0);
  policy.note_attempt(0);
  policy.note_attempt(2000);
  CHECK_EQ(policy.attempts(), 2u);
  CHECK_EQ(policy.delay_ms(), 4000u);

  policy.reset();
  CHECK(!policy.armed());
  CHECK(!policy.exhausted());
  CHECK_EQ(policy.attempts(), 0u);
  CHECK_EQ(policy.delay_ms(), 1000u);

  // the next failure gets the full budget again
  policy.arm(10000);
  CHECK(policy.armed());
  CHECK(policy.due(11000));
}

TEST(recovery_reset_revives_an_exhausted_policy) {
  RecoveryPolicy policy = make_policy(1000, 30000, 1);
  policy.arm(0);
  policy.note_attempt(0);
  CHECK(policy.exhausted());
  policy.reset();
  CHECK(!policy.exhausted());
  policy.arm(5000);
  CHECK(policy.due(6000));
}

// ---------------------------------------------------------------------------------- millis wrap

TEST(recovery_deadline_across_the_millis_wrap) {
  // millis() wraps every 49.7 days. A `now >= deadline` comparison would report the deadline as
  // already past for the whole 1000 ms before the wrap, so the writer would retry immediately and
  // then keep retrying every pass.
  RecoveryPolicy policy = make_policy(1000, 30000);
  const uint32_t before_wrap = 0xFFFFFF00u;  // deadline lands at 744, past the wrap
  policy.arm(before_wrap);
  CHECK(!policy.due(before_wrap));
  CHECK(!policy.due(0xFFFFFFFFu));
  CHECK(!policy.due(0u));
  CHECK(!policy.due(743u));
  CHECK(policy.due(744u));
  CHECK(policy.due(745u));
}

TEST(recovery_ladder_across_the_millis_wrap) {
  RecoveryPolicy policy = make_policy(1000, 4000);
  policy.arm(0xFFFFF000u);
  policy.note_attempt(0xFFFFF000u);  // delay 2000 -> deadline 0xFFFFF7D0
  CHECK(!policy.due(0xFFFFF7CFu));
  CHECK(policy.due(0xFFFFF7D0u));
  policy.note_attempt(0xFFFFFF00u);  // delay 4000 -> deadline wraps to 0xFFFFFF00 + 4000
  CHECK(!policy.due(0xFFFFFFFFu));
  CHECK(!policy.due(0u));
  CHECK(policy.due(0xFFFFFF00u + 4000u));
}

// ------------------------------------------------------------------- the in-band reset escalation

TEST(recovery_first_attempt_remounts_without_resetting) {
  // Most card failures are not wedges. A plain remount costs ~100 ms and fixes a transient write
  // error; running the full reset sequence first would spend seconds clocking a healthy card on
  // every hiccup, and the writer is stalled for all of it.
  RecoveryPolicy policy = make_policy();
  policy.configure_reset(true, 2000, 10000);
  policy.arm(0);
  policy.note_attempt(0);

  CHECK_EQ(policy.attempts(), 1u);
  CHECK(!policy.reset_due());
}

TEST(recovery_second_attempt_resets_at_the_configured_budget) {
  RecoveryPolicy policy = make_policy();
  policy.configure_reset(true, 2000, 10000);
  policy.arm(0);
  policy.note_attempt(0);
  policy.note_attempt(1000);

  CHECK(policy.reset_due());
  CHECK_EQ(policy.busy_timeout_ms(), 2000u);  // unmultiplied on the first attempt that resets
}

TEST(recovery_busy_budget_doubles_then_clamps) {
  // The point of the ladder: a card whose internal write recovery needs longer than the first
  // budget is indistinguishable from a dead one until something waits longer.
  RecoveryPolicy policy = make_policy();
  policy.configure_reset(true, 2000, 10000);
  policy.arm(0);
  policy.note_attempt(0);  // attempt 1: no reset
  policy.note_attempt(0);  // attempt 2: 2000
  CHECK_EQ(policy.busy_timeout_ms(), 2000u);
  policy.note_attempt(0);  // attempt 3
  CHECK_EQ(policy.busy_timeout_ms(), 4000u);
  policy.note_attempt(0);  // attempt 4
  CHECK_EQ(policy.busy_timeout_ms(), 8000u);
  policy.note_attempt(0);  // attempt 5: clamped, not 16000
  CHECK_EQ(policy.busy_timeout_ms(), 10000u);
  policy.note_attempt(0);
  CHECK_EQ(policy.busy_timeout_ms(), 10000u);
}

TEST(recovery_busy_budget_clamp_does_not_overflow) {
  // Same failure the delay ladder had: doubling before clamping wraps and hands the card reset a
  // budget of a few milliseconds, which reads on the bench as "the reset does nothing".
  RecoveryPolicy policy = make_policy();
  policy.configure_reset(true, 0x40000000u, 0xF0000000u);
  policy.arm(0);
  for (int i = 0; i < 6; i++)
    policy.note_attempt(0);
  CHECK_EQ(policy.busy_timeout_ms(), 0xF0000000u);
}

TEST(recovery_success_rearms_the_reset_ladder) {
  RecoveryPolicy policy = make_policy();
  policy.configure_reset(true, 2000, 10000);
  policy.arm(0);
  policy.note_attempt(0);
  policy.note_attempt(0);
  policy.note_attempt(0);
  CHECK_EQ(policy.busy_timeout_ms(), 4000u);

  policy.reset();  // the card came back
  CHECK(!policy.reset_due());
  CHECK_EQ(policy.busy_timeout_ms(), 2000u);
}

TEST(recovery_reset_can_be_turned_off_entirely) {
  // `in_band_reset: false` keeps the ladder but never touches the bus by hand — the escape hatch
  // for a board where something else shares the SPI pins.
  RecoveryPolicy policy = make_policy();
  policy.configure_reset(false, 2000, 10000);
  policy.arm(0);
  for (int i = 0; i < 5; i++)
    policy.note_attempt(0);
  CHECK(!policy.reset_due());
}

TEST(recovery_reset_defaults_to_off_until_configured) {
  // A policy whose Python never called set_recovery_reset() must not start clocking the card.
  RecoveryPolicy policy = make_policy();
  policy.arm(0);
  policy.note_attempt(0);
  policy.note_attempt(0);
  CHECK(!policy.reset_due());
}

TEST(recovery_exhausted_budget_stops_the_reset_ladder_too) {
  RecoveryPolicy policy = make_policy(1000, 30000, /*max_attempts=*/2);
  policy.configure_reset(true, 2000, 10000);
  policy.arm(0);
  policy.note_attempt(0);
  policy.note_attempt(0);
  CHECK(policy.exhausted());
  CHECK(!policy.due(1000000));
}
