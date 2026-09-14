// CCP CRO/DTO protocol cases, including firmware-verified Intel address order and read chunk planning.
#include "../../components/ccp/ccp_proto.h"
#include "harness.h"

#include <cstdint>

using namespace esphome::ccp;

TEST(ccp_proto_connect_and_set_mta_are_little_endian) {
  const Cro connect_frame = connect(0x12, 0x1234, ByteOrder::LITTLE);
  const uint8_t expected_connect[] = {CONNECT, 0x12, 0x34, 0x12, 0, 0, 0, 0};
  CHECK_BYTES(connect_frame.data, expected_connect, CCP_FRAME_LEN);
  const Cro mta = set_mta(0x13, 0, 0, 0x10203040, ByteOrder::LITTLE);
  const uint8_t expected_mta[] = {SET_MTA, 0x13, 0, 0, 0x40, 0x30, 0x20, 0x10};
  CHECK_BYTES(mta.data, expected_mta, CCP_FRAME_LEN);
}

TEST(ccp_proto_transfer_encoders) {
  const Cro up = upload(2, 5);
  CHECK_EQ(up.data[0], UPLOAD);
  CHECK_EQ(up.data[1], 2);
  CHECK_EQ(up.data[2], 5);
  const Cro short_read = short_up(3, 2, 1, 0x10203040, ByteOrder::LITTLE);
  const uint8_t expected_short[] = {SHORT_UP, 3, 2, 1, 0x40, 0x30, 0x20, 0x10};
  CHECK_BYTES(short_read.data, expected_short, CCP_FRAME_LEN);
  const uint8_t write[] = {1, 2, 3, 4, 5};
  const Cro down = dnload(4, write, 5);
  const uint8_t expected_down[] = {DNLOAD, 4, 5, 1, 2, 3, 4, 5};
  CHECK_BYTES(down.data, expected_down, CCP_FRAME_LEN);
}

TEST(ccp_proto_start_stop_prescaler_uses_requested_byte_order) {
  const Cro little = start_stop(0x20, 1, 0, 0, 0, 10, ByteOrder::LITTLE);
  const uint8_t expected_little[] = {START_STOP, 0x20, 1, 0, 0, 0, 0x0A, 0x00};
  CHECK_BYTES(little.data, expected_little, CCP_FRAME_LEN);
  const Cro big = start_stop(0x21, 1, 0, 0, 0, 10, ByteOrder::BIG);
  const uint8_t expected_big[] = {START_STOP, 0x21, 1, 0, 0, 0, 0x00, 0x0A};
  CHECK_BYTES(big.data, expected_big, CCP_FRAME_LEN);
}

TEST(ccp_proto_decodes_response_matches_counter_and_routes_daq) {
  const uint8_t response[] = {0xFF, 0x00, 0x42, 1, 2, 3, 4, 5};
  const Dto dto = decode_dto(response, sizeof(response));
  CHECK_EQ(dto.kind, DtoKind::RESPONSE);
  CHECK_EQ(dto.return_code, 0);
  CHECK_EQ(dto.ctr, 0x42);
  CHECK_EQ(dto.data_len, 5);
  CHECK(matches_response(dto, 0x42));
  CHECK(!matches_response(dto, 0x43));
  const uint8_t daq[] = {0x01, 9, 8, 7};
  const Dto sample = decode_dto(daq, sizeof(daq));
  CHECK_EQ(sample.kind, DtoKind::DAQ);
  CHECK_EQ(sample.pid, 1);
  CHECK_EQ(sample.data_len, 3);
  const uint8_t runt[] = {0xFF, 0x00};
  CHECK_EQ(decode_dto(runt, sizeof(runt)).kind, DtoKind::INVALID);
}

TEST(ccp_proto_return_codes_and_write_classification) {
  CHECK(std::string(return_code_name(0x00)) == "acknowledge");
  CHECK(std::string(return_code_name(0x33)) == "access denied");
  CHECK(is_write_command(DNLOAD));
  CHECK(is_write_command(PROGRAM_6));
  CHECK(!is_write_command(UPLOAD));
}

TEST(ccp_proto_daq_layout_totals_fit_and_offsets) {
  const DaqField fields[] = {{4, 0, 0x10203050}, {1, 0, 0x10203060}, {2, 0, 0x10203070}};
  CHECK_EQ(CCP_ODT_PAYLOAD, 7);
  CHECK_EQ(daq_total_size(fields, 3), 7);
  CHECK(daq_fits_one_odt(fields, 3));
  CHECK_EQ(daq_field_offset(fields, 3, 0), 1);
  CHECK_EQ(daq_field_offset(fields, 3, 1), 5);
  CHECK_EQ(daq_field_offset(fields, 3, 2), 6);

  const DaqField no_anchor[] = {{1, 0, 0x10203060}, {2, 0, 0x10203070}};
  CHECK_EQ(daq_total_size(no_anchor, 2), 3);
  CHECK_EQ(daq_field_offset(no_anchor, 2, 0), 1);
  CHECK_EQ(daq_field_offset(no_anchor, 2, 1), 2);

  const DaqField too_large[] = {{4, 0, 1}, {4, 0, 2}};
  CHECK_EQ(daq_total_size(too_large, 2), 8);
  CHECK(!daq_fits_one_odt(too_large, 2));
}

TEST(ccp_proto_daq_anchor_comparison) {
  const uint8_t frame[] = {0x00, 0x01, 0xAD, 0x04, 0x02, 0xAA, 0x55, 0x66};
  const uint8_t expect[] = {0x01, 0xAD, 0x04, 0x02};
  const uint8_t wrong[] = {0x01, 0xAD, 0x04, 0x03};
  CHECK(daq_anchor_matches(frame, sizeof(frame), 1, expect, sizeof(expect)));
  CHECK(!daq_anchor_matches(frame, sizeof(frame), 1, wrong, sizeof(wrong)));
  CHECK(!daq_anchor_matches(frame, 4, 1, expect, sizeof(expect)));
}

TEST(ccp_proto_read_memory_chunks_twelve_bytes_as_five_five_two) {
  uint16_t remaining = 12;
  const uint8_t expected[] = {5, 5, 2};
  for (uint8_t n : expected) {
    CHECK_EQ(read_chunk(remaining), n);
    remaining -= n;
  }
  CHECK_EQ(remaining, 0);
}
