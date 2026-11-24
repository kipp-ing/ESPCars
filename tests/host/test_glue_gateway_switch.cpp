// Test our gateway switch with a minimal Switch fake, not installed core code.
#include "harness.h"
#include <optional>
#include <cstdint>

namespace gateway_switch_test {
template<typename T> using optional = std::optional<T>;
namespace switch_ {
// Mirrors ESPHome 2026.11.0 esphome/components/switch/switch.h and switch.cpp:
// restore -> get_initial_state_with_restore_mode -> control -> turn_on/off.
// Only restore-mode selection, inversion and logical publication/persistence
// are modeled. Storage backends, callbacks and publish deduplication are omitted.
constexpr uint8_t RESTORE_MODE_ON_MASK = 0x01;
constexpr uint8_t RESTORE_MODE_PERSISTENT_MASK = 0x02;
constexpr uint8_t RESTORE_MODE_INVERTED_MASK = 0x04;
constexpr uint8_t RESTORE_MODE_DISABLED_MASK = 0x08;
enum SwitchRestoreMode : uint8_t {
  SWITCH_ALWAYS_OFF = 0,
  SWITCH_ALWAYS_ON = RESTORE_MODE_ON_MASK,
  SWITCH_RESTORE_DEFAULT_OFF = RESTORE_MODE_PERSISTENT_MASK,
  SWITCH_RESTORE_DEFAULT_ON = RESTORE_MODE_PERSISTENT_MASK | RESTORE_MODE_ON_MASK,
  SWITCH_RESTORE_INVERTED_DEFAULT_OFF = RESTORE_MODE_PERSISTENT_MASK | RESTORE_MODE_INVERTED_MASK,
  SWITCH_RESTORE_INVERTED_DEFAULT_ON = RESTORE_MODE_PERSISTENT_MASK | RESTORE_MODE_INVERTED_MASK | RESTORE_MODE_ON_MASK,
  SWITCH_RESTORE_DISABLED = RESTORE_MODE_DISABLED_MASK,
};
class Switch {
 public:
  optional<bool> *saved{nullptr};
  bool state{false};
  bool has_state{false};
  void set_inverted(bool value) { this->inverted_ = value; }
  void set_restore_mode(SwitchRestoreMode value) { this->restore_mode_ = value; }
  optional<bool> get_initial_state_with_restore_mode() {
    if (this->restore_mode_ & RESTORE_MODE_DISABLED_MASK)
      return {};
    bool initial = this->restore_mode_ & RESTORE_MODE_ON_MASK;
    if ((this->restore_mode_ & RESTORE_MODE_PERSISTENT_MASK) && this->saved != nullptr && this->saved->has_value())
      initial = this->restore_mode_ & RESTORE_MODE_INVERTED_MASK ? !this->saved->value() : this->saved->value();
    return initial;
  }
  void restore() {
    if (auto initial = this->get_initial_state_with_restore_mode())
      this->control(*initial);
  }
  void control(bool logical) {
    if (logical)
      this->turn_on();
    else
      this->turn_off();
  }
  void turn_on() { this->write_state(!this->inverted_); }
  void turn_off() { this->write_state(this->inverted_); }
  void publish_state(bool physical) {
    this->state = physical != this->inverted_;
    this->has_state = true;
    if ((this->restore_mode_ & RESTORE_MODE_PERSISTENT_MASK) && this->saved != nullptr)
      *this->saved = this->state;
  }

 protected:
  virtual void write_state(bool state) = 0;
  bool inverted_{false};
  SwitchRestoreMode restore_mode_{SWITCH_RESTORE_DEFAULT_OFF};
};
}  // namespace switch_

namespace setup_priority {
constexpr float HARDWARE = 800.0f;
}
struct Component {
  virtual void setup() {}
  virtual float get_setup_priority() const { return 0; }
};
template<typename T> struct Parented {
  T *parent_{nullptr};
};
struct CanGateway {
  bool enabled{true};
  unsigned writes{0};
  void set_enabled(bool value) {
    this->enabled = value;
    this->writes++;
  }
};
#include "gateway_switch.inc"

TEST(gateway_switch_restore_preserves_physical_forwarding) {
  for (bool inverted : {false, true}) {
    for (bool logical : {false, true}) {
      optional<bool> saved;
      CanGateway before;
      CanGatewaySwitch first;
      first.saved = &saved;
      first.parent_ = &before;
      first.set_inverted(inverted);
      first.set_restore_mode(switch_::SWITCH_RESTORE_DEFAULT_ON);
      first.setup();
      first.control(logical);
      CHECK_EQ(saved.value(), logical);
      CHECK_EQ(before.enabled, logical != inverted);

      CanGateway after;
      CanGatewaySwitch reboot;
      reboot.saved = &saved;
      reboot.parent_ = &after;
      reboot.set_inverted(inverted);
      reboot.set_restore_mode(switch_::SWITCH_RESTORE_DEFAULT_ON);
      reboot.setup();
      CHECK_EQ(after.enabled, before.enabled);
      CHECK_EQ(reboot.state, logical);
      CHECK_EQ(saved.value(), logical);
      CHECK(reboot.get_setup_priority() > setup_priority::HARDWARE);
    }
  }
}

TEST(gateway_switch_restore_modes_follow_stock_semantics) {
  using namespace switch_;
  for (auto mode : {SWITCH_ALWAYS_OFF, SWITCH_ALWAYS_ON, SWITCH_RESTORE_DEFAULT_OFF, SWITCH_RESTORE_DEFAULT_ON,
                    SWITCH_RESTORE_INVERTED_DEFAULT_OFF, SWITCH_RESTORE_INVERTED_DEFAULT_ON, SWITCH_RESTORE_DISABLED}) {
    for (bool inverted : {false, true}) {
      for (optional<bool> initial : {optional<bool>{}, optional<bool>{false}, optional<bool>{true}}) {
        optional<bool> saved = initial;
        CanGateway parent;
        CanGatewaySwitch entity;
        entity.saved = &saved;
        entity.parent_ = &parent;
        entity.set_inverted(inverted);
        entity.set_restore_mode(mode);
        auto logical = entity.get_initial_state_with_restore_mode();
        optional<bool> expected = logical.has_value() ? optional<bool>{*logical != inverted} : optional<bool>{};
        entity.setup();
        CHECK_EQ(parent.writes, expected.has_value() ? 1u : 0u);
        CHECK_EQ(parent.enabled, expected.value_or(true));
        CHECK_EQ(entity.has_state, expected.has_value());
        if (expected)
          CHECK_EQ(entity.state, *expected != inverted);
        else
          CHECK_EQ(saved, initial);
      }
    }
  }
}
}  // namespace gateway_switch_test
