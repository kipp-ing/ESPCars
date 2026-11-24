#pragma once
#include <atomic>
#include <cstdint>
namespace linbus_test {
inline std::atomic<int64_t> now_us{1'000'000};
}
inline int64_t esp_timer_get_time() { return linbus_test::now_us.load(); }
