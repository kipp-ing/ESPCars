#pragma once
#include <functional>
#include <optional>
namespace esphome {
using std::optional;
template<typename... Ts> class Trigger {
 public:
  std::function<void(Ts...)> callback;
  void trigger(Ts... args) {
    if (callback)
      callback(args...);
  }
};
template<typename... Ts> class Action {
 public:
  virtual void play(const Ts &...) = 0;
};
template<typename T> struct TemplateValue {
  template<typename... Ts> T value(Ts...) { return T{}; }
};
}  // namespace esphome
#define TEMPLATABLE_VALUE(type, name) esphome::TemplateValue<type> name##_;
