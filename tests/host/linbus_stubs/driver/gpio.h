#pragma once
#include <cstdint>
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1;
constexpr int GPIO_PULLUP_DISABLE = 0;
constexpr int GPIO_PULLDOWN_DISABLE = 0;
constexpr int GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
  uint64_t pin_bit_mask;
  int mode;
  int pull_up_en;
  int pull_down_en;
  int intr_type;
};
inline void gpio_config(gpio_config_t *) {}
namespace linbus_test {
inline unsigned raw_gpio_writes = 0;
}
inline void gpio_set_level(gpio_num_t, int) { ++linbus_test::raw_gpio_writes; }
