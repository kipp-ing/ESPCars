#pragma once
// Test-only ESPHome boundary: the production UdsHub and catalog/scheduler code run unchanged.
#include <cmath>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
inline uint32_t uds_test_now = 0;
inline std::vector<std::string> uds_test_logs;
inline uint32_t millis() { return uds_test_now; }
inline void uds_test_log(const char *, const char *fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  uds_test_logs.emplace_back(buf);
}
namespace setup_priority {
inline constexpr float DATA = 600;
}
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return setup_priority::DATA; }
  void mark_failed(const char *) { failed = true; }
  void status_set_warning() {
    if (!warning)
      warning_sets++;
    warning = true;
  }
  void status_clear_warning() {
    if (warning)
      warning_clears++;
    warning = false;
  }
  unsigned warning_sets{0};
  unsigned warning_clears{0};
  bool is_failed() const { return failed; }
  bool failed{false};
  bool warning{false};
};
class PollingComponent : public Component {
 public:
  virtual void update() {}
};
template<typename T> class Parented {
 protected:
  T *parent_{nullptr};
};
template<typename... Ts> class Action {
 public:
  virtual void play(const Ts &...x) = 0;
};
template<typename Signature> class LazyCallbackManager {
 public:
  template<typename F> void add(F &&f) { callbacks.emplace_back(std::forward<F>(f)); }
  bool empty() const { return callbacks.empty(); }
  template<typename... Ts> void call(Ts... x) {
    for (auto &f : callbacks)
      f(x...);
  }
  std::vector<std::function<Signature>> callbacks;
};
struct TestApplication {
  uint32_t get_loop_component_start_time() const { return uds_test_now; }
};
inline TestApplication App;
namespace sensor {
struct Sensor {
  float state{NAN};
  void publish_state(float value) { state = value; }
};
}  // namespace sensor
namespace text_sensor {
struct TextSensor {
  std::string state;
  void publish_state(const char *value) { state = value; }
};
}  // namespace text_sensor
namespace isotp {
enum class IsoTpError { TIMEOUT };
struct MessageView {
  const uint8_t *data;
  uint16_t size;
};
class IsoTpConsumer {
 public:
  virtual void on_message(const MessageView &) = 0;
  virtual void on_error(IsoTpError) = 0;
  virtual void on_sent() {}
};
class IsoTpProtocol {
 public:
  void set_consumer(IsoTpConsumer *c) { consumer = c; }
  uint16_t get_max_message_size() const { return capacity; }
  bool receiving() const { return rx_busy; }
  void complete_send() {
    busy = false;
    if (consumer != nullptr)
      consumer->on_sent();
  }
  bool send(const uint8_t *data, uint16_t size) {
    attempts++;
    if (busy || size > capacity)
      return false;
    sent.emplace_back(data, data + size);
    return true;
  }
  uint16_t capacity{512};
  bool busy{false};
  bool rx_busy{false};
  unsigned attempts{0};
  IsoTpConsumer *consumer{nullptr};
  std::vector<std::vector<uint8_t>> sent;
};
}  // namespace isotp
}  // namespace esphome
#define ESPHOME_LOG_TAG(name, value) static constexpr const char *name = value
#define ESP_LOGE(...) esphome::uds_test_log(__VA_ARGS__)
#define ESP_LOGW(...) esphome::uds_test_log(__VA_ARGS__)
#define ESP_LOGI(...) esphome::uds_test_log(__VA_ARGS__)
#define ESP_LOGD(...) esphome::uds_test_log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) esphome::uds_test_log(__VA_ARGS__)
#define LOG_STR_LITERAL(x) x
#define LOG_STR(x) x
#define LOG_SENSOR(...) ((void) 0)
#define LOG_UPDATE_INTERVAL(...) ((void) 0)
#define TEMPLATABLE_VALUE(type, name) \
  struct { \
    template<typename... Args> type value(Args...) { return {}; } \
  } name##_;
