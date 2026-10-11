#pragma once

/// The wall-clock basis of a file (docs/sd_logger-clock-spec.md): which clock the device trusts at
/// boot, the `#clock` meta line that states it, and the bounded tail scan that recovers the previous
/// run's last known UTC from the card.
///
/// ESPHome-free and IDF-free for the same reason as `utc_anchor.h`: every mistake here produces a
/// plausible timestamp. A floor that is a few seconds too late, an estimate that quietly passes for
/// a reading, a torn line read as a time — none of them fails a bench run. The host suite is where
/// they are caught.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "log_format.h"

namespace esphome {
namespace sd_logger {

/// How good a stated time is. Ordered: a greater value is a better basis, and a later line only
/// overrides an earlier one of the same or a worse basis (spec §4, reading rule).
enum class ClockBasis : uint8_t {
  NONE = 0,
  EST = 1,    ///< a lower bound: the true time is at least this (card floor, build time)
  DRIFT = 2,  ///< a reading that ran unsupervised through deep sleep; error of either sign
  UTC = 3,    ///< a reading: synced this boot, or kept through a warm reset
};

enum class ClockSrc : uint8_t { NONE = 0, SYNC, WARM, SLEEP, CARD, BUILD };

/// How the chip came up. Only the logger's own reading of `esp_reset_reason()`: POWER_ON clears the
/// RTC timer, DEEP_SLEEP keeps it on the slow clock, OTHER (software restart, panic, watchdog,
/// brownout) may keep it — and the plausibility check, not the label, decides whether it did.
enum class WakeKind : uint8_t { POWER_ON = 0, DEEP_SLEEP, OTHER };

/// One correspondence between boot time and UTC, plus how much it is worth. Like `#utc`, it is an
/// anchor pair: `utc(b) = utc_us + (b - t_us)`.
struct ClockPoint {
  uint64_t t_us{0};    ///< boot µs (the reconstructed 64-bit domain of `reconstruct_us()`)
  uint64_t utc_us{0};  ///< UTC µs at `t_us`
  ClockBasis basis{ClockBasis::NONE};
  ClockSrc src{ClockSrc::NONE};
  bool has_detail{false};
  uint64_t detail{0};  ///< CARD: the seq the floor came from. SLEEP: seconds slept.

  bool valid() const { return this->basis != ClockBasis::NONE; }
};

/// Tolerance for "before": two independent clocks sampled at slightly different moments, one of
/// them possibly second-resolution (ESPHome's `synchronize_epoch_()` sets `tv_usec = 0`).
static const uint64_t SD_LOG_CLOCK_SLACK_US = 2000000;

/// Restatement cadence (spec §4): whichever bound fires first. The byte bound is what lets the boot
/// scan stop at `SD_LOG_CLOCK_TAIL_BYTES`; the time bound is what keeps a quiet bus's floor fresh.
static const uint32_t SD_LOG_CLOCK_RESTATE_BYTES = 32 * 1024;
static const uint64_t SD_LOG_CLOCK_RESTATE_US = 10000000;

/// The boot scan: this much off the end of each file, at most this many files (C5). Twice the byte
/// bound, so a restatement is always inside the window even when one pass overshoots the bound.
static const uint32_t SD_LOG_CLOCK_TAIL_BYTES = 2 * SD_LOG_CLOCK_RESTATE_BYTES;
static const uint8_t SD_LOG_CLOCK_TAIL_FILES = 3;

/// A system clock below this is not a clock: it is 1970 plus uptime, which is what the RTC timer
/// reads after a power-on reset. 2024-01-01T00:00:00Z. Also the floor when no build time is known.
static const uint64_t SD_LOG_CLOCK_MIN_EPOCH_US = 1704067200ull * 1000000ull;

/// `utc(boot_us)` through a point, saturating at both ends like `resolve_utc_us()`.
inline uint64_t clock_project(const ClockPoint &p, uint64_t boot_us) {
  if (boot_us >= p.t_us) {
    const uint64_t delta = boot_us - p.t_us;
    return delta > std::numeric_limits<uint64_t>::max() - p.utc_us ? std::numeric_limits<uint64_t>::max()
                                                                     : p.utc_us + delta;
  }
  const uint64_t delta = p.t_us - boot_us;
  return delta > p.utc_us ? 0 : p.utc_us - delta;
}

/// The same correspondence restated at another boot time. Exact: projection is a shift.
inline ClockPoint clock_at(const ClockPoint &p, uint64_t boot_us) {
  ClockPoint out = p;
  out.t_us = boot_us;
  out.utc_us = clock_project(p, boot_us);
  return out;
}

inline const char *clock_basis_str(ClockBasis basis) {
  switch (basis) {
    case ClockBasis::EST:
      return "est";
    case ClockBasis::DRIFT:
      return "drift";
    case ClockBasis::UTC:
      return "utc";
    default:
      return "none";
  }
}

inline const char *clock_src_str(ClockSrc src) {
  switch (src) {
    case ClockSrc::SYNC:
      return "sync";
    case ClockSrc::WARM:
      return "warm";
    case ClockSrc::SLEEP:
      return "sleep";
    case ClockSrc::CARD:
      return "card";
    case ClockSrc::BUILD:
      return "build";
    default:
      return "none";
  }
}

/// `#clock,@<t_hex>,<utc_us_hex>,<basis>,<src>[,<detail>]` (spec §4). `t` is anchor-marked like
/// every timed meta line and stays outside the delta chain; `utc_us` is a bare value like `#utc`'s.
inline size_t format_clock(char *out, size_t room, const ClockPoint &p) {
  if (room < SD_LOG_MIN_ROOM || !p.valid())
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#clock,");
  w.stamp(/*anchor=*/true, p.t_us);
  w.comma();
  w.u64_hex(p.utc_us);
  w.comma();
  w.str(clock_basis_str(p.basis));
  w.comma();
  w.str(clock_src_str(p.src));
  if (p.has_detail) {
    w.comma();
    w.u64(p.detail);
  }
  return w.finish();
}

// ---------------------------------------------------------------------------------------------
// Parsing — strict, because a lenient parser here turns a torn or garbled line into a time
// ---------------------------------------------------------------------------------------------

namespace clock_detail {

/// Uppercase hex only, 1..16 digits — exactly what `u64_hex()` can produce.
inline bool parse_hex(const char *s, size_t n, uint64_t *out) {
  if (n == 0 || n > 16)
    return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    uint8_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<uint8_t>(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<uint8_t>(c - 'A' + 10);
    } else {
      return false;
    }
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

inline bool parse_dec(const char *s, size_t n, uint64_t *out) {
  if (n == 0 || n > 20)
    return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return false;
    const uint64_t d = static_cast<uint64_t>(s[i] - '0');
    if (v > (std::numeric_limits<uint64_t>::max() - d) / 10)
      return false;
    v = v * 10 + d;
  }
  *out = v;
  return true;
}

inline bool field_is(const char *s, size_t n, const char *word) {
  return std::strlen(word) == n && std::memcmp(s, word, n) == 0;
}

/// Split `s[0..n)` on commas into at most `max` fields. Returns the field count, or `max + 1` when
/// there are more — a line with a field too many is not a line this parser understands.
inline size_t split(const char *s, size_t n, const char **starts, size_t *lens, size_t max) {
  size_t count = 0;
  size_t begin = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || s[i] == ',') {
      if (count == max)
        return max + 1;
      starts[count] = s + begin;
      lens[count] = i - begin;
      count++;
      begin = i + 1;
    }
  }
  return count;
}

}  // namespace clock_detail

/// One `#clock` line, without its terminator. False for anything `format_clock()` cannot have
/// written — including a detail on a source that never carries one.
inline bool parse_clock_line(const char *s, size_t n, ClockPoint *out) {
  using namespace clock_detail;
  const char *f[7];
  size_t l[7];
  const size_t count = split(s, n, f, l, 6);
  if (count != 5 && count != 6)
    return false;
  if (!field_is(f[0], l[0], "#clock") || l[1] < 2 || f[1][0] != SD_LOG_ANCHOR_MARK)
    return false;
  ClockPoint p;
  if (!parse_hex(f[1] + 1, l[1] - 1, &p.t_us) || !parse_hex(f[2], l[2], &p.utc_us))
    return false;
  if (field_is(f[3], l[3], "utc")) {
    p.basis = ClockBasis::UTC;
  } else if (field_is(f[3], l[3], "drift")) {
    p.basis = ClockBasis::DRIFT;
  } else if (field_is(f[3], l[3], "est")) {
    p.basis = ClockBasis::EST;
  } else {
    return false;
  }
  // The pairing is part of the format: a `sync` that claims to be an estimate is not a line the
  // writer produces, and believing either half of it would be a guess.
  if (field_is(f[4], l[4], "sync") && p.basis == ClockBasis::UTC) {
    p.src = ClockSrc::SYNC;
  } else if (field_is(f[4], l[4], "warm") && p.basis == ClockBasis::UTC) {
    p.src = ClockSrc::WARM;
  } else if (field_is(f[4], l[4], "sleep") && p.basis == ClockBasis::DRIFT) {
    p.src = ClockSrc::SLEEP;
  } else if (field_is(f[4], l[4], "card") && p.basis == ClockBasis::EST) {
    p.src = ClockSrc::CARD;
  } else if (field_is(f[4], l[4], "build") && p.basis == ClockBasis::EST) {
    p.src = ClockSrc::BUILD;
  } else {
    return false;
  }
  if (count == 6) {
    if (p.src != ClockSrc::CARD && p.src != ClockSrc::SLEEP)
      return false;
    if (!parse_dec(f[5], l[5], &p.detail))
      return false;
    p.has_detail = true;
  } else if (p.src == ClockSrc::CARD) {
    return false;  // a card floor always names the file it came from
  }
  *out = p;
  return true;
}

/// One `#utc` line, without its terminator — the clock statement of files written before `#clock`
/// existed. It is a reading by definition: the writer only ever emitted it after a sync.
inline bool parse_utc_line(const char *s, size_t n, ClockPoint *out) {
  using namespace clock_detail;
  const char *f[4];
  size_t l[4];
  if (split(s, n, f, l, 3) != 3 || !field_is(f[0], l[0], "#utc"))
    return false;
  ClockPoint p;
  if (!parse_hex(f[1], l[1], &p.t_us) || !parse_hex(f[2], l[2], &p.utc_us))
    return false;
  p.basis = ClockBasis::UTC;
  p.src = ClockSrc::SYNC;
  *out = p;
  return true;
}

/// The boot-time stamp of a timed meta line (`#close,@…`, `#rotate,@…`, `#drop,@…`, `#gap,@…`):
/// field 1, anchor-marked. False for every other line.
inline bool parse_meta_stamp(const char *s, size_t n, uint64_t *t_out) {
  if (n < 4 || s[0] != '#')
    return false;
  const char *comma = static_cast<const char *>(std::memchr(s, ',', n));
  if (comma == nullptr)
    return false;
  const char *field = comma + 1;
  const size_t rest = n - static_cast<size_t>(field - s);
  if (rest < 2 || field[0] != SD_LOG_ANCHOR_MARK)
    return false;
  const char *end = static_cast<const char *>(std::memchr(field, ',', rest));
  const size_t len = end == nullptr ? rest : static_cast<size_t>(end - field);
  return clock_detail::parse_hex(field + 1, len - 1, t_out);
}

/// Streams the tail of one file and recovers when it ended on the wall clock (C4).
///
/// Only whole lines count. A window that starts mid-file starts mid-line, so everything up to the
/// first newline is skipped; a file cut by a power loss ends mid-line, and a line is only acted on
/// once its newline arrives, so the torn tail is never seen. A line longer than the format allows
/// is skipped whole rather than read as its first 512 bytes.
class ClockTailScanner {
 public:
  explicit ClockTailScanner(bool starts_mid_file) : skipping_(starts_mid_file) {}

  void feed(const char *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
      const char c = data[i];
      if (c == '\n') {
        if (!this->skipping_ && !this->overlong_)
          this->line_(this->buf_, this->len_);
        this->skipping_ = false;
        this->overlong_ = false;
        this->len_ = 0;
        continue;
      }
      if (this->skipping_ || this->overlong_)
        continue;
      if (this->len_ == sizeof(this->buf_)) {
        this->overlong_ = true;
        continue;
      }
      this->buf_[this->len_++] = c;
    }
  }

  /// The newest clock statement, restated at the latest timed line after it. False when the window
  /// held none — an old file, or a window of nothing but records.
  bool end(ClockPoint *out) const {
    if (!this->clock_.valid())
      return false;
    *out = clock_at(this->clock_, this->max_t_);
    return true;
  }

 private:
  void line_(const char *s, size_t n) {
    if (n == 0 || s[0] != '#')
      return;
    ClockPoint p;
    if (n >= 7 && std::memcmp(s, "#sdlog,", 7) == 0) {
      // A header: a new file — or, if a seq was ever reused and appended to, a new boot whose
      // boot-time domain has nothing to do with the lines above. Nothing before it projects.
      this->clock_ = ClockPoint{};
      this->max_t_ = 0;
      return;
    }
    const bool clock_line = n >= 7 && std::memcmp(s, "#clock,", 7) == 0;
    const bool utc_line = n >= 5 && std::memcmp(s, "#utc,", 5) == 0;
    if (clock_line || utc_line) {
      // A garbled clock line is ignored whole: its stamp field may still parse, but nothing on a
      // line that failed to parse is evidence of anything.
      if (clock_line ? parse_clock_line(s, n, &p) : parse_utc_line(s, n, &p)) {
        this->clock_ = p;
        this->max_t_ = p.t_us;
      }
      return;
    }
    uint64_t t = 0;
    if (this->clock_.valid() && parse_meta_stamp(s, n, &t) && t > this->max_t_)
      this->max_t_ = t;
  }

  char buf_[SD_LOG_MAX_LINE];
  size_t len_{0};
  bool skipping_;
  bool overlong_{false};
  ClockPoint clock_{};
  uint64_t max_t_{0};
};

// ---------------------------------------------------------------------------------------------
// The boot decision (spec §3, C1/C3)
// ---------------------------------------------------------------------------------------------

struct BootClockInputs {
  uint64_t now_boot_us{0};  ///< esp_timer, sampled next to `sys_utc_us`
  uint64_t sys_utc_us{0};   ///< gettimeofday(): whatever the RTC timer kept, or 1970 + uptime
  WakeKind wake{WakeKind::POWER_ON};
  /// The system clock has run through a deep sleep since its last sync (kept in RTC memory, which
  /// survives deep sleep and every reset but power-on). A warm reset does not wash that drift out.
  bool sys_drifted{false};
  uint64_t build_utc_us{0};  ///< App.get_build_time(), 0 when unknown
  bool card_valid{false};
  ClockPoint card_end{};  ///< ClockTailScanner::end() of the newest file that had one
  uint32_t card_seq{0};
};

struct BootClockDecision {
  ClockPoint best;   ///< what the header states
  ClockPoint floor;  ///< T3/T4 at boot µs 0, kept for C8 even when T2 wins
  bool sys_rejected{false};  ///< a non-power-on boot whose system clock failed C1
};

inline BootClockDecision decide_boot_clock(const BootClockInputs &in) {
  BootClockDecision out;

  // T3/T4, C3: the previous run's last line was written before this boot powered up, so at boot
  // µs 0 the wall clock was at least that. Projecting through t=0 adds exactly this boot's uptime —
  // the "+x" — and nothing guessed.
  const uint64_t build = in.build_utc_us;
  const uint64_t card = in.card_valid ? in.card_end.utc_us : 0;
  if (in.card_valid && card >= build) {
    out.floor.utc_us = card;
    out.floor.src = ClockSrc::CARD;
    out.floor.has_detail = true;
    out.floor.detail = in.card_seq;
  } else if (build != 0) {
    out.floor.utc_us = build;
    out.floor.src = ClockSrc::BUILD;
  }
  if (out.floor.src != ClockSrc::NONE) {
    out.floor.t_us = 0;
    out.floor.basis = ClockBasis::EST;
  }

  // T2, C1: a clock is only a clock if it is past both what the firmware and the card prove.
  const uint64_t min_epoch = build > SD_LOG_CLOCK_MIN_EPOCH_US ? build : SD_LOG_CLOCK_MIN_EPOCH_US;
  const uint64_t floor_now = out.floor.valid() ? clock_project(out.floor, in.now_boot_us) : 0;
  const bool plausible = in.sys_utc_us >= min_epoch && in.sys_utc_us + SD_LOG_CLOCK_SLACK_US >= floor_now;
  if (in.wake != WakeKind::POWER_ON && plausible) {
    out.best.t_us = in.now_boot_us;
    out.best.utc_us = in.sys_utc_us;
    if (in.wake == WakeKind::DEEP_SLEEP || in.sys_drifted) {
      out.best.basis = ClockBasis::DRIFT;
      out.best.src = ClockSrc::SLEEP;
      // Slept = this boot's µs 0 on the device's own clock, minus the instant the last file says it
      // stopped. Only stated for an actual wake, when the card had a clock to measure from and the
      // answer is forward; a warm reset after a wake carries the drift but slept nothing.
      const uint64_t boot0 = clock_project(out.best, 0);
      if (in.wake == WakeKind::DEEP_SLEEP && in.card_valid && boot0 >= card) {
        out.best.has_detail = true;
        out.best.detail = (boot0 - card) / 1000000ull;
      }
    } else {
      out.best.basis = ClockBasis::UTC;
      out.best.src = ClockSrc::WARM;
    }
    return out;
  }
  out.sys_rejected = in.wake != WakeKind::POWER_ON;
  out.best = out.floor;
  return out;
}

/// C8: a real clock that lands before the floor. Accepted all the same — the caller counts it.
inline bool clock_contradicts_floor(const ClockPoint &floor, uint64_t utc_us, uint64_t boot_us) {
  if (!floor.valid())
    return false;
  const uint64_t floor_now = clock_project(floor, boot_us);
  return utc_us + SD_LOG_CLOCK_SLACK_US < floor_now;
}

/// When the restatement is owed (spec §4 item 2).
inline bool clock_restate_due(uint32_t bytes_since, uint64_t now_us, uint64_t last_us) {
  return bytes_since >= SD_LOG_CLOCK_RESTATE_BYTES || (now_us >= last_us && now_us - last_us >= SD_LOG_CLOCK_RESTATE_US);
}

}  // namespace sd_logger
}  // namespace esphome
