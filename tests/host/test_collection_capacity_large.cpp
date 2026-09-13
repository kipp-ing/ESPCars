// The chunk index at a capacity *above* the shipped default, where `SD_LOG_SEQ_WRAP_WINDOW` has to
// grow with it (components/sd_logger/collection_policy.h).
//
// The sibling `test_collection_capacity.cpp` builds below the default and catches a bound that was
// hardcoded rather than read from the macro. This one exists for the other branch of
//
//     SD_LOG_SEQ_WRAP_WINDOW = SD_LOG_MAX_CHUNKS > 256 ? SD_LOG_MAX_CHUNKS : 256
//
// which nothing else in the suite compiles: at the default the ternary picks the floor, so every
// case in the default build passes just as well against the flat `= 256u` this replaced. Only a
// build past 256 can tell those two apart, and `collection: max_chunks:` accepts up to 2048, so it
// is a supported configuration and not a hypothetical.
//
// Same anonymous-namespace inclusion as the sibling, for the same ODR reason — see its header
// comment. The capacity is 307: past the default, prime, and not a round number anything would
// arrive at by accident.

#include "harness.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#define SD_LOG_MAX_CHUNKS 307
namespace {
#include "collection_policy.h"
}  // namespace

namespace {

using esphome::sd_logger::ChunkEntry;
using esphome::sd_logger::ChunkState;
using esphome::sd_logger::CollectionPolicy;
using esphome::sd_logger::SD_LOG_SEQ_MODULUS;
using esphome::sd_logger::SD_LOG_SEQ_WRAP_WINDOW;

static const uint32_t SEQ_NONE = 0xFFFFFFFFu;

uint32_t seq_of(const ChunkEntry *entry) { return entry == nullptr ? SEQ_NONE : entry->seq; }

static const uint32_t CAP = static_cast<uint32_t>(SD_LOG_MAX_CHUNKS);

}  // namespace

TEST(collection_large_capacity_widens_the_wrap_window_with_it) {
  CHECK_EQ(CAP, 307u);
  CHECK_EQ(SD_LOG_SEQ_WRAP_WINDOW, CAP);  // the ternary's other branch, compiled only here

  // and the property the constant exists for, stated functionally: a monotonic run that fills the
  // index and straddles the 9999999 -> 0 ceiling still orders oldest-first. Its widest pair is 306
  // apart, so a window left at a flat 256 drops it onto the magnitude comparison and retention
  // deletes the newest chunk on the card instead of the oldest — once, on the one boot that wraps,
  // with nothing left afterwards to show what happened.
  CollectionPolicy policy;
  policy.configure(90);
  const uint32_t first = SD_LOG_SEQ_MODULUS - CAP / 2;
  for (uint32_t i = 0; i < CAP; i++)
    CHECK_MSG(policy.add((first + i) % SD_LOG_SEQ_MODULUS, 1, ChunkState::SEALED), std::to_string(i));
  CHECK(policy.full());

  uint32_t oldest = SEQ_NONE;
  CHECK(policy.oldest_uncollected(&oldest));
  CHECK_EQ(oldest, first);
  CHECK_EQ(seq_of(policy.next_victim(95)), first);
}

TEST(collection_large_capacity_fills_and_refuses_at_its_own_bound) {
  // A bound hardcoded to 256 stops the index 51 entries short of the array it was given, which on a
  // device is 21 minutes of coverage silently unspent — the same shape of bug as the 64-entry
  // default this whole change came from, and invisible at the default capacity.
  CollectionPolicy policy;
  policy.configure(80);
  for (uint32_t i = 0; i + 1 < CAP; i++) {
    CHECK_MSG(policy.add(i, 1, ChunkState::SEALED), std::to_string(i));
    CHECK_MSG(!policy.full(), std::to_string(i));
  }
  CHECK(policy.add(CAP - 1, 1, ChunkState::SEALED));
  CHECK(policy.full());
  CHECK_EQ(policy.count(), CAP);
  CHECK(!policy.add(CAP, 1, ChunkState::SEALED));
  CHECK_EQ(policy.count(), CAP);
}
