// Production setup/control methods against a small TWAI/FreeRTOS boundary.
#include "gateway_core.h"
#include "harness.h"
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#define USE_BINARY_SENSOR
#define USE_CAN_GATEWAY_ISR_HOOK
#define CAN_GATEWAY_INJECT_SLOTS 4
#define ESP_LOGE(...) ((void) 0)
#define ESP_LOGI(...) ((void) 0)
#define ESP_LOGW(...) warn(__VA_ARGS__)
#define LOG_STR(reason) reason
#define portENTER_CRITICAL(mux) ((void) (mux))
#define portEXIT_CRITICAL(mux) ((void) (mux))

namespace gateway_port_test {
using namespace esphome::can_gateway;
static unsigned warnings = 0;
static constexpr const char *TAG = "gateway-test";
template<typename... Ts> void warn(const char *, const char *, Ts...) { ++warnings; }
static uint32_t millis() { return 2000; }
using TaskHandle_t = std::thread::id;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return std::this_thread::get_id(); }
static constexpr int ESP_OK = 0;
static constexpr int TWAI_ERROR_ACTIVE = 0;
static constexpr int TWAI_ERROR_BUS_OFF = 1;
static constexpr int GPIO_NUM_NC = -1;
static constexpr int GPIO_MODE_INPUT_OUTPUT_OD = 1;
using gpio_num_t = int;
static void gpio_set_direction(int, int) {}
struct Node {};
using twai_node_handle_t = Node *;
struct twai_frame_t {
  struct {
    uint32_t id = 0;
    bool ide = false, rtr = false;
    uint8_t dlc = 0;
  } header;
  uint8_t *buffer = nullptr;
  uint8_t buffer_len = 0;
};
struct twai_onchip_node_config_t {
  struct {
    int tx, rx, quanta_clk_out, bus_off_indicator;
  } io_cfg;
  struct {
    uint32_t bitrate;
  } bit_timing;
  struct {
    bool enable_listen_only, enable_self_test;
  } flags;
  int fail_retry_cnt, tx_queue_depth, intr_priority;
};
struct twai_event_callbacks_t {
  int on_rx_done, on_tx_done, on_state_change, on_error;
};
static constexpr int on_rx_done_cb = 1, on_tx_done_cb = 2, on_state_change_cb = 3, on_error_cb = 4;
struct twai_mask_filter_config_t {
  uint32_t id = 0, mask = 0;
  bool is_ext = false;
};
static Node node;
static int fail_step = 0;
static unsigned submissions = 0, disabled = 0, filter_calls = 0;
static twai_mask_filter_config_t installed_filter;
static int twai_new_node_onchip(twai_onchip_node_config_t *, twai_node_handle_t *out) {
  if (fail_step == 1)
    return -1;
  *out = &node;
  return ESP_OK;
}
static int twai_node_register_event_callbacks(Node *, twai_event_callbacks_t *, void *) {
  return fail_step == 2 ? -1 : ESP_OK;
}
static int twai_node_enable(Node *) { return fail_step == 3 ? -1 : ESP_OK; }
static int twai_node_disable(Node *) {
  disabled++;
  return ESP_OK;
}
static int twai_node_recover(Node *) { return ESP_OK; }
static int twai_node_transmit(Node *, twai_frame_t *, int) {
  submissions++;
  return ESP_OK;
}
static int twai_node_config_mask_filter(Node *, int, twai_mask_filter_config_t *filter) {
  filter_calls++;
  installed_filter = *filter;
  return ESP_OK;
}
namespace binary_sensor {
struct BinarySensor {
  std::vector<bool> samples;
  void publish_state(bool value) { this->samples.push_back(value); }
};
}  // namespace binary_sensor
struct Callback {
  void call() {}
};
struct GatewayPort;
struct GatewayRoute {
  GatewayPort *from_ = nullptr;
  RouteTable table_{};
  std::vector<RuleEntry> rules_;
  bool observed_ = false;
};
struct TxSlot {
  twai_frame_t frame{};
  uint8_t payload[8]{};
  GatewayPort *port = nullptr;
  uint8_t index = 0;
};
using IsrFrameHook = void (*)(void *, uint32_t, uint32_t, bool, bool, uint8_t, const uint8_t *);
struct GatewayPort {
  bool start_(GatewayRoute *, const std::atomic<bool> *, uint8_t);
  void try_hw_filter_offload_(const GatewayRoute *);
  bool inject(uint32_t, bool, bool, const uint8_t *, uint8_t);
  void loop_(uint32_t);
  bool is_bus_off() const { return this->state_.load() == TWAI_ERROR_BUS_OFF; }
  bool read_error_counters(uint16_t &, uint16_t &) const { return false; }
  void reclaim_orphan_(TxSlot *slot) { this->inject_pool_.release(slot->index); }
  uint8_t index_ = 0, tx_queue_depth_ = 8;
  int8_t rx_pin_ = 1, tx_pin_ = 2;
  uint32_t bit_rate_ = 500000;
  bool listen_only_ = false, self_test_ = false, open_drain_tx_ = false, hw_filter_engaged_ = false;
  Node *node_ = nullptr;
  TaskHandle_t loop_task_{};
  const std::atomic<bool> *gateway_enabled_ = nullptr;
  std::atomic<uint8_t> state_{TWAI_ERROR_ACTIVE};
  uint8_t last_loop_state_ = TWAI_ERROR_ACTIVE;
  RecoveryBackoff backoff_;
  Callback bus_off_callback_, recovered_callback_;
  binary_sensor::BinarySensor *bus_off_sensor_ = nullptr;
  bool bus_off_head_reclaimed_ = true;
  GatewayRoute *route_out_ = nullptr;
  SlotPool<4> inject_pool_;
  TxSlot inject_slots_[4];
  OutstandingTracker<TxSlot *, 16> outstanding_;
  int mux_ = 0;
  PortCounters counters_;
  std::atomic<uint32_t> last_inject_warn_ms_{0};
  uint32_t last_err_poll_ms_ = 0;
  static constexpr uint32_t ERR_POLL_INTERVAL_MS = 500;
  bool err_counters_seeded_ = false;
  uint16_t last_tec_ = 0, last_rec_ = 0;
  IsrFrameHook isr_frame_hook_ = nullptr;
  void *isr_frame_hook_ctx_ = nullptr;
};
struct CanGateway {
  void setup();
  void mark_failed(const char *reason = nullptr) {
    this->failed = true;
    this->failure_reason = reason;
  }
  std::vector<GatewayPort *> ports_;
  std::vector<GatewayRoute *> routes_;
  std::atomic<bool> enabled_{true};
  uint8_t interrupt_priority_ = 2;
  bool failed = false;
  const char *failure_reason = nullptr;
};
#include "gateway_port_start.inc"
#include "gateway_offload.inc"
#include "gateway_inject.inc"
#include "gateway_port_loop.inc"
#include "gateway_setup.inc"

TEST(gateway_inject_rejects_other_task_before_touching_tracker) {
  fail_step = 0;
  submissions = 0;
  warnings = 0;
  GatewayPort port;
  CHECK(port.start_(nullptr, nullptr, 2));
  // Real thread stands in for a task on the other core. Even with spare
  // slots/queue capacity it must fail before driver/tracker mutation.
  bool accepted = true;
  std::thread other([&]() { accepted = port.inject(0x100, false, false, nullptr, 0); });
  other.join();
  CHECK(!accepted);
  CHECK_EQ(submissions, 0u);
  CHECK_EQ(port.outstanding_.size(), 0u);
  for (uint8_t i = 0; i < 4; i++)
    CHECK(!port.inject_pool_.is_used(i));
  CHECK_EQ(warnings, 1u);
  CHECK(port.inject(0x100, false, false, nullptr, 0));
  CHECK_EQ(submissions, 1u);
  CHECK_EQ(port.outstanding_.size(), 1u);
}

TEST(gateway_hook_disables_offload_for_route_rejected_id) {
  GatewayPort port;
  GatewayRoute route;
  route.table_.default_drop = true;
  route.rules_.push_back({0x100, MAX_STANDARD_ID, 0});
  filter_calls = 0;
  port.try_hw_filter_offload_(&route);
  CHECK_EQ(filter_calls, 1u);  // no hook: existing offload still works
  port.hw_filter_engaged_ = false;
  unsigned received = 0;
  port.isr_frame_hook_ = [](void *ctx, uint32_t, uint32_t id, bool, bool, uint8_t, const uint8_t *) {
    if (id == 0x200)
      ++*static_cast<unsigned *>(ctx);
  };
  port.isr_frame_hook_ctx_ = &received;
  filter_calls = 0;
  port.try_hw_filter_offload_(&route);
  CHECK_EQ(filter_calls, 0u);
  // Model the hardware admission in front of the existing tested RX hook:
  // an installed filter would prevent the callback from ever seeing 0x200.
  if (!port.hw_filter_engaged_ || (0x200 & installed_filter.mask) == installed_filter.id)
    port.isr_frame_hook_(port.isr_frame_hook_ctx_, 0, 0x200, false, false, 0, nullptr);
  CHECK_EQ(received, 1u);
}

TEST(gateway_bus_off_initial_state_and_recovery_are_published) {
  fail_step = 0;
  GatewayPort port;
  binary_sensor::BinarySensor sensor;
  port.bus_off_sensor_ = &sensor;
  CHECK(port.start_(nullptr, nullptr, 2));
  CHECK_EQ(sensor.samples.size(), 1u);
  if (!sensor.samples.empty())
    CHECK_EQ(sensor.samples.front(), false);
  port.state_.store(TWAI_ERROR_BUS_OFF);
  port.loop_(1);
  port.state_.store(TWAI_ERROR_ACTIVE);
  port.loop_(2);
  CHECK_EQ(sensor.samples.size(), 3u);
  if (sensor.samples.size() == 3) {
    CHECK_EQ(sensor.samples[1], true);
    CHECK_EQ(sensor.samples[2], false);
  }
}

TEST(gateway_start_failure_persists_reason_and_never_wires_routes) {
  for (int step : {1, 2, 3}) {
    fail_step = step;
    GatewayPort port;
    binary_sensor::BinarySensor sensor;
    port.bus_off_sensor_ = &sensor;
    CanGateway gateway;
    gateway.ports_.push_back(&port);
    gateway.setup();
    CHECK(gateway.failed);
    CHECK(gateway.failure_reason != nullptr);
    CHECK(port.route_out_ == nullptr);
    CHECK(sensor.samples.empty());
  }
  fail_step = 0;
}
}  // namespace gateway_port_test
