#pragma once
#include <string>
#include "gpio.h"
namespace esphome {
namespace setup_priority {
constexpr float BUS = 100;
}
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }
  void mark_failed(const char *reason = "") {
    failed = true;
    failure_reason = reason;
  }
  bool is_failed() const { return failed; }
  void status_set_warning(const char * = "") { warning = true; }
  void status_clear_warning() { warning = false; }
  bool failed = false, warning = false;
  std::string failure_reason;
};
}  // namespace esphome
