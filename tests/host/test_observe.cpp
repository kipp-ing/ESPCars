// Observation plane: ObserveRing (SPSC FIFO, sheds the newest on overflow),
// SubscribedSet (the ISR membership test), dispatch_record and select_rx_mode.
//
// Contract under test (gateway_core.h):
//   - ObserveRing preserves arrival order and, unlike SnapshotRing, sheds the
//     NEWEST record on overflow (B22) — a stalled loop must never build an
//     unbounded backlog of stale frames delivered late
//   - a rejected push mutates nothing: the queued records stay intact
//   - SubscribedSet stays sorted and de-duplicated, and keys 11-bit and 29-bit
//     IDs apart
//   - dispatch_record delivers in registration order (B24)

#include "gateway_core.h"
#include "harness.h"

#include <cstdint>
#include <vector>

using namespace esphome::can_gateway;

namespace {

FrameRecord record_for(uint32_t can_id, uint8_t dlc = 8) {
  FrameRecord record{};
  record.can_id = can_id;
  record.dlc = dlc;
  record.extended = (can_id & 1u) != 0;
  record.rtr = (can_id & 2u) != 0;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    record.data[i] = static_cast<uint8_t>(can_id * 31u + i);
  return record;
}

bool record_matches(const FrameRecord &record, uint32_t can_id) {
  FrameRecord expected = record_for(can_id, record.dlc);
  if (record.can_id != can_id || record.extended != expected.extended || record.rtr != expected.rtr)
    return false;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    if (record.data[i] != expected.data[i])
      return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// ObserveRing
// ---------------------------------------------------------------------------

TEST(observe_ring_starts_empty) {
  ObserveRing<4> ring;
  CHECK_EQ(ring.empty(), true);
  CHECK_EQ(ring.size(), 0);
  CHECK_EQ(ObserveRing<4>::capacity(), 4);

  FrameRecord out{};
  CHECK_EQ(ring.pop(out), false);
}

TEST(observe_ring_preserves_arrival_order) {
  ObserveRing<8> ring;
  for (uint32_t id = 1; id <= 5; id++)
    CHECK_EQ(ring.push(record_for(id)), true);
  CHECK_EQ(ring.size(), 5);
  CHECK_EQ(ring.empty(), false);

  for (uint32_t id = 1; id <= 5; id++) {
    FrameRecord out{};
    CHECK_EQ(ring.pop(out), true);
    CHECK_MSG(record_matches(out, id), "record came out of order or corrupted");
  }
  CHECK_EQ(ring.empty(), true);
  FrameRecord out{};
  CHECK_EQ(ring.pop(out), false);
}

TEST(observe_ring_round_trips_every_field) {
  ObserveRing<2> ring;
  FrameRecord in{};
  in.can_id = 0x1FFFFFFF;
  in.dlc = 3;
  in.extended = true;
  in.rtr = true;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    in.data[i] = static_cast<uint8_t>(0xA0 + i);

  CHECK_EQ(ring.push(in), true);
  FrameRecord out{};
  CHECK_EQ(ring.pop(out), true);
  CHECK_EQ(out.can_id, 0x1FFFFFFFu);
  CHECK_EQ(out.dlc, 3);
  CHECK_EQ(out.extended, true);
  CHECK_EQ(out.rtr, true);
  CHECK_BYTES(out.data, in.data, MAX_FRAME_DATA_LEN);
}

TEST(observe_ring_overflow_sheds_the_newest_and_keeps_content) {
  // B22: the ring is full, so the NEW frame is dropped. Every record already
  // queued must survive untouched — the opposite of SnapshotRing's keep-newest.
  ObserveRing<4> ring;
  for (uint32_t id = 1; id <= 4; id++)
    CHECK_EQ(ring.push(record_for(id)), true);
  CHECK_EQ(ring.size(), 4);

  for (uint32_t id = 100; id < 110; id++)
    CHECK_EQ_MSG(ring.push(record_for(id)), false, "a full ring must reject the newest frame");
  CHECK_EQ(ring.size(), 4);

  for (uint32_t id = 1; id <= 4; id++) {
    FrameRecord out{};
    CHECK_EQ(ring.pop(out), true);
    CHECK_MSG(record_matches(out, id), "a rejected push corrupted the queued records");
  }
  CHECK_EQ(ring.empty(), true);
}

TEST(observe_ring_recovers_after_the_consumer_drains) {
  ObserveRing<4> ring;
  for (uint32_t id = 1; id <= 4; id++)
    ring.push(record_for(id));
  CHECK_EQ(ring.push(record_for(5)), false);

  FrameRecord out{};
  CHECK_EQ(ring.pop(out), true);  // one slot freed
  CHECK_MSG(record_matches(out, 1), "wrong record popped");
  CHECK_EQ(ring.push(record_for(5)), true);
  CHECK_EQ(ring.size(), 4);

  for (uint32_t id = 2; id <= 5; id++) {
    CHECK_EQ(ring.pop(out), true);
    CHECK_MSG(record_matches(out, id), "order broken after refill");
  }
}

TEST(observe_ring_interleaved_push_pop_wraps_correctly) {
  // head_/tail_ are free-running counters indexed modulo N; run many times N so
  // an index bug shows up as a lost or duplicated record.
  ObserveRing<4> ring;
  uint32_t next_push = 0;
  uint32_t next_pop = 0;
  for (int step = 0; step < 500; step++) {
    if (ring.push(record_for(next_push)))
      next_push++;
    if ((step % 3) == 0) {
      FrameRecord out{};
      if (ring.pop(out)) {
        CHECK_MSG(record_matches(out, next_pop), "wrapped ring returned the wrong record");
        next_pop++;
      }
    }
  }
  CHECK_EQ(next_push - next_pop, ring.size());
  // Drain and verify the tail of the sequence.
  FrameRecord out{};
  while (ring.pop(out)) {
    CHECK_MSG(record_matches(out, next_pop), "wrapped ring returned the wrong record on drain");
    next_pop++;
  }
  CHECK_EQ(next_push, next_pop);
}

TEST(observe_ring_size_tracks_occupancy) {
  ObserveRing<4> ring;
  CHECK_EQ(ring.size(), 0);
  ring.push(record_for(1));
  CHECK_EQ(ring.size(), 1);
  ring.push(record_for(2));
  CHECK_EQ(ring.size(), 2);
  FrameRecord out{};
  ring.pop(out);
  CHECK_EQ(ring.size(), 1);
  ring.pop(out);
  CHECK_EQ(ring.size(), 0);
  CHECK_EQ(ring.empty(), true);
}

TEST(observe_ring_minimum_size_two) {
  ObserveRing<2> ring;
  CHECK_EQ(ring.push(record_for(1)), true);
  CHECK_EQ(ring.push(record_for(2)), true);
  CHECK_EQ(ring.push(record_for(3)), false);
  FrameRecord out{};
  CHECK_EQ(ring.pop(out), true);
  CHECK_MSG(record_matches(out, 1), "wrong record");
  CHECK_EQ(ring.pop(out), true);
  CHECK_MSG(record_matches(out, 2), "wrong record");
  CHECK_EQ(ring.pop(out), false);
}

TEST(observe_record_is_a_compact_sixteen_bytes) {
  // The header static_asserts this; assert it at runtime too so the reason is
  // visible in the test log rather than only as a compile error.
  CHECK_EQ(sizeof(FrameRecord), 16u);
}

// ---------------------------------------------------------------------------
// SubscribedSet — the per-frame membership test the RX ISR runs
// ---------------------------------------------------------------------------

TEST(subscribed_set_keys_separate_standard_and_extended) {
  SubscribedSet<8> set;
  CHECK_EQ(set.insert(0x123, /*extended=*/false), true);
  CHECK_EQ(set.size(), 1);
  CHECK_EQ(set.contains(0x123, false), true);
  CHECK_EQ(set.contains(0x123, true), false);  // same number, other frame type

  CHECK_EQ(set.insert(0x123, true), true);
  CHECK_EQ(set.size(), 2);
  CHECK_EQ(set.contains(0x123, true), true);
  CHECK_EQ(set.contains(0x123, false), true);
}

TEST(subscribed_set_deduplicates_and_stays_searchable) {
  SubscribedSet<8> set;
  const uint32_t ids[] = {0x700, 0x100, 0x400, 0x000, 0x7FF, 0x100, 0x400};
  for (uint32_t id : ids)
    CHECK_EQ(set.insert(id, false), true);
  CHECK_EQ(set.size(), 5);  // two duplicates collapsed

  const uint32_t distinct[] = {0x000, 0x100, 0x400, 0x700, 0x7FF};
  for (uint32_t id : distinct)
    CHECK_EQ_MSG(set.contains(id, false), true, "inserted id not found");
  for (uint32_t id : {0x001u, 0x0FFu, 0x101u, 0x7FEu, 0x123u})
    CHECK_EQ_MSG(set.contains(id, false), false, "found an id that was never inserted");
}

TEST(subscribed_set_reports_full_only_for_new_keys) {
  SubscribedSet<3> set;
  CHECK_EQ(set.insert(1, false), true);
  CHECK_EQ(set.insert(2, false), true);
  CHECK_EQ(set.insert(3, false), true);
  CHECK_EQ(set.size(), 3);
  CHECK_EQ(set.insert(4, false), false);  // full
  CHECK_EQ(set.insert(2, false), true);   // already present, not an error
  CHECK_EQ(set.size(), 3);
  CHECK_EQ(set.contains(4, false), false);
  CHECK_EQ(set.contains(2, false), true);
}

TEST(subscribed_set_handles_the_extreme_ids) {
  SubscribedSet<4> set;
  CHECK_EQ(set.insert(0, false), true);
  CHECK_EQ(set.insert(MAX_STANDARD_ID, false), true);
  CHECK_EQ(set.insert(MAX_EXTENDED_ID, true), true);
  CHECK_EQ(set.contains(0, false), true);
  CHECK_EQ(set.contains(0, true), false);
  CHECK_EQ(set.contains(MAX_STANDARD_ID, false), true);
  CHECK_EQ(set.contains(MAX_EXTENDED_ID, true), true);
  CHECK_EQ(set.contains(MAX_EXTENDED_ID, false), false);
}

TEST(subscribed_set_empty_contains_nothing) {
  SubscribedSet<4> set;
  CHECK_EQ(set.size(), 0);
  CHECK_EQ(set.contains(0, false), false);
  CHECK_EQ(set.contains(0x123, true), false);
}

// ---------------------------------------------------------------------------
// dispatch_record / select_rx_mode
// ---------------------------------------------------------------------------

namespace {

struct DispatchLog {
  std::vector<int> order;
  std::vector<uint32_t> ids;
  std::vector<uint8_t> first_bytes;
};

struct Sub {
  DispatchLog *log;
  int tag;
};

void trampoline(void *ctx, const FrameView &view) {
  Sub *sub = static_cast<Sub *>(ctx);
  sub->log->order.push_back(sub->tag);
  sub->log->ids.push_back(view.can_id);
  sub->log->first_bytes.push_back(view.dlc > 0 ? view.data[0] : 0);
}

}  // namespace

TEST(dispatch_delivers_in_registration_order_to_matching_subscribers) {
  DispatchLog log;
  Sub ctx0{&log, 0};
  Sub ctx1{&log, 1};
  Sub ctx2{&log, 2};
  Sub ctx3{&log, 3};

  FrameSubscriber subs[4] = {};
  subs[0] = FrameSubscriber{0x100, false, false, trampoline, &ctx0};
  subs[1] = FrameSubscriber{0x200, false, false, trampoline, &ctx1};  // no match
  subs[2] = FrameSubscriber{0, false, true, trampoline, &ctx2};       // subscribe_all
  subs[3] = FrameSubscriber{0x100, true, false, trampoline, &ctx3};   // extended only

  FrameRecord record = record_for(0x100, 8);
  record.extended = false;
  dispatch_record(subs, 4, record);

  CHECK_EQ(log.order.size(), 2u);
  CHECK_EQ(log.order[0], 0);
  CHECK_EQ(log.order[1], 2);
  CHECK_EQ(log.ids[0], 0x100u);
  CHECK_EQ(log.first_bytes[0], record.data[0]);
}

TEST(dispatch_to_no_subscribers_is_a_no_op) {
  FrameRecord record = record_for(0x100);
  dispatch_record(nullptr, 0, record);  // must not dereference
}

TEST(subscriber_matches_respects_frame_type) {
  FrameSubscriber sub{0x123, false, false, nullptr, nullptr};
  CHECK_EQ(sub.matches(0x123, false), true);
  CHECK_EQ(sub.matches(0x123, true), false);
  CHECK_EQ(sub.matches(0x124, false), false);

  FrameSubscriber all{0x123, false, true, nullptr, nullptr};
  CHECK_EQ(all.matches(0x999, true), true);
  CHECK_EQ(all.matches(0x000, false), true);
}

TEST(select_rx_mode_prefers_forwarding) {
  CHECK_EQ(select_rx_mode(/*has_route=*/true, /*observes=*/false), RxReceiveMode::FORWARD_SLOT);
  CHECK_EQ(select_rx_mode(true, true), RxReceiveMode::FORWARD_SLOT);
  CHECK_EQ(select_rx_mode(false, true), RxReceiveMode::STAGING);
  CHECK_EQ(select_rx_mode(false, false), RxReceiveMode::DISCARD);
}
