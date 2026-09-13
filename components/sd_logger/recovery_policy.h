#pragma once

/// When the writer retries a failed card, how long it waits between attempts, how hard each attempt
/// tries, and when it gives up (docs/sd_logger-spec.md §7 Layer C).
///
/// ESPHome-free and IDF-free, like `log_format.h` and for exactly the same reason: every
/// interesting case in here costs a *staged card failure* to reach on the bench and a microsecond
/// to reach in `tests/host/test_recovery_policy.cpp`. The doubling ceiling, an attempt budget
/// running out, and a deadline that straddles the `millis()` wrap are all one-line bugs that
/// present on hardware as "the card never came back", days apart, with no evidence left.
///
/// Time is the caller's `millis()`: uint32_t milliseconds that wrap every 49.7 days. Every
/// comparison goes through a signed difference, so a deadline on the far side of the wrap is
/// still correctly in the future.
///
/// One deliberate non-feature: a card that mounts, accepts a header and then fails again on its
/// first write re-arms from the *initial* delay each time, because `reset()` on a successful
/// remount clears the ladder. So a flapping card retries at `initial_delay` forever rather than
/// backing off. That is the honest behaviour — the card really is failing, each attempt is one
/// bounded mount, and the alternative needs a "healthy for N ms" notion that no observed failure
/// has yet called for.

#include <cstdint>

namespace esphome {
namespace sd_logger {

/// `max_attempts: 0` means "keep trying for as long as the board runs".
static const uint32_t SD_LOG_RECOVERY_UNLIMITED = 0;

enum class RecoveryState : uint8_t {
  IDLE,       ///< nothing to recover: the card is healthy, or recovery is off.
  WAITING,    ///< a retry is scheduled for `deadline_ms_`.
  EXHAUSTED,  ///< the attempt budget ran out; nothing will be tried again.
};

/// The retry ladder. Holds no card state and touches no hardware — the writer task asks it
/// "may I try now?" and tells it what happened.
class RecoveryPolicy {
 public:
  /// Turn recovery on with a ladder that starts at `initial_delay_ms` and doubles to
  /// `max_delay_ms`. `max_attempts` of 0 is unlimited. Both delays are clamped into a sane
  /// relationship here as well as in the schema (V20), because this class is also reachable
  /// from a hand-written test.
  void configure(uint32_t initial_delay_ms, uint32_t max_delay_ms, uint32_t max_attempts) {
    this->enabled_ = true;
    this->initial_delay_ms_ = initial_delay_ms == 0 ? 1 : initial_delay_ms;
    this->max_delay_ms_ = max_delay_ms < this->initial_delay_ms_ ? this->initial_delay_ms_ : max_delay_ms;
    this->max_attempts_ = max_attempts;
    this->reset();
  }

  /// The in-band reset ladder (`card_reset.h`), which is a separate axis from the retry delay:
  /// *when* to try again is this class's original job, *how hard* to try is this one.
  ///
  /// The first attempt after a failure never runs it. Most card failures are not wedges — a
  /// transient write error, a card pulled and pushed back — and for those a plain remount costs
  /// ~100 ms and works, while the reset sequence would spend seconds clocking a card that was
  /// always fine. From the second attempt on, the budget doubles: a card that needs longer than
  /// `busy_timeout` to finish its internal write recovery is exactly the case a fixed budget
  /// cannot distinguish from a dead one, and doubling costs nothing when the card is simply gone.
  void configure_reset(bool enabled, uint32_t busy_timeout_ms, uint32_t max_busy_timeout_ms) {
    this->in_band_reset_ = enabled;
    this->initial_busy_timeout_ms_ = busy_timeout_ms == 0 ? 1 : busy_timeout_ms;
    this->max_busy_timeout_ms_ =
        max_busy_timeout_ms < this->initial_busy_timeout_ms_ ? this->initial_busy_timeout_ms_ : max_busy_timeout_ms;
    this->busy_timeout_ms_ = this->initial_busy_timeout_ms_;
  }

  /// Should the attempt now being made run the in-band reset before remounting?
  bool reset_due() const { return this->in_band_reset_ && this->attempts_ >= 2; }

  /// The busy budget for the attempt now being made.
  uint32_t busy_timeout_ms() const { return this->busy_timeout_ms_; }

  bool in_band_reset() const { return this->in_band_reset_; }
  uint32_t max_busy_timeout_ms() const { return this->max_busy_timeout_ms_; }

  /// `recovery: {enabled: false}` — a card failure stays a one-way trip for the run, which is
  /// what every release before this one did.
  void disable() {
    this->enabled_ = false;
    this->reset();
  }

  bool enabled() const { return this->enabled_; }

  /// A failure just happened: schedule the first retry. A no-op unless idle, so a second failure
  /// arriving while a retry is already pending cannot restart the ladder at the bottom.
  void arm(uint32_t now_ms) {
    if (!this->enabled_ || this->state_ != RecoveryState::IDLE)
      return;
    this->state_ = RecoveryState::WAITING;
    this->delay_ms_ = this->initial_delay_ms_;
    this->deadline_ms_ = now_ms + this->delay_ms_;
  }

  /// May the caller attempt a remount right now?
  bool due(uint32_t now_ms) const {
    // Signed difference, never `now >= deadline`: the direct comparison is wrong for the ~50 ms
    // either side of the millis() wrap and would either fire every pass or never fire again.
    return this->state_ == RecoveryState::WAITING && static_cast<int32_t>(now_ms - this->deadline_ms_) >= 0;
  }

  /// An attempt was just made. Counts it and schedules the next one; spends the budget.
  /// Call this *before* the attempt, so a remount that hangs cannot be retried for free.
  void note_attempt(uint32_t now_ms) {
    if (this->state_ != RecoveryState::WAITING)
      return;
    this->attempts_++;
    if (this->max_attempts_ != SD_LOG_RECOVERY_UNLIMITED && this->attempts_ >= this->max_attempts_) {
      this->state_ = RecoveryState::EXHAUSTED;
      return;
    }
    // Compare against half the cap before multiplying: doubling first and clamping after would
    // overflow the ladder for any max_delay above 2^31.
    this->delay_ms_ = this->delay_ms_ >= this->max_delay_ms_ / 2 ? this->max_delay_ms_ : this->delay_ms_ * 2;
    this->deadline_ms_ = now_ms + this->delay_ms_;
    // The busy budget starts doubling only from the *third* attempt: the second is the first one
    // that resets at all, and it gets the configured budget unmultiplied. Same overflow-safe clamp.
    if (this->attempts_ > 2) {
      this->busy_timeout_ms_ = this->busy_timeout_ms_ >= this->max_busy_timeout_ms_ / 2 ? this->max_busy_timeout_ms_
                                                                                        : this->busy_timeout_ms_ * 2;
    }
  }

  /// The card is back. Clears the ladder so the next failure, whenever it comes, gets the full
  /// attempt budget again.
  void reset() {
    this->state_ = RecoveryState::IDLE;
    this->attempts_ = 0;
    this->delay_ms_ = this->initial_delay_ms_;
    this->deadline_ms_ = 0;
    this->busy_timeout_ms_ = this->initial_busy_timeout_ms_;
  }

  bool exhausted() const { return this->state_ == RecoveryState::EXHAUSTED; }
  bool armed() const { return this->state_ == RecoveryState::WAITING; }
  uint32_t attempts() const { return this->attempts_; }
  /// The configured attempt budget, 0 meaning unlimited; for dump_config.
  uint32_t attempts_limit() const { return this->max_attempts_; }
  /// The delay that will be waited before the *next* attempt; for the log line that announces it.
  uint32_t delay_ms() const { return this->delay_ms_; }

 protected:
  bool enabled_{false};
  RecoveryState state_{RecoveryState::IDLE};
  uint32_t initial_delay_ms_{1000};
  uint32_t max_delay_ms_{30000};
  uint32_t max_attempts_{SD_LOG_RECOVERY_UNLIMITED};
  uint32_t attempts_{0};
  uint32_t delay_ms_{1000};
  uint32_t deadline_ms_{0};
  bool in_band_reset_{false};
  uint32_t initial_busy_timeout_ms_{2000};
  uint32_t max_busy_timeout_ms_{10000};
  uint32_t busy_timeout_ms_{2000};
};

}  // namespace sd_logger
}  // namespace esphome
