#pragma once

#include <memory>

#include "diag_server_core.h"
#include "esphome/components/isotp/isotp.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

namespace esphome::diag_server {

class DiagServer : public Component, public isotp::IsoTpConsumer {
 public:
  DiagServer(isotp::IsoTpProtocol *transport, const char *name, const uint8_t *bytes, const Entry *entries,
             uint16_t entry_count, uint16_t reply_capacity, bool enabled, uint32_t s3_ms, uint32_t delay_ms,
             bool pending_78, uint32_t drop_every)
      : transport_(transport),
        name_(name),
        core_({bytes, entries, entry_count}, s3_ms),
        reply_capacity_(reply_capacity) {
    this->set_enabled(enabled);
    this->set_delay(delay_ms);
    this->set_pending_78(pending_78);
    this->set_drop_every(drop_every);
  }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void on_message(const isotp::MessageView &msg) override;
  void on_sent() override;

  void set_enabled(bool enabled) {
    if (!enabled)
      this->in_flight_ = {};
    this->core_.set_enabled(enabled);
    this->transport_->set_active(enabled);
  }
  bool enabled() const { return this->core_.enabled(); }
  void set_pending_78(bool pending) { this->core_.set_pending_78(pending); }
  bool pending_78() const { return this->core_.pending_78(); }
  void set_delay(uint32_t delay_ms) { this->core_.set_delay(delay_ms); }
  uint32_t delay() const { return this->core_.delay(); }
  void set_drop_every(uint32_t every) { this->core_.set_drop_every(every); }
  uint32_t drop_every() const { return this->core_.drop_every(); }
  const Counters &counters() const { return this->core_.counters(); }

 protected:
  void send_ready_(uint32_t now);
  void log_request_(const isotp::MessageView &msg, const Decision &decision) const;

  isotp::IsoTpProtocol *transport_;
  const char *name_;
  ServerCore core_;
  uint16_t reply_capacity_;
  std::unique_ptr<uint8_t[]> reply_buffer_;
  Reply in_flight_{};
};

#ifdef USE_SWITCH
class DiagServerSwitch : public switch_::Switch, public Component {
 public:
  DiagServerSwitch(DiagServer *parent, bool pending_fault) : parent_(parent), pending_fault_(pending_fault) {}
  void setup() override {
    this->publish_state(this->pending_fault_ ? this->parent_->pending_78() : this->parent_->enabled());
  }

 protected:
  void write_state(bool state) override {
    if (this->pending_fault_) {
      this->parent_->set_pending_78(state);
    } else {
      this->parent_->set_enabled(state);
    }
    this->publish_state(state);
  }

  DiagServer *parent_;
  bool pending_fault_;
};
#endif

#ifdef USE_NUMBER
class FaultNumber : public number::Number, public Component {
 public:
  FaultNumber(DiagServer *parent, bool delay) : parent_(parent), delay_(delay) {}
  void setup() override { this->publish_state(this->delay_ ? this->parent_->delay() : this->parent_->drop_every()); }

 protected:
  void control(float value) override;

  DiagServer *parent_;
  bool delay_;
};
#endif

#ifdef USE_SENSOR
class Diagnostics : public PollingComponent {
 public:
  explicit Diagnostics(DiagServer *parent) : parent_(parent) {}
  void set_requests_sensor(sensor::Sensor *entity) { this->requests_ = entity; }
  void set_replies_sensor(sensor::Sensor *entity) { this->replies_ = entity; }
  void set_misses_sensor(sensor::Sensor *entity) { this->misses_ = entity; }
  void update() override;

 protected:
  DiagServer *parent_;
  sensor::Sensor *requests_{nullptr};
  sensor::Sensor *replies_{nullptr};
  sensor::Sensor *misses_{nullptr};
};
#endif

}  // namespace esphome::diag_server
