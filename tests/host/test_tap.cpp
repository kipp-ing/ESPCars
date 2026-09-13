// Datalogger tap plane: TapRecord and ObserveRing<N, TapRecord>.
//
// Contract under test (gateway_core.h):
//   - TapRecord stays 20 bytes: at the design load (90% of 500 kbps on two
//     segments, ~7200 frames/s) every byte costs ~7 KB/s of ring bandwidth
//   - templating ObserveRing on the record type changed nothing about the queue
//     contract it already had: FIFO order, shed-the-newest on overflow, and a
//     rejected push that mutates nothing
//   - the flag encoding is shared by the ISR producer and the writer, so the
//     two cannot drift apart on what bit means what
//   - the SPSC contract holds against a REAL thread, not a reasoned argument:
//     no record is torn, duplicated, reordered or invented under contention.
//     This is the case CLAUDE.md demands for a concurrency contract, and the
//     one that found the SnapshotRing reader fence.

#include "gateway_core.h"
#include "harness.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace esphome::can_gateway;

namespace {

// Aliases, not raw ObserveRing<N, TapRecord>: the comma in a template argument
// list splits CHECK_EQ's macro arguments.
template<uint16_t N> using TapRing = ObserveRing<N, TapRecord>;

TapRecord tap_for(uint32_t seq) {
  TapRecord record{};
  record.t_us = seq * 1000u;
  record.can_id = 0x100u + (seq & 0x7FFu);
  record.dlc = static_cast<uint8_t>(seq % (MAX_FRAME_DATA_LEN + 1));
  record.flags = static_cast<uint8_t>(seq & (TAP_FLAG_EXTENDED | TAP_FLAG_RTR | TAP_FLAG_SHED | TAP_FLAG_TX));
  record.source = static_cast<uint8_t>(seq & 0xFFu);
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    record.data[i] = static_cast<uint8_t>(seq * 31u + i);
  return record;
}

// A record is self-consistent when every field agrees with the one sequence
// number that produced it — this is what catches a torn copy.
bool tap_is_consistent(const TapRecord &record) {
  TapRecord expected = tap_for(record.t_us / 1000u);
  if (record.can_id != expected.can_id || record.dlc != expected.dlc || record.flags != expected.flags ||
      record.source != expected.source)
    return false;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    if (record.data[i] != expected.data[i])
      return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// TapRecord layout
// ---------------------------------------------------------------------------

TEST(tap_record_is_twenty_bytes) {
  CHECK_EQ(sizeof(TapRecord), 20u);
  // A default-constructed record must be all-zero: the ISR fills only the
  // fields it knows and the writer reads the rest.
  TapRecord record{};
  CHECK_EQ(record.t_us, 0u);
  CHECK_EQ(record.can_id, 0u);
  CHECK_EQ(record.dlc, 0);
  CHECK_EQ(record.flags, 0);
  CHECK_EQ(record.source, 0);
  CHECK_EQ(record.reserved, 0);
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    CHECK_EQ(record.data[i], 0);
}

TEST(tap_flags_are_distinct_bits) {
  CHECK_EQ(TAP_FLAG_EXTENDED, 0x01);
  CHECK_EQ(TAP_FLAG_RTR, 0x02);
  CHECK_EQ(TAP_FLAG_SHED, 0x04);
  CHECK_EQ(TAP_FLAG_TX, 0x08);
  // Distinct single bits: a frame can be extended AND rtr AND shed at once, so
  // the encoding must never alias.
  CHECK_EQ(TAP_FLAG_EXTENDED & TAP_FLAG_RTR, 0);
  CHECK_EQ(TAP_FLAG_EXTENDED & TAP_FLAG_SHED, 0);
  CHECK_EQ(TAP_FLAG_RTR & TAP_FLAG_SHED, 0);
  CHECK_EQ(TAP_FLAG_SHED & TAP_FLAG_TX, 0);
}

// ---------------------------------------------------------------------------
// The queue contract, on the tap record type
// ---------------------------------------------------------------------------

TEST(tap_ring_preserves_order) {
  TapRing<8> ring;
  CHECK_EQ(ring.empty(), true);
  CHECK_EQ(TapRing<8>::capacity(), 8);

  for (uint32_t k = 1; k <= 5; k++)
    CHECK_EQ(ring.push(tap_for(k)), true);
  CHECK_EQ(ring.size(), 5);

  for (uint32_t k = 1; k <= 5; k++) {
    TapRecord out{};
    CHECK_EQ(ring.pop(out), true);
    CHECK_EQ(out.t_us, k * 1000u);
    CHECK_EQ(tap_is_consistent(out), true);
  }
  CHECK_EQ(ring.empty(), true);
}

TEST(tap_ring_sheds_newest_when_full) {
  TapRing<4> ring;
  for (uint32_t k = 1; k <= 4; k++)
    CHECK_EQ(ring.push(tap_for(k)), true);

  // Full: the NEWEST record is refused, the queued ones are untouched (B22).
  CHECK_EQ(ring.push(tap_for(99)), false);
  CHECK_EQ(ring.size(), 4);

  for (uint32_t k = 1; k <= 4; k++) {
    TapRecord out{};
    CHECK_EQ(ring.pop(out), true);
    CHECK_EQ(out.t_us, k * 1000u);
  }
  CHECK_EQ(ring.empty(), true);
}

TEST(tap_ring_refills_after_drain) {
  TapRing<4> ring;
  for (uint32_t k = 1; k <= 4; k++)
    ring.push(tap_for(k));

  TapRecord out{};
  CHECK_EQ(ring.pop(out), true);  // frees exactly one slot
  CHECK_EQ(ring.push(tap_for(10)), true);
  CHECK_EQ(ring.push(tap_for(11)), false);  // and only one

  // Order across the wrap: 2,3,4 then the newly admitted 10.
  const uint32_t expected[] = {2, 3, 4, 10};
  for (uint32_t e : expected) {
    CHECK_EQ(ring.pop(out), true);
    CHECK_EQ(out.t_us, e * 1000u);
  }
  CHECK_EQ(ring.empty(), true);
}

// ---------------------------------------------------------------------------
// SPSC under real contention — a thread, not an argument
// ---------------------------------------------------------------------------

TEST(tap_ring_spsc_survives_a_real_thread) {
  // Small ring, many pushes: guarantees the producer laps the consumer and the
  // full/empty boundaries are crossed thousands of times rather than never.
  static const uint32_t PUSH_COUNT = 200000;
  TapRing<8> ring;
  std::atomic<bool> done{false};
  std::atomic<uint32_t> pushed{0};

  std::thread producer([&ring, &done, &pushed]() {
    for (uint32_t k = 1; k <= PUSH_COUNT; k++) {
      if (ring.push(tap_for(k)))
        pushed.fetch_add(1, std::memory_order_relaxed);
      // A refused push is the documented overflow policy, not an error: the
      // producer never blocks and never retries.
    }
    done.store(true, std::memory_order_release);
  });

  uint32_t popped = 0;
  uint32_t torn = 0;
  uint32_t out_of_order = 0;
  uint32_t last_seq = 0;

  auto drain = [&]() {
    TapRecord out{};
    while (ring.pop(out)) {
      popped++;
      if (!tap_is_consistent(out))
        torn++;
      uint32_t seq = out.t_us / 1000u;
      // Sheds create gaps, but never inversions: what survives stays in order.
      if (seq <= last_seq)
        out_of_order++;
      last_seq = seq;
    }
  };

  while (!done.load(std::memory_order_acquire))
    drain();
  producer.join();
  drain();  // final drain after the producer stopped

  CHECK_EQ(torn, 0u);
  CHECK_EQ(out_of_order, 0u);
  // Everything the producer was told it enqueued must come back out — no
  // record invented, none lost between the two indices.
  CHECK_EQ(popped, pushed.load(std::memory_order_relaxed));
  CHECK_EQ(ring.empty(), true);
  // The run has to have actually contended, or it proved nothing.
  CHECK_EQ(popped > 0u, true);
  CHECK_EQ(pushed.load(std::memory_order_relaxed) < PUSH_COUNT, true);
}

TEST(tap_rx_and_tx_producers_keep_records_intact_in_their_own_spsc_rings) {
  // RX and TX are independent ISR producers. They must never share an ObserveRing: push() has a
  // deliberately non-atomic slot-copy plus tail publication. This runs both producers for real,
  // drains round-robin as sd_logger does, and verifies that each surviving record is whole.
  static const uint32_t PUSH_COUNT = 100000;
  TapRing<16> rx;
  TapRing<16> tx;
  std::atomic<bool> rx_done{false};
  std::atomic<bool> tx_done{false};
  std::atomic<uint32_t> accepted{0};
  std::atomic<uint32_t> drained{0};
  std::atomic<uint32_t> record_ring_accepted{0};

  auto producer = [&](TapRing<16> &ring, uint32_t base, std::atomic<bool> &done) {
    for (uint32_t k = 1; k <= PUSH_COUNT; k++) {
      if (ring.push(tap_for(base + k)))
        accepted.fetch_add(1, std::memory_order_relaxed);
    }
    done.store(true, std::memory_order_release);
  };
  std::thread rx_producer(producer, std::ref(rx), 0u, std::ref(rx_done));
  std::thread tx_producer(producer, std::ref(tx), 1000000u, std::ref(tx_done));

  uint32_t torn = 0;
  auto drain_one = [&](TapRing<16> &ring) {
    TapRecord out{};
    if (!ring.pop(out))
      return false;
    drained.fetch_add(1, std::memory_order_relaxed);
    if (!tap_is_consistent(out))
      torn++;
    record_ring_accepted.fetch_add(1, std::memory_order_relaxed);
    return true;
  };
  while (!rx_done.load(std::memory_order_acquire) || !tx_done.load(std::memory_order_acquire)) {
    (void) drain_one(rx);
    (void) drain_one(tx);
  }
  rx_producer.join();
  tx_producer.join();
  while (drain_one(rx) || drain_one(tx)) {
  }

  CHECK_EQ(torn, 0u);
  CHECK_EQ(drained.load(std::memory_order_relaxed), accepted.load(std::memory_order_relaxed));
  CHECK_EQ(record_ring_accepted.load(std::memory_order_relaxed), accepted.load(std::memory_order_relaxed));
  CHECK_EQ(rx.empty(), true);
  CHECK_EQ(tx.empty(), true);
}

// ---------------------------------------------------------------------------
// Consumer-stall tolerance — the arithmetic docs/HANDOVER-m6-headroom.md §11 measured
// ---------------------------------------------------------------------------
//
// §4.3 on the bench: two segments at 88.8 % of 500 kbit/s (~7000 f/s) against a
// consumer that went away for 220-360 ms lost 4765 records from 1024-slot rings
// in stall-shaped bursts. The sizing rule this pins: a refuse-newest ring of
// depth D tolerates a consumer gap G at frame interval T iff D >= G/T plus the
// steady-state occupancy of one drain interval.
//
// The simulation runs on TapRing because that is what compiles on the host, but
// the rule is what sizes BOTH rings in the deployed shape: the tap-drain task
// bounds the tap ring's consumer gap to its own scheduling latency, and hands
// the writer's real outages (the 220-360 ms card stalls) to sd_logger's record
// ring — same 20 B records, same refuse-newest-when-full policy, depth 4096.
// Virtual time, no threads: the numbers are exact and the test cannot flake.

namespace {

struct StallSimResult {
  uint32_t steady_refused = 0;  // refusals while the consumer keeps its cadence — must be 0
  uint32_t gap_refused = 0;     // refusals during one long consumer outage
};

template<uint16_t N> StallSimResult run_stall_sim(uint32_t gap_us) {
  static const uint32_t FRAME_INTERVAL_US = 143;   // ~7000 f/s, the §4.3 arrival rate
  static const uint32_t DRAIN_INTERVAL_US = 5000;  // the tap-drain task's poll cadence
  auto ring = std::make_unique<TapRing<N>>();      // 4096 slots is 80 KB — keep it off the stack
  StallSimResult result;
  uint32_t seq = 1;

  // Phase 1: 100 ms of steady state, full drain every consumer pass.
  uint32_t next_drain = DRAIN_INTERVAL_US;
  for (uint32_t t = 0; t < 100000; t += FRAME_INTERVAL_US) {
    if (t >= next_drain) {
      TapRecord out{};
      while (ring->pop(out)) {
      }
      next_drain += DRAIN_INTERVAL_US;
    }
    if (!ring->push(tap_for(seq++)))
      result.steady_refused++;
  }

  // Phase 2: the consumer stalls for gap_us; frames keep arriving at rate.
  const uint32_t gap_end = 100000 + gap_us;
  for (uint32_t t = 100000; t < gap_end; t += FRAME_INTERVAL_US) {
    if (!ring->push(tap_for(seq++)))
      result.gap_refused++;
  }
  return result;
}

}  // namespace

TEST(tap_ring_4096_survives_a_500ms_consumer_stall_at_full_rate) {
  // 500 ms is the sizing target (§11: measured outages 220-360 ms, so size to
  // ~2x the worst observed, not to the loop instrument's 169 ms).
  StallSimResult r = run_stall_sim<4096>(500000);
  CHECK_EQ(r.steady_refused, 0u);
  CHECK_EQ(r.gap_refused, 0u);
}

TEST(tap_ring_1024_overruns_exactly_as_the_bench_measured) {
  // The control: depth 1024 against the same stall must lose the §4.3 burst.
  // ~3497 arrivals into ~1000 free slots -> ~2500 refusals; the band allows for
  // the steady-state occupancy at stall onset (0..35 records).
  StallSimResult r = run_stall_sim<1024>(500000);
  CHECK_EQ(r.steady_refused, 0u);
  CHECK_EQ(r.gap_refused > 2400u, true);
  CHECK_EQ(r.gap_refused < 2600u, true);
}
