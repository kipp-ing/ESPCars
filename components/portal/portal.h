#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#include "esphome/components/wifi/wifi_component.h"
#endif
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Quoted, no path: ESPHome copies external component files flat into one build directory.
#include "portal_core.h"

namespace esphome::portal {

/// ESPHome binding for the freestanding core (portal_core.h): an AsyncWebHandler on the shared
/// web_server_base (docs/portal-spec.md section 8). The httpd task validates and enqueues action
/// requests; every entity call happens in loop() (spec section 6.2).
class Portal : public Component, public AsyncWebHandler {
 public:
  void set_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_web_asset(const uint8_t *data, size_t size) {
    this->web_asset_ = data;
    this->web_asset_size_ = size;
  }
  void set_build(const char *build) { this->build_ = build; }
  void set_board(const char *board) { this->board_ = board; }
  void set_node_id(const char *node_id) { this->node_id_ = node_id; }
  void set_mac(const char *mac) { this->mac_ = mac; }
  /// Optional; guards the POST actions only (viewing is always open).
  void set_auth(const char *username, const char *password) {
    this->auth_username_ = username;
    this->auth_password_ = password;
    this->auth_enabled_ = true;
  }
  void set_owns(const char *const *owns, size_t count) {
    this->owns_ = owns;
    this->owns_count_ = count;
  }
  void set_components(const char *const *components, size_t count) {
    this->components_ = components;
    this->components_count_ = count;
  }

  /// Appends one node in pre-order (parent already added). Children reference their parent by
  /// the index a previous call returned; Python tracks that bookkeeping at config time.
  uint16_t add_node(const char *id, const char *kind, const char *label, uint16_t parent, uint32_t budget_ms,
                    bool budget_known, const char *absent_why, const char *role, const char *roles_available,
                    const char *unknown_value_key, const char *unknown_equals, const char *unknown_why,
                    const char *trust_value_key, core::NodeAbsentFn absent_fn = nullptr, void *absent_ctx = nullptr,
                    const char *absent_evidence = nullptr);
  /// The add_value_* / add_series / add_action_* calls attach to whichever node the most recent
  /// add_node() started: codegen emits a node's own members before its children, which keeps
  /// each node's ranges contiguous without tracking offsets.
  void add_value_sensor(const char *key, const char *label, const char *unit, int8_t decimals,
                        sensor::Sensor *value_sensor);
  void add_value_text_sensor(const char *key, const char *label, const char *unit, int8_t decimals,
                             text_sensor::TextSensor *value_sensor);
  void add_value_binary_sensor(const char *key, const char *label, const char *unit,
                               binary_sensor::BinarySensor *value_sensor);
  void add_series(const char *key, const char *label, const char *unit, const char *index_frame, int32_t index_base,
                  int32_t pack_index_base, bool pack_index_known, uint16_t count, int8_t decimals,
                  core::SeriesElementFn element = nullptr, void *ctx = nullptr);
#ifdef USE_SWITCH
  void add_action_switch(const char *key, const char *label, switch_::Switch *entity);
#endif
#ifdef USE_BUTTON
  void add_action_button(const char *key, const char *label, button::Button *entity);
#endif
#ifdef USE_SELECT
  void add_action_select(const char *key, const char *label, select::Select *entity);
#endif
#ifdef USE_NUMBER
  void add_action_number(const char *key, const char *label, int8_t decimals, number::Number *entity);
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  // AsyncWebHandler
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;

 protected:
  struct ValueBinding {
    const char *key{nullptr};
    const char *label{nullptr};
    const char *unit{nullptr};
    int8_t decimals{0};
    bool is_text{false};
    bool is_bool{false};
    sensor::Sensor *value_sensor{nullptr};
    text_sensor::TextSensor *value_text_sensor{nullptr};
    binary_sensor::BinarySensor *value_binary_sensor{nullptr};
  };
  // age_ms is the time since the entity last PUBLISHED, not since its bits last changed: stamped
  // from add_on_state_callback, which Sensor / TextSensor fire on every publish_state(). A
  // BinarySensor fast-paths out when republished unchanged, so for a boolean it is time since
  // the last change (the config layer forbids a budget on such nodes).
  struct ValueRuntime {
    uint32_t last_publish_ms{0};
    bool ever_published{false};
  };

  struct ActionBinding {
    core::Control control{core::Control::SWITCH};
#ifdef USE_SWITCH
    switch_::Switch *sw{nullptr};
#endif
#ifdef USE_BUTTON
    button::Button *button{nullptr};
#endif
#ifdef USE_SELECT
    select::Select *select{nullptr};
#endif
#ifdef USE_NUMBER
    number::Number *number{nullptr};
#endif
    std::vector<const char *> options;  // select: copied at setup
  };
  // Guarded by mutex_. One pending slot per action: while `pending`, a further POST is busy.
  struct ActionRuntime {
    bool pending{false};
    core::ActionValue pending_value;
    core::ActionResult result;
    uint32_t result_ms{0};
  };

  uint16_t add_action_(const char *key, const char *label, core::Control control, int8_t decimals);

  void sample_();
  void sample_action_(size_t index, core::ActionSample *out);
  void run_actions_();
  void perform_action_(size_t index, const core::ActionValue &value);
  void mark_published_(size_t index);
  void update_seq_locked_();
  bool copy_snapshot_(core::Snapshot *out);
  static bool flush_chunk_(void *ctx, const char *data, size_t size);

  bool authorized_(AsyncWebServerRequest *request);
  void send_unauthorized_(httpd_req_t *req);
  void serve_state_(httpd_req_t *req);
  void serve_frontend_(httpd_req_t *req);
  void serve_action_(AsyncWebServerRequest *request, const core::RouteMatch &route);
  void send_status_(httpd_req_t *req, const char *status);
  void log_stack_(const char *what);
  bool ap_active_() const;
  void serve_redirect_(httpd_req_t *req);

  web_server_base::WebServerBase *base_{nullptr};
  const uint8_t *web_asset_{nullptr};
  size_t web_asset_size_{0};
  const char *build_{nullptr};
  const char *board_{nullptr};
  const char *node_id_{nullptr};
  const char *mac_{nullptr};
  const char *const *owns_{nullptr};
  size_t owns_count_{0};
  const char *const *components_{nullptr};
  size_t components_count_{0};
  bool auth_enabled_{false};
  const char *auth_username_{nullptr};
  const char *auth_password_{nullptr};

  core::NodeDef nodes_[PORTAL_MAX_NODES]{};
  size_t node_count_{0};
  uint16_t current_node_{0};

  // Fixed-size, never a std::vector: NodeDef::series points into this array, and a reallocating
  // container would dangle every pointer already handed out.
  core::SeriesDef series_defs_[PORTAL_MAX_SERIES]{};
  size_t series_count_{0};

  ValueBinding value_defs_[PORTAL_MAX_VALUES]{};
  size_t value_count_{0};

  core::ActionDef action_defs_[PORTAL_MAX_ACTIONS]{};
  ActionBinding action_bind_[PORTAL_MAX_ACTIONS]{};
  ActionRuntime action_rt_[PORTAL_MAX_ACTIONS]{};
  size_t action_count_{0};

  // Sampled once per loop() under mutex_. text is a pointer into sample_text_[i]; both are
  // member-owned (never on the stack) so the mutex-protected copy is a fixed-cost memcpy per
  // value with no allocation.
  core::ValueSample samples_[PORTAL_MAX_VALUES]{};
  char sample_text_[PORTAL_MAX_VALUES][PORTAL_TEXT_MAX]{};
  ValueRuntime value_runtime_[PORTAL_MAX_VALUES]{};
  core::ActionSample action_samples_[PORTAL_MAX_ACTIONS]{};

  // Request-local copy-out (the httpd task works from frozen buffers, so loop()'s next sample can
  // never mutate a response that is still streaming).
  core::ValueSample request_values_[PORTAL_MAX_VALUES]{};
  char request_text_[PORTAL_MAX_VALUES][PORTAL_TEXT_MAX]{};
  core::ActionSample request_actions_[PORTAL_MAX_ACTIONS]{};

  core::State resolved_states_[PORTAL_MAX_NODES]{};
  uint32_t resolved_ages_[PORTAL_MAX_NODES]{};
  bool resolved_age_known_[PORTAL_MAX_NODES]{};
  core::SeriesElement series_scratch_[core::SERIES_MAX_ELEMENTS]{};  // keeps it off the httpd stack

  core::LinkDef links_[1]{{"wifi_setup", "WiFi setup", "/wifi"}};
  size_t link_count_{0};  // 1 while the access point is active

  uint32_t seq_{1};
  uint32_t last_fingerprint_{0};
  bool have_fingerprint_{false};

  SemaphoreHandle_t mutex_{nullptr};
  char chunk_buf_[core::CHUNK_BYTES]{};  // httpd task only
  // POST body of the request being handled (httpd task only; one request at a time).
  char body_buf_[core::ACTION_BODY_MAX]{};
  size_t body_total_{0};
  size_t body_stored_{0};
  const AsyncWebServerRequest *body_req_{nullptr};  // request that streamed body_buf_
};

}  // namespace esphome::portal
