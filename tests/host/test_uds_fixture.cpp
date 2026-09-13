// The C++ reader against a catalog the PYTHON compiler wrote.
//
// Every other uds host case reads a blob from uds_blob_builder.h — a second encoding of the format,
// which catches a reader that disagrees with the spec. What it cannot catch is the reader and the
// builder being wrong in the SAME way, and that is exactly what happened: both treated a group's
// `req_off` as pool-relative when §1 makes an "offset" absolute. 387 cases passed while a real bench
// ECU's compiled catalog resolved every request into the string pool instead — the catalog opened
// cleanly, names resolved, and then every single group was refused with "cannot assemble" because
// req[0] was an ASCII byte rather than the SID.
//
// The format doc always said the two implementations "meet on one checked-in artifact"
// (tests/uds/fixtures/mini.dcat). This file is that meeting. It is deliberately the *shipped*
// fixture and not a fresh compile, so it runs with no Python and no external database — pytest owns
// asserting the fixture is byte-identical to a fresh compile.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "harness.h"
#include "uds_catalog.h"
#include "uds_decode.h"
#include "uds_proto.h"

namespace {

using namespace esphome::uds;

/// The Makefile runs cases from tests/host, so the fixture is one directory up and over. Read as
/// bytes; a missing file fails loudly rather than skipping, because a silently skipped
/// cross-implementation check is worth nothing.
std::vector<uint8_t> load_fixture(const char *rel) {
  std::FILE *f = std::fopen(rel, "rb");
  if (f == nullptr)
    return {};
  std::vector<uint8_t> data;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    data.insert(data.end(), buf, buf + n);
  std::fclose(f);
  return data;
}

const char *const FIXTURE = "../uds/fixtures/mini.dcat";

/// Same tolerance idiom as test_uds_decode.cpp.
bool approx(float got, float want, float tol = 1e-3f) { return std::fabs(got - want) <= tol; }

TEST(uds_fixture_from_the_python_compiler_opens) {
  const std::vector<uint8_t> blob = load_fixture(FIXTURE);
  CHECK(!blob.empty());  // fixture missing => the check is not running; that is a failure
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  CHECK(cat.group_count() > 0);
  CHECK(cat.field_count() > 0);
}

// THE case that would have caught the bench failure. A group's request bytes must be the request,
// not whatever lies at that offset in another pool.
TEST(uds_fixture_request_bytes_are_the_request) {
  const std::vector<uint8_t> blob = load_fixture(FIXTURE);
  CHECK(!blob.empty());
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));

  bool seen_any = false;
  for (uint32_t i = 0; i < cat.group_count(); i++) {
    GroupView g;
    CHECK(cat.group(i, &g));
    const uint8_t *req = cat.request_bytes(g);
    CHECK(req != nullptr);
    CHECK(g.req_len >= 1);
    // The invariant build_request() enforces, and the one that failed on hardware.
    CHECK_EQ(static_cast<int>(req[0]), static_cast<int>(g.sid));
    // For a DID-style read the request must also carry the identifier the record names. Guarded on
    // did != NO_DID because the fixture deliberately holds a non-0x22 group (a 4-byte SID 0x31) to
    // exercise the SAFE_READ gate — asserting 0x22 for every group was this test over-reaching, not
    // the fixture being wrong.
    if (g.did != 0xFFFF) {
      CHECK_EQ(static_cast<int>(g.sid), 0x22);
      CHECK_EQ(static_cast<int>(g.req_len), 3);
      const uint16_t did_from_bytes = static_cast<uint16_t>(req[1] << 8 | req[2]);
      CHECK_EQ(static_cast<int>(did_from_bytes), static_cast<int>(g.did));
      seen_any = true;
    }
  }
  CHECK(seen_any);
}

TEST(uds_fixture_build_request_assembles_every_group) {
  const std::vector<uint8_t> blob = load_fixture(FIXTURE);
  CHECK(!blob.empty());
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  for (uint32_t i = 0; i < cat.group_count(); i++) {
    GroupView g;
    CHECK(cat.group(i, &g));
    uint8_t out[32] = {0};
    // 0 means refused, which is precisely what the whole bench run produced.
    CHECK_EQ(static_cast<int>(build_request(cat, g, out, sizeof(out))), static_cast<int>(g.req_len));
  }
}

TEST(uds_fixture_names_resolve_and_decode) {
  const std::vector<uint8_t> blob = load_fixture(FIXTURE);
  CHECK(!blob.empty());
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));

  uint32_t idx = 0;
  CHECK(cat.find("DT_Mini_Voltage_Maximum_Cell", NameKind::FIELD, &idx));
  FieldView f;
  CHECK(cat.field(idx, &f));
  CHECK_EQ(static_cast<int>(f.bit_pos), 24);  // first payload byte, header included (§5)
  CHECK(std::string(f.unit) == "V");

  // 62 01 01 <2200 big-endian> -> raw 2200, x0.001 + 1.5 = 3.700 V, the scale the factory
  // database declares for a cell voltage.
  const uint8_t resp[] = {0x62, 0x01, 0x01, 0x08, 0x98, 0x00, 0x00};
  const Decoded d = decode_numeric(cat, f, resp, sizeof(resp));
  CHECK(d.valid);
  CHECK(!d.is_text);
  CHECK(approx(d.value, 3.700f));

  // 0xFFFF is the row the compiler marked INVALID: not available, not a confident 67 V.
  const uint8_t sna[] = {0x62, 0x01, 0x01, 0xFF, 0xFF, 0x00, 0x00};
  const Decoded n = decode_numeric(cat, f, sna, sizeof(sna));
  CHECK(!n.valid);
}

// A group alias written by the Python compiler must resolve to its group — the §7 promise that
// cost the index four bytes per entry.
TEST(uds_fixture_group_is_addressable_by_name) {
  const std::vector<uint8_t> blob = load_fixture(FIXTURE);
  CHECK(!blob.empty());
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  uint32_t gi = 0;
  CHECK(cat.find("DT_Mini_Voltage", NameKind::GROUP, &gi));
  GroupView g;
  CHECK(cat.group(gi, &g));
  CHECK_EQ(static_cast<int>(g.did), 0x0101);
  const uint8_t *req = cat.request_bytes(g);
  CHECK(req != nullptr);
  CHECK_EQ(static_cast<int>(req[1]), 0x01);
  CHECK_EQ(static_cast<int>(req[2]), 0x01);
}

}  // namespace
