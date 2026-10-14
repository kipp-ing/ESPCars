#include "portal.h"

#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

#include <esp_random.h>

namespace esphome::portal {

static const char *const TAG = "portal";

// ---- tree construction (called from generated setup code) ------------------------------------

uint16_t Portal::add_node(const char *id, const char *kind, const char *label, uint16_t parent, uint32_t budget_ms,
                          bool budget_known, const char *absent_why, const char *role, const char *roles_available,
                          const char *unknown_value_key, const char *unknown_equals, const char *unknown_why,
                          const char *trust_value_key, core::NodeAbsentFn absent_fn, void *absent_ctx,
                          const char *absent_evidence, bool on_demand) {
  const uint16_t index = static_cast<uint16_t>(this->node_count_);
  auto &node = this->nodes_[this->node_count_];
  node.id = id;
  node.kind = kind;
  node.label = label;
  node.parent = parent;
  node.value_first = static_cast<uint16_t>(this->value_count_);
  node.value_count = 0;
  node.action_first = static_cast<uint16_t>(this->action_count_);
  node.action_count = 0;
  node.budget_ms = budget_ms;
  node.budget_known = budget_known;
  node.absent_why = absent_why;
  node.role = role;
  node.roles_available = roles_available;
  node.unknown_value_key = unknown_value_key;
  node.unknown_equals = unknown_equals;
  node.unknown_why = unknown_why;
  node.trust_value_key = trust_value_key;
  node.absent_fn = absent_fn;
  node.absent_ctx = absent_ctx;
  node.absent_evidence = absent_evidence;
  node.on_demand = on_demand;
  this->node_count_++;
  this->current_node_ = index;
  return index;
}

void Portal::add_value_sensor(const char *key, const char *label, const char *unit, int8_t decimals,
                              sensor::Sensor *value_sensor, const core::ValueLimit *limit) {
  auto &def = this->value_defs_[this->value_count_];
  def.limit = limit;
  def.key = key;
  def.label = label;
  def.unit = unit;
  def.decimals = decimals;
  def.value_sensor = value_sensor;
  this->value_count_++;
  this->nodes_[this->current_node_].value_count++;
}

void Portal::add_value_text_sensor(const char *key, const char *label, const char *unit, int8_t decimals,
                                   text_sensor::TextSensor *value_sensor) {
  auto &def = this->value_defs_[this->value_count_];
  def.key = key;
  def.label = label;
  def.unit = unit;
  def.decimals = decimals;
  def.is_text = true;
  def.value_text_sensor = value_sensor;
  this->value_count_++;
  this->nodes_[this->current_node_].value_count++;
}

void Portal::add_value_binary_sensor(const char *key, const char *label, const char *unit,
                                     binary_sensor::BinarySensor *value_sensor, const core::ValueLimit *limit) {
  auto &def = this->value_defs_[this->value_count_];
  def.limit = limit;
  def.key = key;
  def.label = label;
  def.unit = unit;
  def.is_bool = true;
  def.value_binary_sensor = value_sensor;
  this->value_count_++;
  this->nodes_[this->current_node_].value_count++;
}

void Portal::add_series(const char *key, const char *label, const char *unit, const char *index_frame,
                        int32_t index_base, int32_t pack_index_base, bool pack_index_known, uint16_t count,
                        int8_t decimals, core::SeriesElementFn element, void *ctx) {
  auto &def = this->series_defs_[this->series_count_];
  def.key = key;
  def.label = label;
  def.unit = unit;
  def.index_frame = index_frame;
  def.index_base = index_base;
  def.pack_index_base = pack_index_base;
  def.pack_index_known = pack_index_known;
  def.count = count;
  def.decimals = decimals;
  def.element = element;
  def.ctx = ctx;
  this->nodes_[this->current_node_].series = &def;
  this->series_count_++;
}

uint16_t Portal::add_action_(const char *key, const char *label, core::Control control, int8_t decimals) {
  const size_t index = this->action_count_;
  auto &def = this->action_defs_[index];
  def.key = key;
  def.label = label;
  def.control = control;
  def.decimals = decimals;
  this->action_bind_[index].control = control;
  this->action_count_++;
  this->nodes_[this->current_node_].action_count++;
  return static_cast<uint16_t>(index);
}

#ifdef USE_SWITCH
void Portal::add_action_switch(const char *key, const char *label, switch_::Switch *entity) {
  this->action_bind_[this->add_action_(key, label, core::Control::SWITCH, 0)].sw = entity;
}
#endif
#ifdef USE_BUTTON
void Portal::add_action_button(const char *key, const char *label, button::Button *entity) {
  this->action_bind_[this->add_action_(key, label, core::Control::BUTTON, 0)].button = entity;
}
#endif
#ifdef USE_SELECT
void Portal::add_action_select(const char *key, const char *label, select::Select *entity) {
  this->action_bind_[this->add_action_(key, label, core::Control::SELECT, 0)].select = entity;
}
#endif
#ifdef USE_NUMBER
void Portal::add_action_number(const char *key, const char *label, int8_t decimals, number::Number *entity) {
  this->action_bind_[this->add_action_(key, label, core::Control::NUMBER, decimals)].number = entity;
}
#endif

// ---- lifecycle -------------------------------------------------------------------------------

void Portal::setup() {
  this->mutex_ = xSemaphoreCreateMutex();
  if (this->mutex_ == nullptr) {
    ESP_LOGE(TAG, "mutex allocation failed");
    this->mark_failed();
    return;
  }
  this->seq_ = 1;
  // Stamp last_publish_ms from the entity's own callback. value_defs_ is fully populated by
  // now: the generated add_node()/add_value_*() calls run before any Component::setup().
  for (size_t i = 0; i < this->value_count_; i++) {
    const auto &def = this->value_defs_[i];
    if (def.is_bool) {
      if (def.value_binary_sensor != nullptr)
        def.value_binary_sensor->add_on_state_callback([this, i](bool) { this->mark_published_(i); });
    } else if (def.is_text) {
      if (def.value_text_sensor != nullptr)
        def.value_text_sensor->add_on_state_callback([this, i](const std::string &) { this->mark_published_(i); });
    } else {
      if (def.value_sensor != nullptr)
        def.value_sensor->add_on_state_callback([this, i](float) { this->mark_published_(i); });
    }
  }
#ifdef USE_SELECT
  for (size_t i = 0; i < this->action_count_; i++) {
    auto &bind = this->action_bind_[i];
    if (bind.control != core::Control::SELECT || bind.select == nullptr)
      continue;
    for (const char *option : bind.select->traits.get_options())
      bind.options.push_back(option);
    this->action_defs_[i].options = bind.options.data();
    this->action_defs_[i].option_count = static_cast<uint16_t>(bind.options.size());
  }
#endif
  this->base_->init();
  this->base_->add_handler_without_auth(this);
}

void Portal::dump_config() {
  ESP_LOGCONFIG(TAG, "Portal: port %u, %u nodes, %u values, %u actions, web asset %u bytes, auth: %s",
                this->base_ != nullptr ? this->base_->get_port() : 0, static_cast<unsigned>(this->node_count_),
                static_cast<unsigned>(this->value_count_), static_cast<unsigned>(this->action_count_),
                static_cast<unsigned>(this->web_asset_size_),
                this->auth_enabled_ ? "digest (POST actions only)" : "none");
}

void Portal::mark_published_(size_t index) {
  this->value_runtime_[index].last_publish_ms = millis();
  this->value_runtime_[index].ever_published = true;
}

void Portal::loop() {
  this->run_actions_();
  this->sample_();
}

// ---- sampling (main loop) --------------------------------------------------------------------

void Portal::sample_action_(size_t index, core::ActionSample *out) {
  const auto &bind = this->action_bind_[index];
  out->enabled = true;
  out->has_value = false;
  out->has_min = out->has_max = out->has_step = false;
  out->unit = nullptr;
  switch (bind.control) {
    case core::Control::SWITCH:
#ifdef USE_SWITCH
      if (bind.sw != nullptr) {
        out->has_value = true;
        out->boolean = bind.sw->state;
      }
#endif
      break;
    case core::Control::BUTTON:
      break;
    case core::Control::SELECT:
#ifdef USE_SELECT
      if (bind.select != nullptr) {
        auto active = bind.select->active_index();
        if (active.has_value()) {
          const char *option = bind.select->option_at(*active);
          if (option != nullptr) {
            std::strncpy(out->text, option, core::ACTION_TEXT_MAX - 1);
            out->text[core::ACTION_TEXT_MAX - 1] = '\0';
            out->has_value = true;
          }
        }
      }
#endif
      out->enabled = out->has_value;
      break;
    case core::Control::NUMBER:
#ifdef USE_NUMBER
      if (bind.number != nullptr) {
        const auto &traits = bind.number->traits;
        out->has_value = !std::isnan(bind.number->state);
        out->number = bind.number->state;
        out->has_min = !std::isnan(traits.get_min_value());
        out->min = traits.get_min_value();
        out->has_max = !std::isnan(traits.get_max_value());
        out->max = traits.get_max_value();
        out->has_step = !std::isnan(traits.get_step());
        out->step = traits.get_step();
        const auto unit = bind.number->get_unit_of_measurement_ref();
        out->unit = unit.empty() ? nullptr : unit.c_str();
      }
#endif
      out->enabled = out->has_value;
      break;
  }
}

void Portal::sample_() {
  if (this->mutex_ == nullptr || xSemaphoreTake(this->mutex_, 0) != pdTRUE)
    return;
  const uint32_t now = millis();
  for (size_t i = 0; i < this->value_count_; i++) {
    const auto &def = this->value_defs_[i];
    core::ValueSample &sample = this->samples_[i];
    sample.key = def.key;
    sample.label = def.label;
    sample.unit = def.unit;
    sample.decimals = def.decimals;
    sample.is_text = def.is_text;
    sample.is_bool = def.is_bool;
    sample.limit = def.limit;
    sample.text = nullptr;
    sample.number = 0.0f;
    sample.text_overflow = false;

    bool published = false;
    if (def.is_bool) {
      if (def.value_binary_sensor != nullptr && def.value_binary_sensor->has_state()) {
        published = true;
        sample.number = def.value_binary_sensor->state ? 1.0f : 0.0f;
      }
    } else if (def.is_text) {
      if (def.value_text_sensor != nullptr && def.value_text_sensor->has_state()) {
        published = true;
        const std::string &state = def.value_text_sensor->state;
        const size_t max_len = PORTAL_TEXT_MAX - 1;
        sample.text_overflow = state.size() > max_len;
        std::strncpy(this->sample_text_[i], state.c_str(), max_len);
        this->sample_text_[i][max_len] = '\0';
        sample.text = this->sample_text_[i];
      }
    } else {
      if (def.value_sensor != nullptr && def.value_sensor->has_state()) {
        published = true;
        sample.number = def.value_sensor->state;
      }
    }
    sample.has_value = published;
    sample.age_known = published;

    // The ever_published self-heal covers a value already published before this component's
    // setup() ran (so its callback never fired into us): "first time we notice it" counts as
    // "just published" rather than an inflated age counted from boot.
    auto &rt = this->value_runtime_[i];
    if (published) {
      if (!rt.ever_published) {
        rt.ever_published = true;
        rt.last_publish_ms = now;
      }
      sample.age_ms = now - rt.last_publish_ms;
    } else {
      rt.ever_published = false;
      sample.age_ms = 0;
    }
  }
  for (size_t i = 0; i < this->action_count_; i++) {
    this->sample_action_(i, &this->action_samples_[i]);
    this->action_samples_[i].result = this->action_rt_[i].result;
  }
  this->link_count_ = this->ap_active_() ? 1 : 0;
  this->update_seq_locked_();
  xSemaphoreGive(this->mutex_);
}

/// Advances seq when the fingerprint of the current samples changed. Caller holds mutex_.
void Portal::update_seq_locked_() {
  core::Snapshot view{};
  view.nodes = this->nodes_;
  view.node_count = this->node_count_;
  view.values = this->samples_;
  view.value_count = this->value_count_;
  view.actions = this->action_defs_;
  view.action_samples = this->action_samples_;
  view.action_count = this->action_count_;
  view.links = this->links_;
  view.link_count = this->link_count_;
  const uint32_t fp = core::fingerprint(view);
  if (this->have_fingerprint_ && fp != this->last_fingerprint_) {
    this->seq_++;
    if (this->seq_ == 0)
      this->seq_ = 1;
  }
  this->last_fingerprint_ = fp;
  this->have_fingerprint_ = true;
}

// ---- actions (main loop: the only place an entity is touched) --------------------------------

void Portal::run_actions_() {
  for (size_t i = 0; i < this->action_count_; i++) {
    core::ActionValue value;
    if (this->mutex_ == nullptr || xSemaphoreTake(this->mutex_, 0) != pdTRUE)
      return;
    const bool pending = this->action_rt_[i].pending;
    if (pending)
      value = this->action_rt_[i].pending_value;
    xSemaphoreGive(this->mutex_);
    if (!pending)
      continue;

    this->perform_action_(i, value);

    // Held only for short copies elsewhere, so waiting is bounded in practice; the entity call
    // above must never repeat, so the record may not be skipped.
    xSemaphoreTake(this->mutex_, portMAX_DELAY);
    auto &rt = this->action_rt_[i];
    rt.pending = false;
    rt.result.state = core::ActionState::ACCEPTED;
    rt.result.code = core::ActionCode::APPLIED;
    rt.result_ms = millis();
    this->action_samples_[i].result = rt.result;
    this->update_seq_locked_();
    xSemaphoreGive(this->mutex_);
  }
}

void Portal::perform_action_(size_t index, const core::ActionValue &value) {
  const auto &bind = this->action_bind_[index];
  ESP_LOGD(TAG, "action %s performed", this->action_defs_[index].key);
  switch (bind.control) {
    case core::Control::SWITCH:
#ifdef USE_SWITCH
      if (bind.sw != nullptr) {
        if (value.boolean)
          bind.sw->turn_on();
        else
          bind.sw->turn_off();
      }
#endif
      break;
    case core::Control::BUTTON:
#ifdef USE_BUTTON
      if (bind.button != nullptr)
        bind.button->press();
#endif
      break;
    case core::Control::SELECT:
#ifdef USE_SELECT
      if (bind.select != nullptr)
        bind.select->make_call().set_option(value.text).perform();
#endif
      break;
    case core::Control::NUMBER:
#ifdef USE_NUMBER
      if (bind.number != nullptr)
        bind.number->make_call().set_value(value.number).perform();
#endif
      break;
  }
}

// ---- request copy-out ------------------------------------------------------------------------

bool Portal::flush_chunk_(void *ctx, const char *data, size_t size) {
  return httpd_resp_send_chunk(static_cast<httpd_req_t *>(ctx), data, size) == ESP_OK;
}

bool Portal::copy_snapshot_(core::Snapshot *out) {
  if (this->mutex_ == nullptr || xSemaphoreTake(this->mutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;
  const uint32_t now = millis();
  for (size_t i = 0; i < this->value_count_; i++) {
    this->request_values_[i] = this->samples_[i];
    if (this->samples_[i].is_text && this->samples_[i].text != nullptr) {
      std::strncpy(this->request_text_[i], this->sample_text_[i], PORTAL_TEXT_MAX - 1);
      this->request_text_[i][PORTAL_TEXT_MAX - 1] = '\0';
      this->request_values_[i].text = this->request_text_[i];
    }
  }
  for (size_t i = 0; i < this->action_count_; i++) {
    this->request_actions_[i] = this->action_samples_[i];
    this->request_actions_[i].result.age_ms = now - this->action_rt_[i].result_ms;
  }
  const uint32_t seq = this->seq_;
  xSemaphoreGive(this->mutex_);

  if (this->node_count_ == 0)
    return false;

  out->seq = seq;
  out->uptime_ms = now;
  out->identity.board = this->board_;
  out->identity.node_id = this->node_id_;
  out->identity.mac = this->mac_;
  out->identity.build = this->build_;
  out->identity.owns = this->owns_;
  out->identity.owns_count = this->owns_count_;
  out->identity.components = this->components_;
  out->identity.components_count = this->components_count_;
  out->nodes = this->nodes_;
  out->node_count = this->node_count_;
  out->values = this->request_values_;
  out->value_count = this->value_count_;
  out->actions = this->action_defs_;
  out->action_samples = this->request_actions_;
  out->action_count = this->action_count_;
  out->links = this->links_;
  out->link_count = this->link_count_;
  return true;
}

// ---- HTTP (httpd task) -----------------------------------------------------------------------

bool Portal::canHandle(AsyncWebServerRequest *request) const {
  httpd_req_t *req = *request;
  if (req->method != HTTP_GET && req->method != HTTP_POST)
    return false;
  return core::match_route(req->method == HTTP_POST, req->uri, this->action_count_ != 0, this->ap_active_()).route !=
         core::Route::NONE;
}

bool Portal::ap_active_() const {
#ifdef USE_CAPTIVE_PORTAL
  return captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active();
#else
  return false;
#endif
}

void Portal::serve_redirect_(httpd_req_t *req) {
#ifdef USE_CAPTIVE_PORTAL
  char ip[network::IP_ADDRESS_BUFFER_SIZE];
  char location[64];
  wifi::global_wifi_component->wifi_soft_ap_ip().str_to(ip);
  snprintf(location, sizeof(location), "http://%s/", ip);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);  // must outlive the send below
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, nullptr, 0);
#else
  this->send_status_(req, "404 Not Found");
#endif
}

void Portal::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  // A body belongs to the request that streamed it: an aborted stream's leftovers are ignored
  // by the next request (serve_action_ checks body_req_).
  if (index == 0 || this->body_req_ != request) {
    this->body_stored_ = 0;
    this->body_req_ = request;
  }
  this->body_total_ = total;
  for (size_t i = 0; i < len && this->body_stored_ < sizeof(this->body_buf_); i++)
    this->body_buf_[this->body_stored_++] = static_cast<char>(data[i]);
}

void Portal::handleRequest(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  const core::RouteMatch route =
      core::match_route(req->method == HTTP_POST, req->uri, this->action_count_ != 0, this->ap_active_());
  switch (route.route) {
    case core::Route::PAGE:
      this->serve_frontend_(req);
      break;
    case core::Route::STATE:
      this->serve_state_(req);
      break;
    case core::Route::ACTION:
      this->serve_action_(request, route);
      break;
    case core::Route::REDIRECT:
      this->serve_redirect_(req);
      break;
    case core::Route::NONE:
      this->send_status_(req, "404 Not Found");
      break;
  }
  this->body_total_ = 0;
  this->body_stored_ = 0;
  this->body_req_ = nullptr;
}

void Portal::send_status_(httpd_req_t *req, const char *status) {
  httpd_resp_set_status(req, status);
  httpd_resp_send(req, nullptr, 0);
}

void Portal::log_stack_(const char *what) {
  ESP_LOGD(TAG, "httpd stack high-water mark after %s: %u bytes free", what,
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

/// True if the request carries valid credentials, or no login is configured. On failure the
/// 401 Digest challenge has already been sent (empty body).
bool Portal::authorized_(AsyncWebServerRequest *request) {
#ifdef USE_WEBSERVER_AUTH
  if (!this->auth_enabled_)
    return true;
  if (request->authenticate(this->auth_username_, this->auth_password_))
    return true;
  this->send_unauthorized_(*request);
  return false;
#else
  (void) request;
  return true;  // no auth: configured
#endif
}

void Portal::send_unauthorized_(httpd_req_t *req) {
  // Same stateless challenge web_server_idf issues, but with an empty body.
  uint8_t random_bytes[16];
  char nonce[33];
  char opaque[33];
  char header[160];
  static const char HEX[] = "0123456789abcdef";
  esp_fill_random(random_bytes, sizeof(random_bytes));
  for (size_t i = 0; i < sizeof(random_bytes); i++) {
    nonce[2 * i] = HEX[random_bytes[i] >> 4];
    nonce[2 * i + 1] = HEX[random_bytes[i] & 0x0F];
  }
  nonce[32] = '\0';
  esp_fill_random(random_bytes, sizeof(random_bytes));
  for (size_t i = 0; i < sizeof(random_bytes); i++) {
    opaque[2 * i] = HEX[random_bytes[i] >> 4];
    opaque[2 * i + 1] = HEX[random_bytes[i] & 0x0F];
  }
  opaque[32] = '\0';
  snprintf(header, sizeof(header), R"(Digest realm="Login Required", qop="auth", nonce="%s", opaque="%s")", nonce,
           opaque);
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "WWW-Authenticate", header);  // header must outlive the send below
  httpd_resp_send(req, nullptr, 0);
}

void Portal::serve_state_(httpd_req_t *req) {
  core::Snapshot snapshot{};
  if (!this->copy_snapshot_(&snapshot)) {
    this->send_status_(req, "503 Service Unavailable");
    return;
  }

  const size_t query_length = httpd_req_get_url_query_len(req);
  if (query_length >= 32) {
    this->send_status_(req, "400 Bad Request");
    return;
  }

  char path_buf[40]{};
  bool has_path_param = false;
  char after_buf[16]{};
  bool has_after_param = false;
  uint32_t after_value = 0;

  if (query_length != 0) {
    char query[32]{};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
      this->send_status_(req, "400 Bad Request");
      return;
    }
    const esp_err_t path_status = httpd_query_key_value(query, "path", path_buf, sizeof(path_buf));
    if (path_status != ESP_OK && path_status != ESP_ERR_NOT_FOUND) {
      this->send_status_(req, "400 Bad Request");
      return;
    }
    has_path_param = path_status == ESP_OK;

    const esp_err_t after_status = httpd_query_key_value(query, "after", after_buf, sizeof(after_buf));
    if (after_status != ESP_OK && after_status != ESP_ERR_NOT_FOUND) {
      this->send_status_(req, "400 Bad Request");
      return;
    }
    has_after_param = after_status == ESP_OK;

    if ((!has_path_param && !has_after_param) || (has_after_param && !core::parse_after(after_buf, &after_value))) {
      this->send_status_(req, "400 Bad Request");
      return;
    }
  }

  // path present and unknown -> 404 BEFORE the after check: a 204 for a path this device does
  // not have would be a lie, and the page's poll falls back to the full tree on 404.
  const char *path = has_path_param ? path_buf : nullptr;
  if (path != nullptr && !core::has_node(snapshot, path)) {
    this->send_status_(req, "404 Not Found");
    return;
  }
  if (has_after_param && after_value == snapshot.seq) {
    this->send_status_(req, "204 No Content");
    return;
  }

  core::Resolved resolved{this->resolved_states_, this->resolved_ages_, this->resolved_age_known_, this->node_count_,
                          this->series_scratch_};
  core::resolve(snapshot, resolved);

  core::ChunkWriter writer;
  writer.init(this->chunk_buf_, sizeof(this->chunk_buf_), &Portal::flush_chunk_, req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (!core::write_envelope(writer, snapshot, resolved, path) || !writer.finish())
    return;
  httpd_resp_send_chunk(req, nullptr, 0);
  this->log_stack_("state");
}

void Portal::serve_frontend_(httpd_req_t *req) {
  if (this->web_asset_ == nullptr || this->web_asset_size_ == 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "frontend unavailable");
    return;
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                     "connect-src 'self'");
  httpd_resp_send(req, reinterpret_cast<const char *>(this->web_asset_), this->web_asset_size_);
}

void Portal::serve_action_(AsyncWebServerRequest *request, const core::RouteMatch &route) {
  httpd_req_t *req = *request;
  if (!this->authorized_(request))
    return;

  // Resolve node id + key to an action index.
  size_t found = this->action_count_;
  const core::NodeDef *found_node = nullptr;
  for (size_t n = 0; n < this->node_count_ && found == this->action_count_; n++) {
    const core::NodeDef &node = this->nodes_[n];
    if (node.action_count == 0 || std::strlen(node.id) != route.node_len ||
        std::strncmp(node.id, route.node, route.node_len) != 0)
      continue;
    for (uint16_t k = 0; k < node.action_count; k++) {
      const core::ActionDef &def = this->action_defs_[node.action_first + k];
      if (std::strlen(def.key) == route.key_len && std::strncmp(def.key, route.key, route.key_len) == 0) {
        found = node.action_first + k;
        found_node = &node;
        break;
      }
    }
  }
  if (found_node == nullptr) {
    this->send_status_(req, "404 Not Found");
    return;
  }

  const core::ActionDef &def = this->action_defs_[found];
  const bool is_form = [&] {
    auto type = request->get_header("Content-Type");
    return type.has_value() && type.value().find("application/x-www-form-urlencoded") != std::string::npos;
  }();
  std::string form_text;
  bool form_has_value = false;
  if (is_form) {
    form_has_value = request->hasArg("value");
    if (form_has_value)
      form_text = request->arg("value");
  }

  if (this->mutex_ == nullptr || xSemaphoreTake(this->mutex_, pdMS_TO_TICKS(200)) != pdTRUE) {
    this->send_status_(req, "503 Service Unavailable");
    return;
  }
  auto &rt = this->action_rt_[found];
  const core::ActionSample &sample = this->action_samples_[found];
  const bool busy = rt.pending;
  core::ActionOutcome outcome =
      is_form
          ? core::evaluate_action_text(def, sample, busy, form_has_value, form_text.c_str(), request->contentLength())
          : core::evaluate_action_json(def, sample, busy, this->body_req_ == request ? this->body_buf_ : "",
                                       this->body_req_ == request ? this->body_total_ : request->contentLength());

  if (outcome.verdict == core::Verdict::BUSY) {
    xSemaphoreGive(this->mutex_);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
    return;
  }

  rt.result.state = outcome.verdict == core::Verdict::ACCEPT ? core::ActionState::RUNNING : core::ActionState::REFUSED;
  rt.result.code = outcome.code;
  rt.result.value = outcome.value;
  rt.result_ms = millis();
  if (outcome.verdict == core::Verdict::ACCEPT) {
    rt.pending = true;
    rt.pending_value = outcome.value;
  }
  this->action_samples_[found].result = rt.result;
  this->update_seq_locked_();
  const uint32_t seq = this->seq_;
  core::ActionResult result = rt.result;
  result.age_ms = 0;
  xSemaphoreGive(this->mutex_);

  const char *status = "400 Bad Request";
  switch (outcome.http) {
    case 202:
      status = "202 Accepted";
      break;
    case 409:
      status = "409 Conflict";
      break;
    case 422:
      status = "422 Unprocessable Entity";
      break;
    default:
      break;
  }
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  core::ChunkWriter writer;
  writer.init(this->chunk_buf_, sizeof(this->chunk_buf_), &Portal::flush_chunk_, req);
  if (!core::detail::write_action_response(writer, seq, found_node->id, def.key, result, def.decimals) ||
      !writer.finish())
    return;
  httpd_resp_send_chunk(req, nullptr, 0);
  this->log_stack_("action");
}

}  // namespace esphome::portal
