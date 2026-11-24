#pragma once
// Hardware boundaries only; generate_portal_runtime.py compiles the complete production binding.
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define USE_SWITCH
#define USE_SELECT
#define USE_CAPTIVE_PORTAL
#define PORTAL_MAX_NODES 8
#define PORTAL_MAX_VALUES 8
#define PORTAL_MAX_SERIES 1
#define PORTAL_MAX_ACTIONS 4
#define PORTAL_TEXT_MAX 48
#define PORTAL_PATH_MAX 96

inline std::vector<std::string> portal_test_logs;
inline bool portal_test_mutex_failure = false;
inline std::mutex portal_test_mutex;
inline std::function<void()> portal_test_after_unlock;
inline std::thread::id portal_test_http_thread;
inline void portal_test_log(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  portal_test_logs.emplace_back(buf);
}
#define ESPHOME_LOG_TAG(name, value) static constexpr const char *name = value
#define ESP_LOGE(tag, ...) portal_test_log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) portal_test_log(__VA_ARGS__)
template<typename... Args> inline void portal_test_ignore(const Args &...) {}
#define ESP_LOGD(tag, ...) portal_test_ignore(__VA_ARGS__)
#define ESP_LOGCONFIG(tag, ...) portal_test_ignore(__VA_ARGS__)
#define LOG_STR(x) x
#define LOG_STR_LITERAL(x) x

using SemaphoreHandle_t = std::mutex *;
inline constexpr int pdTRUE = 1;
inline constexpr unsigned portMAX_DELAY = ~0u;
inline unsigned pdMS_TO_TICKS(unsigned x) { return x; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return portal_test_mutex_failure ? nullptr : &portal_test_mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout) {
  if (timeout == 0)
    return mutex->try_lock() ? pdTRUE : 0;
  mutex->lock();
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) {
  mutex->unlock();
  if (std::this_thread::get_id() == portal_test_http_thread && portal_test_after_unlock)
    portal_test_after_unlock();
}
inline unsigned uxTaskGetStackHighWaterMark(void *) { return 4096; }
using esp_err_t = int;
inline constexpr int ESP_OK = 0, ESP_ERR_NOT_FOUND = 1, ESP_ERR_INVALID_SIZE = 2;
inline constexpr int HTTP_GET = 0, HTTP_POST = 1, HTTPD_500_INTERNAL_SERVER_ERROR = 500;
inline constexpr int HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
  int method{HTTP_GET};
  const char *uri{"/api/v1/state"};
  std::string query, status{"200 OK"}, response;
};
inline size_t httpd_req_get_url_query_len(httpd_req_t *r) { return r->query.size(); }
inline int httpd_req_get_url_query_str(httpd_req_t *r, char *out, size_t cap) {
  if (r->query.size() >= cap)
    return ESP_ERR_INVALID_SIZE;
  std::memcpy(out, r->query.c_str(), r->query.size() + 1);
  return ESP_OK;
}
inline int httpd_query_key_value(const char *query, const char *key, char *out, size_t cap) {
  const std::string q(query), prefix = std::string(key) + "=";
  size_t start = 0;
  while (start < q.size()) {
    size_t end = q.find('&', start);
    if (end == std::string::npos)
      end = q.size();
    if (q.compare(start, prefix.size(), prefix) == 0) {
      auto val = q.substr(start + prefix.size(), end - start - prefix.size());
      if (val.size() >= cap)
        return ESP_ERR_INVALID_SIZE;
      std::memcpy(out, val.c_str(), val.size() + 1);
      return ESP_OK;
    }
    start = end + 1;
  }
  return ESP_ERR_NOT_FOUND;
}
inline int httpd_resp_set_status(httpd_req_t *r, const char *status) {
  r->status = status;
  return ESP_OK;
}
inline int httpd_resp_set_type(httpd_req_t *, const char *) { return ESP_OK; }
inline int httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) { return ESP_OK; }
inline int httpd_resp_send(httpd_req_t *r, const char *data, int size) {
  if (data != nullptr)
    r->response.append(data, size == HTTPD_RESP_USE_STRLEN ? std::strlen(data) : size);
  return ESP_OK;
}
inline int httpd_resp_send_chunk(httpd_req_t *r, const char *data, size_t size) {
  if (data != nullptr)
    r->response.append(data, size);
  return ESP_OK;
}
inline int httpd_resp_send_err(httpd_req_t *, int, const char *) { return ESP_OK; }
inline void esp_fill_random(void *data, size_t size) { std::memset(data, 1, size); }
struct AsyncWebServerRequest {
  httpd_req_t req;
  std::string form_value;
  bool form{false};
  operator httpd_req_t *() { return &req; }
  void send(int status) { req.status = std::to_string(status); }
  bool authenticate(const char *, const char *) { return true; }
  std::optional<std::string> get_header(const char *) {
    return form ? std::optional<std::string>("application/x-www-form-urlencoded") : std::nullopt;
  }
  bool hasArg(const char *) { return form; }
  std::string arg(const char *) { return form_value; }
  size_t contentLength() { return form_value.size(); }
};
struct AsyncWebHandler {
  virtual ~AsyncWebHandler() = default;
  virtual bool canHandle(AsyncWebServerRequest *) const { return true; }
  virtual void handleRequest(AsyncWebServerRequest *) {}
  virtual void handleBody(AsyncWebServerRequest *, uint8_t *, size_t, size_t, size_t) {}
};
namespace esphome {
inline uint32_t millis() { return 100; }
namespace setup_priority {
inline constexpr float WIFI = 250;
}
struct Component {
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }
  void mark_failed(const char *reason = nullptr) {
    failed = true;
    failure_reason = reason;
  }
  void status_set_warning() { warning = true; }
  bool failed{false}, warning{false};
  const char *failure_reason{nullptr};
};
inline struct Application {
  uint32_t get_loop_component_start_time() const { return millis(); }
} App;
namespace sensor {
struct Sensor {
  float state{0};
  bool has_state() const { return true; }
  template<typename F> void add_on_state_callback(F) {}
};
}  // namespace sensor
namespace text_sensor {
struct TextSensor {
  std::string state;
  bool has_state() const { return true; }
  template<typename F> void add_on_state_callback(F) {}
};
}  // namespace text_sensor
namespace binary_sensor {
struct BinarySensor {
  bool state{false};
  bool has_state() const { return true; }
  template<typename F> void add_on_state_callback(F) {}
};
}  // namespace binary_sensor
namespace switch_ {
struct Switch {
  bool state{false};
  void turn_on() { state = true; }
  void turn_off() { state = false; }
};
}  // namespace switch_
namespace select {
struct Select {
  struct Traits {
    std::vector<const char *> options;
    const auto &get_options() const { return options; }
  } traits;
  std::optional<size_t> active_index() const { return active; }
  const char *option_at(size_t i) const { return i < traits.options.size() ? traits.options[i] : nullptr; }
  Select &make_call() { return *this; }
  Select &set_option(const char *) { return *this; }
  void perform() {}
  std::optional<size_t> active{0};
};
}  // namespace select
namespace captive_portal {
inline struct CaptivePortal {
  bool active{false};
  bool is_active() const { return active; }
} portal_test_ap;
inline auto *global_captive_portal = &portal_test_ap;
}  // namespace captive_portal
namespace network {
inline constexpr size_t IP_ADDRESS_BUFFER_SIZE = 48;
}
namespace wifi {
inline struct Wifi {
  struct IP {
    void str_to(char *s) { std::strcpy(s, "192.0.2.1"); }
  };
  IP wifi_soft_ap_ip() { return {}; }
} portal_test_wifi;
inline auto *global_wifi_component = &portal_test_wifi;
}  // namespace wifi
namespace web_server_base {
namespace internal {
struct GuardMiddlewareHandler : AsyncWebHandler {
  explicit GuardMiddlewareHandler(AsyncWebHandler *next) : next_(next) {}
  bool check_origin_(AsyncWebServerRequest *) { return true; }
  AsyncWebHandler *next_;
};
}  // namespace internal
struct WebServerBase {
  void init() {}
  void add_handler_without_auth(AsyncWebHandler *h) { handler.reset(h); }
  unsigned get_port() const { return 80; }
  std::unique_ptr<AsyncWebHandler> handler;
};
}  // namespace web_server_base
}  // namespace esphome
