// Request assembly and response classification (components/uds/uds_proto.h) against
// docs/DESIGN-uds.md §4: the SID echo, the byte-for-byte identifier echo (the stale-answer
// trap), the PARTIAL/TOO_SHORT split around resp_min_len, and every NRC row of the reaction table.

#include "uds_blob_builder.h"
#include "uds_proto.h"
#include "harness.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::uds;
using udstest::BlobBuilder;

namespace {

const uint8_t REQ[] = {0x22, 0x02, 0x07};
constexpr uint16_t RESP_MIN_LEN = 9;

std::string hex(uint8_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0x%02X", v);
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Request assembly
// ---------------------------------------------------------------------------

TEST(uds_proto_build_request_from_group) {
  BlobBuilder b;
  b.add_group({"DT_Demo_Voltage", {0x22, 0x02, 0x07}, GROUP_FLAG_SAFE_READ, 0, 3, 0x0207, RESP_MIN_LEN});
  const std::vector<uint8_t> blob = b.build();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  GroupView g;
  CHECK(cat.group(0, &g));
  uint8_t out[8] = {0};
  CHECK_EQ(build_request(cat, g, out, sizeof(out)), 3);
  CHECK_BYTES(out, REQ, 3);
  // A buffer too small gets nothing, not a truncated request.
  CHECK_EQ(build_request(cat, g, out, 2), 0);
  // A group whose sid does not match its first request byte is a compiler bug the format rules
  // out (§4) — refused here rather than judged with the wrong SID later.
  GroupView lying = g;
  lying.sid = 0x23;
  CHECK_EQ(build_request(cat, lying, out, sizeof(out)), 0);
  // A pool reference that does not fit the pool is refused by the catalog underneath.
  GroupView oob = g;
  oob.req_off = 0xFFFF0000u;
  CHECK_EQ(build_request(cat, oob, out, sizeof(out)), 0);
}

// ---------------------------------------------------------------------------
// Positive response validation
// ---------------------------------------------------------------------------

TEST(uds_proto_accepts_a_good_response) {
  const uint8_t resp[] = {0x62, 0x02, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const ResponseCheck r = classify_response(REQ, sizeof(REQ), resp, sizeof(resp), RESP_MIN_LEN);
  CHECK_EQ(r.verdict, RespVerdict::ACCEPT);
  CHECK_EQ(r.nrc_class, NrcClass::NONE);
  CHECK_EQ(r.nrc, 0);
}

TEST(uds_proto_rejects_wrong_sid) {
  // The echo of some other service: not ours, however plausible the rest looks.
  const uint8_t resp[] = {0x61, 0x02, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), resp, sizeof(resp), RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
  const uint8_t junk[] = {0x00, 0x02, 0x07};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), junk, sizeof(junk), RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
}

TEST(uds_proto_rejects_stale_identifier_echo) {
  // The stale-answer trap: right SID, wrong DID. On a shared bus the late answer to the
  // *previous* request looks exactly like this, and accepting it would decode field values from
  // another group's response.
  const uint8_t stale[] = {0x62, 0x02, 0x03, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), stale, sizeof(stale), RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
  const uint8_t half[] = {0x62, 0x03, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), half, sizeof(half), RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
}

TEST(uds_proto_short_but_valid_response_is_partial) {
  // Shorter than resp_min_len but echo-verified: PARTIAL, and that is a SUCCESS. The covered
  // fields decode and the uncovered ones are skipped and counted.
  //
  // This is the 0x0208 case, and it is why resp_min_len cannot be a gate: a real factory database
  // can declare more array elements than a particular unit actually has wired, so a healthy ECU's
  // response legitimately falls short of resp_min_len. Rejecting on length reads every element as
  // unavailable off a perfectly healthy unit.
  const uint8_t shorter[] = {0x62, 0x02, 0x07, 0x11};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), shorter, sizeof(shorter), RESP_MIN_LEN).verdict, RespVerdict::PARTIAL);
  // One byte past the echo is already partial rather than rejected: there is a field's worth of
  // data there or there is not, and the per-field bounds check in extract_raw() decides.
  const uint8_t minimal[] = {0x62, 0x02, 0x07};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), minimal, sizeof(minimal), RESP_MIN_LEN).verdict, RespVerdict::PARTIAL);
  // Exactly resp_min_len covers every field, so the fast path applies.
  const uint8_t exact[] = {0x62, 0x02, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), exact, RESP_MIN_LEN, RESP_MIN_LEN).verdict, RespVerdict::ACCEPT);
  // A longer response than declared is still ACCEPT, not an anomaly.
  const uint8_t longer[] = {0x62, 0x02, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), longer, sizeof(longer), RESP_MIN_LEN).verdict, RespVerdict::ACCEPT);
  // A raw exchange declares no fields, so nothing is ever partial.
  CHECK_EQ(classify_response(REQ, sizeof(REQ), minimal, sizeof(minimal), 0).verdict, RespVerdict::ACCEPT);
}

TEST(uds_proto_rejects_a_response_too_short_to_verify) {
  // One byte short of `1 + echo_len` is the ONLY length that rejects: our SID, but no echo to
  // check it against, so a stale answer to the previous request would be indistinguishable.
  const uint8_t stub[] = {0x62, 0x02};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), stub, sizeof(stub), RESP_MIN_LEN).verdict, RespVerdict::TOO_SHORT);
  const uint8_t sid_only[] = {0x62};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), sid_only, sizeof(sid_only), RESP_MIN_LEN).verdict,
           RespVerdict::TOO_SHORT);
  // A runt frame carrying somebody else's service is still provably foreign: the SID is checkable
  // from one byte, and reporting NOT_OURS as TOO_SHORT would discard what we do know.
  const uint8_t foreign_runt[] = {0x61, 0x02};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), foreign_runt, sizeof(foreign_runt), RESP_MIN_LEN).verdict,
           RespVerdict::NOT_OURS);
  // A service with no echo rule needs only its SID, so nothing is ever too short to verify.
  const uint8_t reset_req[] = {0x11, 0x01};
  const uint8_t reset_resp[] = {0x51};
  CHECK_EQ(classify_response(reset_req, sizeof(reset_req), reset_resp, sizeof(reset_resp), 0).verdict,
           RespVerdict::ACCEPT);
}

TEST(uds_proto_rejects_empty_and_null_inputs) {
  const uint8_t resp[] = {0x62, 0x02, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  CHECK_EQ(classify_response(nullptr, 3, resp, sizeof(resp), RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
  CHECK_EQ(classify_response(REQ, 0, resp, sizeof(resp), RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
  CHECK_EQ(classify_response(REQ, sizeof(REQ), nullptr, 9, RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
  CHECK_EQ(classify_response(REQ, sizeof(REQ), resp, 0, RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
}

// ---------------------------------------------------------------------------
// Negative responses — every row of the design §4 reaction table
// ---------------------------------------------------------------------------

TEST(uds_proto_classifies_every_nrc_row) {
  struct Row {
    uint8_t nrc;
    NrcClass expected;
  };
  const Row rows[] = {
      {0x78, NrcClass::PENDING},      // response pending: extend to p2_ext, keep waiting
      {0x21, NrcClass::BUSY},         // busy, repeat request
      {0x22, NrcClass::BUSY},         // conditions not correct
      {0x31, NrcClass::UNSUPPORTED},  // request out of range: permanent, stop polling
      {0x33, NrcClass::DENIED},       // security access denied
      {0x35, NrcClass::DENIED},       // invalid key
      {0x10, NrcClass::OTHER},        // general reject
      {0x11, NrcClass::OTHER},        // service not supported
      {0x13, NrcClass::OTHER},        // incorrect length
      {0x7F, NrcClass::OTHER},
  };
  for (const Row &row : rows) {
    const uint8_t resp[] = {0x7F, 0x22, row.nrc};
    const ResponseCheck r = classify_response(REQ, sizeof(REQ), resp, sizeof(resp), RESP_MIN_LEN);
    CHECK_EQ_MSG(r.verdict, RespVerdict::NEGATIVE, "NRC " + hex(row.nrc));
    CHECK_EQ_MSG(r.nrc_class, row.expected, "NRC " + hex(row.nrc));
    CHECK_EQ_MSG(r.nrc, row.nrc, "NRC " + hex(row.nrc));
  }
}

TEST(uds_proto_negative_for_another_sid_is_not_ours) {
  // A 7F echoing a different SID answers someone else's request; reacting to it would back off
  // or suspend a group that was never asked.
  const uint8_t other[] = {0x7F, 0x10, 0x78};
  const ResponseCheck r = classify_response(REQ, sizeof(REQ), other, sizeof(other), RESP_MIN_LEN);
  CHECK_EQ(r.verdict, RespVerdict::NOT_OURS);
  CHECK_EQ(r.nrc_class, NrcClass::NONE);
}

TEST(uds_proto_malformed_negative_is_too_short) {
  // This case asserted NOT_OURS until the PARTIAL/TOO_SHORT split, and the change is deliberate: a
  // `7F` with no NRC is not a foreign answer, it is an unreadable one. Keeping one rule for "too
  // short to interpret" whatever the polarity is what makes the counters mean something — a runt
  // negative lands beside a runt positive rather than beside a stale answer from another tester.
  const uint8_t stub[] = {0x7F, 0x22};  // our SID echoed, but no NRC byte to classify
  CHECK_EQ(classify_response(REQ, sizeof(REQ), stub, sizeof(stub), RESP_MIN_LEN).verdict, RespVerdict::TOO_SHORT);
  const uint8_t bare[] = {0x7F};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), bare, sizeof(bare), RESP_MIN_LEN).verdict, RespVerdict::TOO_SHORT);
  // A `7F` long enough to read but echoing another service stays NOT_OURS: that we can prove.
  const uint8_t foreign[] = {0x7F, 0x21, 0x31};
  CHECK_EQ(classify_response(REQ, sizeof(REQ), foreign, sizeof(foreign), RESP_MIN_LEN).verdict, RespVerdict::NOT_OURS);
}

// ---------------------------------------------------------------------------
// Identifier echo lengths per service
// ---------------------------------------------------------------------------

TEST(uds_proto_echo_len_by_service) {
  CHECK_EQ(echo_len(0x22, 3), 2);  // ReadDataByIdentifier echoes the 16-bit DID
  CHECK_EQ(echo_len(0x1A, 2), 1);  // ReadEcuIdentification echoes its identifier
  CHECK_EQ(echo_len(0x21, 2), 1);  // ReadDataByLocalIdentifier echoes its local id
  CHECK_EQ(echo_len(0x19, 3), 1);  // ReadDTCInformation echoes the sub-function
  CHECK_EQ(echo_len(0x10, 2), 0);  // no echo rule: the SID check alone guards these
  // The echo can never claim bytes the request does not have.
  CHECK_EQ(echo_len(0x22, 2), 1);
  CHECK_EQ(echo_len(0x22, 1), 0);
  CHECK_EQ(echo_len(0x1A, 1), 0);
}

TEST(uds_proto_local_identifier_echo_guards_0x21) {
  const uint8_t req[] = {0x21, 0x07};
  const uint8_t good[] = {0x61, 0x07, 0x11, 0x22};
  CHECK_EQ(classify_response(req, sizeof(req), good, sizeof(good), 4).verdict, RespVerdict::ACCEPT);
  const uint8_t stale[] = {0x61, 0x03, 0x11, 0x22};
  CHECK_EQ(classify_response(req, sizeof(req), stale, sizeof(stale), 4).verdict, RespVerdict::NOT_OURS);
}
