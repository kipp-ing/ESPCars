#pragma once
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
namespace linbus_test {
inline std::mutex log_mutex;
inline std::vector<std::string> logs;
template<typename... Args> void log(const char *format, Args... args) {
  char buf[512];
  if constexpr (sizeof...(args) == 0)
    snprintf(buf, sizeof(buf), "%s", format);
  else
    snprintf(buf, sizeof(buf), format, args...);
  std::lock_guard<std::mutex> lock(log_mutex);
  logs.emplace_back(buf);
}
}  // namespace linbus_test
#define ESPHOME_LOG_TAG(name, value) [[maybe_unused]] static constexpr const char *name = value
#define ESP_LOGV(tag, ...) ::linbus_test::log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) ::linbus_test::log(__VA_ARGS__)
#define ESP_LOGE(tag, ...) ::linbus_test::log(__VA_ARGS__)
#define LOG_STR_LITERAL(value) value
