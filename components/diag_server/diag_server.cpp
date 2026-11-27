#include "diag_server.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::diag_server {

ESPHOME_LOG_TAG(TAG, "diag_server");

void DiagServer::setup() {
  this->reply_buffer_ = std::make_unique<uint8_t[]>(this->reply_capacity_);
  this->transport_->set_consumer(this);
}

void DiagServer::loop() { this->send_ready_(millis()); }

void DiagServer::on_message(const isotp::MessageView &msg) {
  const uint32_t now = millis();
  const auto decision =
      this->core_.on_request(now, msg.data, msg.size, this->transport_->is_busy(), this->reply_buffer_.get());
  this->log_request_(msg, decision);
  switch (decision.rule) {
    case Rule::BUSY:
      ESP_LOGW(TAG, "%s: dropped request while a previous reply was outstanding", this->name_);
      return;
    case Rule::DROP:
      ESP_LOGD(TAG, "%s: drop_every dropped request", this->name_);
      return;
    case Rule::OUT_OF_RANGE:
    case Rule::UNSUPPORTED:
    case Rule::WRONG_SESSION:
      ESP_LOGD(TAG, "%s: request missed for service 0x%02X", this->name_, msg.data[0]);
      break;
    default:
      break;
  }
  if (decision.reply_size == 0)
    return;
  if (decision.pending) {
    ESP_LOGD(TAG, "%s: response pending, final reply delayed %lu ms", this->name_,
             static_cast<unsigned long>(decision.delay_ms));
  } else if (decision.delay_ms != 0) {
    ESP_LOGD(TAG, "%s: reply delayed %lu ms", this->name_, static_cast<unsigned long>(decision.delay_ms));
  }
  this->send_ready_(now);
}

void DiagServer::on_sent() {
  if (this->in_flight_.size != 0) {
    this->core_.reply_sent(millis(), this->in_flight_);
    this->in_flight_ = {};
  }
}

void DiagServer::send_ready_(uint32_t now) {
  const bool busy = this->transport_->is_busy();
  if (!busy)
    this->in_flight_ = {};  // Discard tracking for a transfer that failed.
  uint8_t pending[3];
  const auto reply = this->core_.poll(now, busy, pending);
  if (reply.size == 0)
    return;
  if (this->transport_->send(reply.pending ? pending : this->reply_buffer_.get(), reply.size)) {
    this->in_flight_ = reply;
  } else {
    ESP_LOGW(TAG, "%s: ISO-TP refused reply", this->name_);
  }
}

void DiagServer::log_request_(const isotp::MessageView &msg, const Decision &decision) const {
#ifdef ESPHOME_LOG_HAS_VERBOSE
  // Fit the request and its result on one line with the default logger buffer.
  static constexpr uint16_t MAX_LOG_BYTES = 64;
  static const char *const RULE_NAMES[]{
      "ignored",  "busy",    "drop_every",    "entry",        "tester_present",
      "suppress", "session", "wrong_session", "out_of_range", "unsupported",
  };
  char hex[format_hex_size(MAX_LOG_BYTES)];
  const auto size = msg.size > MAX_LOG_BYTES ? MAX_LOG_BYTES : msg.size;
  format_hex_to(hex, msg.data, size);
  const char *suffix = size < msg.size ? "..." : "";
  const char *rule = RULE_NAMES[static_cast<uint8_t>(decision.rule)];
  if (decision.entry == NO_ENTRY) {
    ESP_LOGV(TAG, "%s: [%s%s] %s, reply %u bytes", this->name_, hex, suffix, rule, decision.reply_size);
  } else {
    ESP_LOGV(TAG, "%s: [%s%s] %s #%u, reply %u bytes", this->name_, hex, suffix, rule, decision.entry,
             decision.reply_size);
  }
#endif
}

#ifdef USE_NUMBER
void FaultNumber::control(float value) {
  const auto integer = static_cast<uint32_t>(std::round(value));
  if (this->delay_) {
    this->parent_->set_delay(integer);
  } else {
    this->parent_->set_drop_every(integer);
  }
  this->publish_state(integer);
}
#endif

#ifdef USE_SENSOR
void Diagnostics::update() {
  const auto &values = this->parent_->counters();
  if (this->requests_ != nullptr)
    this->requests_->publish_state(values.requests);
  if (this->replies_ != nullptr)
    this->replies_->publish_state(values.replies);
  if (this->misses_ != nullptr)
    this->misses_->publish_state(values.misses);
}
#endif

}  // namespace esphome::diag_server
