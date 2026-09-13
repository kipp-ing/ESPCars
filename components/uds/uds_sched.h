#pragma once

/// Freestanding per-group polling scheduler for the uds component (docs/DESIGN-uds.md §5).
/// No ESPHome, no ESP-IDF, no allocation, no exceptions. Storage is caller-supplied through
/// init() — the isotp_core.h pattern — so allocation stays in the component's setup() and this
/// header stays pure. The scheduler never does I/O: next() picks a slot, the caller sends the
/// request, and reports the outcome back through on_success()/on_failure().
///
/// Scheduling is per *group*, not per sensor: one request serves every field decoded from its
/// response, and per-sensor due times against a shared request would put the same bytes on the
/// wire once per sensor (design §5). One entry is 16 bytes, so 32 polled groups cost 512 B.
///
/// One request in flight per scheduler, because ISO-TP has no multiplexing and `isotp` enforces
/// the same rule one layer down.
///
/// **The response deadline lives here, not in the caller.** An ECU that simply never answers —
/// wrong identifier, sleeping bus, a battery nobody plugged in — produces no `IsoTpError` at all:
/// isotp's N_Bs and N_Cr timers only run *during* a transfer, so total silence yields silence.
/// A scheduler that learned about failures only from transport errors would keep `in_flight_` set
/// forever, and because in-flight is global (one request at a time) that stops polling *every*
/// group, not just the silent one — one unplugged ECU, and every entity quietly stops updating.
/// So next() arms a P2 deadline as it hands out a slot, and poll() converts its expiry into the
/// ordinary failure path. The obligation is discharged by the code that owns the state rather
/// than documented for the caller to remember.
///
/// The deadline is one scalar on the scheduler, not a field in PollEntry: one-in-flight means at
/// most one deadline is ever live, so per-entry storage would spend 4 bytes an entry on something
/// only one entry can use, and would make two simultaneous deadlines representable when the
/// transport cannot produce them.
///
/// The driving loop, end to end:
///   slot = next(now, p2_ms)              → SCHED_NONE, or a slot marked in flight and armed
///   build_request(...) and send it
///     send refused (transport busy)      → cancel_in_flight(): backpressure, not a failure
///     send accepted                      → optionally arm_deadline(now, p2_ms) at the true end of
///                                          transmission, which is where ISO 14229 starts P2
///   response ACCEPTed                    → on_success(slot, now)
///   NRC 0x78 (response pending)          → extend_deadline(now, p2_ext_ms), keep waiting
///   NRC 0x31                             → suspend(slot)
///   IsoTpError, or any other rejection   → on_failure(slot, now)
///   every loop iteration                 → poll(now), which fails a silent ECU on its own

#include <cstddef>
#include <cstdint>

namespace esphome::uds {

/// Returned by next() when nothing is due or a request is already in flight.
static constexpr int16_t SCHED_NONE = -1;

/// Exponential backoff ceiling (design §4: double from the group interval, capped at 60 s).
static constexpr uint32_t SCHED_BACKOFF_CAP_MS = 60000;

/// Entry flags.
static constexpr uint8_t POLL_FLAG_SUSPENDED = 1 << 0;    ///< NRC 0x31: permanently unsupported, never polled again
static constexpr uint8_t POLL_FLAG_UNAVAILABLE = 1 << 1;  ///< entities currently published as unavailable

/// What poll() found. `slot` is SCHED_NONE when nothing expired; otherwise the P2 deadline for
/// that slot ran out with nothing received, the failure has already been recorded, and
/// `publish_unavailable` carries on_failure()'s threshold-crossing signal so the caller publishes
/// exactly once.
struct SchedTimeout {
  int16_t slot;
  bool publish_unavailable;
};

/// One polled group. 16 bytes, caller-allocated as an array.
struct PollEntry {
  uint32_t interval_ms;  ///< the minimum update_interval across the group's sensors
  uint32_t next_due_ms;
  uint32_t backoff_ms;  ///< current backed-off delay; 0 while healthy
  uint16_t group_index;
  uint8_t consecutive_failures;
  uint8_t flags;
};

/// Millisecond wrap safety, the isotp_core.h `elapsed_ms` idea: `now - due` is *modular*
/// subtraction, and reading the difference as "overdue by" is correct while the true distance is
/// under 2^31 ms (~24.8 days). A naive `now >= due` comparison instead breaks at the 49.7-day
/// counter wrap — the classic uptime bug that no bench run is long enough to catch. add()
/// rejects intervals at or beyond the horizon so the invariant holds by construction.
inline bool sched_time_reached(uint32_t now_ms, uint32_t due_ms) { return (now_ms - due_ms) < 0x80000000u; }

class GroupScheduler {
 public:
  /// Supply the entry table. Called once, from the component's setup().
  /// `max_consecutive_failures` is the YAML knob: crossing it flags the group unavailable so its
  /// entities publish NaN rather than a value that stopped being true.
  void init(PollEntry *entries, uint8_t capacity, uint8_t max_consecutive_failures) {
    this->entries_ = entries;
    this->capacity_ = capacity;
    this->max_failures_ = max_consecutive_failures;
    this->count_ = 0;
    this->in_flight_ = SCHED_NONE;
    this->deadline_ms_ = 0;
  }

  uint8_t size() const { return this->count_; }
  uint8_t capacity() const { return this->capacity_; }
  int16_t in_flight() const { return this->in_flight_; }

  const PollEntry &entry(uint8_t slot) const { return this->entries_[slot]; }

  bool is_suspended(uint8_t slot) const {
    return slot < this->count_ && (this->entries_[slot].flags & POLL_FLAG_SUSPENDED) != 0;
  }
  bool is_unavailable(uint8_t slot) const {
    return slot < this->count_ && (this->entries_[slot].flags & POLL_FLAG_UNAVAILABLE) != 0;
  }

  /// Register a group for polling; it is due immediately. Returns false when the table is full
  /// or the interval is at or beyond the wrap-safety horizon (see sched_time_reached).
  /// `update_interval: never` groups are simply not added.
  bool add(uint16_t group_index, uint32_t interval_ms, uint32_t now_ms) {
    if (this->entries_ == nullptr || this->count_ >= this->capacity_ || interval_ms >= 0x80000000u)
      return false;
    PollEntry &e = this->entries_[this->count_];
    e.interval_ms = interval_ms;
    e.next_due_ms = now_ms;
    e.backoff_ms = 0;
    e.group_index = group_index;
    e.consecutive_failures = 0;
    e.flags = 0;
    this->count_++;
    return true;
  }

  /// Pick the most-overdue due group, mark it in flight, arm its P2 deadline, and return its
  /// slot. SCHED_NONE when a request is already in flight (one at a time — ISO-TP has no
  /// multiplexing) or nothing is due. A linear scan on purpose: the table is tens of entries and
  /// the scan is unmeasurable next to a CAN frame (design §5).
  ///
  /// `p2_ms` is the ECU's response deadline from the catalog. It is armed here, at selection,
  /// rather than after transmission, so no caller mistake can leave a slot in flight with no
  /// deadline — the wedge described at the top of this file. The pessimism is one or two loop
  /// iterations against a P2 of hundreds of milliseconds; arm_deadline() re-arms from the true
  /// end of transmission for a caller that wants ISO 14229's exact origin.
  int16_t next(uint32_t now_ms, uint32_t p2_ms) {
    if (this->in_flight_ != SCHED_NONE)
      return SCHED_NONE;
    int16_t best = SCHED_NONE;
    uint32_t best_overdue = 0;
    for (uint8_t i = 0; i < this->count_; i++) {
      const PollEntry &e = this->entries_[i];
      if ((e.flags & POLL_FLAG_SUSPENDED) != 0)
        continue;
      const uint32_t overdue = now_ms - e.next_due_ms;  // modular; see sched_time_reached
      if (overdue >= 0x80000000u)
        continue;  // not yet due
      if (best == SCHED_NONE || overdue > best_overdue) {
        best = static_cast<int16_t>(i);
        best_overdue = overdue;
      }
    }
    if (best != SCHED_NONE) {
      this->in_flight_ = best;
      this->deadline_ms_ = now_ms + p2_ms;
    }
    return best;
  }

  /// Re-arm the in-flight request's deadline from `now_ms` — the end of transmission, which is
  /// where ISO 14229 starts P2. Optional: next() already armed one.
  void arm_deadline(uint32_t now_ms, uint32_t p2_ms) {
    if (this->in_flight_ != SCHED_NONE)
      this->deadline_ms_ = now_ms + p2_ms;
  }

  /// NRC 0x78, response pending: the ECU is working on it, so extend to the catalog's p2_ext_ms
  /// and keep waiting rather than counting a failure (design §4).
  void extend_deadline(uint32_t now_ms, uint32_t p2_ext_ms) { this->arm_deadline(now_ms, p2_ext_ms); }

  bool deadline_pending() const { return this->in_flight_ != SCHED_NONE; }
  uint32_t deadline_ms() const { return this->deadline_ms_; }

  /// Release the in-flight slot without judging it: no backoff, no failure count, `next_due_ms`
  /// untouched, so the same group is picked again on the next pass.
  ///
  /// This is for a send the transport refused — `isotp` returns false while another transfer is in
  /// flight, and that is backpressure, not an answer the ECU failed to give. Without this the glue
  /// could only wedge the scheduler (leave it in flight forever) or call on_failure() and apply a
  /// doubling backoff plus an unavailability count to a request that was never asked.
  void cancel_in_flight() { this->in_flight_ = SCHED_NONE; }

  /// Call once per loop iteration. Fails the in-flight request when its P2 deadline expires with
  /// nothing received — the silent-ECU case, which produces no IsoTpError to react to.
  SchedTimeout poll(uint32_t now_ms) {
    SchedTimeout out{SCHED_NONE, false};
    if (this->in_flight_ == SCHED_NONE)
      return out;
    if (!sched_time_reached(now_ms, this->deadline_ms_))
      return out;
    const uint8_t slot = static_cast<uint8_t>(this->in_flight_);
    out.slot = this->in_flight_;
    out.publish_unavailable = this->on_failure(slot, now_ms);
    return out;
  }

  /// Report a validated positive response for the slot. Clears the failure streak and the
  /// backoff, schedules the next poll one interval out. Returns true when the group had been
  /// flagged unavailable — the caller republishes real values exactly once on recovery.
  bool on_success(uint8_t slot, uint32_t now_ms) {
    if (slot >= this->count_)
      return false;
    if (this->in_flight_ == slot)
      this->in_flight_ = SCHED_NONE;
    PollEntry &e = this->entries_[slot];
    e.consecutive_failures = 0;
    e.backoff_ms = 0;
    e.next_due_ms = now_ms + e.interval_ms;
    const bool recovered = (e.flags & POLL_FLAG_UNAVAILABLE) != 0;
    e.flags &= static_cast<uint8_t>(~POLL_FLAG_UNAVAILABLE);
    return recovered;
  }

  /// Report a failure (timeout, transport error, busy NRC). The retry delay doubles from the
  /// group interval and is capped at 60 s: 2x, 4x, 8x ... interval, then the cap; on_success
  /// resets it. Returns true exactly when this failure crosses max_consecutive_failures — the
  /// caller publishes unavailable once, and polling continues (backed off) so recovery is
  /// possible without anyone's intervention.
  bool on_failure(uint8_t slot, uint32_t now_ms) {
    if (slot >= this->count_)
      return false;
    if (this->in_flight_ == slot)
      this->in_flight_ = SCHED_NONE;
    PollEntry &e = this->entries_[slot];
    if (e.consecutive_failures < 0xFF)
      e.consecutive_failures++;
    // Seed with the interval (floored at 1 ms so a 0 interval still escapes), then double with
    // each consecutive failure. The cap comparison happens before the doubling so the arithmetic
    // cannot overflow — intervals are < 2^31 by add()'s guard.
    uint32_t backoff = e.backoff_ms != 0 ? e.backoff_ms : (e.interval_ms != 0 ? e.interval_ms : 1);
    backoff = backoff > SCHED_BACKOFF_CAP_MS / 2 ? SCHED_BACKOFF_CAP_MS : backoff * 2;
    e.backoff_ms = backoff;
    e.next_due_ms = now_ms + backoff;
    if ((e.flags & POLL_FLAG_UNAVAILABLE) == 0 && e.consecutive_failures >= this->max_failures_) {
      e.flags |= POLL_FLAG_UNAVAILABLE;
      return true;
    }
    return false;
  }

  /// NRC 0x31: the ECU does not implement this group. Stop polling it permanently — continuing
  /// is how a diagnostic client makes itself a nuisance on a bus (design §4).
  void suspend(uint8_t slot) {
    if (slot >= this->count_)
      return;
    if (this->in_flight_ == slot)
      this->in_flight_ = SCHED_NONE;
    this->entries_[slot].flags |= POLL_FLAG_SUSPENDED;
  }

 protected:
  PollEntry *entries_{nullptr};
  /// Only meaningful while in_flight_ != SCHED_NONE; every path that clears in-flight abandons it.
  uint32_t deadline_ms_{0};
  int16_t in_flight_{SCHED_NONE};
  uint8_t capacity_{0};
  uint8_t count_{0};
  uint8_t max_failures_{3};
};

}  // namespace esphome::uds
