// Execute the production decoder classes and methods with a recording entity boundary.
#include "gateway_core.h"
#include "harness.h"
#include <cmath>
#include <cinttypes>
#include <string>
#include <vector>

namespace gateway_decode_test {
using namespace esphome::can_gateway;
static uint32_t now_ms = 0;
static uint32_t millis() { return now_ms; }

// The real Sensor/TextSensor/BinarySensor have protected filter_list_ under
// their respective USE_*_FILTER defines. Count inputs to publish_state():
// every timeout/window/sample filter depends on this exact stream of inputs.
template<typename T> class RecordingEntity {
 public:
  std::vector<T> samples;
  void add_filter() { this->filter_list_ = this; }
  void clear_filters() { this->filter_list_ = nullptr; }
  void publish_state(T value) { this->samples.push_back(value); }

 protected:
  void *filter_list_{nullptr};
};
namespace sensor {
using Sensor = RecordingEntity<float>;
}
namespace binary_sensor {
using BinarySensor = RecordingEntity<bool>;
}
namespace text_sensor {
using TextSensor = RecordingEntity<std::string>;
}
struct CanGatewayFrameConsumer {
  virtual void on_frame(const FrameView &) = 0;
};
struct CanGatewayEnumEntry {
  uint32_t key;
  const char *value;
};
#include "gateway_CanGatewayDecodeSensor.inc"
#include "gateway_CanGatewayDecodeBinarySensor.inc"
#include "gateway_CanGatewayDecodeTextSensor.inc"
#include "gateway_decode_sensor.inc"
#include "gateway_decode_binary.inc"
#include "gateway_decode_text.inc"

TEST(gateway_decode_rtr_after_data_never_publishes) {
  CanGatewayDecodeSensor numeric;
  numeric.set_signal(0, 1, false, false, false);
  CanGatewayDecodeBinarySensor binary;
  CanGatewayDecodeTextSensor text;
  uint8_t payload[]{1};
  FrameView view{0x100, payload, 1, false, false};
  numeric.on_frame(view);
  binary.on_frame(view);
  text.on_frame(view);
  // Driver buffers retain bytes across remote requests. Use different stale
  // bytes too, so raw dedup cannot accidentally hide an RTR publication.
  view.rtr = true;
  payload[0] = 2;
  numeric.on_frame(view);
  binary.on_frame(view);
  text.on_frame(view);
  CHECK_EQ(numeric.samples.size(), 1u);
  CHECK_EQ(binary.samples.size(), 1u);
  CHECK_EQ(text.samples.size(), 1u);
  view.rtr = false;
  numeric.on_frame(view);
  binary.on_frame(view);
  text.on_frame(view);
  CHECK_EQ(numeric.samples.size(), 2u);
  CHECK_EQ(binary.samples.size(), 2u);
  CHECK_EQ(text.samples.size(), 2u);
}

TEST(gateway_decode_filters_receive_steady_samples) {
  CanGatewayDecodeSensor numeric;
  numeric.set_signal(0, 1, false, false, false);
  CanGatewayDecodeBinarySensor binary;
  CanGatewayDecodeTextSensor text;
  numeric.add_filter();
  binary.add_filter();
  text.add_filter();
  uint8_t payload[]{1};
  FrameView view{0x100, payload, 1, false, false};
  for (unsigned i = 0; i < 12; i++) {
    now_ms = i * 100;
    numeric.on_frame(view);
    binary.on_frame(view);
    text.on_frame(view);
  }
  CHECK_EQ(numeric.samples.size(), 12u);
  CHECK_EQ(binary.samples.size(), 12u);
  CHECK_EQ(text.samples.size(), 12u);
}

TEST(gateway_decode_no_filters_remains_change_only) {
  CanGatewayDecodeSensor numeric;
  numeric.set_signal(0, 1, false, false, false);
  CanGatewayDecodeBinarySensor binary;
  CanGatewayDecodeTextSensor text;
  uint8_t payload[]{1};
  FrameView view{0x100, payload, 1, false, false};
  for (unsigned i = 0; i < 12; i++) {
    numeric.on_frame(view);
    binary.on_frame(view);
    text.on_frame(view);
  }
  CHECK_EQ(numeric.samples.size(), 1u);
  CHECK_EQ(binary.samples.size(), 1u);
  CHECK_EQ(text.samples.size(), 1u);
}

TEST(gateway_decode_explicit_throttle_and_sna_preserved) {
  CanGatewayDecodeSensor numeric;
  numeric.set_signal(0, 1, false, false, false);
  numeric.set_sna(SnaMode::ALL_ONES, 0);
  numeric.set_throttle(100);
  numeric.add_filter();
  CanGatewayDecodeTextSensor text;
  text.set_throttle(100);
  text.add_filter();
  uint8_t payload[]{0xFF};
  FrameView view{0x100, payload, 1, false, false};
  for (uint32_t time : {0u, 50u, 100u}) {
    now_ms = time;
    numeric.on_frame(view);
    text.on_frame(view);
  }
  CHECK_EQ(numeric.samples.size(), 2u);
  CHECK_EQ(text.samples.size(), 2u);
  CHECK(std::isnan(numeric.samples.back()));
}
}  // namespace gateway_decode_test
