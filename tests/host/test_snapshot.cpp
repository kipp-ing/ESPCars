// SnapshotRing: the seqlock-protected keep-newest ring behind the last_frame
// debug sensor.
//
// Contract under test (gateway_core.h SnapshotRing):
//   - read_latest() returns the NEWEST record, not the oldest
//   - it returns false when the ring is empty and when nothing new arrived
//   - overwritten pushes are counted in missed()
//   - a record torn by a concurrent producer is never returned: the seqlock
//     stamp must make read_latest() retry or give up, never hand out a mix
//
// The last property needs a real concurrent producer, so this file starts a
// thread. The producer/consumer race on Entry::value is intentional (that is
// what the seq stamp guards), which is why this target runs under ASan/UBSan
// rather than TSAN — TSAN reports every seqlock as a data race by design.

#include "gateway_core.h"
#include "harness.h"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace esphome::can_gateway;

namespace {

/// A snapshot whose every field is derived from `k`, so any mix of two
/// different pushes is detectable from the record alone.
FrameSnapshot snapshot_for(uint32_t k) {
  FrameSnapshot snapshot{};
  snapshot.can_id = k;
  snapshot.dlc = static_cast<uint8_t>(k % (MAX_FRAME_DATA_LEN + 1));
  snapshot.extended = (k & 1u) != 0;
  snapshot.rtr = (k & 2u) != 0;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    snapshot.data[i] = static_cast<uint8_t>(k * 31u + i);
  return snapshot;
}

bool snapshot_is_consistent(const FrameSnapshot &snapshot) {
  FrameSnapshot expected = snapshot_for(snapshot.can_id);
  if (snapshot.dlc != expected.dlc || snapshot.extended != expected.extended || snapshot.rtr != expected.rtr)
    return false;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    if (snapshot.data[i] != expected.data[i])
      return false;
  }
  return true;
}

}  // namespace

TEST(snapshot_ring_empty_reads_false) {
  SnapshotRing<4> ring;
  FrameSnapshot out{};
  CHECK_EQ(ring.read_latest(out), false);
  CHECK_EQ(ring.pushed(), 0u);
  CHECK_EQ(ring.missed(), 0u);
}

TEST(snapshot_ring_returns_the_single_pushed_record) {
  SnapshotRing<4> ring;
  ring.push(snapshot_for(0x123));

  FrameSnapshot out{};
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, 0x123u);
  CHECK_MSG(snapshot_is_consistent(out), "returned record is internally inconsistent");
  CHECK_EQ(ring.pushed(), 1u);
  CHECK_EQ(ring.missed(), 0u);
}

TEST(snapshot_ring_round_trips_every_field) {
  SnapshotRing<2> ring;
  FrameSnapshot in{};
  in.can_id = MAX_EXTENDED_ID;
  in.dlc = 8;
  in.extended = true;
  in.rtr = true;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    in.data[i] = static_cast<uint8_t>(0x10 + i);
  ring.push(in);

  FrameSnapshot out{};
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, MAX_EXTENDED_ID);
  CHECK_EQ(out.dlc, 8);
  CHECK_EQ(out.extended, true);
  CHECK_EQ(out.rtr, true);
  CHECK_BYTES(out.data, in.data, MAX_FRAME_DATA_LEN);
}

TEST(snapshot_ring_returns_the_newest_not_the_oldest) {
  SnapshotRing<4> ring;
  for (uint32_t k = 1; k <= 3; k++)
    ring.push(snapshot_for(k));

  FrameSnapshot out{};
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, 3u);
  CHECK_EQ(ring.missed(), 2u);  // 1 and 2 were never seen
  CHECK_EQ(ring.pushed(), 3u);
}

TEST(snapshot_ring_second_read_without_a_push_is_false) {
  SnapshotRing<4> ring;
  ring.push(snapshot_for(7));

  FrameSnapshot out{};
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, 7u);

  out = FrameSnapshot{};
  CHECK_EQ(ring.read_latest(out), false);
  CHECK_EQ(out.can_id, 0u);  // out is not touched on a false return
  CHECK_EQ(ring.missed(), 0u);

  ring.push(snapshot_for(8));
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, 8u);
  CHECK_EQ(ring.missed(), 0u);
}

TEST(snapshot_ring_overwrites_the_oldest_and_counts_the_misses) {
  SnapshotRing<4> ring;
  for (uint32_t k = 1; k <= 10; k++)
    ring.push(snapshot_for(k));

  FrameSnapshot out{};
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, 10u);
  CHECK_MSG(snapshot_is_consistent(out), "wrapped ring returned a mixed record");
  CHECK_EQ(ring.pushed(), 10u);
  CHECK_EQ(ring.missed(), 9u);

  for (uint32_t k = 11; k <= 17; k++)
    ring.push(snapshot_for(k));
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, 17u);
  CHECK_EQ(ring.pushed(), 17u);
  CHECK_EQ(ring.missed(), 15u);  // 9 + the 6 skipped in this batch
}

TEST(snapshot_ring_drained_every_push_misses_nothing) {
  SnapshotRing<4> ring;
  FrameSnapshot out{};
  for (uint32_t k = 1; k <= 50; k++) {
    ring.push(snapshot_for(k));
    CHECK_EQ(ring.read_latest(out), true);
    CHECK_EQ_MSG(out.can_id, k, "keep-newest read returned the wrong record");
    CHECK_MSG(snapshot_is_consistent(out), "record is internally inconsistent");
  }
  CHECK_EQ(ring.missed(), 0u);
  CHECK_EQ(ring.pushed(), 50u);
}

TEST(snapshot_ring_minimum_size_two) {
  SnapshotRing<2> ring;
  ring.push(snapshot_for(1));
  ring.push(snapshot_for(2));
  ring.push(snapshot_for(3));
  FrameSnapshot out{};
  CHECK_EQ(ring.read_latest(out), true);
  CHECK_EQ(out.can_id, 3u);
  CHECK_EQ(ring.missed(), 2u);
}

// Regression test for the seqlock reader fence in SnapshotRing::read_latest.
// Before the fix this reproduced reliably on arm64 (~666 torn reads in 125k):
// the validation used a bare acquire load, which orders only what follows it, so
// the non-atomic payload copy could sink below seq_after and a torn record passed
// validation. If this ever fails again, the acquire fence went missing.
TEST(snapshot_ring_concurrent_reader_never_sees_a_torn_record) {
  // A producer thread stands in for the RX ISR. Every returned record must be
  // internally consistent; a seqlock failure shows up as a record whose payload
  // belongs to a different can_id than its header. read_latest() giving up
  // (false) after three attempts IS allowed by the contract and is not counted.
  //
  // Several rounds: on a weakly-ordered host roughly every round trips, but the
  // window is narrow enough that a single round is not certain.
  static constexpr uint32_t PUSH_COUNT = 400000;
  static constexpr int ROUNDS = 20;

  uint32_t torn = 0;
  uint64_t reads = 0;
  uint32_t rounds_run = 0;
  bool went_backwards = false;

  for (int round = 0; round < ROUNDS && torn == 0; round++) {
    rounds_run++;
    SnapshotRing<4> ring;
    std::atomic<bool> done{false};

    std::thread producer([&ring, &done]() {
      for (uint32_t k = 1; k <= PUSH_COUNT; k++)
        ring.push(snapshot_for(k));
      done.store(true, std::memory_order_release);
    });

    uint32_t last_id = 0;
    while (!done.load(std::memory_order_acquire)) {
      FrameSnapshot out{};
      if (ring.read_latest(out)) {
        reads++;
        if (!snapshot_is_consistent(out))
          torn++;
        if (out.can_id < last_id)
          went_backwards = true;
        last_id = out.can_id;
      }
    }
    producer.join();

    // Final drain after the producer stopped.
    FrameSnapshot out{};
    if (ring.read_latest(out)) {
      reads++;
      if (!snapshot_is_consistent(out))
        torn++;
    }
    CHECK_EQ(ring.pushed(), PUSH_COUNT);
  }

  CHECK_EQ_MSG(torn, 0u, "read_latest() returned a record torn by the producer");
  CHECK_MSG(reads > 0, "the consumer never observed a single record");
  CHECK_MSG(!went_backwards, "keep-newest ring returned an older record than a previous read");
  std::printf("           (%u round%s, %llu successful reads, %u torn)\n", rounds_run, rounds_run == 1 ? "" : "s",
              static_cast<unsigned long long>(reads), torn);
}
