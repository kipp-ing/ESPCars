// CCP 2.1 master implementation: one outstanding CRO, DTO response/DAQ routing, and memory helpers.
#include "ccp.h"

#include <algorithm>

namespace esphome::ccp {

static const char *const TAG = "ccp";

void CcpHub::setup() { this->port_->subscribe_consumer(this->response_id_, false, this); }

void CcpHub::loop() {
  if (this->in_flight_ && static_cast<int32_t>(millis() - this->deadline_) >= 0) {
    ESP_LOGW(TAG, "CCP 0x%03X response timeout for command 0x%02X", static_cast<unsigned>(this->response_id_),
             this->in_flight_command_);
    this->finish_(false, CCP_TIMEOUT, nullptr, 0);
  }
}

void CcpHub::dump_config() {
  ESP_LOGCONFIG(TAG, "CCP master:");
  ESP_LOGCONFIG(TAG, "  CRO: 0x%03X, DTO: 0x%03X, station: 0x%04X", static_cast<unsigned>(this->command_id_),
                static_cast<unsigned>(this->response_id_), this->station_address_);
  ESP_LOGCONFIG(TAG, "  Byte order: %s, response timeout: %u ms, writes: %s",
                this->byte_order_ == ByteOrder::LITTLE ? "little" : "big",
                static_cast<unsigned>(this->response_timeout_), this->allow_write_ ? "enabled" : "disabled");
}

bool CcpHub::write_allowed_(uint8_t command) const {
  if (!is_write_command(command))
    return true;
#ifdef USE_CCP_WRITE
  if (this->allow_write_)
    return true;
#endif
  ESP_LOGW(TAG, "CCP write command 0x%02X refused: allow_write is false (or this binary was built without CCP writes)",
           command);
  return false;
}

bool CcpHub::send_(Cro cro, CommandCallback callback) {
  if (!this->enabled_) {
    ESP_LOGW(TAG, "CCP command 0x%02X refused: component disabled", cro.data[0]);
    return false;
  }
  if (!this->write_allowed_(cro.data[0]))
    return false;
  if (this->in_flight_) {
    ESP_LOGW(TAG, "CCP command 0x%02X refused: command 0x%02X is still in flight", cro.data[0],
             this->in_flight_command_);
    return false;
  }
  const uint8_t ctr = this->ctr_++;
  cro.data[1] = ctr;
  if (!this->port_->inject(this->command_id_, false, false, cro.data, CCP_FRAME_LEN)) {
    ESP_LOGW(TAG, "CCP command 0x%02X refused: CAN port cannot transmit", cro.data[0]);
    return false;
  }
  this->in_flight_ = true;
  this->in_flight_ctr_ = ctr;
  this->in_flight_command_ = cro.data[0];
  this->deadline_ = millis() + this->response_timeout_;
  this->command_callback_ = std::move(callback);
  return true;
}

void CcpHub::finish_(bool success, uint8_t code, const uint8_t *data, uint8_t len) {
  const uint8_t command = this->in_flight_command_;
  CommandCallback callback = std::move(this->command_callback_);
  this->command_callback_ = {};
  this->in_flight_ = false;
  if (success) {
    std::vector<uint8_t> payload(data, data + len);
    this->response_callback_.call(command, code, payload);
    if (command == CONNECT) {
      this->connected_ = true;
      this->connected_callback_.call();
    }
    if (command == DISCONNECT)
      this->connected_ = false;
  } else {
    this->error_callback_.call(command, code);
  }
  if (callback)
    callback(success, code, data, len);
}

void CcpHub::on_frame(const can_gateway::FrameView &frame) {
  if (frame.extended || frame.rtr)
    return;
  const Dto dto = decode_dto(frame.data, frame.dlc);
  if (dto.kind == DtoKind::DAQ) {
    this->daq_callback_.call(dto.pid, std::vector<uint8_t>(dto.data, dto.data + dto.data_len));
    return;
  }
  if (dto.kind != DtoKind::RESPONSE || !this->in_flight_ || !matches_response(dto, this->in_flight_ctr_))
    return;
  this->finish_(dto.return_code == 0, dto.return_code, dto.data, dto.data_len);
}

bool CcpHub::connect(uint16_t station) {
  return this->send_(ccp::connect(0, station == 0 ? this->station_address_ : station, this->byte_order_));
}
bool CcpHub::disconnect(bool end) {
  return this->send_(ccp::disconnect(0, end, this->station_address_, this->byte_order_));
}
bool CcpHub::get_ccp_version(uint8_t main, uint8_t release) {
  Cro c = command(GET_CCP_VERSION, 0);
  c.data[2] = main;
  c.data[3] = release;
  return this->send_(c);
}
bool CcpHub::exchange_id() { return this->send_(command(EXCHANGE_ID, 0)); }
bool CcpHub::get_seed(uint8_t resource) {
  Cro c = command(GET_SEED, 0);
  c.data[2] = resource;
  return this->send_(c);
}
bool CcpHub::unlock(const uint8_t *key, uint8_t len) {
  if (len > 6)
    return false;
  Cro c = command(UNLOCK, 0);
  if (key != nullptr)
    std::memcpy(c.data + 2, key, len);
  return this->send_(c);
}
bool CcpHub::set_mta(uint8_t mta, uint8_t ext, uint32_t address) {
  return this->send_(ccp::set_mta(0, mta, ext, address, this->byte_order_));
}
bool CcpHub::dnload(const uint8_t *data, uint8_t len) {
  if (len == 0 || len > CCP_MAX_TRANSFER)
    return false;
  return this->send_(ccp::dnload(0, data, len));
}
bool CcpHub::dnload6(const uint8_t *data) { return data != nullptr && this->send_(ccp::dnload6(0, data)); }
bool CcpHub::upload(uint8_t size) { return size > 0 && size <= CCP_MAX_TRANSFER && this->send_(ccp::upload(0, size)); }
bool CcpHub::short_up(uint8_t size, uint8_t ext, uint32_t address) {
  return size > 0 && size <= CCP_MAX_TRANSFER && this->send_(ccp::short_up(0, size, ext, address, this->byte_order_));
}
bool CcpHub::select_cal_page() { return this->send_(command(SELECT_CAL_PAGE, 0)); }
bool CcpHub::get_active_cal_page() { return this->send_(command(GET_ACTIVE_CAL_PAGE, 0)); }
bool CcpHub::get_daq_size(uint8_t list, uint32_t dto_id) {
  Cro c = command(GET_DAQ_SIZE, 0);
  c.data[2] = list;
  put_u32(c.data + 4, dto_id, this->byte_order_);
  return this->send_(c);
}
bool CcpHub::set_daq_ptr(uint8_t list, uint8_t odt, uint8_t element) {
  Cro c = command(SET_DAQ_PTR, 0);
  c.data[2] = list;
  c.data[3] = odt;
  c.data[4] = element;
  return this->send_(c);
}
bool CcpHub::write_daq(uint8_t size, uint8_t ext, uint32_t address) {
  Cro c = command(WRITE_DAQ, 0);
  c.data[2] = size;
  c.data[3] = ext;
  put_u32(c.data + 4, address, this->byte_order_);
  return this->send_(c);
}
bool CcpHub::start_stop(uint8_t mode, uint8_t list, uint8_t last_odt, uint8_t event, uint16_t rate) {
  Cro c = command(START_STOP, 0);
  c.data[2] = mode;
  c.data[3] = list;
  c.data[4] = last_odt;
  c.data[5] = event;
  put_u16(c.data + 6, rate, ByteOrder::BIG);
  return this->send_(c);
}
bool CcpHub::start_stop_all(uint8_t mode) {
  Cro c = command(START_STOP_ALL, 0);
  c.data[2] = mode;
  return this->send_(c);
}
bool CcpHub::set_s_status(uint8_t status) {
  Cro c = command(SET_S_STATUS, 0);
  c.data[2] = status;
  return this->send_(c);
}
bool CcpHub::get_s_status() { return this->send_(command(GET_S_STATUS, 0)); }
bool CcpHub::build_checksum(uint32_t size) { return this->send_(sized_u32(BUILD_CHKSUM, 0, size, this->byte_order_)); }
bool CcpHub::move(uint32_t size) { return this->send_(sized_u32(MOVE, 0, size, this->byte_order_)); }
bool CcpHub::test(uint16_t station) {
  Cro c = command(TEST, 0);
  put_u16(c.data + 2, station == 0 ? this->station_address_ : station, this->byte_order_);
  return this->send_(c);
}
bool CcpHub::clear_memory() { return this->send_(command(CLEAR_MEMORY, 0)); }
bool CcpHub::program() { return this->send_(command(PROGRAM, 0)); }
bool CcpHub::program6(const uint8_t *data) {
  Cro c = command(PROGRAM_6, 0);
  if (data == nullptr)
    return false;
  std::memcpy(c.data + 2, data, 6);
  return this->send_(c);
}
bool CcpHub::diag_service(const uint8_t *data, uint8_t len) {
  if (len > 6)
    return false;
  Cro c = command(DIAG_SERVICE, 0);
  if (data != nullptr)
    std::memcpy(c.data + 2, data, len);
  return this->send_(c);
}
bool CcpHub::action_service(const uint8_t *data, uint8_t len) {
  if (len > 6)
    return false;
  Cro c = command(ACTION_SERVICE, 0);
  if (data != nullptr)
    std::memcpy(c.data + 2, data, len);
  return this->send_(c);
}
bool CcpHub::raw(const uint8_t *cro, uint8_t len) {
  if (cro == nullptr || len < 1 || len > CCP_FRAME_LEN)
    return false;
  Cro c{};
  std::memcpy(c.data, cro, len);
  return this->send_(c);
}

bool CcpHub::connect_and(std::function<void(bool)> callback) {
  if (this->connected_) {
    callback(true);
    return true;
  }
  this->connect_callback_ = std::move(callback);
  return this->send_(ccp::connect(0, this->station_address_, this->byte_order_),
                     [this](bool ok, uint8_t, const uint8_t *, uint8_t) {
                       auto callback = std::move(this->connect_callback_);
                       this->connect_callback_ = {};
                       if (callback)
                         callback(ok);
                     });
}

bool CcpHub::read_memory(uint8_t ext, uint32_t address, uint16_t len,
                         std::function<void(std::vector<uint8_t>)> callback) {
  if (len == 0 || this->in_flight_ || this->read_remaining_ != 0)
    return false;
  this->read_callback_ = std::move(callback);
  this->read_data_.clear();
  this->read_data_.reserve(len);
  this->read_remaining_ = len;
  return this->send_(ccp::set_mta(0, 0, ext, address, this->byte_order_),
                     [this](bool ok, uint8_t, const uint8_t *, uint8_t) {
                       if (!ok) {
                         this->fail_sequence_();
                         return;
                       }
                       const uint8_t n = read_chunk(this->read_remaining_);
                       this->send_(ccp::upload(0, n), [this](bool success, uint8_t, const uint8_t *data, uint8_t len) {
                         this->continue_read_(success, data, len);
                       });
                     });
}
void CcpHub::continue_read_(bool success, const uint8_t *data, uint8_t len) {
  if (!success || len < read_chunk(this->read_remaining_)) {
    this->fail_sequence_();
    return;
  }
  const uint8_t n = read_chunk(this->read_remaining_);
  this->read_data_.insert(this->read_data_.end(), data, data + n);
  this->read_remaining_ -= n;
  if (this->read_remaining_ == 0) {
    auto callback = std::move(this->read_callback_);
    this->read_callback_ = {};
    if (callback)
      callback(this->read_data_);
    this->read_data_.clear();
    return;
  }
  const uint8_t next = read_chunk(this->read_remaining_);
  this->send_(ccp::upload(0, next), [this](bool ok, uint8_t, const uint8_t *response, uint8_t response_len) {
    this->continue_read_(ok, response, response_len);
  });
}
bool CcpHub::write_memory(uint8_t ext, uint32_t address, const std::vector<uint8_t> &data,
                          std::function<void(bool)> callback) {
  if (data.empty() || this->in_flight_ || !this->write_allowed_(DNLOAD))
    return false;
  this->write_data_ = data;
  this->write_offset_ = 0;
  this->write_callback_ = std::move(callback);
  return this->send_(ccp::set_mta(0, 0, ext, address, this->byte_order_),
                     [this](bool ok, uint8_t, const uint8_t *, uint8_t) { this->continue_write_(ok); });
}
void CcpHub::continue_write_(bool success) {
  if (!success) {
    this->fail_sequence_();
    return;
  }
  if (this->write_offset_ == this->write_data_.size()) {
    auto callback = std::move(this->write_callback_);
    this->write_callback_ = {};
    this->write_data_.clear();
    if (callback)
      callback(true);
    return;
  }
  const size_t remaining = this->write_data_.size() - this->write_offset_;
  const uint8_t n = remaining >= 6 ? 6 : static_cast<uint8_t>(remaining);
  Cro c = n == 6 ? ccp::dnload6(0, this->write_data_.data() + this->write_offset_)
                 : ccp::dnload(0, this->write_data_.data() + this->write_offset_, n);
  this->write_offset_ += n;
  this->send_(c, [this](bool ok, uint8_t, const uint8_t *, uint8_t) { this->continue_write_(ok); });
}
void CcpHub::fail_sequence_() {
  if (this->read_remaining_ != 0) {
    this->read_remaining_ = 0;
    this->read_data_.clear();
    this->read_callback_ = {};
  }
  if (!this->write_data_.empty()) {
    this->write_data_.clear();
    this->write_offset_ = 0;
    auto callback = std::move(this->write_callback_);
    this->write_callback_ = {};
    if (callback)
      callback(false);
  }
}

}  // namespace esphome::ccp
