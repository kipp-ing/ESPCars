#pragma once

/// F2c — a file torn by a power cut is sealed by the next boot (docs/sd_logger-spec.md F2c).
///
/// The writer always holds one file OPEN, so a hard power-off — the normal end of a run in a car —
/// leaves it without `#close`. The boot scan lists every `.LOG` as SEALED, and a collector must
/// refuse a SEALED chunk without `#close`, because over HTTP that can only mean short bytes. So the
/// boot read (clock spec C5) also classifies the tail of each `.LOG` it reads and, for a torn one,
/// cuts it back to its last complete line and appends `#close,@<t>,recovered`.
///
/// ESPHome- and IDF-free like `clock_basis.h`: a seal that cuts at the wrong byte or writes into a
/// file that already ended cleanly produces a plausible file, and nothing on the bench would notice.
/// The classifier streams the same bytes the clock scan reads (no second pass), and the seal goes
/// through a three-call I/O interface so the host can fail each step.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "clock_basis.h"
#include "log_format.h"

namespace esphome {
namespace sd_logger {

/// The `#close` reason the boot writes for a torn file. Never written by the writer itself.
static const char SD_LOG_CLOSE_RECOVERED[] = "recovered";

/// R4: the one append per file is at most this long. `#close,@` + 16 hex digits + `,recovered\n`
/// is 35 bytes; the bound is what the caller's stack buffer is sized to.
static const size_t SD_LOG_SEAL_LINE_MAX = 64;

/// How much of a `.LOG` the boot reads first when the clock scan does not need the file: enough to
/// see a clean `#close` (one line) behind any NUL or garbage tail a cut can leave in one sector. Only
/// a file that does not end in `#close` is then read over the full C5 window for its stamp.
static const uint32_t SD_LOG_SEAL_PROBE_BYTES = 1024;

/// The `#close` line F2c appends: `#close,@<t>,recovered\n`. Returns the bytes written, newline
/// included; 0 if `room` is too small.
inline size_t format_recovered_close(char *out, size_t room, uint64_t t_us) {
  return format_close(out, room, t_us, SD_LOG_CLOSE_RECOVERED);
}

enum class TailKind : uint8_t {
  CLOSED,   ///< the last complete line is `#close`: never written (R2)
  TORN,     ///< seal it: cut() and stamp() are valid
  NO_CUT,   ///< the window starts mid-file and holds no line boundary to cut at — left as it is
  READ_ERR  ///< the read failed part-way, so nothing about the end is known — left as it is
};

/// Streams the tail window of one file and decides how it ends. Only the start of each line is kept
/// (enough for a meta token and its `@` stamp), so it costs a few dozen bytes on whichever stack runs
/// the boot read, not a line buffer.
///
/// The rules, all taken from the spec rather than invented here:
///  * the last line is the last **complete** one — everything after the last `\n` is a torn line or
///    the NUL padding a cut can leave (F1e), and is what the seal drops;
///  * a window that starts mid-file starts mid-line, so its first partial line is skipped (as in
///    ClockTailScanner) — its newline still counts as a cut point, but it is never "the last line",
///    because its start was not read;
///  * `stamp()` is the newest `@`-timed meta line kept (C4: `#close`, `#rotate`, `#drop`, `#gap`,
///    `#clock`), restarted at a `#sdlog` header, because a header starts a new boot-time domain.
///    Records are deliberately not counted, so the appended line projects exactly as the file did
///    before it was sealed and the next boot's clock floor (C4) does not move.
class TornTailScanner {
 public:
  /// `window_start` is the file offset of the first byte fed. Nonzero means mid-file.
  explicit TornTailScanner(uint64_t window_start = 0)
      : pos_(window_start), window_start_(window_start), skipping_(window_start > 0) {}

  void feed(const char *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
      const char c = data[i];
      this->pos_++;
      if (c == '\n') {
        this->end_line_();
        continue;
      }
      if (this->skipping_)
        continue;
      if (this->len_ < sizeof(this->head_))
        this->head_[this->len_] = c;
      else
        this->overlong_ = true;
      this->len_++;
    }
  }

  /// The read stopped short (an I/O error): what was fed is not the end of the file.
  void read_failed() { this->read_failed_ = true; }

  TailKind kind() const {
    if (this->read_failed_)
      return TailKind::READ_ERR;
    if (!this->have_nl_) {
      // No line boundary at all. From the start of the file that is a file of one torn line (or of
      // nothing): the cut is offset 0. From mid-file it is a window of garbage with no way to tell
      // where the last good line ended.
      return this->window_start_ == 0 ? TailKind::TORN : TailKind::NO_CUT;
    }
    if (!this->last_known_)
      return TailKind::NO_CUT;  // the only newline ended the skipped partial line
    return this->last_closed_ ? TailKind::CLOSED : TailKind::TORN;
  }

  /// The file offset just after the last `\n` — the length the seal truncates to.
  uint64_t cut() const { return this->have_nl_ ? this->cut_ : 0; }
  /// The offset one past the last byte fed: the file's size when the window ran to its end.
  uint64_t end() const { return this->pos_; }
  /// The newest `@` stamp among the complete meta lines kept, 0 if there is none (R1: `@0`).
  uint64_t stamp() const { return this->stamp_; }

 private:
  void end_line_() {
    this->have_nl_ = true;
    this->cut_ = this->pos_;
    if (this->skipping_) {
      // The partial first line of a mid-file window: a cut point, but not a line we can judge.
      this->skipping_ = false;
      this->last_known_ = false;
      this->reset_line_();
      return;
    }
    this->last_known_ = true;
    const size_t n = this->len_ < sizeof(this->head_) ? this->len_ : sizeof(this->head_);
    this->last_closed_ = n >= 7 && std::memcmp(this->head_, "#close,", 7) == 0;
    if (n >= 7 && std::memcmp(this->head_, "#sdlog,", 7) == 0) {
      this->stamp_ = 0;  // a new boot-time domain: nothing above projects into it
    } else if (n > 0 && this->head_[0] == '#') {
      // Only a stamp field the head holds whole counts: a line longer than the head is parsed only
      // when the field's closing comma is inside it, never as a number cut off mid-digit.
      bool whole = true;
      if (this->overlong_) {
        const char *first = static_cast<const char *>(std::memchr(this->head_, ',', n));
        const size_t rest = first == nullptr ? 0 : n - static_cast<size_t>(first + 1 - this->head_);
        whole = first != nullptr && std::memchr(first + 1, ',', rest) != nullptr;
      }
      uint64_t t = 0;
      if (whole && parse_meta_stamp(this->head_, n, &t) && t > this->stamp_)
        this->stamp_ = t;
    }
    this->reset_line_();
  }

  void reset_line_() {
    this->len_ = 0;
    this->overlong_ = false;
  }

  // `#rotate,@` + 16 hex + `,` is 26 bytes; 48 leaves room for any meta token this format has.
  char head_[48];
  size_t len_{0};
  bool overlong_{false};
  uint64_t pos_;
  uint64_t window_start_;
  bool skipping_;
  bool have_nl_{false};
  bool last_known_{false};
  bool last_closed_{false};
  bool read_failed_{false};
  uint64_t cut_{0};
  uint64_t stamp_{0};
};

/// The outcome of one file's seal attempt, in the order the steps run. Every value but CLOSED and
/// SEALED leaves the file unsealed (R3): it is indexed as it is and counted as `torn_unsealed`.
enum class SealResult : uint8_t {
  CLOSED,        ///< ends in `#close` — not written (R2)
  SEALED,        ///< truncated, `#close,…,recovered` appended, fsynced
  NO_CUT,        ///< no line boundary in the window — not written
  READ_FAILED,   ///< the tail could not be read — not written
  OPEN_FAILED,   ///< the file could not be opened for writing — not written
  TRUNC_FAILED,  ///< truncate failed — the file is as it was, or cut back to `cut()`
  WRITE_FAILED,  ///< the `#close` line did not go out whole — the next boot finds it torn again
  SYNC_FAILED,   ///< fsync failed — the line may or may not be on the card
};

inline const char *seal_result_str(SealResult r) {
  switch (r) {
    case SealResult::CLOSED:
      return "closed";
    case SealResult::SEALED:
      return "sealed";
    case SealResult::NO_CUT:
      return "no line boundary in the tail window";
    case SealResult::READ_FAILED:
      return "read failed";
    case SealResult::OPEN_FAILED:
      return "open for write failed";
    case SealResult::TRUNC_FAILED:
      return "truncate failed";
    case SealResult::WRITE_FAILED:
      return "write failed";
    case SealResult::SYNC_FAILED:
      return "fsync failed";
  }
  return "?";
}

/// Seals the file `tail` describes, if it is torn. `Io` provides four calls, each `true` on success:
///
///     bool open();                                     // for writing, never creating, never truncating
///     bool truncate(uint64_t length);
///     bool write_at(uint64_t offset, const char *data, size_t n);  // false on a short write too
///     bool sync();
///
/// Exactly one truncate and one append (R4), and nothing at all for a file that is not TORN (R2).
/// The order is what makes a failure at any step safe to retry on the next boot: a truncate alone
/// leaves a file that still has no `#close`, and a short write leaves a torn `#close` line, which
/// the next boot's scanner treats like any other torn line and cuts away.
template<typename Io> SealResult seal_torn_tail(Io &io, const TornTailScanner &tail) {
  switch (tail.kind()) {
    case TailKind::CLOSED:
      return SealResult::CLOSED;
    case TailKind::NO_CUT:
      return SealResult::NO_CUT;
    case TailKind::READ_ERR:
      return SealResult::READ_FAILED;
    case TailKind::TORN:
      break;
  }
  char line[SD_LOG_SEAL_LINE_MAX];
  const size_t n = format_recovered_close(line, sizeof(line), tail.stamp());
  if (!io.open())
    return SealResult::OPEN_FAILED;
  if (!io.truncate(tail.cut()))
    return SealResult::TRUNC_FAILED;
  if (!io.write_at(tail.cut(), line, n))
    return SealResult::WRITE_FAILED;
  if (!io.sync())
    return SealResult::SYNC_FAILED;
  return SealResult::SEALED;
}

}  // namespace sd_logger
}  // namespace esphome
