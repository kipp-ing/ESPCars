#pragma once

/// The chunk lifecycle, the retention rule and the rotation bounds that make collection possible
/// (docs/sdlog-collection-design.md §3 "Chunk lifecycle", §4 "Rotation", §7 "Retention").
///
/// ESPHome-free and IDF-free, like `log_format.h` and `recovery_policy.h`, and for exactly the same
/// reason: every interesting case in here costs *hours of a filling card* to reach on the bench and
/// a microsecond to reach in `tests/host/test_collection_policy.cpp`. A retention rule that picks
/// the wrong victim deletes a chunk nobody collected; a confirm that lands on the open file renames
/// the one the writer is still holding an fd to; a serving guard checked one line too late unlinks
/// a file mid-transfer. None of those leave evidence — the file is simply gone, and the only
/// witness would have been the `#gap` line the same buggy path failed to emit.
///
/// The three tracked states and the one ordering rule this header exists to keep true (§3):
///
///     OPEN       L#######.LOG, writer holds the fd    never listed, never served, never deleted
///     SEALED     L#######.LOG, closed with #close     servable
///     CONFIRMED  L#######.UPL, the collector acked    deletable
///
///     serve bytes  ->  puller confirms  ->  rename to .UPL
///
/// Only that order is safe. A crash anywhere inside it re-serves a chunk, which is correct — the
/// puller dedups on `(device, seq)`. The inverse order (rename first, serve later) loses the chunk
/// permanently on the same crash.
///
/// Nothing here touches a filesystem. §8 gives every mutation (open, append, rotate, rename,
/// unlink) to the writer task; this class is the bookkeeping that task consults, so the httpd task
/// can post intents without ever holding an opinion about what is on the card.
///
/// **Index capacity is a known, deliberate limit.** `SD_LOG_MAX_CHUNKS` entries is not a full card:
/// a 32 GB card at the 4 MB chunk size §4 targets holds ~8000 chunks, and the default 256 entries
/// index only ~1 GB of it. `add()` therefore *refuses* when full rather than evicting — forgetting
/// a file that still exists is the one failure retention cannot recover from, because that chunk is
/// then never listed, never served and never deleted while the card fills behind it.
///
/// The default was 64 until a 12-minute PERF soak on the bench (2026-07-28) rotated 29 times and
/// found the index **already full at the first rotation**: every chunk of that run was untracked,
/// retention could reclaim nothing, and the only witness was one `chunk index full` warning that
/// scrolls past. At the 4 MB / 60 s bounds that run used, a chunk lands every ~25 s, so 64 entries
/// was ~27 minutes of cumulative logging — less than a drive to work. 256 entries is ~1.8 hours and
/// costs 3 KB (12 B an entry, below) against the 1.5 KB the old default spent. Refusals are now
/// counted and printed in sd_logger's periodic stats line (`index_refused=`), so a run that outgrows
/// its index says so every stats interval rather than once.
///
/// Two ways further out: raise the macro from the build — `collection: max_chunks:` does exactly
/// that, and even the 2048 the schema allows is only 24 KB of the ~100–200 KB free — or have the
/// writer re-scan the directory and rebuild the index in seq order each retention pass so the set it
/// holds is always the *oldest* window rather than the first one the boot scan happened to see. The
/// second is the honest fix for a card that outruns any fixed index, and is a decision about SD
/// access cost, not about this header.

#include <cstddef>
#include <cstdint>

#include "log_format.h"

namespace esphome {
namespace sd_logger {

/// The filename carries 7 digits, so the sequence space is [0, 9999999] and it *wraps* — at ~4 MB a
/// chunk that is 40 TB of logging away, but the wrap is one modulo and the alternative is an
/// ordering that silently inverts for one boot somewhere in the far future.
static const uint32_t SD_LOG_SEQ_MODULUS = 10000000u;
static const uint32_t SD_LOG_SEQ_MAX = SD_LOG_SEQ_MODULUS - 1u;

/// How many chunks the in-RAM index holds. Static, like every other allocation in this component.
/// Overridable from the build — `collection: max_chunks:` emits this define from the component's
/// Python, the same way `SD_LOG_MAX_SOURCES` is sized. `add()` refuses rather than evicting when it
/// is reached, because silently forgetting a chunk means silently never deleting its file.
#ifndef SD_LOG_MAX_CHUNKS
#define SD_LOG_MAX_CHUNKS 256
#endif

/// `count_` is a uint16_t and the victim folds are linear, so the index can be grown by a couple of
/// orders of magnitude from the build but not beyond a 16-bit count. The *config-time* range is much
/// tighter than this ([16, 2048], see `__init__.py`): this bound says what the data structure can
/// represent, not what fits in the RAM of a C6 — 65535 entries would be 768 KB and the schema is
/// what keeps a typo from asking for it.
static_assert(SD_LOG_MAX_CHUNKS > 0 && SD_LOG_MAX_CHUNKS <= 65535, "SD_LOG_MAX_CHUNKS must be in [1, 65535]");

/// How far apart two sequence numbers may be before the *wrap* reading of their order stops being
/// offered — see `seq_before()`, which is the only thing that uses it.
///
/// **Scales with the index, and 256 is a floor rather than the value.** It has to be at least as
/// wide as the largest gap the index can hold between two live entries, and the tempting bound —
/// "a full index spans at most `SD_LOG_MAX_CHUNKS - 1` seqs" — is not true. Once `add()` starts
/// refusing at capacity, the chunks it turned away still get written and still consume sequence
/// numbers; a later `discard()` frees a slot, and the seq that fills it can be arbitrarily far
/// above the oldest entry still held. `next_victim()` skipping a chunk with a transfer in flight
/// leaves the same kind of hole. So the index is a sparse window over the sequence space, not a
/// contiguous run, and sizing this to the capacity is the honest approximation, not a proof.
///
/// Being too NARROW is the only direction that can misorder anything, and only for a pair that
/// straddles the wrap: away from it, a pair outside the window falls through to the magnitude
/// comparison, which is already correct. Widening therefore costs nothing and is what a raised
/// `collection: max_chunks:` gets. The default (256 entries) leaves this at exactly the 256 it has
/// always been.
static const uint32_t SD_LOG_SEQ_WRAP_WINDOW = SD_LOG_MAX_CHUNKS > 256u ? (uint32_t) SD_LOG_MAX_CHUNKS : 256u;

enum class ChunkState : uint8_t {
  NONE = 0,   ///< not a chunk at all: an unparseable name, or "no entry".
  OPEN,       ///< the writer is appending to it. Not listed, not servable, never a victim.
  SEALED,     ///< `#close` written and fsynced. The collector may read it.
  CONFIRMED,  ///< renamed to `.UPL`; the puller acknowledged it. Retention may delete it.
};

/// One tracked chunk. `bytes` is the final size for a SEALED/CONFIRMED chunk and the size at the
/// last update for the OPEN one — retention needs it to say how much a `#gap` cost.
///
/// **12 bytes, and `bytes` is deliberately 32-bit.** The index is `SD_LOG_MAX_CHUNKS` of these in
/// static RAM, so the entry size is what decides how much card the index can cover; a 64-bit field
/// made the struct 24 B, half of it alignment padding, for a number that cannot use the range. A
/// chunk is bounded by `max_file_bytes_`, which is itself a `uint32_t`, so no file this component
/// writes reaches 4 GiB.
///
/// The one caller that can present a larger number is the boot scan, which takes `st_size` from
/// `stat()` — and that file may be a stale or foreign one that merely happens to be named
/// `L#######.LOG`. Such a size is **saturated on the way in, never truncated** (`chunk_bytes()`),
/// because saturation is the only narrowing that stays monotone: `min(x, 0xFFFFFFFF)` never orders
/// two sizes the wrong way round, while truncation wraps a 4 GiB file to **0 bytes** and bills a
/// discarded chunk as costing nothing at all. Both narrowings under-state an oversized file; only
/// one of them can under-state it without bound, and by an amount that grows as the file does.
///
/// Note which direction of error this is, because design §7a is the arbiter and it points the
/// other way: **over**-reporting is the failure mode `#gap` must not have, since a line claiming
/// six hours it did not lose sends someone hunting for files that are not missing. Saturation
/// cannot over-report — it is bounded above by the true size — so it is safe on the axis §7a
/// cares about, and merely bounded on the other. No file this component writes can reach the clamp
/// at all; only a foreign one can, and a chunk billed at exactly 4 GiB−1 reads as the anomaly it is.
///
/// The public API still speaks `uint64_t` throughout (`add()`, `add_name()`, `seal()`,
/// `discarded_bytes()`), so callers and the accumulating counters are unchanged: only the per-entry
/// storage shrank. `discarded_bytes_` in particular stays 64-bit — it sums across a whole run.
struct ChunkEntry {
  uint32_t seq;
  uint32_t bytes;
  ChunkState state;
  bool serving;  ///< a transfer is in flight; retention must skip it (§7).
};

/// Narrow a byte count into `ChunkEntry::bytes`, saturating. See the struct's note for why the
/// clamp is not a truncation.
inline uint32_t chunk_bytes(uint64_t bytes) {
  return bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes);
}

/// The `#gap,<t_us>,<chunks>,<bytes>,<first_seq>,<last_seq>` payload — see `format_gap()` in
/// log_format.h. Field types match that signature exactly so the marker pass is a pass-through.
struct DiscardStats {
  uint32_t chunks;
  uint64_t bytes;
  uint32_t first_seq;
  uint32_t last_seq;
};

// ---------------------------------------------------------------------------------------------
// Names (§3)
// ---------------------------------------------------------------------------------------------

/// "Is `a` older than `b`", over a 7-digit sequence space that wraps.
///
/// **Numeric, with a wrap window, and not a plain `a < b`.** The seq increments by exactly one per
/// rotation, so straddling the 9999999 -> 0 ceiling is a *local* event: the only pairs that can sit
/// on either side of it inside one index are near neighbours. So:
///
///   * within `SD_LOG_SEQ_WRAP_WINDOW` of each other, the modular reading wins — 9999999 is older
///     than 0, and 9999990 is older than 9. A plain `a < b` reports the freshly wrapped chunk as the
///     oldest thing on the card and retention then deletes the *newest* data first, once, on one
///     board, with nothing left afterwards to show for it;
///   * further apart than that, the pair cannot be two chunks of one monotonic run either side of
///     the ceiling, so the wrap reading is not available to it and magnitude decides. This half
///     matters as much as the first: a card carrying a stale file from another device, or a
///     directory entry with a garbage seq, must not be read as "ancient" and deleted before real
///     data. A half-the-space comparator (RFC 1982 style) has no such fallback — it calls 9999999
///     older than 433 whether or not a wrap ever happened.
///
/// Strict and irreflexive: `seq_before(x, x)` is false, or a fold over equal seqs picks arbitrarily.
/// Antisymmetric everywhere, transitive only inside a window — like every modular ordering.
///
/// The window tracks the index capacity — see `SD_LOG_SEQ_WRAP_WINDOW` for why that is an
/// approximation and not a proof (a sparse index can hold a pair further apart than its own
/// capacity). The residual is bounded and remote: only a pair straddling the wrap can be misordered
/// at all, everything else falls through to the magnitude comparison, and reaching the wrap costs
/// 40 TB of logging. On that one wrap, retention picks the newest chunk once.
inline bool seq_before(uint32_t a, uint32_t b) {
  const uint32_t aa = a % SD_LOG_SEQ_MODULUS;
  const uint32_t bb = b % SD_LOG_SEQ_MODULUS;
  // Modular distance a -> b, computed without ever letting the uint32 subtraction wrap at 2^32:
  // 2^32 is not a multiple of the modulus, so a wrapped difference is off by 4 294 967 296 % 10^7.
  const uint32_t forward = bb >= aa ? bb - aa : SD_LOG_SEQ_MODULUS - (aa - bb);
  if (forward == 0)
    return false;
  if (forward <= SD_LOG_SEQ_WRAP_WINDOW)
    return true;  // b is just ahead of a, possibly across the ceiling: a is older.
  if (SD_LOG_SEQ_MODULUS - forward <= SD_LOG_SEQ_WRAP_WINDOW)
    return false;  // a is just ahead of b, same reading in the other direction.
  return aa < bb;  // too far apart to be neighbours across the wrap; magnitude decides.
}

/// Parse a chunk filename into its sequence number and the state its *name* implies.
///
/// Accepts `L#######.LOG` (SEALED) and `L#######.UPL` (CONFIRMED), and nothing else. Note what it
/// deliberately does *not* accept, in contrast to `parse_log_seq()` in log_format.h, which is the
/// boot scan's matcher and must take `.CSV` so an M1-era card does not restart the sequence at 0:
/// an M1 file has no `#sdlog` header, no `#close`, and no seq the puller could dedup on, so it is
/// **not a chunk**. Serving it would ship a file no reader understands; deleting it to make room
/// would destroy the only copy of data that predates this design. It is left alone.
///
/// A name alone can never say OPEN — the on-card name of the open file is `.LOG`, identical to a
/// sealed one. Only the writer knows which seq it holds an fd to, and it says so via `add()`.
///
/// On rejection the out-params are left untouched, so a caller that pre-seeded them keeps its
/// value rather than getting a half-parsed one.
inline bool parse_chunk_name(const char *name, uint32_t *seq_out, ChunkState *state_out) {
  if (name == nullptr || name[0] != 'L')
    return false;
  uint32_t seq = 0;
  for (int i = 1; i <= 7; i++) {
    // Also the bounds check: a short name hits its NUL here, which is not a digit, so nothing past
    // the terminator is ever read. Six digits, eight digits and a non-digit all land on this.
    if (name[i] < '0' || name[i] > '9')
      return false;
    seq = seq * 10 + static_cast<uint32_t>(name[i] - '0');
  }
  if (name[8] != '.')
    return false;
  // Compared character by character, short-circuiting, for the same reason: each byte is only read
  // once the previous one proved it is not the terminator. The trailing NUL check is what rejects
  // `.LOGX` and `.UPL.LOG`, and the case sensitivity is deliberate — ESPHome builds FatFs without
  // long names (CONFIG_FATFS_LFN_NONE), so an 8.3 entry of ours is always uppercase.
  ChunkState state;
  if (name[9] == 'L' && name[10] == 'O' && name[11] == 'G' && name[12] == '\0') {
    state = ChunkState::SEALED;
  } else if (name[9] == 'U' && name[10] == 'P' && name[11] == 'L' && name[12] == '\0') {
    state = ChunkState::CONFIRMED;
  } else {
    return false;
  }
  if (seq_out != nullptr)
    *seq_out = seq;
  if (state_out != nullptr)
    *state_out = state;
  return true;
}

/// Render the on-card name for `seq` in `state`: `.LOG` for OPEN and SEALED, `.UPL` for CONFIRMED.
/// `out` needs SD_LOG_NAME_LEN bytes. ChunkState::NONE writes an empty string.
///
/// This is the other half of the rename: the writer formats the source name from the SEALED state
/// and the destination from CONFIRMED, so the 8.3 rule lives in one place (`format_log_name()`)
/// rather than being open-coded at the one call site that matters.
inline void format_chunk_name(char *out, uint32_t seq, ChunkState state) {
  if (out == nullptr)
    return;
  if (state == ChunkState::NONE) {
    out[0] = '\0';
    return;
  }
  format_log_name(out, seq);
  if (state == ChunkState::CONFIRMED) {
    out[9] = 'U';
    out[10] = 'P';
    out[11] = 'L';
  }
}

// ---------------------------------------------------------------------------------------------
// The policy
// ---------------------------------------------------------------------------------------------

/// The chunk index plus the two decisions that read it: "should the writer rotate now?" and "which
/// file dies so logging can continue?".
///
/// Holds no card state and touches no hardware, exactly like `RecoveryPolicy` — the writer task
/// tells it what happened and asks it what to do next.
class CollectionPolicy {
 public:
  // ------------------------------------------------------------------------------- rotation (§4)

  /// Both bounds, whichever fires first. **0 means unbounded** on either, and both 0 means the
  /// file never rotates on its own.
  ///
  /// Rotation is size-only today, and a quiet bus therefore leaves the newest data trapped in an
  /// OPEN file indefinitely — and an OPEN file is never servable. The time bound is what makes
  /// "upload automatically" true for a parked car.
  void configure_rotation(uint32_t max_file_bytes, uint32_t max_file_seconds) {
    this->max_file_bytes_ = max_file_bytes;
    this->max_file_seconds_ = max_file_seconds;
    // Widened *before* the multiply, and kept in that form. Done in 32 bits, 4295 s wraps to ~0.7 s
    // and every file rotates almost immediately — which looks on the card like a component that
    // works, until someone counts the files.
    this->max_file_us_ = static_cast<uint64_t>(max_file_seconds) * 1000000ull;
  }

  /// Either bound reached. `file_bytes` is the current file's size, `elapsed_us` the time since it
  /// was opened; the caller owns both, so this stays a pure function of two numbers.
  ///
  /// `>=` on both, so `max_file_size: 4MB` produces files of at most 4 MB rather than 4 MB plus one
  /// record. A disarmed bound (0) is checked first and never trips: the inverted reading of "0 means
  /// unbounded" is "0 is always met", which rotates on every record and turns the card into a
  /// directory of empty files.
  bool should_rotate(uint64_t file_bytes, uint64_t elapsed_us) const {
    if (this->max_file_bytes_ != 0 && file_bytes >= this->max_file_bytes_)
      return true;
    return this->max_file_us_ != 0 && elapsed_us >= this->max_file_us_;
  }

  uint32_t max_file_bytes() const { return this->max_file_bytes_; }
  uint32_t max_file_seconds() const { return this->max_file_seconds_; }

  // ------------------------------------------------------------------------------ retention (§7)

  /// Arm retention at `retention_percent` card fill. Clamped into **[1, 99]** here, because this
  /// class is also reachable from hand-written code: a threshold of 0 would delete on an empty card
  /// and one of 100 would arm only on a card with no room left to seal the file that triggered it.
  ///
  /// The schema (V24) is **narrower than this clamp, on purpose**:
  /// `cv.int_range(min=1, max=99, min_included=False, max_included=False)` — both ends exclusive,
  /// so YAML accepts 2..98. Every value that survives validation therefore already satisfies the
  /// clamp, and the clamp can never fire on a number a user actually wrote. That is the point: a
  /// silently clamped threshold is a config that reads `retention_percent: 100` while the device
  /// retains at 99, with no error anywhere to point at and `dump_config` the only witness. Making
  /// the ranges merely equal would put the two boundaries one edit apart from disagreeing again.
  /// Widening the schema toward the clamp later is not a breaking change; narrowing it would be,
  /// which is why the tighter of the two is the one that shipped, and `tests/sd_logger/test_schema.py`
  /// pins it. This clamp is a defensive last resort for a value that did not come through the
  /// schema, not a second opinion about policy.
  ///
  /// Does not touch the chunk set: `collection:` is parsed long before the boot scan runs, and a
  /// re-configure at runtime must not forget what is on the card.
  void configure(uint8_t retention_percent) {
    this->enabled_ = true;
    this->retention_percent_ = retention_percent < 1 ? 1 : (retention_percent > 99 ? 99 : retention_percent);
  }

  /// `collection: {enabled: false}`, or no `collection:` block at all — the card fills and the
  /// writer stops, which is what every release before this one did. The index stays: listing and
  /// serving are useful without retention, and only `next_victim()` goes quiet.
  void disable() { this->enabled_ = false; }

  bool enabled() const { return this->enabled_; }
  uint8_t retention_percent() const { return this->retention_percent_; }

  // ------------------------------------------------------------------------- the chunk set (§3)

  /// Forget every tracked chunk (a remount rescans the card). Does not touch the discard counters:
  /// those describe data that is already gone and outlive any card state — the same reasoning that
  /// makes `card_dropped_` the one baseline the header writer deliberately does not reset.
  void clear() { this->count_ = 0; }

  /// Track a chunk. `false`, and nothing changes, when:
  ///   * `state` is NONE — that is "not a chunk", not a tracked one;
  ///   * `seq` is outside the 7 digits a filename can carry, so no file could be named for it;
  ///   * the seq is already tracked. The boot scan can see one seq twice — `L0000433.LOG` left by a
  ///     crash and `L0000433.UPL` from the confirm that renamed it — and two entries would let
  ///     retention delete one and leave the other pointing at a file that is gone;
  ///   * the index is full (see the capacity note at the top of this header);
  ///   * `state` is OPEN and another OPEN chunk already exists. There is exactly one writer and
  ///     exactly one open file; a second entry would make the first unreachable, and the
  ///     never-listed / never-deleted guard would then be protecting the wrong file.
  bool add(uint32_t seq, uint64_t bytes, ChunkState state) {
    if (state == ChunkState::NONE || seq > SD_LOG_SEQ_MAX)
      return false;
    if (this->count_ >= SD_LOG_MAX_CHUNKS || this->find(seq) != nullptr)
      return false;
    if (state == ChunkState::OPEN && this->find_open_() != nullptr)
      return false;
    ChunkEntry &entry = this->chunks_[this->count_];
    entry.seq = seq;
    entry.bytes = chunk_bytes(bytes);
    entry.state = state;
    entry.serving = false;
    this->count_++;
    return true;
  }

  /// The boot scan's entry point: parse a directory entry and track it if it is a chunk. `false`
  /// for anything `parse_chunk_name()` rejects, and the set is left untouched.
  ///
  /// This is the one path whose `bytes` did not come from this component's own writer — it is
  /// `st_size` off the card, and the file may be a stranger. Sizes past 4 GiB are saturated rather
  /// than truncated on the way into the entry; see `ChunkEntry`.
  bool add_name(const char *name, uint64_t bytes) {
    uint32_t seq = 0;
    ChunkState state = ChunkState::NONE;
    if (!parse_chunk_name(name, &seq, &state))
      return false;
    return this->add(seq, bytes, state);
  }

  /// OPEN -> SEALED, recording the final size. `false` for an unknown seq or a chunk that is not
  /// OPEN — a second seal must not rewrite the size of a file the collector may already be reading,
  /// because that number is the `Content-Length` it was given.
  bool seal(uint32_t seq, uint64_t bytes) {
    ChunkEntry *entry = this->find_mut_(seq);
    if (entry == nullptr || entry->state != ChunkState::OPEN)
      return false;
    entry->state = ChunkState::SEALED;
    entry->bytes = chunk_bytes(bytes);
    return true;
  }

  /// SEALED -> CONFIRMED: the puller acknowledged the chunk, and the writer may now rename it.
  ///
  /// **Idempotent** — the puller retries a `POST /sdlog/done/<name>` whose response it never saw,
  /// and a second confirm returns `true` for an already-CONFIRMED chunk without touching anything.
  ///
  /// `false`, and a no-op, for the OPEN chunk (a confirm racing rotation would rename the file the
  /// writer still holds an fd to, and FATFS would keep appending to a directory entry that no longer
  /// carries the name anyone will look for) and for a seq that is not tracked (retention may have
  /// discarded it while the transfer was in flight — the answer is "gone", not a resurrected entry
  /// naming a file whose rename would then fail on every pass).
  ///
  /// The serving flag is deliberately left alone: the flag, not the state, is what guards deletion,
  /// and only the handler's exit path knows the transfer is over.
  bool confirm(uint32_t seq) {
    ChunkEntry *entry = this->find_mut_(seq);
    if (entry == nullptr)
      return false;
    if (entry->state == ChunkState::CONFIRMED)
      return true;
    if (entry->state != ChunkState::SEALED)
      return false;
    entry->state = ChunkState::CONFIRMED;
    return true;
  }

  uint16_t count() const { return this->count_; }
  bool full() const { return this->count_ >= SD_LOG_MAX_CHUNKS; }

  /// Index into the tracked set, in insertion order — `nullptr` past the end. Iteration order is
  /// not sequence order; ask `oldest_uncollected()` or `next_victim()` for that.
  const ChunkEntry *at(uint16_t index) const { return index < this->count_ ? &this->chunks_[index] : nullptr; }

  const ChunkEntry *find(uint32_t seq) const {
    for (uint16_t i = 0; i < this->count_; i++) {
      if (this->chunks_[i].seq == seq)
        return &this->chunks_[i];
    }
    return nullptr;
  }

  /// Listed by `GET /sdlog/index` and readable by `GET /sdlog/f/<name>`: SEALED only. The OPEN file
  /// is not servable at any point and CONFIRMED chunks are not listed again.
  bool is_servable(uint32_t seq) const {
    const ChunkEntry *entry = this->find(seq);
    return entry != nullptr && entry->state == ChunkState::SEALED;
  }

  /// Deletable under the *normal* flow: CONFIRMED, and not currently being served. The pressure
  /// path that deletes SEALED chunks is data loss and goes through `next_victim()` alone, so that
  /// no caller can reach it by asking a question this innocuous.
  bool is_deletable(uint32_t seq) const {
    const ChunkEntry *entry = this->find(seq);
    return entry != nullptr && entry->state == ChunkState::CONFIRMED && !entry->serving;
  }

  /// Oldest SEALED seq — the "oldest un-collected" of `GET /sdlog/status`. The OPEN chunk is
  /// excluded because it cannot be collected yet, and CONFIRMED ones because they already were.
  /// `false` when nothing is waiting, which is the healthy steady state and not an error; the
  /// out-param is then left untouched.
  bool oldest_uncollected(uint32_t *seq_out) const {
    const ChunkEntry *oldest = this->oldest_(ChunkState::SEALED, /*skip_serving=*/false);
    if (oldest == nullptr)
      return false;
    if (seq_out != nullptr)
      *seq_out = oldest->seq;
    return true;
  }

  // -------------------------------------------------------------------------- serving guard (§7)

  /// A transfer is starting. `false` for an unknown seq and for the OPEN chunk, which is never
  /// served in the first place. A CONFIRMED chunk can be marked: the fixed order is
  /// serve -> confirm -> rename, so the confirm lands while the handler is still finishing.
  bool mark_serving(uint32_t seq) {
    ChunkEntry *entry = this->find_mut_(seq);
    if (entry == nullptr || entry->state == ChunkState::OPEN)
      return false;
    entry->serving = true;
    return true;
  }

  /// The transfer finished, failed, or the connection dropped. Must be called on every exit path:
  /// a flag left set pins the oldest chunk and retention walks past it forever. Idempotent, and
  /// tolerates a seq that is no longer tracked.
  void clear_serving(uint32_t seq) {
    ChunkEntry *entry = this->find_mut_(seq);
    if (entry != nullptr)
      entry->serving = false;
  }

  bool is_serving(uint32_t seq) const {
    const ChunkEntry *entry = this->find(seq);
    return entry != nullptr && entry->serving;
  }

  // ---------------------------------------------------------------- the retention decision (§7)

  /// Which chunk dies next, given the card's current fill. `nullptr` when retention is disabled,
  /// when `fill_percent` is below the threshold, or when nothing is eligible.
  ///
  /// The order is the whole policy — "drop oldest un-collected":
  ///
  ///   1. the oldest CONFIRMED chunk (already collected: deleting it loses nothing);
  ///   2. only if there is no CONFIRMED chunk at all, the oldest SEALED one. **That is real data
  ///      loss** and `discard()` bills it to the `#gap` counters.
  ///
  /// Preference beats age between the two tiers: a CONFIRMED chunk newer than every SEALED one is
  /// still the victim, because the free list is the collected chunks and the whole point is to
  /// exhaust it before losing anything. A CONFIRMED chunk that is merely *busy* blocks the fallback
  /// for the same reason — it is about to become free, and spending a never-collected chunk while
  /// the free list is one transfer away from refilling is the trade this policy exists to avoid.
  ///
  /// Never the OPEN chunk: with nothing else left, a full card yields no victim at all and logging
  /// wins. Never a chunk marked serving.
  const ChunkEntry *next_victim(uint8_t fill_percent) const {
    if (!this->enabled_ || fill_percent < this->retention_percent_)
      return nullptr;
    const ChunkEntry *victim = this->oldest_(ChunkState::CONFIRMED, /*skip_serving=*/true);
    if (victim != nullptr)
      return victim;
    if (this->oldest_(ChunkState::CONFIRMED, /*skip_serving=*/false) != nullptr)
      return nullptr;  // the free list is not empty, only busy — wait for it rather than lose data.
    return this->oldest_(ChunkState::SEALED, /*skip_serving=*/true);
  }

  /// The file is gone: drop the entry and, if it was SEALED, bill it to the discard window.
  /// Deleting a CONFIRMED chunk is not a gap — the collector has it, and a `#gap` line would claim a
  /// hole in an archive that is complete.
  ///
  /// `false`, and nothing changes, for an unknown seq, for the OPEN chunk and for a chunk marked
  /// serving. `next_victim()` already refuses those; this refuses them again because the caller in
  /// between is a task boundary (§8), and the flag can be set after the victim was chosen.
  bool discard(uint32_t seq) {
    ChunkEntry *entry = this->find_mut_(seq);
    if (entry == nullptr || entry->state == ChunkState::OPEN || entry->serving)
      return false;
    if (entry->state == ChunkState::SEALED) {
      // Never collected: this is the loss the `#gap` line exists to state in-band.
      if (this->pending_chunks_ == 0)
        this->pending_first_seq_ = entry->seq;
      this->pending_last_seq_ = entry->seq;
      this->pending_chunks_++;
      this->pending_bytes_ += entry->bytes;
      this->discarded_chunks_++;
      this->discarded_bytes_ += entry->bytes;
    }
    // Close the hole rather than swapping the last entry in: `at()` promises insertion order, and
    // the boot scan's order is the only clue a dump gives about how the card was found.
    const uint16_t index = static_cast<uint16_t>(entry - this->chunks_);
    for (uint16_t i = index; i + 1 < this->count_; i++)
      this->chunks_[i] = this->chunks_[i + 1];
    this->count_--;
    return true;
  }

  // ------------------------------------------------------------------- discard accounting (§7)

  /// Is there an un-emitted `#gap` window? The marker pass checks this *before* it reserves line
  /// room, exactly like the `#drop` pass compares against its `marked_` baseline: taking the window
  /// and then discovering the line does not fit would drop the only in-band record that the data
  /// was lost. A pure peek — it consumes nothing, however often it is called.
  bool has_pending_discards() const { return this->pending_chunks_ != 0; }

  /// Read **and clear** the window: chunk count, byte total and the seq range that went away since
  /// the last call. Read-and-clear rather than the `#drop` pass's read-and-mark because the seq
  /// range is a property of the window, not of a lifetime total — a `marked_` baseline can
  /// reconstruct a delta but not a first/last pair. A second call on an empty window is zeroed, not
  /// a repeat, or a loss would print a duplicate `#gap` into every file that rotates after it.
  ///
  /// `first_seq`/`last_seq` are the ends **in the order retention emptied the window**, not a
  /// numeric min and max: sorting them prints `#gap,…,0,9999999` across the wrap and claims the
  /// entire card went away.
  ///
  /// They are also not a claim that everything between them is gone. Only SEALED discards are
  /// billed here, so a chunk the puller confirmed mid-window never enters the count while the
  /// endpoints still straddle it — discard 10, confirm 11, discard 12 and the window is `10..12`
  /// with `chunks == 2`. `chunks` is the loss; the span is only the width. A reader that expands
  /// the span into a list of missing files over-reports the loss, which is the one thing `#gap`
  /// exists to prevent — see `format_gap()` in log_format.h for the punctured-window wording the
  /// reader side is held to.
  ///
  /// Call it only when the `#gap` line is definitely going to be written.
  DiscardStats take_pending_discards() {
    const DiscardStats window{this->pending_chunks_, this->pending_bytes_, this->pending_first_seq_,
                              this->pending_last_seq_};
    this->pending_chunks_ = 0;
    this->pending_bytes_ = 0;
    this->pending_first_seq_ = 0;
    this->pending_last_seq_ = 0;
    return window;
  }

  /// Lifetime totals for `GET /sdlog/status` and `dump_config`, never cleared. 64-bit bytes on
  /// purpose: at 148 KB/s a card sheds more than 4 GB in eight hours, and a 32-bit total would
  /// wrap back through zero and report a healthy device.
  uint32_t discarded_chunks() const { return this->discarded_chunks_; }
  uint64_t discarded_bytes() const { return this->discarded_bytes_; }

 protected:
  ChunkEntry *find_mut_(uint32_t seq) {
    for (uint16_t i = 0; i < this->count_; i++) {
      if (this->chunks_[i].seq == seq)
        return &this->chunks_[i];
    }
    return nullptr;
  }

  const ChunkEntry *find_open_() const {
    for (uint16_t i = 0; i < this->count_; i++) {
      if (this->chunks_[i].state == ChunkState::OPEN)
        return &this->chunks_[i];
    }
    return nullptr;
  }

  /// Oldest entry in `state` by `seq_before()` — the one fold both retention tiers and
  /// `oldest_uncollected()` are written in terms of, so "oldest" cannot come to mean two things.
  const ChunkEntry *oldest_(ChunkState state, bool skip_serving) const {
    const ChunkEntry *best = nullptr;
    for (uint16_t i = 0; i < this->count_; i++) {
      const ChunkEntry &entry = this->chunks_[i];
      if (entry.state != state || (skip_serving && entry.serving))
        continue;
      if (best == nullptr || seq_before(entry.seq, best->seq))
        best = &entry;
    }
    return best;
  }

  ChunkEntry chunks_[SD_LOG_MAX_CHUNKS]{};
  uint16_t count_{0};

  bool enabled_{false};
  uint8_t retention_percent_{90};

  uint32_t max_file_bytes_{0};
  /// Kept alongside the microsecond form so the accessor reports back exactly what was configured
  /// and `should_rotate()` costs no division on the record path.
  uint32_t max_file_seconds_{0};
  uint64_t max_file_us_{0};

  /// The un-emitted `#gap` window.
  uint32_t pending_chunks_{0};
  uint64_t pending_bytes_{0};
  uint32_t pending_first_seq_{0};
  uint32_t pending_last_seq_{0};

  /// Lifetime, never cleared.
  uint32_t discarded_chunks_{0};
  uint64_t discarded_bytes_{0};
};

}  // namespace sd_logger
}  // namespace esphome
