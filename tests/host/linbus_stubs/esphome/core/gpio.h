#pragma once
namespace esphome {
class GPIOPin {
 public:
  virtual ~GPIOPin() = default;
  virtual void setup() {}
  virtual bool is_internal() { return false; }
  virtual void digital_write(bool value) { physical = value != inverted; }
  bool inverted = false;
  bool physical = false;
};
class InternalGPIOPin : public GPIOPin {
 public:
  bool is_internal() override { return true; }
  int get_pin() { return 4; }
};
}  // namespace esphome
#define LOG_PIN(...) ((void) 0)
