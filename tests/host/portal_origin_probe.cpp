// Driven by tests/portal/test_origin_guard.py; hardware-free HTTP dispatch probe.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using PlatformString = std::string;
struct AsyncWebServerRequest {
  std::string origin;
  std::string host{"device.local"};
  const char *content_type;
  int status{0};
  template<size_t N> size_t get_header_to(const char *name, char (&out)[N]) {
    const auto &value = std::strcmp(name, "Origin") == 0 ? origin : host;
    if (value.size() >= N)
      return SIZE_MAX;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return value.size();
  }
  void send(int code) { status = code; }
};
class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler() = default;
  virtual bool canHandle(AsyncWebServerRequest *) const { return true; }
  virtual void handleRequest(AsyncWebServerRequest *) { requests++; }
  virtual void handleBody(AsyncWebServerRequest *, uint8_t *, size_t, size_t, size_t) { bodies++; }
  virtual void handleUpload(AsyncWebServerRequest *, const PlatformString &, size_t, uint8_t *, size_t, bool) {}
  virtual bool isRequestHandlerTrivial() const { return false; }
  unsigned requests{0};
  unsigned bodies{0};
};
namespace esphome::web_server_base {
class WebServerBase {
 public:
  void init() {}
  void add_handler_without_auth(AsyncWebHandler *handler) { registered = handler; }
  const char *const *allowed_origins_{nullptr};
  AsyncWebHandler *registered{nullptr};
};
WebServerBase *global_web_server_base;
}  // namespace esphome::web_server_base

static unsigned warnings = 0;
#define ESP_LOGW(...) (++warnings)
#include "portal_origin_extracted.h"

int main() {
  using namespace esphome;
  web_server_base::WebServerBase base;
  static const char *const allowed[] = {"https://trusted.example", nullptr};
  base.allowed_origins_ = allowed;
  web_server_base::global_web_server_base = &base;
  portal::Portal portal;
  portal.base_ = &base;
  portal.setup();
  // A middleware is server-owned, the component itself is not.
  std::unique_ptr<AsyncWebHandler> owner(base.registered == &portal ? nullptr : base.registered);
  int failures = 0;
  for (const char *type : {"application/x-www-form-urlencoded", "text/plain", "application/json"}) {
    for (const char *origin : {"https://foreign.example", "null", "http://device.local.evil", "http://device.local",
                               "https://trusted.example", ""}) {
      AsyncWebServerRequest request;
      request.origin = origin;
      request.content_type = type;
      bool permitted = request.origin.empty() || request.origin == "http://device.local";
#ifdef USE_WEBSERVER_ALLOWED_ORIGINS
      permitted |= request.origin == allowed[0];
#endif
#ifdef USE_WEBSERVER_ALLOW_ANY_ORIGIN
      permitted = true;
#endif
      const unsigned before_requests = portal.requests, before_bodies = portal.bodies, before_warnings = warnings;
      uint8_t body[] = {'x'};
      base.registered->handleBody(&request, body, 1, 0, 1);
      base.registered->handleRequest(&request);
      if (portal.requests != before_requests + permitted || portal.bodies != before_bodies + permitted ||
          (!permitted && (request.status != 403 || warnings == before_warnings))) {
        std::fprintf(stderr, "origin=%s type=%s permitted=%d status=%d dispatched=%u buffered=%u\n", origin, type,
                     permitted, request.status, portal.requests - before_requests, portal.bodies - before_bodies);
        failures++;
      }
    }
  }
  return failures == 0 ? 0 : 1;
}
