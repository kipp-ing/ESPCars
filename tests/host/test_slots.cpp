// TX slot lifecycle: OutstandingTracker (bus-off orphan reclaim) and SlotPool.
//
// Why this matters (Todo 4): the tracker decides which TX slots are handed back
// to the pool. A slot released twice is re-acquired and overwritten while the
// TWAI driver still holds a pointer into it — the driver transmits whatever the
// second user wrote. The HIL bench cannot reach any of this: the release gate
// asserts bus-off never happens, and orphan reclaim only runs after a bus-off.
//
// Contract under test (gateway_core.h OutstandingTracker):
//   - push() mirrors the driver FIFO, oldest first; returns false when full
//   - complete(done) reclaims exactly the entries OLDER than `done`, once, in
//     order, and returns how many; `done` itself is popped but not reclaimed
//   - complete() of an untracked item mutates nothing and reclaims nothing
//   - reclaim_head() pops only the head
//   - an entry newer than `done` is never released

#include "gateway_core.h"
#include "harness.h"

#include <cstdint>
#include <vector>

using namespace esphome::can_gateway;

namespace {

/// Records every reclaim callback so a test can assert order and multiplicity.
struct ReclaimLog {
  std::vector<uint8_t> items;
  void operator()(uint8_t item) { this->items.push_back(item); }
};

}  // namespace

// ---------------------------------------------------------------------------
// OutstandingTracker
// ---------------------------------------------------------------------------

TEST(tracker_starts_empty) {
  OutstandingTracker<uint8_t, 4> tracker;
  CHECK_EQ(tracker.size(), 0);
  uint8_t out = 0xEE;
  CHECK_EQ(tracker.reclaim_head(out), false);
  CHECK_EQ(out, 0xEE);  // untouched on failure

  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{1}, log), 0);
  CHECK_EQ(log.items.size(), 0u);
}

TEST(tracker_push_reports_full_without_mutating) {
  OutstandingTracker<uint8_t, 4> tracker;
  for (uint8_t i = 0; i < 4; i++)
    CHECK_EQ(tracker.push(i), true);
  CHECK_EQ(tracker.size(), 4);

  CHECK_EQ(tracker.push(uint8_t{99}), false);
  CHECK_EQ(tracker.size(), 4);

  // The rejected item must not have displaced anything.
  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{3}, log), 3);
  CHECK_EQ(log.items.size(), 3u);
  CHECK_EQ(log.items[0], 0);
  CHECK_EQ(log.items[1], 1);
  CHECK_EQ(log.items[2], 2);
  CHECK_EQ(tracker.size(), 0);
}

TEST(tracker_complete_head_reclaims_nothing) {
  // Normal operation: completions arrive in submission order, so the completed
  // frame is always the head and nothing is orphaned.
  OutstandingTracker<uint8_t, 4> tracker;
  tracker.push(1);
  tracker.push(2);
  tracker.push(3);

  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{1}, log), 0);
  CHECK_EQ(log.items.size(), 0u);
  CHECK_EQ(tracker.size(), 2);

  CHECK_EQ(tracker.complete(uint8_t{2}, log), 0);
  CHECK_EQ(tracker.complete(uint8_t{3}, log), 0);
  CHECK_EQ(tracker.size(), 0);
  CHECK_EQ(log.items.size(), 0u);
}

TEST(tracker_complete_reclaims_older_entries_in_order) {
  OutstandingTracker<uint8_t, 8> tracker;
  for (uint8_t i = 1; i <= 5; i++)
    tracker.push(i);

  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{4}, log), 3);
  CHECK_EQ(log.items.size(), 3u);
  CHECK_EQ(log.items[0], 1);  // oldest first
  CHECK_EQ(log.items[1], 2);
  CHECK_EQ(log.items[2], 3);
  CHECK_EQ(tracker.size(), 1);  // only 5 is still in flight

  // 5 is now the head; completing it reclaims nothing more.
  CHECK_EQ(tracker.complete(uint8_t{5}, log), 0);
  CHECK_EQ(log.items.size(), 3u);
  CHECK_EQ(tracker.size(), 0);
}

TEST(tracker_reclaims_each_orphan_exactly_once) {
  // The whole point of Todo 4: an orphan handed to the caller must never be
  // handed out a second time, or the slot is released twice.
  OutstandingTracker<uint8_t, 8> tracker;
  for (uint8_t i = 0; i < 6; i++)
    tracker.push(i);

  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{3}, log), 3);  // 0,1,2 orphaned
  CHECK_EQ(tracker.complete(uint8_t{5}, log), 1);  // only 4 orphaned

  int seen[8] = {0};
  for (uint8_t item : log.items)
    seen[item]++;
  CHECK_EQ(seen[0], 1);
  CHECK_EQ(seen[1], 1);
  CHECK_EQ(seen[2], 1);
  CHECK_EQ(seen[3], 0);  // completed, released by the caller
  CHECK_EQ(seen[4], 1);
  CHECK_EQ(seen[5], 0);
  CHECK_EQ(tracker.size(), 0);
}

TEST(tracker_never_reclaims_a_newer_entry) {
  // Entries submitted after `done` are still live in the driver.
  OutstandingTracker<uint8_t, 8> tracker;
  for (uint8_t i = 1; i <= 5; i++)
    tracker.push(i);

  ReclaimLog log;
  tracker.complete(uint8_t{2}, log);
  for (uint8_t item : log.items)
    CHECK_MSG(item < 2, "reclaimed a slot newer than the completed one");
  CHECK_EQ(tracker.size(), 3);  // 3,4,5 still in flight

  // ... and they are still tracked in the right order.
  ReclaimLog log2;
  CHECK_EQ(tracker.complete(uint8_t{5}, log2), 2);
  CHECK_EQ(log2.items[0], 3);
  CHECK_EQ(log2.items[1], 4);
}

TEST(tracker_complete_unknown_mutates_nothing) {
  OutstandingTracker<uint8_t, 4> tracker;
  tracker.push(1);
  tracker.push(2);

  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{9}, log), 0);
  CHECK_EQ(log.items.size(), 0u);
  CHECK_EQ(tracker.size(), 2);

  // State is intact: the FIFO still starts at 1.
  CHECK_EQ(tracker.complete(uint8_t{2}, log), 1);
  CHECK_EQ(log.items.size(), 1u);
  CHECK_EQ(log.items[0], 1);
  CHECK_EQ(tracker.size(), 0);
}

TEST(tracker_complete_on_empty_tracker_is_inert) {
  OutstandingTracker<uint8_t, 4> tracker;
  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{0}, log), 0);
  CHECK_EQ(log.items.size(), 0u);
  CHECK_EQ(tracker.size(), 0);
  CHECK_EQ(tracker.push(uint8_t{7}), true);
  CHECK_EQ(tracker.complete(uint8_t{7}, log), 0);
  CHECK_EQ(tracker.size(), 0);
}

TEST(tracker_reclaim_head_pops_only_the_head) {
  OutstandingTracker<uint8_t, 4> tracker;
  tracker.push(1);
  tracker.push(2);
  tracker.push(3);

  uint8_t out = 0;
  CHECK_EQ(tracker.reclaim_head(out), true);
  CHECK_EQ(out, 1);
  CHECK_EQ(tracker.size(), 2);

  // 2 is the head now, so completing it reclaims nothing.
  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{2}, log), 0);
  CHECK_EQ(log.items.size(), 0u);
  CHECK_EQ(tracker.size(), 1);
}

TEST(tracker_reclaim_head_then_complete_never_double_releases) {
  // The bus-off sequence from the class comment: the control plane frees the
  // halted head eagerly while the port is bus-off, then the replayed frames
  // complete normally after recovery. Every slot must be released exactly once.
  OutstandingTracker<uint8_t, 8> tracker;
  for (uint8_t i = 0; i < 4; i++)
    tracker.push(i);

  int released[8] = {0};

  uint8_t halted = 0xFF;
  CHECK_EQ(tracker.reclaim_head(halted), true);
  CHECK_EQ(halted, 0);
  released[halted]++;  // control plane releases the halted frame

  // Recovery replays 1,2,3; suppose only 3's completion is observed.
  ReclaimLog log;
  uint8_t reclaimed = tracker.complete(uint8_t{3}, log);
  CHECK_EQ(reclaimed, 2);
  for (uint8_t item : log.items)
    released[item]++;
  released[3]++;  // the caller releases the completed slot itself

  for (uint8_t i = 0; i < 4; i++)
    CHECK_EQ_MSG(released[i], 1, "slot released exactly once");
  CHECK_EQ(tracker.size(), 0);
}

TEST(tracker_reclaim_head_empties_the_fifo_one_at_a_time) {
  OutstandingTracker<uint8_t, 4> tracker;
  tracker.push(7);
  tracker.push(8);

  uint8_t out = 0;
  CHECK_EQ(tracker.reclaim_head(out), true);
  CHECK_EQ(out, 7);
  CHECK_EQ(tracker.reclaim_head(out), true);
  CHECK_EQ(out, 8);
  CHECK_EQ(tracker.reclaim_head(out), false);
  CHECK_EQ(out, 8);  // unchanged when empty
  CHECK_EQ(tracker.size(), 0);
}

TEST(tracker_wraps_around_the_ring) {
  // head_ advances modulo N; run well past N so a wrap bug shows up as a
  // mis-ordered or lost entry.
  OutstandingTracker<uint8_t, 4> tracker;
  ReclaimLog log;
  uint8_t next = 0;
  for (int cycle = 0; cycle < 50; cycle++) {
    for (int i = 0; i < 3; i++)
      CHECK_EQ(tracker.push(next++), true);
    CHECK_EQ(tracker.size(), 3);
    uint8_t newest = static_cast<uint8_t>(next - 1);
    CHECK_EQ(tracker.complete(newest, log), 2);
    CHECK_EQ(tracker.size(), 0);
  }
  CHECK_EQ(log.items.size(), 100u);
  // Cycle c pushed 3c, 3c+1, 3c+2 and completed 3c+2, so the reclaims must be
  // exactly 3c and 3c+1, in that order, for every cycle.
  for (int cycle = 0; cycle < 50; cycle++) {
    CHECK_EQ(log.items[static_cast<size_t>(cycle) * 2], static_cast<uint8_t>(cycle * 3));
    CHECK_EQ(log.items[static_cast<size_t>(cycle) * 2 + 1], static_cast<uint8_t>(cycle * 3 + 1));
  }
}

TEST(tracker_wraps_with_a_persistently_full_fifo) {
  OutstandingTracker<uint8_t, 4> tracker;
  ReclaimLog log;
  for (uint8_t i = 0; i < 4; i++)
    tracker.push(i);
  for (int i = 0; i < 40; i++) {
    uint8_t head_value = static_cast<uint8_t>(i % 256);
    CHECK_EQ(tracker.complete(head_value, log), 0);  // FIFO order, no orphans
    CHECK_EQ(tracker.size(), 3);
    CHECK_EQ(tracker.push(static_cast<uint8_t>(i + 4)), true);
    CHECK_EQ(tracker.size(), 4);
  }
  CHECK_EQ(log.items.size(), 0u);
}

TEST(tracker_capacity_one_behaves) {
  OutstandingTracker<uint8_t, 1> tracker;
  CHECK_EQ(tracker.push(uint8_t{5}), true);
  CHECK_EQ(tracker.push(uint8_t{6}), false);
  ReclaimLog log;
  CHECK_EQ(tracker.complete(uint8_t{5}, log), 0);
  CHECK_EQ(tracker.size(), 0);
  CHECK_EQ(tracker.push(uint8_t{6}), true);
  uint8_t out = 0;
  CHECK_EQ(tracker.reclaim_head(out), true);
  CHECK_EQ(out, 6);
}

TEST(tracker_handles_pointer_items) {
  // The component instantiates the tracker over TxSlot pointers, not indices.
  int a = 1, b = 2, c = 3;
  OutstandingTracker<int *, 4> tracker;
  tracker.push(&a);
  tracker.push(&b);
  tracker.push(&c);

  std::vector<int *> reclaimed;
  auto sink = [&reclaimed](int *p) { reclaimed.push_back(p); };
  CHECK_EQ(tracker.complete(&c, sink), 2);
  CHECK_EQ(reclaimed.size(), 2u);
  CHECK_EQ(reclaimed[0], &a);
  CHECK_EQ(reclaimed[1], &b);
  CHECK_EQ(tracker.size(), 0);
}

// ---------------------------------------------------------------------------
// SlotPool — the pool the tracker hands slots back to
// ---------------------------------------------------------------------------

TEST(slot_pool_acquires_every_slot_then_reports_exhaustion) {
  SlotPool<4> pool;
  CHECK_EQ(SlotPool<4>::capacity(), 4);
  CHECK_EQ(pool.in_use(), 0);

  uint8_t slots[4];
  for (uint8_t i = 0; i < 4; i++) {
    slots[i] = pool.acquire();
    CHECK_MSG(slots[i] != SLOT_NONE, "pool exhausted early");
    CHECK(pool.is_used(slots[i]));
  }
  CHECK_EQ(pool.in_use(), 4);
  CHECK_EQ(pool.acquire(), SLOT_NONE);
  CHECK_EQ(pool.in_use(), 4);  // a failed acquire must not mark anything

  // All indices are distinct.
  bool seen[4] = {false, false, false, false};
  for (uint8_t i = 0; i < 4; i++) {
    CHECK_MSG(!seen[slots[i]], "the pool handed out the same slot twice");
    seen[slots[i]] = true;
  }
}

TEST(slot_pool_release_makes_a_slot_available_again) {
  SlotPool<3> pool;
  uint8_t a = pool.acquire();
  uint8_t b = pool.acquire();
  uint8_t c = pool.acquire();
  CHECK_EQ(pool.acquire(), SLOT_NONE);

  pool.release(b);
  CHECK_EQ(pool.is_used(b), false);
  CHECK_EQ(pool.in_use(), 2);

  uint8_t again = pool.acquire();
  CHECK_EQ(again, b);
  CHECK_EQ(pool.in_use(), 3);

  pool.release(a);
  pool.release(c);
  pool.release(again);
  CHECK_EQ(pool.in_use(), 0);
}

TEST(slot_pool_double_release_frees_a_slot_only_once) {
  // release() is idempotent per index — the danger of a double release is not
  // in the pool bookkeeping but in the caller handing out the same index twice
  // (see the tracker cases above).
  SlotPool<2> pool;
  uint8_t a = pool.acquire();
  pool.release(a);
  pool.release(a);
  CHECK_EQ(pool.in_use(), 0);
  CHECK_EQ(pool.acquire(), a);
  CHECK_EQ(pool.in_use(), 1);
}
