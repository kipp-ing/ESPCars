// Included in separate namespaces for minimal compile-time feature combinations.
// The route-less ISR branch, predicate and tap dispatch are production code;
// receiving from the TWAI driver is doubled with one fixed fictional frame.
using namespace esphome::can_gateway;
struct GatewayRoute {};
struct GatewayPort {
  GatewayRoute *route_out_{nullptr};
  unsigned receives{0};
  unsigned observed{0};
  uint8_t index_{0};
  std::atomic<uint32_t> rx_bits_{0};
  std::atomic<uint32_t> log_tap_rx_accepted_{0};
  std::atomic<uint32_t> log_tap_dropped_{0};
  SnapshotRing<4> *snapshot_ring_{nullptr};
  ObserveRing<4> *observe_ring_{nullptr};
  ObserveRing<4, TapRecord> *log_tap_ring_{nullptr};
  void (*isr_frame_hook_)(void *, uint32_t, uint32_t, bool, bool, uint8_t, const uint8_t *){nullptr};
  void *isr_frame_hook_ctx_{nullptr};
  void observe_frame_(uint32_t, bool, bool, uint8_t, const uint8_t *) { this->observed++; }
  void receive_into_staging_(bool shed = false) {
    this->receives++;
    const uint8_t data[]{0x12, 0x34};
    this->run_rx_taps_(0x123, false, false, 2, data, shed);
  }
#ifdef CAN_GATEWAY_HAS_RX_TAPS
#include "gateway_rx_taps_active_.inc"
#include "gateway_run_rx_taps_.inc"
#else
  void run_rx_taps_(uint32_t, bool, bool, uint8_t, const uint8_t *, bool) {}
#endif
  void handle_rx_isr() {
#include "gateway_route_less.inc"
  }
};

#define RX_TEST(name) TEST(name)
RX_TEST(RX_TEST_NAME) {
  GatewayPort port;
  [[maybe_unused]] SnapshotRing<4> snapshots;
  [[maybe_unused]] ObserveRing<4> observations;
  [[maybe_unused]] ObserveRing<4, TapRecord> logs;
  [[maybe_unused]] unsigned hooks = 0;
#if defined(USE_CAN_GATEWAY_SNAPSHOT)
  port.snapshot_ring_ = &snapshots;
#elif defined(USE_CAN_GATEWAY_OBSERVE)
  port.observe_ring_ = &observations;
#elif defined(USE_CAN_GATEWAY_LOG_TAP)
  port.log_tap_ring_ = &logs;
#elif defined(USE_CAN_GATEWAY_ISR_HOOK)
  port.isr_frame_hook_ctx_ = &hooks;
  port.isr_frame_hook_ = [](void *ctx, uint32_t, uint32_t id, bool, bool, uint8_t dlc, const uint8_t *data) {
    (*static_cast<unsigned *>(ctx))++;
    CHECK_EQ(id, 0x123u);
    CHECK_EQ(dlc, 2u);
    CHECK_EQ(data[0], 0x12u);
  };
#endif
  port.handle_rx_isr();
#ifdef CAN_GATEWAY_HAS_RX_TAPS
  CHECK_EQ(port.receives, 1u);
#else
  CHECK_EQ(port.receives, 0u);
#endif
#ifdef USE_CAN_GATEWAY_STATS
  CHECK_EQ(port.rx_bits_.load(), estimate_frame_bits(false, false, 2));
#endif
#ifdef USE_CAN_GATEWAY_SNAPSHOT
  FrameSnapshot frame;
  bool popped = snapshots.read_latest(frame);
  CHECK(popped);
  if (popped) {
    CHECK_EQ(frame.can_id, 0x123u);
    CHECK_EQ(frame.dlc, 2u);
    CHECK_EQ(frame.data[0], 0x12u);
    CHECK_EQ(frame.data[1], 0x34u);
  }
#endif
#ifdef USE_CAN_GATEWAY_OBSERVE
  CHECK_EQ(port.observed, 1u);
#endif
#ifdef USE_CAN_GATEWAY_LOG_TAP
  TapRecord frame;
  CHECK(logs.pop(frame));
  CHECK_EQ(frame.can_id, 0x123u);
  CHECK_EQ(frame.data[0], 0x12u);
  CHECK_EQ(port.log_tap_rx_accepted_.load(), 1u);
#endif
#ifdef USE_CAN_GATEWAY_ISR_HOOK
  CHECK_EQ(hooks, 1u);
#endif
  // A destination-only port with compiled but unarmed optional consumers must
  // still leave disposal to the driver. Statistics, if compiled, are global.
  GatewayPort unarmed;
  unarmed.handle_rx_isr();
#ifdef USE_CAN_GATEWAY_STATS
  CHECK_EQ(unarmed.receives, 1u);
#else
  CHECK_EQ(unarmed.receives, 0u);
#endif
}
#undef RX_TEST
