#pragma once

// Only platform IO is faked. transport_runtime.h contains the production methods.
#include "gateway_core.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#define ESPHOME_LOG_TAG(name, value) static constexpr const char *name = value
#define ESP_LOGW(...) esphome::transport_log(__VA_ARGS__)
#define ESP_LOGV(...) esphome::transport_log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) esphome::transport_log(__VA_ARGS__)
#define LOG_STR_LITERAL(value) value

namespace esphome {
inline std::vector<std::string> transport_logs;
template<typename... Args> void transport_log(const char *, const char *format, const Args &...args) {
  if constexpr (sizeof...(args) == 0) {
    transport_logs.emplace_back(format);
  } else {
    char message[512];
    std::snprintf(message, sizeof(message), format, args...);
    transport_logs.emplace_back(message);
  }
}
inline uint32_t transport_now = 0;
inline uint32_t millis() { return transport_now; }
struct Application {
  uint32_t get_loop_component_start_time() const { return transport_now; }
};
inline Application App;
template<typename T> class Parented {
 public:
  void set_parent(T *parent) { this->parent_ = parent; }

 protected:
  T *parent_{nullptr};
};
template<typename... Ts> class Action {
 public:
  virtual ~Action() = default;
  virtual void play(const Ts &...) = 0;
};
template<typename T, typename... Ts> class TemplatableValue {
 public:
  void set(T value) { this->value_ = value; }
  T value(const Ts &...) const { return this->value_; }

 protected:
  T value_{};
};
#define TEMPLATABLE_VALUE(type, name) \
 protected: \
  TemplatableValue<type, Ts...> name##_; \
\
 public: \
  void set_##name(type value) { this->name##_.set(value); }

namespace setup_priority {
static constexpr float DATA = 600.0f;
}
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return setup_priority::DATA; }
  void status_set_warning() { warning = true; }
  void status_clear_warning() { warning = false; }
  bool warning{false};
};
template<typename Signature> class LazyCallbackManager;
template<typename... Args> class LazyCallbackManager<void(Args...)> {
 public:
  template<typename F> void add(F &&callback) { this->callbacks_.emplace_back(std::forward<F>(callback)); }
  void call(Args... args) {
    for (const auto &callback : this->callbacks_)
      callback(args...);
  }

 protected:
  std::vector<std::function<void(Args...)>> callbacks_;
};
namespace can_gateway {
class CanGatewayFrameConsumer {
 public:
  virtual ~CanGatewayFrameConsumer() = default;
  virtual void on_frame(const FrameView &frame) = 0;
};
class GatewayPort {
 public:
  struct Subscriber {
    uint32_t id;
    CanGatewayFrameConsumer *consumer;
  };
  struct Sent {
    uint32_t id;
    uint8_t data[8];
    uint8_t dlc;
  };
  bool accept{true};
  std::vector<Sent> sent;
  std::vector<Subscriber> subscribers;
  std::vector<uint32_t> frozen_ids;
  void subscribe_consumer(uint32_t id, bool, CanGatewayFrameConsumer *consumer) {
    this->subscribers.push_back({id, consumer});
  }
  void freeze() {
    for (const auto &subscriber : this->subscribers)
      this->frozen_ids.push_back(subscriber.id);
  }
  void receive(const FrameView &view) {
    if (std::find(this->frozen_ids.begin(), this->frozen_ids.end(), view.can_id) == this->frozen_ids.end())
      return;
    for (const auto &subscriber : this->subscribers) {
      if (subscriber.id == view.can_id)
        subscriber.consumer->on_frame(view);
    }
  }
  bool inject(uint32_t id, bool, bool, const uint8_t *data, uint8_t dlc) {
    if (!this->accept)
      return false;
    Sent frame{id, {}, dlc};
    std::copy(data, data + dlc, frame.data);
    this->sent.push_back(frame);
    return true;
  }
};
}  // namespace can_gateway
}  // namespace esphome
