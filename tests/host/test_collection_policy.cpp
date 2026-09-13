// The chunk lifecycle, the retention rule and the rotation bounds
// (components/sd_logger/collection_policy.h, docs/sdlog-collection-design.md §3, §4, §7).
//
// Why this is a host test and not a bench check: staging one retention case costs *hours* of a
// filling card — 12.4 GB a day at the measured rate, so a 32 GB card takes about 60 hours to reach
// the threshold that arms any of this — and the failures are all silent by construction:
//
//   * a victim order that picks SEALED before CONFIRMED deletes exactly the chunks nobody has a
//     copy of, while the collector's archive fills up normally and every counter on the board
//     reads healthy;
//   * a confirm that lands on the OPEN chunk renames the file the writer still holds an fd to, and
//     FATFS keeps happily appending to a directory entry the reader will never find again;
//   * a serving guard consulted before the flag is set unlinks a file mid-transfer, which the
//     puller sees as one truncated download among thousands;
//   * `seq` wraps at 9999999, and a plain `a < b` ordering then reports the newest chunk on the
//     card as the oldest — one boot, once, with no evidence left afterwards;
//   * a `#gap` window taken by a marker pass that then found no room for the line loses the only
//     in-band record that six hours of history are missing.
//
// Every one of those produces a card that looks right and an archive that is quietly wrong, which
// is the same reason log_format.h and recovery_policy.h are ESPHome-free. These cases are the only
// thing that can catch them.

#include "harness.h"
#include "collection_policy.h"

#include <cstdint>
#include <cstring>
#include <string>

using esphome::sd_logger::chunk_bytes;
using esphome::sd_logger::ChunkEntry;
using esphome::sd_logger::ChunkState;
using esphome::sd_logger::CollectionPolicy;
using esphome::sd_logger::DiscardStats;
using esphome::sd_logger::format_chunk_name;
using esphome::sd_logger::parse_chunk_name;
using esphome::sd_logger::parse_log_seq;
using esphome::sd_logger::SD_LOG_NAME_LEN;
using esphome::sd_logger::SD_LOG_SEQ_MAX;
using esphome::sd_logger::SD_LOG_SEQ_MODULUS;
using esphome::sd_logger::SD_LOG_SEQ_WRAP_WINDOW;
using esphome::sd_logger::seq_before;

namespace {

/// Reported instead of dereferencing a null entry, so a case that expected a chunk prints
/// "got 4294967295, want 10" rather than taking the whole target down with a segfault.
static const uint32_t SEQ_NONE = 0xFFFFFFFFu;

uint32_t seq_of(const ChunkEntry *entry) { return entry == nullptr ? SEQ_NONE : entry->seq; }
uint64_t bytes_of(const ChunkEntry *entry) { return entry == nullptr ? 0 : entry->bytes; }
ChunkState state_of(const ChunkEntry *entry) { return entry == nullptr ? ChunkState::NONE : entry->state; }

/// Retention armed, because `next_victim()` returning nothing is the *correct* answer for a
/// disabled policy and every case below would then pass for the wrong reason.
CollectionPolicy make_policy(uint8_t retention_percent = 80) {
  CollectionPolicy policy;
  policy.configure(retention_percent);
  return policy;
}

/// 4 MB — the chunk size §4 targets.
static const uint64_t CHUNK = 4ull * 1024 * 1024;

/// What `chunk_bytes()` clamps to, and the largest number a 12-byte `ChunkEntry` can carry. No
/// chunk this component writes reaches it: `max_file_bytes_` is itself a uint32_t, so seeing this
/// value in a `#gap` line is the anomaly, not a size.
static const uint64_t SATURATED = 0xFFFFFFFFull;

/// The index capacity as a plain number. Every capacity case below is written in terms of this and
/// never in terms of 64, 256 or any other literal — the bound is a build-time define that
/// `collection: max_chunks:` emits, and a case that spells the number out passes on a build the
/// class has stopped honouring. `test_collection_capacity.cpp` is where the *number* is pinned, at
/// a capacity that is neither the old default nor the new one.
static const uint32_t CAP = static_cast<uint32_t>(SD_LOG_MAX_CHUNKS);

}  // namespace

// ------------------------------------------------------------------------------------- names (§3)

TEST(collection_parse_sealed_name) {
  uint32_t seq = SEQ_NONE;
  ChunkState state = ChunkState::NONE;
  CHECK(parse_chunk_name("L0000433.LOG", &seq, &state));
  CHECK_EQ(seq, 433u);
  // A name can never say OPEN: the open file is a `.LOG` on the card exactly like a sealed one.
  // Only the writer knows which seq it holds an fd to.
  CHECK_EQ(state, ChunkState::SEALED);
}

TEST(collection_parse_confirmed_name) {
  uint32_t seq = SEQ_NONE;
  ChunkState state = ChunkState::NONE;
  CHECK(parse_chunk_name("L0000433.UPL", &seq, &state));
  CHECK_EQ(seq, 433u);
  CHECK_EQ(state, ChunkState::CONFIRMED);
}

TEST(collection_parse_covers_the_whole_seq_range) {
  uint32_t seq = SEQ_NONE;
  CHECK(parse_chunk_name("L0000000.LOG", &seq, nullptr));
  CHECK_EQ(seq, 0u);
  CHECK(parse_chunk_name("L9999999.LOG", &seq, nullptr));
  CHECK_EQ(seq, SD_LOG_SEQ_MAX);
  CHECK(parse_chunk_name("L1234567.UPL", &seq, nullptr));
  CHECK_EQ(seq, 1234567u);
}

TEST(collection_parse_rejects_the_m1_csv_name) {
  // `.CSV` is the M1 era: no `#sdlog` header, no `#close`, no seq the puller could dedup on — so
  // it is not a chunk. Serving it would ship a file no reader understands, and deleting it under
  // pressure would destroy the only copy of data that predates this design.
  uint32_t seq = SEQ_NONE;
  ChunkState state = ChunkState::OPEN;
  CHECK(!parse_chunk_name("L0000433.CSV", &seq, &state));
  CHECK_EQ(seq, SEQ_NONE);                                // out-params untouched on rejection
  CHECK_EQ(state, ChunkState::OPEN);                      // ditto
  CHECK(parse_chunk_name("L0000433.LOG", &seq, &state));  // the same file, current extension
  CHECK_EQ(seq, 433u);

  // And the *boot scan's* matcher must keep taking it, or a card holding both eras restarts the
  // sequence at 0 and the writer walks the whole file set one open at a time (log_format.h F2a).
  // The two parsers deliberately disagree; this is the case that says so.
  uint32_t boot_seq = SEQ_NONE;
  CHECK(parse_log_seq("L0000433.CSV", &boot_seq));
  CHECK_EQ(boot_seq, 433u);
}

TEST(collection_parse_rejects_malformed_names) {
  static const char *const BAD[] = {
      "",               // an empty directory entry
      "L",              //
      "L.LOG",          // no digits
      "L000043.LOG",    // six digits
      "L00004333.LOG",  // eight digits, so no '.' where one belongs
      "L0000433",       // no extension at all
      "L0000433.",      // dot, no extension
      "L0000433LOG",    // missing the dot
      "X0000433.LOG",   // wrong prefix letter
      "0000433.LOG",    // no prefix
      " L0000433.LOG",  // leading space
      "L000043a.LOG",   // non-digit inside the number
      "L-000433.LOG",   // signed, and one digit short
      "L 000433.LOG",   // space inside the number
      "L0000433.log",   // FatFs 8.3 without LFN is uppercase; a lowercase name is not ours
      "L0000433.LOGX",  // trailing garbage
      "L0000433.UPLX",  // ditto on the confirmed extension
      "L0000433.TMP",   // some other tool's scratch file
      "L0000433.UPL.LOG",
  };
  uint32_t seq = SEQ_NONE;
  ChunkState state = ChunkState::NONE;
  for (const char *name : BAD) {
    CHECK_MSG(!parse_chunk_name(name, &seq, &state), std::string("accepted '") + name + "'");
    CHECK_EQ_MSG(seq, SEQ_NONE, name);
    CHECK_EQ_MSG(state, ChunkState::NONE, name);
  }
  CHECK(!parse_chunk_name(nullptr, &seq, &state));  // a null entry must not be read

  // Both out-params are optional: some callers only want the yes/no.
  CHECK(parse_chunk_name("L0000433.LOG", nullptr, nullptr));
}

TEST(collection_format_chunk_name_round_trips) {
  char name[SD_LOG_NAME_LEN];

  format_chunk_name(name, 433, ChunkState::SEALED);
  CHECK_MSG(std::strcmp(name, "L0000433.LOG") == 0, std::string("sealed -> '") + name + "'");
  // The rename's destination. This is the *only* mutation that moves a chunk to CONFIRMED — never
  // an in-file edit, which would be a read-modify-write on the file we are trying to protect.
  format_chunk_name(name, 433, ChunkState::CONFIRMED);
  CHECK_MSG(std::strcmp(name, "L0000433.UPL") == 0, std::string("confirmed -> '") + name + "'");
  // The open file is on the card under its `.LOG` name like any sealed chunk.
  format_chunk_name(name, 433, ChunkState::OPEN);
  CHECK_MSG(std::strcmp(name, "L0000433.LOG") == 0, std::string("open -> '") + name + "'");

  static const uint32_t SEQS[] = {0u, 1u, 433u, 1234567u, SD_LOG_SEQ_MAX};
  for (uint32_t want : SEQS) {
    for (ChunkState want_state : {ChunkState::SEALED, ChunkState::CONFIRMED}) {
      format_chunk_name(name, want, want_state);
      CHECK_EQ_MSG(std::strlen(name), SD_LOG_NAME_LEN - 1, name);
      uint32_t got = SEQ_NONE;
      ChunkState got_state = ChunkState::NONE;
      CHECK_MSG(parse_chunk_name(name, &got, &got_state), name);
      CHECK_EQ_MSG(got, want, name);
      CHECK_EQ_MSG(got_state, want_state, name);
    }
  }
}

// -------------------------------------------------------------------------- sequence ordering (§3)

TEST(collection_seq_before_orders_the_ordinary_case) {
  CHECK(seq_before(0u, 1u));
  CHECK(seq_before(432u, 433u));
  CHECK(seq_before(433u, 9999999u));
  CHECK(!seq_before(433u, 432u));
  CHECK(!seq_before(433u, 433u));  // irreflexive: "older" is strict, or the fold picks arbitrarily
}

TEST(collection_seq_before_wraps_at_the_seven_digit_ceiling) {
  // The filename has room for 7 digits, so after L9999999 comes L0000000. A plain `a < b` reports
  // the freshly wrapped chunk as the oldest thing on the card and retention deletes the newest
  // data first — once, on one board, with nothing left to show for it afterwards.
  CHECK_EQ(SD_LOG_SEQ_MODULUS, 10000000u);
  CHECK_EQ(SD_LOG_SEQ_MAX, 9999999u);
  CHECK(seq_before(SD_LOG_SEQ_MAX, 0u));
  CHECK(!seq_before(0u, SD_LOG_SEQ_MAX));
  CHECK(seq_before(9999998u, 1u));
  CHECK(!seq_before(1u, 9999998u));
  CHECK(seq_before(9999990u, 9u));
  CHECK(!seq_before(9u, 9999990u));
}

TEST(collection_seq_before_is_antisymmetric_across_the_wrap) {
  // Twenty consecutive chunks straddling the wrap, compared pairwise. This is the property the
  // victim fold actually relies on; the individual comparisons above are just its readable cases.
  uint32_t seqs[20];
  for (uint32_t i = 0; i < 20; i++)
    seqs[i] = (SD_LOG_SEQ_MAX - 9u + i) % SD_LOG_SEQ_MODULUS;
  for (uint32_t i = 0; i < 20; i++) {
    for (uint32_t j = i + 1; j < 20; j++) {
      const std::string tag = std::to_string(seqs[i]) + " vs " + std::to_string(seqs[j]);
      CHECK_MSG(seq_before(seqs[i], seqs[j]), tag);
      CHECK_MSG(!seq_before(seqs[j], seqs[i]), tag);
    }
  }
}

// ------------------------------------------------------------------------------ the chunk set (§3)

TEST(collection_empty_set_answers_nothing) {
  CollectionPolicy policy = make_policy(80);
  CHECK(policy.enabled());
  CHECK_EQ(policy.count(), 0u);
  CHECK(!policy.full());
  CHECK(policy.find(433) == nullptr);
  CHECK(policy.at(0) == nullptr);
  CHECK(!policy.is_servable(433));
  CHECK(!policy.is_deletable(433));
  uint32_t seq = SEQ_NONE;
  CHECK(!policy.oldest_uncollected(&seq));
  // A full card with nothing tracked on it is not a crash and not a victim: logging wins.
  CHECK(policy.next_victim(100) == nullptr);
  CHECK(!policy.confirm(433));
  CHECK(!policy.discard(433));
  CHECK(!policy.has_pending_discards());
}

TEST(collection_add_tracks_a_chunk) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, CHUNK, ChunkState::SEALED));
  CHECK_EQ(policy.count(), 1u);
  const ChunkEntry *entry = policy.find(433);
  CHECK(entry != nullptr);
  CHECK_EQ(seq_of(entry), 433u);
  CHECK_EQ(bytes_of(entry), CHUNK);
  CHECK_EQ(state_of(entry), ChunkState::SEALED);
  CHECK(!policy.is_serving(433));
  CHECK(policy.at(0) == entry);
  CHECK(policy.at(1) == nullptr);
}

TEST(collection_add_rejects_a_duplicate_seq) {
  // The boot scan can see the same seq twice — `L0000433.LOG` left behind by a crash and
  // `L0000433.UPL` from the confirm that renamed it. Tracking both would let retention delete one
  // entry and leave the other pointing at a file that is gone.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, 100, ChunkState::SEALED));
  CHECK(!policy.add(433, 999, ChunkState::CONFIRMED));
  CHECK_EQ(policy.count(), 1u);
  CHECK_EQ(state_of(policy.find(433)), ChunkState::SEALED);
  CHECK_EQ(bytes_of(policy.find(433)), 100u);
}

TEST(collection_add_rejects_a_second_open_chunk) {
  // One writer, one open file. A second OPEN entry means the first is unreachable, and the
  // never-listed / never-deleted guard would then protect the wrong file.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 0, ChunkState::OPEN));
  CHECK(!policy.add(11, 0, ChunkState::OPEN));
  CHECK_EQ(policy.count(), 1u);
  CHECK(policy.find(11) == nullptr);

  // Sealing the first frees the slot — that is exactly what rotation does.
  CHECK(policy.seal(10, CHUNK));
  CHECK(policy.add(11, 0, ChunkState::OPEN));
  CHECK_EQ(policy.count(), 2u);
  CHECK_EQ(state_of(policy.find(10)), ChunkState::SEALED);
  CHECK_EQ(state_of(policy.find(11)), ChunkState::OPEN);
}

TEST(collection_add_name_parses_the_directory_entry) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add_name("L0000433.LOG", 1000));
  CHECK_EQ(state_of(policy.find(433)), ChunkState::SEALED);
  CHECK(policy.add_name("L0000434.UPL", 2000));
  CHECK_EQ(state_of(policy.find(434)), ChunkState::CONFIRMED);
  CHECK_EQ(bytes_of(policy.find(434)), 2000u);

  // Anything the parser rejects leaves the set alone — in particular the M1 `.CSV` files, which
  // stay on the card untouched.
  CHECK(!policy.add_name("L0000435.CSV", 3000));
  CHECK(!policy.add_name("SYSTEM~1.TXT", 10));
  CHECK(!policy.add_name(nullptr, 10));
  CHECK_EQ(policy.count(), 2u);
}

TEST(collection_full_index_refuses_without_losing_what_it_holds) {
  // The index is static. Evicting an entry to make room would mean forgetting a file that still
  // exists, which is the one failure retention cannot recover from: the chunk is never listed,
  // never served and never deleted, and the card fills behind it.
  CollectionPolicy policy = make_policy();
  const uint32_t cap = static_cast<uint32_t>(SD_LOG_MAX_CHUNKS);
  for (uint32_t i = 0; i < cap; i++)
    CHECK_MSG(policy.add(i, 10 + i, ChunkState::SEALED), std::to_string(i));
  CHECK_EQ(policy.count(), cap);
  CHECK(policy.full());

  CHECK(!policy.add(9000, 1, ChunkState::SEALED));
  CHECK(policy.find(9000) == nullptr);
  CHECK_EQ(policy.count(), cap);
  for (uint32_t i = 0; i < cap; i++)
    CHECK_EQ_MSG(bytes_of(policy.find(i)), 10 + i, std::to_string(i));
}

TEST(collection_full_flips_exactly_at_the_capacity) {
  // The default moved 64 -> 256 after a 12-minute PERF soak rotated 29 times and found the index
  // already full at the *first* rotation: every chunk of that run was untracked, retention could
  // reclaim nothing, and the only witness was one warning that scrolled past. So the boundary
  // itself is worth a case — one entry either way is 25 s of card at the bench's bounds.
  //
  // The *number* 256 is not spelled out here on purpose. `tests/sd_logger/test_schema.py` V26
  // greps the `#define` out of the header and asserts it equals the schema's `max_chunks` default,
  // which is the one place that comparison means something; a second literal here would only be a
  // copy to forget. What this case owns is the behaviour at whatever that number is.
  CollectionPolicy policy = make_policy();
  for (uint32_t i = 0; i + 1 < CAP; i++) {
    CHECK_MSG(policy.add(i, 1, ChunkState::SEALED), std::to_string(i));
    CHECK_MSG(!policy.full(), std::to_string(i));  // full() must not fire one early
  }
  CHECK_EQ(policy.count(), CAP - 1);
  CHECK(policy.add(CAP - 1, 1, ChunkState::SEALED));  // the last slot is usable
  CHECK(policy.full());
  CHECK_EQ(policy.count(), CAP);

  // and the count never moves past the cap, however long the writer keeps trying — this is the
  // steady state of a run that outgrew its index, not a one-off.
  for (uint32_t i = 0; i < 20; i++) {
    CHECK_MSG(!policy.add(CAP + i, 1, ChunkState::SEALED), std::to_string(i));
    CHECK_MSG(policy.full(), std::to_string(i));
    CHECK_EQ_MSG(policy.count(), CAP, std::to_string(i));
  }
  CHECK(!policy.add_name("L0009999.LOG", 1));  // the boot scan's path refuses on the same bound
  CHECK_EQ(policy.count(), CAP);
}

TEST(collection_a_refused_add_leaves_the_index_consistent) {
  // The refusal is correct; what matters on a device is that everything already tracked still
  // works afterwards. The writer goes on sealing and confirming into a full index for the rest of
  // the run, and retention is the only thing that can ever make room again — so a refusal that
  // scrambled the set would take the recovery path down with it.
  CollectionPolicy policy = make_policy(90);
  for (uint32_t i = 0; i < CAP; i++)
    CHECK_MSG(policy.add(i, 100 + i, ChunkState::SEALED), std::to_string(i));

  CHECK(!policy.add(CAP, 1, ChunkState::SEALED));
  CHECK(!policy.add(CAP, 1, ChunkState::OPEN));  // not even the writer's own open file squeezes in

  // every entry still findable, with the size it was added with
  for (uint32_t i = 0; i < CAP; i++) {
    CHECK_EQ_MSG(seq_of(policy.find(i)), i, std::to_string(i));
    CHECK_EQ_MSG(bytes_of(policy.find(i)), 100 + i, std::to_string(i));
  }
  // at() still walks them in insertion order and still stops at the count
  for (uint32_t i = 0; i < CAP; i++)
    CHECK_EQ_MSG(seq_of(policy.at(static_cast<uint16_t>(i))), i, std::to_string(i));
  CHECK(policy.at(static_cast<uint16_t>(CAP)) == nullptr);

  // and retention can still name a victim: a full index that cannot choose never recovers
  CHECK_EQ(seq_of(policy.next_victim(95)), 0u);
  CHECK(policy.is_servable(CAP - 1));
  CHECK(policy.confirm(7));
  CHECK_EQ(seq_of(policy.next_victim(95)), 7u);  // and the CONFIRMED tier still beats the older 0
}

TEST(collection_a_full_index_recovers_when_one_chunk_is_discarded) {
  // The path a real device takes back. It rests on discard() closing the hole rather than leaving
  // a dead entry behind, so the insertion order after the discard is asserted here too: a
  // swap-with-last would be cheaper and would put the recovered chunk somewhere the boot scan's
  // order no longer explains.
  CollectionPolicy policy = make_policy(90);
  for (uint32_t i = 0; i < CAP; i++)
    CHECK_MSG(policy.add(i, 1, ChunkState::SEALED), std::to_string(i));
  CHECK(policy.full());
  CHECK(!policy.add(CAP, 1, ChunkState::SEALED));

  const uint32_t victim = seq_of(policy.next_victim(95));
  CHECK_EQ(victim, 0u);
  CHECK(policy.discard(victim));
  CHECK(!policy.full());
  CHECK_EQ(policy.count(), CAP - 1);

  // the hole closed, so slot 0 is now the chunk that was added second
  CHECK_EQ(seq_of(policy.at(0)), 1u);
  CHECK_EQ(seq_of(policy.at(static_cast<uint16_t>(CAP - 2))), CAP - 1);

  // and the next rotation is tracked again, at the end of the insertion order
  CHECK(policy.add(CAP, 4096, ChunkState::OPEN));
  CHECK(policy.full());
  CHECK_EQ(policy.count(), CAP);
  CHECK_EQ(seq_of(policy.find(CAP)), CAP);
  CHECK_EQ(bytes_of(policy.find(CAP)), 4096u);
  CHECK_EQ(seq_of(policy.at(static_cast<uint16_t>(CAP - 1))), CAP);
  CHECK_EQ(policy.discarded_chunks(), 1u);  // the loss was billed once, and only once
}

TEST(collection_discard_closes_the_hole_in_insertion_order) {
  // `at()` promises insertion order, and on a card that was found rather than written the boot
  // scan's order is the only clue a dump gives about how it was found.
  CollectionPolicy policy = make_policy();
  for (uint32_t i = 0; i < 6; i++)
    CHECK_MSG(policy.add(500 + i, 1, ChunkState::CONFIRMED), std::to_string(i));

  CHECK(policy.discard(502));  // out of the middle, where a swap-with-last would show
  CHECK_EQ(policy.count(), 5u);
  static const uint32_t AFTER[] = {500, 501, 503, 504, 505};
  for (uint16_t i = 0; i < 5; i++)
    CHECK_EQ_MSG(seq_of(policy.at(i)), AFTER[i], std::to_string(i));
  CHECK(policy.at(5) == nullptr);

  CHECK(policy.discard(500));  // the first entry
  CHECK(policy.discard(505));  // and the last, which walks zero elements
  static const uint32_t LEFT[] = {501, 503, 504};
  for (uint16_t i = 0; i < 3; i++)
    CHECK_EQ_MSG(seq_of(policy.at(i)), LEFT[i], std::to_string(i));
  CHECK(policy.at(3) == nullptr);
  CHECK_EQ(policy.count(), 3u);
}

TEST(collection_wrap_window_scales_with_the_index_capacity) {
  // `seq_before()` only offers its wrap reading inside SD_LOG_SEQ_WRAP_WINDOW, so the window and
  // the index capacity are coupled: a capacity the window does not cover drops a wrap-straddling
  // pair back onto the magnitude comparison, and retention then deletes the *newest* chunk on the
  // card. The window used to be a flat 256 — 4x the old 64-entry default, and exactly the new one;
  // `collection: max_chunks:` reaching 2048 is what made a capacity past it configurable.
  CHECK(SD_LOG_SEQ_WRAP_WINDOW >= CAP);
  CHECK(SD_LOG_SEQ_WRAP_WINDOW >= 256u);  // and never narrower than it has always been

  // A monotonic run that fills the index and straddles the ceiling. The widest pair in it is
  // CAP - 1 apart, so the window covers it — this is the easy half, and the case below is the
  // sparse index the refusal path actually produces.
  CollectionPolicy policy = make_policy(90);
  const uint32_t first = SD_LOG_SEQ_MODULUS - CAP / 2;  // half the run below the ceiling, half above
  for (uint32_t i = 0; i < CAP; i++)
    CHECK_MSG(policy.add((first + i) % SD_LOG_SEQ_MODULUS, 1, ChunkState::SEALED), std::to_string(i));
  CHECK(policy.full());

  uint32_t seq = SEQ_NONE;
  CHECK(policy.oldest_uncollected(&seq));
  CHECK_EQ(seq, first);                             // the wrapped-first chunk, not 0
  CHECK_EQ(seq_of(policy.next_victim(95)), first);  // and retention agrees with the status line
}

TEST(collection_a_recovered_slot_leaves_a_sparse_index_wider_than_the_capacity) {
  // "A full index spans at most SD_LOG_MAX_CHUNKS - 1 seqs" is the tempting bound and it is false,
  // which matters now that the capacity is reached in ~1.8 hours rather than never: the chunks
  // `add()` turned away were still written and still consumed sequence numbers, so when a discard
  // frees a slot the seq that fills it can be arbitrarily far above the oldest entry still held.
  // The index is a sparse window over the sequence space, not a contiguous run, and the ordering
  // has to survive that — this is the state a device is in for the rest of any run that outgrew
  // its index, which the PERF soak reached at the first rotation.
  CollectionPolicy policy = make_policy(90);
  for (uint32_t i = 0; i < CAP; i++)
    CHECK_MSG(policy.add(1000 + i, 1, ChunkState::SEALED), std::to_string(i));

  // 500 rotations the index could not track. On a device each of these is one `index_refused=`.
  for (uint32_t i = 0; i < 500; i++)
    CHECK_MSG(!policy.add(1000 + CAP + i, 1, ChunkState::SEALED), std::to_string(i));

  // retention frees exactly one slot, and the next rotation lands 500 seqs past the last tracked
  CHECK_EQ(seq_of(policy.next_victim(95)), 1000u);
  CHECK(policy.discard(1000));
  const uint32_t recovered = 1000 + CAP + 500;
  CHECK(policy.add(recovered, 1, ChunkState::SEALED));
  CHECK(policy.full());

  // the set now spans more than its own capacity, and every answer still has to be right
  CHECK_EQ(seq_of(policy.at(0)), 1001u);
  CHECK(recovered - seq_of(policy.at(0)) > CAP);
  CHECK_EQ(seq_of(policy.next_victim(95)), 1001u);
  uint32_t oldest = SEQ_NONE;
  CHECK(policy.oldest_uncollected(&oldest));
  CHECK_EQ(oldest, 1001u);
  // Away from the wrap a pair outside the window falls through to the magnitude comparison, which
  // is correct on its own — the window is only load-bearing across the ceiling.
  CHECK(seq_before(1001u, recovered));
  CHECK(!seq_before(recovered, 1001u));
}

TEST(collection_clear_forgets_the_chunks_but_not_the_losses) {
  // A remount rescans the card, so the index is rebuilt from scratch. The discard counters are
  // not: they describe data that is already gone, and they outlive any card state — the same
  // reasoning that makes `card_dropped_` the one baseline the header writer does not reset.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 4096, ChunkState::SEALED));
  CHECK(policy.discard(10));
  CHECK(policy.add(11, 1, ChunkState::CONFIRMED));

  policy.clear();
  CHECK_EQ(policy.count(), 0u);
  CHECK(policy.find(11) == nullptr);
  CHECK(policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 1u);
  CHECK_EQ(policy.discarded_bytes(), 4096u);

  // and the index is usable again
  CHECK(policy.add(11, 1, ChunkState::OPEN));
  CHECK_EQ(policy.count(), 1u);
}

// -------------------------------------------------------------------- the 32-bit `bytes` field (§7a)

TEST(collection_chunk_entry_stays_twelve_bytes) {
  // The point of the 64 -> 32 bit change. The index is SD_LOG_MAX_CHUNKS of these in static RAM,
  // so the entry size is what decides how much card the index can cover: a 64-bit `bytes` made the
  // struct 24 B — half of it alignment padding — for a number that cannot use the range, and the
  // 256-entry default would then cost 6 KB on a chip with ~100-200 KB free. A field added here
  // without thought silently doubles it back, and nothing else in the build would say so.
  CHECK_EQ(sizeof(ChunkEntry), 12u);
  CHECK_EQ(sizeof(ChunkEntry::seq), 4u);
  CHECK_EQ(sizeof(ChunkEntry::bytes), 4u);
  CHECK_EQ(alignof(ChunkEntry), 4u);
  // and the index really is that array and not much else: the scalar members are the remainder.
  CHECK(sizeof(CollectionPolicy) >= CAP * sizeof(ChunkEntry));
  CHECK(sizeof(CollectionPolicy) < CAP * sizeof(ChunkEntry) + 128);
}

TEST(collection_chunk_bytes_saturates_and_never_truncates) {
  // A truncating narrow is not a smaller number, it is a *different* one, and the difference is
  // unbounded: 2^32 truncates to zero. Every row below is a size `add_name()` can be handed.
  struct Row {
    uint64_t in;
    uint32_t want;
    const char *why;
  };
  static const Row ROWS[] = {
      {0ull, 0u, "an empty directory entry"},
      {CHUNK, static_cast<uint32_t>(CHUNK), "an ordinary 4 MB chunk"},
      {0xFFFFFFFEull, 0xFFFFFFFEu, "one below the ceiling — still exact"},
      {0xFFFFFFFFull, 0xFFFFFFFFu, "the ceiling itself — still exact"},
      {0x100000000ull, 0xFFFFFFFFu, "2^32, where a truncation reports 0 — the whole point"},
      {0x100000001ull, 0xFFFFFFFFu, "2^32+1, where a truncation reports 1"},
      {5ull * 1024 * 1024 * 1024, 0xFFFFFFFFu, "a 5 GB stranger, which truncates to 1 GB"},
      {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFu, "st_size read as garbage"},
  };
  for (const Row &row : ROWS)
    CHECK_EQ_MSG(chunk_bytes(row.in), row.want, row.why);
}

TEST(collection_add_name_saturates_a_size_past_four_gigabytes) {
  // `add_name()` is the one path whose `bytes` did not come from this component's writer: it is
  // `st_size` off the card, and the file may be a stale or foreign one that merely happens to be
  // named `L#######.LOG`. Nothing bounds it, so the clamp is the only thing that does.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add_name("L0000010.LOG", 0xFFFFFFFFull));
  CHECK_EQ(bytes_of(policy.find(10)), SATURATED);

  CHECK(policy.add_name("L0000011.LOG", 0x100000000ull));  // exactly 2^32
  CHECK_EQ(bytes_of(policy.find(11)), SATURATED);
  CHECK(bytes_of(policy.find(11)) != 0u);  // the wrap this replaces, stated on its own

  CHECK(policy.add_name("L0000012.UPL", 0x100000001ull));
  CHECK_EQ(bytes_of(policy.find(12)), SATURATED);
  CHECK(policy.add_name("L0000013.LOG", 5ull * 1024 * 1024 * 1024));
  CHECK_EQ(bytes_of(policy.find(13)), SATURATED);

  // add() takes the same clamp: the writer reaches it with a uint64_t `file_bytes_`. 6 GiB is the
  // discriminating value — a truncation would report exactly 2 GiB and look plausible.
  CHECK(policy.add(14, 0x180000000ull, ChunkState::SEALED));
  CHECK_EQ(bytes_of(policy.find(14)), SATURATED);

  // and an honest chunk is still recorded to the byte
  CHECK(policy.add(15, CHUNK, ChunkState::SEALED));
  CHECK_EQ(bytes_of(policy.find(15)), CHUNK);
  CHECK_EQ(policy.count(), 6u);
}

TEST(collection_seal_saturates_a_size_past_four_gigabytes) {
  // The other writer of the field. `seal()` records the final size, and that number is the
  // `Content-Length` the collector is handed — a wrapped-small one truncates the transfer, and the
  // puller then dedups a short file against the seq it was told to expect.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 0, ChunkState::OPEN));
  CHECK(policy.seal(10, 0x180000000ull));
  CHECK_EQ(bytes_of(policy.find(10)), SATURATED);

  CollectionPolicy exact = make_policy();
  CHECK(exact.add(11, 0, ChunkState::OPEN));
  CHECK(exact.seal(11, 0x100000000ull));
  CHECK_EQ(bytes_of(exact.find(11)), SATURATED);
  // and the refused second seal leaves the saturated value alone, like any other size
  CHECK(!exact.seal(11, 1));
  CHECK_EQ(bytes_of(exact.find(11)), SATURATED);
}

// ---------------------------------------------------------------------------------- sealing (§3)

TEST(collection_seal_closes_the_open_chunk) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, 0, ChunkState::OPEN));
  CHECK(!policy.is_servable(433));  // an OPEN chunk is never listed and never served
  CHECK(policy.seal(433, CHUNK));
  CHECK_EQ(state_of(policy.find(433)), ChunkState::SEALED);
  CHECK_EQ(bytes_of(policy.find(433)), CHUNK);
  CHECK(policy.is_servable(433));
  CHECK_EQ(policy.count(), 1u);
}

TEST(collection_seal_refuses_anything_but_the_open_chunk) {
  // A second seal would rewrite the recorded size of a file the collector may already be streaming
  // — and `Content-Length` came from that number.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, 500, ChunkState::SEALED));
  CHECK(policy.add(434, 100, ChunkState::CONFIRMED));
  CHECK(!policy.seal(433, 9));
  CHECK_EQ(bytes_of(policy.find(433)), 500u);
  CHECK(!policy.seal(434, 9));
  CHECK_EQ(bytes_of(policy.find(434)), 100u);
  CHECK(!policy.seal(999, 1));  // unknown seq
  CHECK_EQ(policy.count(), 2u);
}

// -------------------------------------------------------------------------------- confirming (§3)

TEST(collection_confirm_promotes_a_sealed_chunk) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, CHUNK, ChunkState::SEALED));
  CHECK(policy.confirm(433));
  CHECK_EQ(state_of(policy.find(433)), ChunkState::CONFIRMED);
  CHECK(!policy.is_servable(433));  // CONFIRMED chunks are not listed again
  CHECK(policy.is_deletable(433));
  CHECK_EQ(bytes_of(policy.find(433)), CHUNK);  // the rename does not change the size
}

TEST(collection_confirm_is_idempotent) {
  // The puller retries a POST whose response it never saw. A second confirm must be a quiet yes,
  // not a state change and not an accounting event.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, CHUNK, ChunkState::SEALED));
  CHECK(policy.confirm(433));
  CHECK(policy.confirm(433));
  CHECK(policy.confirm(433));
  CHECK_EQ(state_of(policy.find(433)), ChunkState::CONFIRMED);
  CHECK_EQ(policy.count(), 1u);
  CHECK(!policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 0u);
}

TEST(collection_confirm_never_touches_the_open_chunk) {
  // A confirm racing rotation would rename the file the writer still holds an fd to: FATFS keeps
  // appending to a directory entry that no longer carries the name anyone will look for.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, 0, ChunkState::OPEN));
  CHECK(!policy.confirm(433));
  CHECK_EQ(state_of(policy.find(433)), ChunkState::OPEN);
  CHECK(!policy.is_deletable(433));
  CHECK_EQ(policy.count(), 1u);
}

TEST(collection_confirm_of_a_discarded_seq_does_not_resurrect_it) {
  // Retention deleted the chunk while the transfer was in flight; the confirm arrives afterwards.
  // The answer is "gone" — an entry created here would name a file that does not exist, and the
  // writer's rename would fail on every pass.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(433, CHUNK, ChunkState::SEALED));
  CHECK(policy.discard(433));
  CHECK(!policy.confirm(433));
  CHECK_EQ(policy.count(), 0u);
  CHECK(policy.find(433) == nullptr);
}

// ----------------------------------------------------------------- servable / deletable (§5b, §7)

TEST(collection_only_sealed_chunks_are_servable) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::OPEN));
  CHECK(policy.add(11, 1, ChunkState::SEALED));
  CHECK(policy.add(12, 1, ChunkState::CONFIRMED));

  CHECK(!policy.is_servable(10));
  CHECK(policy.is_servable(11));
  CHECK(!policy.is_servable(12));
  CHECK(!policy.is_servable(99));

  // is_deletable is the *normal* flow only. The pressure path that deletes SEALED chunks is data
  // loss and is reachable through next_victim() alone.
  CHECK(!policy.is_deletable(10));
  CHECK(!policy.is_deletable(11));
  CHECK(policy.is_deletable(12));
  CHECK(!policy.is_deletable(99));
}

TEST(collection_oldest_uncollected_is_the_oldest_sealed) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(12, 1, ChunkState::SEALED));  // inserted out of order on purpose
  CHECK(policy.add(10, 1, ChunkState::SEALED));
  CHECK(policy.add(11, 1, ChunkState::SEALED));
  uint32_t seq = SEQ_NONE;
  CHECK(policy.oldest_uncollected(&seq));
  CHECK_EQ(seq, 10u);
  CHECK(policy.confirm(10));
  CHECK(policy.oldest_uncollected(&seq));
  CHECK_EQ(seq, 11u);
}

TEST(collection_oldest_uncollected_ignores_open_and_confirmed) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(5, 1, ChunkState::CONFIRMED));  // older, but already collected
  CHECK(policy.add(20, 1, ChunkState::OPEN));      // cannot be collected yet
  CHECK(policy.add(9, 1, ChunkState::SEALED));
  uint32_t seq = SEQ_NONE;
  CHECK(policy.oldest_uncollected(&seq));
  CHECK_EQ(seq, 9u);

  // Nothing waiting is the healthy steady state, not an error — and the out-param is untouched.
  CollectionPolicy drained = make_policy();
  CHECK(drained.add(5, 1, ChunkState::CONFIRMED));
  CHECK(drained.add(20, 1, ChunkState::OPEN));
  seq = SEQ_NONE;
  CHECK(!drained.oldest_uncollected(&seq));
  CHECK_EQ(seq, SEQ_NONE);
}

TEST(collection_oldest_uncollected_across_the_seq_wrap) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(9999998u, 1, ChunkState::SEALED));
  CHECK(policy.add(9999999u, 1, ChunkState::SEALED));
  CHECK(policy.add(0u, 1, ChunkState::SEALED));
  CHECK(policy.add(1u, 1, ChunkState::SEALED));
  uint32_t seq = SEQ_NONE;
  CHECK(policy.oldest_uncollected(&seq));
  CHECK_EQ(seq, 9999998u);  // not 0, which is what a plain `<` would report
}

// --------------------------------------------------------------------------- the serving guard (§7)

TEST(collection_mark_serving_needs_a_tracked_non_open_chunk) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::OPEN));
  CHECK(policy.add(11, 1, ChunkState::SEALED));

  CHECK(!policy.mark_serving(99));  // unknown
  CHECK(!policy.mark_serving(10));  // the open file is never served in the first place
  CHECK(!policy.is_serving(10));
  CHECK(policy.mark_serving(11));
  CHECK(policy.is_serving(11));

  policy.clear_serving(11);
  CHECK(!policy.is_serving(11));
  policy.clear_serving(11);  // idempotent: the handler's error path clears again
  policy.clear_serving(99);  // and an unknown seq must not corrupt anything
  CHECK_EQ(policy.count(), 2u);
  CHECK(!policy.is_serving(11));
}

TEST(collection_serving_survives_the_confirm) {
  // The fixed order is serve -> confirm -> rename, and the confirm can land while the handler is
  // still finishing. Until it clears the flag, retention must keep its hands off.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(11, CHUNK, ChunkState::SEALED));
  CHECK(policy.mark_serving(11));
  CHECK(policy.confirm(11));
  CHECK_EQ(state_of(policy.find(11)), ChunkState::CONFIRMED);
  CHECK(policy.is_serving(11));
  CHECK(!policy.is_deletable(11));
  CHECK(policy.next_victim(100) == nullptr);

  policy.clear_serving(11);
  CHECK(policy.is_deletable(11));
  CHECK_EQ(seq_of(policy.next_victim(100)), 11u);
}

// ---------------------------------------------------------------------- the retention decision (§7)

TEST(collection_retention_is_off_until_configured) {
  CollectionPolicy policy;  // no collection: block at all
  CHECK(!policy.enabled());
  CHECK(policy.add(10, CHUNK, ChunkState::CONFIRMED));
  CHECK(policy.next_victim(100) == nullptr);

  policy.configure(80);
  CHECK(policy.enabled());
  CHECK_EQ(policy.retention_percent(), 80u);
  CHECK_EQ(seq_of(policy.next_victim(100)), 10u);

  policy.disable();
  CHECK(!policy.enabled());
  CHECK(policy.next_victim(100) == nullptr);
  CHECK_EQ(policy.count(), 1u);  // disabling retention does not forget the card
}

TEST(collection_configure_clamps_the_threshold_into_the_open_interval) {
  // V24 rejects these in YAML, but the class is reachable from hand-written code. A threshold of 0
  // would delete on an empty card; one of 100 arms only when there is no room left to seal the
  // file that tripped it.
  CollectionPolicy zero;
  zero.configure(0);
  CHECK(zero.enabled());
  CHECK_EQ(zero.retention_percent(), 1u);

  CollectionPolicy hundred;
  hundred.configure(100);
  CHECK(hundred.enabled());
  CHECK_EQ(hundred.retention_percent(), 99u);

  CollectionPolicy over;
  over.configure(255);
  CHECK_EQ(over.retention_percent(), 99u);

  CollectionPolicy normal;
  normal.configure(90);
  CHECK_EQ(normal.retention_percent(), 90u);
}

TEST(collection_next_victim_arms_at_the_threshold_not_before) {
  CollectionPolicy policy = make_policy(80);
  CHECK(policy.add(10, CHUNK, ChunkState::CONFIRMED));
  CHECK(policy.next_victim(0) == nullptr);
  CHECK(policy.next_victim(79) == nullptr);
  CHECK_EQ(seq_of(policy.next_victim(80)), 10u);  // *at* or above
  CHECK_EQ(seq_of(policy.next_victim(81)), 10u);
  CHECK_EQ(seq_of(policy.next_victim(100)), 10u);
}

TEST(collection_next_victim_prefers_the_oldest_confirmed) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::SEALED));
  CHECK(policy.add(12, 1, ChunkState::CONFIRMED));
  CHECK(policy.add(11, 1, ChunkState::CONFIRMED));
  CHECK_EQ(seq_of(policy.next_victim(90)), 11u);
  CHECK_EQ(state_of(policy.next_victim(90)), ChunkState::CONFIRMED);
}

TEST(collection_next_victim_takes_a_confirmed_chunk_newer_than_every_sealed_one) {
  // Preference beats age *between* the tiers. The collected chunks are the free list, and the
  // whole point of the policy is to exhaust that list before losing anything.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::SEALED));
  CHECK(policy.add(11, 1, ChunkState::SEALED));
  CHECK(policy.add(99, 1, ChunkState::CONFIRMED));
  CHECK_EQ(seq_of(policy.next_victim(95)), 99u);
  CHECK(!policy.has_pending_discards());
}

TEST(collection_next_victim_falls_back_to_the_oldest_sealed) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(12, 1, ChunkState::SEALED));
  CHECK(policy.add(10, 1, ChunkState::SEALED));
  CHECK(policy.add(11, 1, ChunkState::SEALED));
  CHECK_EQ(seq_of(policy.next_victim(95)), 10u);
  CHECK_EQ(state_of(policy.next_victim(95)), ChunkState::SEALED);
}

TEST(collection_next_victim_is_never_the_open_chunk) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, CHUNK, ChunkState::OPEN));
  CHECK(policy.next_victim(100) == nullptr);

  // even when it is the oldest thing on the card
  CHECK(policy.add(11, CHUNK, ChunkState::CONFIRMED));
  CHECK_EQ(seq_of(policy.next_victim(100)), 11u);
  CHECK(policy.discard(11));

  // and with nothing else left, a full card still yields no victim: logging wins.
  CHECK(policy.next_victim(100) == nullptr);
  CHECK_EQ(policy.count(), 1u);
  CHECK_EQ(state_of(policy.find(10)), ChunkState::OPEN);
}

TEST(collection_next_victim_skips_a_chunk_being_served) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::CONFIRMED));
  CHECK(policy.add(11, 1, ChunkState::CONFIRMED));
  CHECK(policy.mark_serving(10));
  CHECK_EQ(seq_of(policy.next_victim(95)), 11u);
  policy.clear_serving(10);
  CHECK_EQ(seq_of(policy.next_victim(95)), 10u);
}

TEST(collection_next_victim_skips_a_served_chunk_in_the_sealed_fallback) {
  // This is the path §7 calls out explicitly: retention deleting SEALED chunks is the only case
  // where deletion and a transfer can overlap at all.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::SEALED));
  CHECK(policy.add(11, 1, ChunkState::SEALED));
  CHECK(policy.mark_serving(10));
  CHECK_EQ(seq_of(policy.next_victim(95)), 11u);

  // and when the only candidate is the one in flight, nothing dies
  CollectionPolicy single = make_policy();
  CHECK(single.add(10, 1, ChunkState::SEALED));
  CHECK(single.mark_serving(10));
  CHECK(single.next_victim(100) == nullptr);
  CHECK_EQ(single.count(), 1u);
}

TEST(collection_next_victim_across_the_seq_wrap) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(0u, 1, ChunkState::CONFIRMED));
  CHECK(policy.add(9999998u, 1, ChunkState::CONFIRMED));
  CHECK(policy.add(1u, 1, ChunkState::CONFIRMED));
  CHECK_EQ(seq_of(policy.next_victim(95)), 9999998u);
  CHECK(policy.discard(9999998u));
  CHECK_EQ(seq_of(policy.next_victim(95)), 0u);
  CHECK(policy.discard(0u));
  CHECK_EQ(seq_of(policy.next_victim(95)), 1u);
}

TEST(collection_every_chunk_confirmed_drains_oldest_first_and_costs_nothing) {
  CollectionPolicy policy = make_policy(90);
  for (uint32_t i = 0; i < 5; i++)
    CHECK_MSG(policy.add(100 + i, 1000, ChunkState::CONFIRMED), std::to_string(i));
  for (uint32_t i = 0; i < 5; i++) {
    CHECK_EQ_MSG(seq_of(policy.next_victim(99)), 100 + i, std::to_string(i));
    CHECK_MSG(policy.discard(100 + i), std::to_string(i));
  }
  CHECK(policy.next_victim(99) == nullptr);
  CHECK_EQ(policy.count(), 0u);
  // Nothing was lost — every one of those chunks is in the archive.
  CHECK(!policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 0u);
  CHECK_EQ(policy.discarded_bytes(), 0u);
}

TEST(collection_every_chunk_sealed_over_threshold_is_billed_as_loss) {
  // The pressure case: collection never ran, or never kept up. Logging continues and the oldest
  // history loses — but it must say so.
  CollectionPolicy policy = make_policy(90);
  for (uint32_t i = 0; i < 3; i++)
    CHECK_MSG(policy.add(200 + i, 1000 * (i + 1), ChunkState::SEALED), std::to_string(i));
  for (uint32_t i = 0; i < 3; i++) {
    CHECK_EQ_MSG(seq_of(policy.next_victim(99)), 200 + i, std::to_string(i));
    CHECK_MSG(policy.discard(200 + i), std::to_string(i));
  }
  CHECK_EQ(policy.count(), 0u);
  CHECK(policy.has_pending_discards());
  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 3u);
  CHECK_EQ(gap.bytes, 6000u);
  CHECK_EQ(gap.first_seq, 200u);
  CHECK_EQ(gap.last_seq, 202u);
}

// ------------------------------------------------------------------------------- discarding (§7)

TEST(collection_discard_of_a_confirmed_chunk_is_not_a_gap) {
  // The collector has it. Deleting it loses nothing, and a `#gap` line here would claim a hole in
  // an archive that is complete.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 4096, ChunkState::CONFIRMED));
  CHECK(policy.discard(10));
  CHECK_EQ(policy.count(), 0u);
  CHECK(policy.find(10) == nullptr);
  CHECK(!policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 0u);
  CHECK_EQ(policy.discarded_bytes(), 0u);
}

TEST(collection_discard_of_a_sealed_chunk_bills_the_gap) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 4096, ChunkState::SEALED));
  CHECK(policy.discard(10));
  CHECK_EQ(policy.count(), 0u);
  CHECK(policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 1u);
  CHECK_EQ(policy.discarded_bytes(), 4096u);
  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 1u);
  CHECK_EQ(gap.bytes, 4096u);
  // A single discarded chunk repeats its seq in both fields: the `#gap` field count never varies,
  // because the reader splits on commas.
  CHECK_EQ(gap.first_seq, 10u);
  CHECK_EQ(gap.last_seq, 10u);
}

TEST(collection_discard_refuses_the_open_chunk_and_a_served_one) {
  // next_victim() already refuses both, and this refuses them again: the caller in between is a
  // task boundary (§8), and the serving flag can be set after the victim was chosen.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::OPEN));
  CHECK(policy.add(11, 1, ChunkState::SEALED));
  CHECK(policy.mark_serving(11));

  CHECK(!policy.discard(10));
  CHECK(!policy.discard(11));
  CHECK_EQ(policy.count(), 2u);
  CHECK(!policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 0u);

  policy.clear_serving(11);
  CHECK(policy.discard(11));
  CHECK_EQ(policy.count(), 1u);
  CHECK_EQ(policy.discarded_chunks(), 1u);
}

TEST(collection_discard_of_an_unknown_seq_is_a_noop) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 1, ChunkState::SEALED));
  CHECK(!policy.discard(11));
  CHECK(!policy.discard(0));
  CHECK_EQ(policy.count(), 1u);
  CHECK(!policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 0u);
  CHECK_EQ(policy.discarded_bytes(), 0u);
}

// ---------------------------------------------------------------------- discard accounting (§7)

TEST(collection_pending_discards_accumulate_until_taken) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(300, 100, ChunkState::SEALED));
  CHECK(policy.add(301, 200, ChunkState::SEALED));
  CHECK(policy.add(302, 300, ChunkState::SEALED));
  CHECK(policy.discard(300));
  CHECK(policy.discard(301));
  CHECK(policy.discard(302));
  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 3u);
  CHECK_EQ(gap.bytes, 600u);
  CHECK_EQ(gap.first_seq, 300u);
  CHECK_EQ(gap.last_seq, 302u);
}

TEST(collection_peeking_at_the_window_does_not_consume_it) {
  // The marker pass checks before it reserves line room, exactly like the `#drop` pass compares
  // against its `marked_` baseline first. Taking the window and *then* finding no room would
  // destroy the only in-band record that the data was lost.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 4096, ChunkState::SEALED));
  CHECK(policy.discard(10));
  CHECK(policy.has_pending_discards());
  CHECK(policy.has_pending_discards());
  CHECK(policy.has_pending_discards());
  CHECK_EQ(policy.discarded_chunks(), 1u);
  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 1u);
  CHECK_EQ(gap.bytes, 4096u);
}

TEST(collection_take_pending_discards_clears_the_window_but_not_the_totals) {
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(10, 100, ChunkState::SEALED));
  CHECK(policy.add(11, 200, ChunkState::SEALED));
  CHECK(policy.discard(10));
  CHECK(policy.discard(11));

  const DiscardStats first = policy.take_pending_discards();
  CHECK_EQ(first.chunks, 2u);
  CHECK_EQ(first.bytes, 300u);
  CHECK(!policy.has_pending_discards());

  // A second take on an empty window is zeroed, not a repeat of the last one — a repeat would put
  // a duplicate `#gap` into every file that rotates after a loss.
  const DiscardStats second = policy.take_pending_discards();
  CHECK_EQ(second.chunks, 0u);
  CHECK_EQ(second.bytes, 0u);
  CHECK_EQ(second.first_seq, 0u);
  CHECK_EQ(second.last_seq, 0u);

  // Lifetime totals survive: they are what `GET /sdlog/status` reports.
  CHECK_EQ(policy.discarded_chunks(), 2u);
  CHECK_EQ(policy.discarded_bytes(), 300u);

  // and the next window starts fresh, with its own seq range
  CHECK(policy.add(50, 400, ChunkState::SEALED));
  CHECK(policy.discard(50));
  const DiscardStats third = policy.take_pending_discards();
  CHECK_EQ(third.chunks, 1u);
  CHECK_EQ(third.bytes, 400u);
  CHECK_EQ(third.first_seq, 50u);
  CHECK_EQ(third.last_seq, 50u);
  CHECK_EQ(policy.discarded_chunks(), 3u);
  CHECK_EQ(policy.discarded_bytes(), 700u);
}

TEST(collection_discard_window_spans_the_seq_wrap) {
  // first/last are the ends of the window in the order retention emptied it, not a numeric min and
  // max: sorting them would print `#gap,…,0,9999999` and claim the entire card went away.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add(9999998u, 10, ChunkState::SEALED));
  CHECK(policy.add(9999999u, 20, ChunkState::SEALED));
  CHECK(policy.add(0u, 30, ChunkState::SEALED));
  CHECK(policy.discard(9999998u));
  CHECK(policy.discard(9999999u));
  CHECK(policy.discard(0u));
  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 3u);
  CHECK_EQ(gap.bytes, 60u);
  CHECK_EQ(gap.first_seq, 9999998u);
  CHECK_EQ(gap.last_seq, 0u);
}

TEST(collection_discarded_bytes_do_not_wrap_at_four_gigabytes) {
  // 12.4 GB a day at the measured rate, so a 32-bit byte total wraps back through zero inside the
  // first day of a card that is shedding — and reports a healthy device while it does.
  CollectionPolicy policy = make_policy();
  const uint64_t big = 512ull * 1024 * 1024;
  for (uint32_t i = 0; i < 10; i++) {
    CHECK_MSG(policy.add(i, big, ChunkState::SEALED), std::to_string(i));
    CHECK_MSG(policy.discard(i), std::to_string(i));
  }
  CHECK_EQ(policy.discarded_chunks(), 10u);
  CHECK_EQ(policy.discarded_bytes(), big * 10);  // 5 368 709 120, past 2^32
  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 10u);
  CHECK_EQ(gap.bytes, big * 10);
  CHECK_EQ(gap.first_seq, 0u);
  CHECK_EQ(gap.last_seq, 9u);
}

TEST(collection_a_saturated_entry_bills_the_saturated_loss) {
  // The consequence that makes the clamp worth having. A boot scan meets a 4 GiB file, retention
  // deletes it: a truncating entry would bill **zero bytes** for that loss, and the `#gap` line
  // would state a deletion that cost nothing. Under-reporting is worse than not reporting, because
  // the line is there and reads healthy — which is exactly what the reader is told to trust.
  CollectionPolicy policy = make_policy();
  CHECK(policy.add_name("L0000010.LOG", 0x100000000ull));  // exactly 2^32
  CHECK_EQ(bytes_of(policy.find(10)), SATURATED);
  CHECK(policy.discard(10));

  CHECK_EQ(policy.discarded_chunks(), 1u);
  CHECK_EQ(policy.discarded_bytes(), SATURATED);
  CHECK(policy.discarded_bytes() != 0u);

  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 1u);
  CHECK_EQ(gap.bytes, SATURATED);
  // The clamp moved the *bytes* and nothing else: one chunk lost, one seq wide, as before.
  CHECK_EQ(gap.first_seq, 10u);
  CHECK_EQ(gap.last_seq, 10u);
}

TEST(collection_discarded_bytes_accumulate_past_thirty_two_bits_at_the_saturation_ceiling) {
  // `ChunkEntry::bytes` shrank; `discarded_bytes_` did not. Two saturated discards already
  // overflow a 32-bit total, so this is the shortest possible proof that the accumulator behind
  // the field is still 64-bit — and ten of them is ~43 GB, which a shedding card reaches in about
  // three and a half days.
  CollectionPolicy policy = make_policy();
  for (uint32_t i = 0; i < 10; i++) {
    CHECK_MSG(policy.add(i, 6ull * 1024 * 1024 * 1024, ChunkState::SEALED), std::to_string(i));
    CHECK_EQ_MSG(bytes_of(policy.find(i)), SATURATED, std::to_string(i));
    CHECK_MSG(policy.discard(i), std::to_string(i));
  }
  CHECK_EQ(policy.discarded_chunks(), 10u);
  CHECK_EQ(policy.discarded_bytes(), 10ull * SATURATED);  // 42 949 672 950, ten times past 2^32
  CHECK(policy.discarded_bytes() > SATURATED);

  // and the window carries the same total: `format_gap()` takes a uint64_t, so this is a
  // pass-through the whole way to the card.
  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 10u);
  CHECK_EQ(gap.bytes, 10ull * SATURATED);
}

TEST(collection_pending_bytes_stay_sixty_four_bit_across_a_take) {
  // The window is emptied and refilled every time a `#gap` line lands, so it is the counter most
  // easily argued into 32 bits. It is not: one retention pass over saturated strangers overflows
  // it in two entries, and the window *after* the take has to be as right as the one before it.
  CollectionPolicy policy = make_policy();
  for (uint32_t i = 0; i < 3; i++) {
    CHECK_MSG(policy.add(i, 0x100000000ull, ChunkState::SEALED), std::to_string(i));
    CHECK_MSG(policy.discard(i), std::to_string(i));
  }
  const DiscardStats first = policy.take_pending_discards();
  CHECK_EQ(first.chunks, 3u);
  CHECK_EQ(first.bytes, 3ull * SATURATED);
  CHECK_EQ(first.first_seq, 0u);
  CHECK_EQ(first.last_seq, 2u);
  CHECK(!policy.has_pending_discards());

  for (uint32_t i = 10; i < 14; i++) {
    CHECK_MSG(policy.add(i, 0x400000000ull, ChunkState::SEALED), std::to_string(i));
    CHECK_MSG(policy.discard(i), std::to_string(i));
  }
  const DiscardStats second = policy.take_pending_discards();
  CHECK_EQ(second.chunks, 4u);
  CHECK_EQ(second.bytes, 4ull * SATURATED);  // the take cleared the window, it did not resize it
  CHECK_EQ(second.first_seq, 10u);
  CHECK_EQ(second.last_seq, 13u);

  // and the lifetime total is the sum of both windows, not the larger of them
  CHECK_EQ(policy.discarded_chunks(), 7u);
  CHECK_EQ(policy.discarded_bytes(), 7ull * SATURATED);
}

TEST(collection_a_confirm_mid_run_punctures_the_window_without_widening_the_loss) {
  // Design §7a's worked example, from the producing side — the reader side of the same window is
  // pinned in tests/sd_logger/test_sdlog_reader.py. Only never-collected (SEALED) chunks are
  // billed, so a confirm landing in the middle of a retention run leaves a hole in the window that
  // is not a hole in the archive: `chunks` is the loss, the seq pair is only the width, and a
  // reader that expands 10..12 into three missing files over-reports the loss by half.
  CollectionPolicy policy = make_policy(90);
  CHECK(policy.add(10, 1000, ChunkState::SEALED));
  CHECK(policy.add(11, 2000, ChunkState::SEALED));
  CHECK(policy.add(12, 3000, ChunkState::SEALED));

  CHECK(policy.discard(10));
  CHECK(policy.confirm(11));  // the puller got this one while retention was working
  CHECK(policy.discard(11));  // so deleting it now costs nothing and is not a gap
  CHECK(policy.discard(12));

  const DiscardStats gap = policy.take_pending_discards();
  CHECK_EQ(gap.chunks, 2u);    // the loss
  CHECK_EQ(gap.bytes, 4000u);  // 1000 + 3000; the collected chunk is not billed
  CHECK_EQ(gap.first_seq, 10u);
  CHECK_EQ(gap.last_seq, 12u);  // a window three seqs wide over a loss of two — punctured
  CHECK_EQ(policy.discarded_chunks(), 2u);
  CHECK_EQ(policy.discarded_bytes(), 4000u);
}

// --------------------------------------------------------------------------------- rotation (§4)

TEST(collection_rotation_is_unbounded_until_configured) {
  CollectionPolicy policy;
  CHECK_EQ(policy.max_file_bytes(), 0u);
  CHECK_EQ(policy.max_file_seconds(), 0u);
  CHECK(!policy.should_rotate(0, 0));
  CHECK(!policy.should_rotate(1ull << 40, 86400ull * 1000000ull));

  policy.configure_rotation(static_cast<uint32_t>(CHUNK), 60);
  CHECK_EQ(policy.max_file_bytes(), static_cast<uint32_t>(CHUNK));
  CHECK_EQ(policy.max_file_seconds(), 60u);
  CHECK(policy.should_rotate(CHUNK, 0));
}

TEST(collection_rotates_exactly_at_the_size_bound) {
  CollectionPolicy policy;
  policy.configure_rotation(static_cast<uint32_t>(CHUNK), 0);
  CHECK(!policy.should_rotate(0, 0));
  CHECK(!policy.should_rotate(CHUNK - 1, 0));
  CHECK(policy.should_rotate(CHUNK, 0));
  CHECK(policy.should_rotate(CHUNK + 1, 0));
  // the time bound is off, so no amount of elapsed time rotates
  CHECK(!policy.should_rotate(CHUNK - 1, 86400ull * 1000000ull));
}

TEST(collection_rotates_exactly_at_the_time_bound) {
  // This is the bound §4 adds, and the reason it exists: a quiet bus otherwise leaves the newest
  // data trapped in an OPEN file indefinitely, and an OPEN file is never servable.
  CollectionPolicy policy;
  policy.configure_rotation(0, 60);
  CHECK(!policy.should_rotate(0, 0));
  CHECK(!policy.should_rotate(0, 59999999ull));
  CHECK(policy.should_rotate(0, 60000000ull));
  CHECK(policy.should_rotate(0, 60000001ull));
  // the size bound is off, so no amount of bytes rotates
  CHECK(!policy.should_rotate(1ull << 40, 59999999ull));
}

TEST(collection_either_bound_rotates) {
  CollectionPolicy policy;
  policy.configure_rotation(static_cast<uint32_t>(CHUNK), 60);
  CHECK(!policy.should_rotate(1000, 1000));
  CHECK(policy.should_rotate(CHUNK, 1000));         // busy bus: size first
  CHECK(policy.should_rotate(1000, 60000000ull));   // parked car: time first
  CHECK(policy.should_rotate(CHUNK, 60000000ull));  // both
}

TEST(collection_a_zero_bound_is_unbounded_not_immediate) {
  // The inverted reading of "0 means unbounded" is "0 means the bound is always met", which
  // rotates on every single record and turns the card into a directory of empty files.
  CollectionPolicy policy;
  policy.configure_rotation(0, 60);
  CHECK(!policy.should_rotate(0, 0));
  CHECK(!policy.should_rotate(1ull << 40, 0));

  policy.configure_rotation(static_cast<uint32_t>(CHUNK), 0);
  CHECK(!policy.should_rotate(0, 0));
  CHECK(!policy.should_rotate(0, 86400ull * 1000000ull));
  CHECK(policy.should_rotate(CHUNK, 0));

  policy.configure_rotation(0, 0);
  CHECK(!policy.should_rotate(1ull << 40, 86400ull * 1000000ull));
}

TEST(collection_time_bound_does_not_overflow_thirty_two_bits) {
  // `max_file_seconds` is seconds and `elapsed_us` is microseconds, so the conversion is a
  // multiply by 1 000 000 that must happen in 64 bits. Done in 32, 4295 s wraps to ~0.7 s and the
  // file rotates almost immediately — which on the card looks like a component that works, until
  // someone counts the files.
  CollectionPolicy policy;
  policy.configure_rotation(0, 4295);
  CHECK_EQ(policy.max_file_seconds(), 4295u);
  CHECK(!policy.should_rotate(0, 1000000ull));
  CHECK(!policy.should_rotate(0, 4294999999ull));
  CHECK(policy.should_rotate(0, 4295000000ull));

  CollectionPolicy huge;
  huge.configure_rotation(0, 4294967295u);
  CHECK_EQ(huge.max_file_seconds(), 4294967295u);
  CHECK(!huge.should_rotate(0, 0));
  CHECK(!huge.should_rotate(0, 4294967294ull * 1000000ull));
  CHECK(huge.should_rotate(0, 4294967295ull * 1000000ull));
}
