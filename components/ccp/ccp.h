// ESPHome CCP 2.1 master hub and YAML automation actions over can_gateway.
#pragma once

#include <functional>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "../can_gateway/can_gateway.h"
#include "ccp_proto.h"

namespace esphome::ccp {

class CcpHub : public Component, public can_gateway::CanGatewayFrameConsumer {
 public:
  explicit CcpHub(can_gateway::GatewayPort *port) : port_(port) {}
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void set_command_id(uint32_t id) { command_id_ = id; }
  void set_response_id(uint32_t id) { response_id_ = id; }
  void set_station_address(uint16_t address) { station_address_ = address; }
  void set_byte_order(ByteOrder order) { byte_order_ = order; }
  void set_response_timeout(uint32_t timeout) { response_timeout_ = timeout; }
  void set_allow_write(bool allow) { allow_write_ = allow; }
  void set_enabled(bool enabled) { enabled_ = enabled; }
  bool is_connected() const { return connected_; }
  bool is_enabled() const { return enabled_; }

  bool connect(uint16_t station = 0);
  bool disconnect(bool end = false);
  bool get_ccp_version(uint8_t desired_main = 2, uint8_t desired_release = 1);
  bool exchange_id();
  bool get_seed(uint8_t resource);
  bool unlock(const uint8_t *key, uint8_t len);
  bool set_mta(uint8_t mta, uint8_t ext, uint32_t address);
  bool dnload(const uint8_t *data, uint8_t len);
  bool dnload6(const uint8_t *data);
  bool upload(uint8_t size);
  bool short_up(uint8_t size, uint8_t ext, uint32_t address);
  bool select_cal_page();
  bool get_active_cal_page();
  bool get_daq_size(uint8_t list, uint32_t dto_id);
  bool set_daq_ptr(uint8_t list, uint8_t odt, uint8_t element);
  bool write_daq(uint8_t size, uint8_t ext, uint32_t address);
  bool start_stop(uint8_t mode, uint8_t list, uint8_t last_odt, uint8_t event, uint16_t rate);
  bool start_stop_all(uint8_t mode);
  bool set_s_status(uint8_t status);
  bool get_s_status();
  bool build_checksum(uint32_t size);
  bool move(uint32_t size);
  bool test(uint16_t station = 0);
  bool clear_memory();
  bool program();
  bool program6(const uint8_t *data);
  bool diag_service(const uint8_t *data, uint8_t len);
  bool action_service(const uint8_t *data, uint8_t len);
  bool raw(const uint8_t *cro, uint8_t len);
  bool read_memory(uint8_t ext, uint32_t address, uint16_t len, std::function<void(std::vector<uint8_t>)> callback);
  bool write_memory(uint8_t ext, uint32_t address, const std::vector<uint8_t> &data,
                    std::function<void(bool)> callback = {});
  bool connect_and(std::function<void(bool)> callback);
  void on_frame(const can_gateway::FrameView &frame) override;

  template<typename F> void add_on_connected_callback(F &&callback) {
    connected_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_response_callback(F &&callback) {
    response_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_error_callback(F &&callback) { error_callback_.add(std::forward<F>(callback)); }
  template<typename F> void add_on_daq_callback(F &&callback) { daq_callback_.add(std::forward<F>(callback)); }

 protected:
  using CommandCallback = std::function<void(bool, uint8_t, const uint8_t *, uint8_t)>;
  bool send_(Cro cro, CommandCallback callback = {});
  bool write_allowed_(uint8_t command) const;
  void finish_(bool success, uint8_t code, const uint8_t *data, uint8_t len);
  void fail_sequence_();
  void continue_read_(bool success, const uint8_t *data, uint8_t len);
  void continue_write_(bool success);
  can_gateway::GatewayPort *port_;
  uint32_t command_id_{0x700};
  uint32_t response_id_{0x701};
  uint16_t station_address_{0x0001};
  ByteOrder byte_order_{ByteOrder::LITTLE};
  uint32_t response_timeout_{100};
  bool allow_write_{false};
  bool enabled_{true};
  bool connected_{false};
  bool in_flight_{false};
  uint8_t ctr_{0};
  uint8_t in_flight_ctr_{0};
  uint8_t in_flight_command_{0};
  uint32_t deadline_{0};
  CommandCallback command_callback_{};
  std::function<void(std::vector<uint8_t>)> read_callback_{};
  std::vector<uint8_t> read_data_{};
  uint16_t read_remaining_{0};
  std::vector<uint8_t> write_data_{};
  size_t write_offset_{0};
  std::function<void(bool)> write_callback_{};
  std::function<void(bool)> connect_callback_{};
  LazyCallbackManager<void()> connected_callback_;
  LazyCallbackManager<void(uint8_t, uint8_t, std::vector<uint8_t>)> response_callback_;
  LazyCallbackManager<void(uint8_t, uint8_t)> error_callback_;
  LazyCallbackManager<void(uint8_t, std::vector<uint8_t>)> daq_callback_;
};

template<typename... Ts> class CcpConnectAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  void play(const Ts &...) override { this->parent_->connect(); }
};
template<typename... Ts> class CcpDisconnectAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(bool, end) void play(const Ts &...x) override { this->parent_->disconnect(this->end_.value(x...)); }
};
template<typename... Ts> class CcpGetVersionAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  void play(const Ts &...) override { this->parent_->get_ccp_version(); }
};
template<typename... Ts> class CcpExchangeIdAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  void play(const Ts &...) override { this->parent_->exchange_id(); }
};
template<typename... Ts> class CcpSetMtaAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(uint8_t, mta)
  TEMPLATABLE_VALUE(uint8_t, ext) TEMPLATABLE_VALUE(uint32_t, address) void play(const Ts &...x) override {
    this->parent_->set_mta(this->mta_.value(x...), this->ext_.value(x...), this->address_.value(x...));
  }
};
template<typename... Ts> class CcpUploadAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(uint8_t, size) void play(const Ts &...x) override {
    this->parent_->upload(this->size_.value(x...));
  }
};
template<typename... Ts> class CcpShortUploadAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(uint8_t, size)
  TEMPLATABLE_VALUE(uint8_t, ext) TEMPLATABLE_VALUE(uint32_t, address) void play(const Ts &...x) override {
    this->parent_->short_up(this->size_.value(x...), this->ext_.value(x...), this->address_.value(x...));
  }
};
template<typename... Ts> class CcpDownloadAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  void set_data_static(const std::vector<uint8_t> &data) { data_ = data; }
  void set_data_template(std::function<std::vector<uint8_t>(Ts...)> func) {
    func_ = std::move(func);
    templated_ = true;
  }
  void play(const Ts &...x) override {
    const auto data = templated_ ? func_(x...) : data_;
    this->parent_->dnload(data.data(), data.size());
  }

 protected:
  bool templated_{false};
  std::function<std::vector<uint8_t>(Ts...)> func_{};
  std::vector<uint8_t> data_{};
};
template<typename... Ts> class CcpSelectCalPageAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  void play(const Ts &...) override { this->parent_->select_cal_page(); }
};
template<typename... Ts> class CcpReadMemoryAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(uint8_t, ext)
  TEMPLATABLE_VALUE(uint32_t, address) TEMPLATABLE_VALUE(uint16_t, length) void play(const Ts &...x) override {
    this->parent_->read_memory(this->ext_.value(x...), this->address_.value(x...), this->length_.value(x...), {});
  }
};
template<typename... Ts> class CcpWriteMemoryAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(uint8_t, ext)
  TEMPLATABLE_VALUE(uint32_t, address) void set_data_static(const std::vector<uint8_t> &data) { data_ = data; }
  void set_data_template(std::function<std::vector<uint8_t>(Ts...)> func) {
    func_ = std::move(func);
    templated_ = true;
  }
  void play(const Ts &...x) override {
    this->parent_->write_memory(this->ext_.value(x...), this->address_.value(x...), templated_ ? func_(x...) : data_);
  }

 protected:
  bool templated_{false};
  std::function<std::vector<uint8_t>(Ts...)> func_{};
  std::vector<uint8_t> data_{};
};
template<typename... Ts> class CcpDaqAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(bool, start) void play(const Ts &...x) override {
    this->parent_->start_stop_all(this->start_.value(x...) ? 1 : 0);
  }
};
template<typename... Ts> class CcpRawAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  void set_data_static(const std::vector<uint8_t> &data) { data_ = data; }
  void set_data_template(std::function<std::vector<uint8_t>(Ts...)> func) {
    func_ = std::move(func);
    templated_ = true;
  }
  void play(const Ts &...x) override {
    const auto data = templated_ ? func_(x...) : data_;
    this->parent_->raw(data.data(), data.size());
  }

 protected:
  bool templated_{false};
  std::function<std::vector<uint8_t>(Ts...)> func_{};
  std::vector<uint8_t> data_{};
};
template<typename... Ts> class CcpSetEnabledAction : public Action<Ts...>, public Parented<CcpHub> {
 public:
  TEMPLATABLE_VALUE(bool, enabled) void play(const Ts &...x) override {
    this->parent_->set_enabled(this->enabled_.value(x...));
  }
};

}  // namespace esphome::ccp
