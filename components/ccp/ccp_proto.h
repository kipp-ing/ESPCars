// CCP 2.1 single-frame CRO/DTO codec, intentionally independent of ESPHome for host tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::ccp {

constexpr uint8_t CCP_FRAME_LEN = 8;
constexpr uint8_t CCP_RESPONSE_MARKER = 0xFF;
constexpr uint8_t CCP_TIMEOUT = 0xFE;
constexpr uint8_t CCP_MAX_TRANSFER = 5;
constexpr uint8_t CCP_ODT_PAYLOAD = 7;

enum class ByteOrder : uint8_t { LITTLE, BIG };
enum Command : uint8_t {
  CONNECT = 0x01,
  SET_MTA = 0x02,
  DNLOAD = 0x03,
  UPLOAD = 0x04,
  TEST = 0x05,
  START_STOP = 0x06,
  DISCONNECT = 0x07,
  START_STOP_ALL = 0x08,
  GET_ACTIVE_CAL_PAGE = 0x09,
  SET_S_STATUS = 0x0C,
  GET_S_STATUS = 0x0D,
  BUILD_CHKSUM = 0x0E,
  SHORT_UP = 0x0F,
  CLEAR_MEMORY = 0x10,
  SELECT_CAL_PAGE = 0x11,
  GET_SEED = 0x12,
  UNLOCK = 0x13,
  GET_DAQ_SIZE = 0x14,
  SET_DAQ_PTR = 0x15,
  WRITE_DAQ = 0x16,
  EXCHANGE_ID = 0x17,
  PROGRAM = 0x18,
  MOVE = 0x19,
  GET_CCP_VERSION = 0x1B,
  DIAG_SERVICE = 0x20,
  ACTION_SERVICE = 0x21,
  PROGRAM_6 = 0x22,
  DNLOAD_6 = 0x23,
};

struct Cro {
  uint8_t data[CCP_FRAME_LEN]{};
};

struct DaqField {
  uint8_t size;
  uint8_t ext;
  uint32_t address;
};

inline void put_u16(uint8_t *out, uint16_t value, ByteOrder order) {
  if (order == ByteOrder::LITTLE) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
  } else {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
  }
}
inline void put_u32(uint8_t *out, uint32_t value, ByteOrder order) {
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t shift = order == ByteOrder::LITTLE ? i : static_cast<uint8_t>(3 - i);
    out[i] = static_cast<uint8_t>(value >> (8 * shift));
  }
}
inline uint32_t get_u32(const uint8_t *in, ByteOrder order) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t shift = order == ByteOrder::LITTLE ? i : static_cast<uint8_t>(3 - i);
    value |= static_cast<uint32_t>(in[i]) << (8 * shift);
  }
  return value;
}

inline Cro command(uint8_t code, uint8_t ctr) {
  Cro out{};
  out.data[0] = code;
  out.data[1] = ctr;
  return out;
}
inline Cro connect(uint8_t ctr, uint16_t station, ByteOrder order) {
  Cro out = command(CONNECT, ctr);
  put_u16(out.data + 2, station, order);
  return out;
}
inline Cro disconnect(uint8_t ctr, bool end, uint16_t station, ByteOrder order) {
  Cro out = command(DISCONNECT, ctr);
  out.data[2] = end ? 1 : 0;
  put_u16(out.data + 4, station, order);
  return out;
}
inline Cro set_mta(uint8_t ctr, uint8_t mta, uint8_t ext, uint32_t address, ByteOrder order) {
  Cro out = command(SET_MTA, ctr);
  out.data[2] = mta;
  out.data[3] = ext;
  put_u32(out.data + 4, address, order);
  return out;
}
inline Cro upload(uint8_t ctr, uint8_t size) {
  Cro out = command(UPLOAD, ctr);
  out.data[2] = size;
  return out;
}
inline Cro short_up(uint8_t ctr, uint8_t size, uint8_t ext, uint32_t address, ByteOrder order) {
  Cro out = command(SHORT_UP, ctr);
  out.data[2] = size;
  out.data[3] = ext;
  put_u32(out.data + 4, address, order);
  return out;
}
inline Cro get_daq_size(uint8_t ctr, uint8_t list, uint32_t dto_id, ByteOrder order) {
  Cro out = command(GET_DAQ_SIZE, ctr);
  out.data[2] = list;
  put_u32(out.data + 4, dto_id, order);
  return out;
}
inline Cro set_daq_ptr(uint8_t ctr, uint8_t list, uint8_t odt, uint8_t element) {
  Cro out = command(SET_DAQ_PTR, ctr);
  out.data[2] = list;
  out.data[3] = odt;
  out.data[4] = element;
  return out;
}
inline Cro write_daq(uint8_t ctr, uint8_t size, uint8_t ext, uint32_t address, ByteOrder order) {
  Cro out = command(WRITE_DAQ, ctr);
  out.data[2] = size;
  out.data[3] = ext;
  put_u32(out.data + 4, address, order);
  return out;
}
inline Cro start_stop(uint8_t ctr, uint8_t mode, uint8_t list, uint8_t last_odt, uint8_t event, uint16_t rate,
                      ByteOrder order) {
  Cro out = command(START_STOP, ctr);
  out.data[2] = mode;
  out.data[3] = list;
  out.data[4] = last_odt;
  out.data[5] = event;
  put_u16(out.data + 6, rate, order);
  return out;
}
inline Cro start_stop_all(uint8_t ctr, uint8_t mode) {
  Cro out = command(START_STOP_ALL, ctr);
  out.data[2] = mode;
  return out;
}
inline Cro set_s_status(uint8_t ctr, uint8_t status) {
  Cro out = command(SET_S_STATUS, ctr);
  out.data[2] = status;
  return out;
}
inline Cro dnload(uint8_t ctr, const uint8_t *data, uint8_t size) {
  Cro out = command(DNLOAD, ctr);
  out.data[2] = size;
  if (data != nullptr && size <= CCP_MAX_TRANSFER)
    std::memcpy(out.data + 3, data, size);
  return out;
}
inline Cro dnload6(uint8_t ctr, const uint8_t *data) {
  Cro out = command(DNLOAD_6, ctr);
  if (data != nullptr)
    std::memcpy(out.data + 2, data, 6);
  return out;
}
inline Cro sized_u32(uint8_t code, uint8_t ctr, uint32_t size, ByteOrder order) {
  Cro out = command(code, ctr);
  put_u32(out.data + 2, size, order);
  return out;
}

enum class DtoKind : uint8_t { INVALID, RESPONSE, DAQ };
struct Dto {
  DtoKind kind{DtoKind::INVALID};
  uint8_t return_code{0};
  uint8_t ctr{0};
  uint8_t pid{0};
  const uint8_t *data{nullptr};
  uint8_t data_len{0};
};
inline Dto decode_dto(const uint8_t *data, uint8_t len) {
  Dto out{};
  if (data == nullptr || len == 0)
    return out;
  if (data[0] != CCP_RESPONSE_MARKER) {
    out.kind = DtoKind::DAQ;
    out.pid = data[0];
    out.data = data + 1;
    out.data_len = static_cast<uint8_t>(len - 1);
    return out;
  }
  if (len < 3)
    return out;
  out.kind = DtoKind::RESPONSE;
  out.return_code = data[1];
  out.ctr = data[2];
  out.data = data + 3;
  out.data_len = static_cast<uint8_t>(len - 3);
  return out;
}
inline bool matches_response(const Dto &dto, uint8_t ctr) { return dto.kind == DtoKind::RESPONSE && dto.ctr == ctr; }
inline bool is_write_command(uint8_t code) {
  return code == DNLOAD || code == DNLOAD_6 || code == WRITE_DAQ || code == MOVE || code == CLEAR_MEMORY ||
         code == PROGRAM || code == PROGRAM_6;
}
inline const char *return_code_name(uint8_t code) {
  switch (code) {
    case 0x00:
      return "acknowledge";
    case 0x01:
      return "daq processor overload";
    case 0x10:
      return "command processor busy";
    case 0x11:
      return "internal timeout";
    case 0x12:
      return "other";
    case 0x30:
      return "unknown command";
    case 0x31:
      return "command syntax";
    case 0x32:
      return "parameter out of range";
    case 0x33:
      return "access denied";
    case 0x34:
      return "overload";
    case 0x35:
      return "overload";
    default:
      return "unknown return code";
  }
}
inline uint8_t read_chunk(uint16_t remaining) {
  return remaining > CCP_MAX_TRANSFER ? CCP_MAX_TRANSFER : static_cast<uint8_t>(remaining);
}
inline uint16_t daq_total_size(const DaqField *fields, size_t count) {
  uint16_t total = 0;
  if (fields == nullptr)
    return total;
  for (size_t i = 0; i < count; i++)
    total = static_cast<uint16_t>(total + fields[i].size);
  return total;
}
inline bool daq_fits_one_odt(const DaqField *fields, size_t count) {
  return daq_total_size(fields, count) <= CCP_ODT_PAYLOAD;
}
inline uint8_t daq_field_offset(const DaqField *fields, size_t count, size_t index) {
  if (fields == nullptr || index >= count)
    return 0;
  uint8_t offset = 1;
  for (size_t i = 0; i < index; i++)
    offset = static_cast<uint8_t>(offset + fields[i].size);
  return offset;
}
inline bool daq_anchor_matches(const uint8_t *frame, uint8_t len, uint8_t offset, const uint8_t *expect,
                               uint8_t expect_len) {
  const size_t end = static_cast<size_t>(offset) + expect_len;
  if (frame == nullptr || expect == nullptr || end > len)
    return false;
  return std::memcmp(frame + offset, expect, expect_len) == 0;
}

}  // namespace esphome::ccp
