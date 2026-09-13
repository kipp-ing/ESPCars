// The chunk index at a capacity that is neither the shipped default nor the one before it
// (components/sd_logger/collection_policy.h, `SD_LOG_MAX_CHUNKS`).
//
// `test_collection_policy.cpp` writes every capacity case in terms of the macro rather than a
// literal, which is necessary but not sufficient: at the default those cases would pass unchanged
// against a class that had 256 hardcoded in `add()` and `full()`. Only a build at a *different*
// capacity can tell the two apart, and `collection: max_chunks:` (V26, [16, 2048]) makes that a
// supported configuration rather than a hypothetical one.
//
// **Its own translation unit, and the header included inside an anonymous namespace.** The macro
// sizes a static array *member*, so overriding it produces a different `CollectionPolicy` — and
// the sibling file in the same binary needs the default one. Two TUs defining the same
// external-linkage class differently is an ODR violation that a linker resolves by picking one
// definition arbitrarily; here that would mean `add()`'s bound and the array it indexes coming
// from different builds, which is a buffer overrun with a plausible-looking test result in front
// of it. The anonymous namespace gives this copy internal linkage, so both capacities coexist
// honestly. The system headers `collection_policy.h` pulls in are included at global scope first,
// where their include guards make the nested inclusions no-ops.
//
// Nothing here is a copy of the source: it is the real header, compiled twice.
//
// The capacity is 19 — not 64 (the old default), not 256 (the current one), not 16 or 2048 (the
// schema's bounds), and not a power of two. Anything in the class that reached for a literal
// instead of the macro fails here and passes everywhere else.

#include "harness.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#define SD_LOG_MAX_CHUNKS 19
namespace {
#include "collection_policy.h"
}  // namespace

namespace {

using esphome::sd_logger::ChunkEntry;
using esphome::sd_logger::ChunkState;
using esphome::sd_logger::CollectionPolicy;

static const uint32_t SEQ_NONE = 0xFFFFFFFFu;

uint32_t seq_of(const ChunkEntry *entry) { return entry == nullptr ? SEQ_NONE : entry->seq; }
uint64_t bytes_of(const ChunkEntry *entry) { return entry == nullptr ? 0 : entry->bytes; }

static const uint32_t CAP = static_cast<uint32_t>(SD_LOG_MAX_CHUNKS);

}  // namespace

TEST(collection_capacity_comes_from_the_macro_not_a_literal) {
  // First that the override took at all — a case built at the default would prove nothing below.
  CHECK_EQ(CAP, 19u);
  CHECK_EQ(sizeof(ChunkEntry), 12u);
  CHECK(sizeof(CollectionPolicy) >= CAP * sizeof(ChunkEntry));
  CHECK(sizeof(CollectionPolicy) < CAP * sizeof(ChunkEntry) + 128);

  CollectionPolicy policy;
  policy.configure(80);
  for (uint32_t i = 0; i + 1 < CAP; i++) {
    CHECK_MSG(policy.add(i, 10 + i, ChunkState::SEALED), std::to_string(i));
    CHECK_MSG(!policy.full(), std::to_string(i));
  }
  CHECK(policy.add(CAP - 1, 10 + CAP - 1, ChunkState::SEALED));
  CHECK(policy.full());
  CHECK_EQ(policy.count(), CAP);

  // A hardcoded 64 or 256 shows here twice over: `add()` would keep accepting, and the nineteenth
  // entry is the end of the array, so ASan is the second assertion in this case.
  CHECK(!policy.add(CAP, 1, ChunkState::SEALED));
  CHECK(!policy.add_name("L0000999.LOG", 1));
  CHECK_EQ(policy.count(), CAP);
  CHECK(policy.find(CAP) == nullptr);
  CHECK(policy.at(static_cast<uint16_t>(CAP)) == nullptr);

  // and everything tracked survived the refusals intact
  for (uint32_t i = 0; i < CAP; i++) {
    CHECK_EQ_MSG(seq_of(policy.at(static_cast<uint16_t>(i))), i, std::to_string(i));
    CHECK_EQ_MSG(bytes_of(policy.find(i)), 10 + i, std::to_string(i));
  }
}

TEST(collection_capacity_recovers_at_a_non_default_bound) {
  // The same recovery the default-capacity suite asserts, at a bound the default-capacity suite
  // cannot reach: retention frees one slot and the next rotation is tracked again.
  CollectionPolicy policy;
  policy.configure(90);
  for (uint32_t i = 0; i < CAP; i++)
    CHECK_MSG(policy.add(100 + i, 1, ChunkState::SEALED), std::to_string(i));
  CHECK(policy.full());
  CHECK(!policy.add(999, 1, ChunkState::SEALED));

  // retention still chooses, and it chooses the oldest
  CHECK_EQ(seq_of(policy.next_victim(95)), 100u);

  // free a slot out of the middle instead, so the hole-close is what makes room
  CHECK(policy.discard(105));
  CHECK(!policy.full());
  CHECK_EQ(policy.count(), CAP - 1);
  CHECK_EQ(seq_of(policy.at(5)), 106u);
  CHECK_EQ(policy.discarded_chunks(), 1u);

  CHECK(policy.add(999, 1, ChunkState::OPEN));
  CHECK(policy.full());
  CHECK_EQ(policy.count(), CAP);
  CHECK_EQ(seq_of(policy.at(static_cast<uint16_t>(CAP - 1))), 999u);
  CHECK_EQ(seq_of(policy.find(999)), 999u);
}
