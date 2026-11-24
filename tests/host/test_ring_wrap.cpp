// Seed counters near UINT32_MAX without billions of pushes. Include standard
// headers before private access, as in test_glue_linbus_self_test.cpp.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#define private public
#include "gateway_core.h"
#undef private
#include "harness.h"

using namespace esphome::can_gateway;

namespace {
template<typename Record> void check_rollover() {
  ObserveRing<48, Record> ring;
  ring.head_.store(UINT32_MAX - 31);
  ring.tail_.store(UINT32_MAX - 31);
  for (unsigned round = 0; round < 4; round++) {
    for (uint32_t i = 0; i < 48; i++) {
      Record record{};
      record.can_id = round * 48 + i;
      CHECK(ring.push(record));
    }
    CHECK_EQ(ring.size(), 48u);
    CHECK(!ring.push(Record{}));
    for (uint32_t i = 0; i < 48; i++) {
      Record out{};
      CHECK(ring.pop(out));
      CHECK_EQ(out.can_id, round * 48 + i);
    }
    CHECK(ring.empty());
  }
}
}  // namespace

TEST(observe_ring_depth48_uint32_rollover_preserves_unread_slots) { check_rollover<FrameRecord>(); }
TEST(tap_ring_depth48_uint32_rollover_preserves_unread_slots) { check_rollover<TapRecord>(); }

// The physical indices are owned separately by the producer and consumer.
// Stress that ownership plus publication ordering on real threads at rollover.
TEST(observe_ring_depth48_threaded_rollover) {
  ObserveRing<48> ring;
  ring.head_.store(UINT32_MAX - 31);
  ring.tail_.store(UINT32_MAX - 31);
  constexpr uint32_t COUNT = 100000;
  std::thread producer([&]() {
    for (uint32_t i = 0; i < COUNT; i++) {
      FrameRecord record{};
      record.can_id = i;
      record.dlc = 8;
      for (uint8_t j = 0; j < 8; j++)
        record.data[j] = static_cast<uint8_t>(i + j);
      while (!ring.push(record))
        std::this_thread::yield();
    }
  });
  for (uint32_t i = 0; i < COUNT; i++) {
    FrameRecord out{};
    while (!ring.pop(out))
      std::this_thread::yield();
    CHECK_EQ(out.can_id, i);
    for (uint8_t j = 0; j < 8; j++)
      CHECK_EQ(out.data[j], static_cast<uint8_t>(i + j));
  }
  producer.join();
  CHECK(ring.empty());
}
