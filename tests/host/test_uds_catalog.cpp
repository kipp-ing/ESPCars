// Catalog reader (components/uds/uds_catalog.h) against docs/uds-catalog-format.md.
//
// The blobs come from uds_blob_builder.h — an independent encoding of the same spec — so every
// expectation here is "what the spec says the bytes mean", not a replay of the reader. The two
// load-bearing suites:
//   - truncation fuzz: a valid blob cut at EVERY length from 0 to full, each copy in a heap
//     buffer of exactly that length, so any read past the end is an ASan report and
//     -fno-sanitize-recover means it cannot exit 0;
//   - name index: the FNV-1a collision pair "costarring"/"liquid" (0x5E4DAA9D) proves the
//     mandatory strcmp — a hash hit that is not a string match must be a miss, never the wrong
//     record.

#include "uds_blob_builder.h"
#include "uds_catalog.h"
#include "harness.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::uds;
using udstest::BlobBuilder;
using udstest::fix_crc;
using udstest::store_u16;
using udstest::store_u32;

namespace {

/// A small but structurally complete catalog: 1 ECU, 2 groups (one the three-fields-one-request
/// case the format exists for), 4 fields, 5 scale rows, and a group alias entry.
std::vector<uint8_t> make_mini_blob() {
  BlobBuilder b;
  b.add_ecu({"DEMO", 0x7E7, 0x7EF, 0, 8, 0, 500, 5000, 0, 2});
  b.add_group({"DT_Demo_Voltage", {0x22, 0x02, 0x07}, GROUP_FLAG_SAFE_READ, 0, 3, 0x0207, 9});
  b.add_group({"DT_Demo_Contactor", {0x22, 0xD0, 0x00}, GROUP_FLAG_SAFE_READ, 3, 1, 0xD000, 4});
  b.add_field({"PRES_Demo_Voltage_Averaged_2Byte", "V", 24, 16, 0, 0, 2, 1, 0, 0});
  b.add_field({"PRES_Demo_Voltage_Maximum_2Byte", "V", 40, 16, 0, 0, 2, 1, 0, 0});
  b.add_field({"PRES_Demo_Voltage_Minimum_2Byte", "V", 56, 16, 0, 0, 2, 1, 0, 0});
  b.add_field({"PRES_Demo_Contactor_State", "", 24, 8, FIELD_FLAG_ENUM, 2, 3, 1, 0, 1});
  b.add_scale({0, 64999, 0.001f, 0.0f, "", 0});
  b.add_scale({65535, 65535, 0.0f, 0.0f, "SNA", SCALE_FLAG_TEXT | SCALE_FLAG_INVALID});
  b.add_scale({0, 0, 1.0f, 0.0f, "OPEN", SCALE_FLAG_TEXT});
  b.add_scale({1, 1, 1.0f, 0.0f, "CLOSED", SCALE_FLAG_TEXT});
  b.add_scale({7, 7, 0.0f, 0.0f, "not available", SCALE_FLAG_TEXT | SCALE_FLAG_INVALID});
  // A merged group's original service qualifier (§7): its own index entry, pointing at group 0
  // whose name_ref is the canonical name.
  b.add_name("DT_Demo_Voltage_Maximum_Cell_Voltage", NameKind::GROUP, 0);
  return b.build();
}

/// Exercise every accessor over an opened catalog. Under ASan this is the "succeed-and-be-safe"
/// half of the fuzz contract: whatever open() accepted must be walkable without leaving the
/// buffer.
void walk_everything(const Catalog &cat) {
  for (uint32_t i = 0; i < cat.ecu_count(); i++) {
    EcuView e;
    CHECK(cat.ecu(i, &e));
    (void) std::strlen(e.name);
  }
  for (uint32_t i = 0; i < cat.group_count(); i++) {
    GroupView g;
    CHECK(cat.group(i, &g));
    (void) std::strlen(g.name);
    const uint8_t *req = cat.request_bytes(g);
    if (req != nullptr && g.req_len > 0) {
      volatile uint8_t sink = req[g.req_len - 1];  // touch the last claimed byte
      (void) sink;
    }
  }
  for (uint32_t i = 0; i < cat.field_count(); i++) {
    FieldView f;
    CHECK(cat.field(i, &f));
    (void) std::strlen(f.name);
    (void) std::strlen(f.unit);
  }
  for (uint32_t i = 0; i < cat.scale_count(); i++) {
    ScaleView s;
    CHECK(cat.scale(i, &s));
    (void) std::strlen(s.text);
  }
  uint32_t idx;
  (void) cat.find("DEMO", NameKind::ECU, &idx);
  (void) cat.find("PRES_Demo_Contactor_State", NameKind::FIELD, &idx);
  (void) cat.find("no_such_name", NameKind::GROUP, &idx);
}

}  // namespace

// ---------------------------------------------------------------------------
// A valid blob, read back record by record
// ---------------------------------------------------------------------------

TEST(uds_catalog_opens_valid_blob_and_reads_records) {
  const std::vector<uint8_t> blob = make_mini_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  CHECK(cat.is_open());
  CHECK_EQ(cat.ecu_count(), 1);
  CHECK_EQ(cat.group_count(), 2);
  CHECK_EQ(cat.field_count(), 4);
  CHECK_EQ(cat.scale_count(), 5);
  CHECK_EQ(cat.index_count(), 8u);  // 1 ecu + 2 groups + 4 fields + 1 alias

  EcuView e;
  CHECK(cat.ecu(0, &e));
  CHECK(std::strcmp(e.name, "DEMO") == 0);
  CHECK_EQ(e.request_id, 0x7E7u);
  CHECK_EQ(e.response_id, 0x7EFu);
  CHECK_EQ(e.block_size, 8);
  CHECK_EQ(e.p2_ms, 500);
  CHECK_EQ(e.p2_ext_ms, 5000);
  CHECK_EQ(e.group_first, 0);
  CHECK_EQ(e.group_count, 2);

  GroupView g;
  CHECK(cat.group(0, &g));
  CHECK(std::strcmp(g.name, "DT_Demo_Voltage") == 0);
  CHECK_EQ(g.sid, 0x22);
  CHECK_EQ(g.req_len, 3);
  CHECK_EQ(g.did, 0x0207);
  CHECK_EQ(g.resp_min_len, 9);
  CHECK_EQ(g.field_first, 0);
  CHECK_EQ(g.field_count, 3);
  CHECK((g.flags & GROUP_FLAG_SAFE_READ) != 0);
  const uint8_t *req = cat.request_bytes(g);
  CHECK(req != nullptr);
  const uint8_t want_req[] = {0x22, 0x02, 0x07};
  CHECK_BYTES(req, want_req, 3);

  FieldView f;
  CHECK(cat.field(0, &f));
  CHECK(std::strcmp(f.name, "PRES_Demo_Voltage_Averaged_2Byte") == 0);
  CHECK(std::strcmp(f.unit, "V") == 0);
  CHECK_EQ(f.bit_pos, 24);
  CHECK_EQ(f.bit_size, 16);
  CHECK_EQ(f.scale_first, 0);
  CHECK_EQ(f.scale_count, 2);
  CHECK_EQ(f.repeat_count, 1);
  CHECK_EQ(f.group_index, 0);
  CHECK(cat.field(3, &f));
  CHECK(std::strcmp(f.unit, "") == 0);  // unit_ref 0 reads as the empty string
  CHECK((f.flags & FIELD_FLAG_ENUM) != 0);
  CHECK_EQ(f.group_index, 1);

  ScaleView s;
  CHECK(cat.scale(0, &s));
  CHECK_EQ(s.low, 0);
  CHECK_EQ(s.high, 64999);
  CHECK(s.factor > 0.0009f && s.factor < 0.0011f);
  CHECK_EQ(s.flags, 0);
  CHECK(cat.scale(1, &s));
  CHECK_EQ(s.low, 65535);
  CHECK(std::strcmp(s.text, "SNA") == 0);
  CHECK_EQ(s.flags, SCALE_FLAG_TEXT | SCALE_FLAG_INVALID);
}

TEST(uds_catalog_accessors_bounds_check_indices) {
  const std::vector<uint8_t> blob = make_mini_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  EcuView e;
  GroupView g;
  FieldView f;
  ScaleView s;
  CHECK(!cat.ecu(1, &e));
  CHECK(!cat.group(2, &g));
  CHECK(!cat.field(4, &f));
  CHECK(!cat.scale(5, &s));
  // A closed catalog answers nothing.
  Catalog closed;
  CHECK(!closed.ecu(0, &e));
  uint32_t idx;
  CHECK(!closed.find("DEMO", NameKind::ECU, &idx));
  CHECK(std::strcmp(closed.string(0), "") == 0);
}

TEST(uds_catalog_string_refs_degrade_to_empty) {
  const std::vector<uint8_t> blob = make_mini_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  CHECK(std::strcmp(cat.string(0), "") == 0);           // reserved empty ref
  CHECK(std::strcmp(cat.string(0xFFFFFFFF), "") == 0);  // out of pool: "" — never a wild pointer
}

TEST(uds_catalog_request_bytes_refuse_a_lying_group) {
  const std::vector<uint8_t> blob = make_mini_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  GroupView g;
  CHECK(cat.group(0, &g));
  // A CRC pass proves integrity, not honesty: a view whose pool reference does not fit the pool
  // must come back nullptr, not as a pointer past it.
  g.req_off = 0xFFFFFFF0u;
  CHECK(cat.request_bytes(g) == nullptr);
  g.req_off = 4;  // 4 + 3 > 6 pool bytes
  CHECK(cat.request_bytes(g) == nullptr);
}

// ---------------------------------------------------------------------------
// Truncation fuzz — format §2: "tests/host truncates a valid blob at every
// possible length and asserts exactly that under ASan"
// ---------------------------------------------------------------------------

TEST(uds_catalog_truncation_fuzz_every_length) {
  const std::vector<uint8_t> blob = make_mini_blob();
  for (size_t len = 0; len <= blob.size(); len++) {
    uint8_t *buf = new uint8_t[len];
    std::memcpy(buf, blob.data(), len);
    Catalog cat;
    const bool ok = cat.open(buf, len);
    if (len < blob.size()) {
      // total_size claims more than the mapping holds, so §2 step 2 must refuse every one.
      CHECK_MSG(!ok, "truncated to " + std::to_string(len) + " of " + std::to_string(blob.size()) + " bytes opened");
    } else {
      CHECK(ok);
    }
    if (ok)
      walk_everything(cat);
    delete[] buf;
  }
}

TEST(uds_catalog_oversized_mapping_is_fine) {
  // The partition is usually larger than the blob; total_size <= mapped size must open.
  const std::vector<uint8_t> blob = make_mini_blob();
  std::vector<uint8_t> padded = blob;
  padded.resize(blob.size() + 512, 0xFF);
  Catalog cat;
  CHECK(cat.open(padded.data(), padded.size()));
  walk_everything(cat);
}

// ---------------------------------------------------------------------------
// Corruption — each §2 validation step made to fire individually
// ---------------------------------------------------------------------------

TEST(uds_catalog_rejects_bad_magic) {
  std::vector<uint8_t> blob = make_mini_blob();
  blob[0] ^= 0xFF;
  Catalog cat;
  CHECK(!cat.open(blob.data(), blob.size()));
  blob[0] ^= 0xFF;
  blob[7] = 'X';  // the magic's trailing NUL is part of the magic
  CHECK(!cat.open(blob.data(), blob.size()));
}

TEST(uds_catalog_rejects_unknown_version) {
  std::vector<uint8_t> blob = make_mini_blob();
  store_u16(blob, 0x08, 2);  // §9: a reader refuses a version it does not know
  Catalog cat;
  CHECK(!cat.open(blob.data(), blob.size()));
  store_u16(blob, 0x08, 0);
  CHECK(!cat.open(blob.data(), blob.size()));
}

TEST(uds_catalog_rejects_wrong_crc) {
  std::vector<uint8_t> blob = make_mini_blob();
  blob[0x60] ^= 0x01;  // one bit inside the covered range, CRC left stale
  Catalog cat;
  CHECK(!cat.open(blob.data(), blob.size()));
  fix_crc(blob);  // same content, recomputed CRC: proves the reject above was the CRC's doing
  CHECK(cat.open(blob.data(), blob.size()));
}

TEST(uds_catalog_rejects_bad_total_size) {
  std::vector<uint8_t> blob = make_mini_blob();
  store_u32(blob, 0x0C, 0x4F);  // below header + extension
  Catalog cat;
  CHECK(!cat.open(blob.data(), blob.size()));
  store_u32(blob, 0x0C, static_cast<uint32_t>(blob.size() + 1));  // beyond the mapping
  CHECK(!cat.open(blob.data(), blob.size()));
}

TEST(uds_catalog_rejects_each_table_offset_out_of_bounds) {
  const std::vector<uint8_t> valid = make_mini_blob();
  const uint32_t total = load_u32(valid.data() + 0x0C);
  const size_t table_off_fields[] = {0x14, 0x1C, 0x24, 0x2C, 0x44};  // ecus, groups, fields, scales, index
  for (size_t hdr : table_off_fields) {
    std::vector<uint8_t> blob = valid;
    store_u32(blob, hdr, total);  // off at total_size: count > 0 records cannot fit
    fix_crc(blob);                // CRC honest, so the *bounds check* is what must refuse
    Catalog cat;
    CHECK_MSG(!cat.open(blob.data(), blob.size()), "table descriptor at header offset " + std::to_string(hdr));
  }
  // Also a count too large for an in-bounds offset.
  std::vector<uint8_t> blob = valid;
  store_u16(blob, 0x18, 0xFFFE);  // ecu_count
  fix_crc(blob);
  Catalog cat;
  CHECK(!cat.open(blob.data(), blob.size()));
}

TEST(uds_catalog_rejects_misaligned_table) {
  std::vector<uint8_t> blob = make_mini_blob();
  const uint32_t ecus_off = load_u32(blob.data() + 0x14);
  store_u32(blob, 0x14, ecus_off + 2);  // still inside the blob, no longer 4-aligned
  fix_crc(blob);
  Catalog cat;
  CHECK(!cat.open(blob.data(), blob.size()));
}

TEST(uds_catalog_rejects_pool_out_of_bounds) {
  const std::vector<uint8_t> valid = make_mini_blob();
  {
    std::vector<uint8_t> blob = valid;
    store_u32(blob, 0x38, 0xFFFF0000u);  // reqbytes_len
    fix_crc(blob);
    Catalog cat;
    CHECK(!cat.open(blob.data(), blob.size()));
  }
  {
    std::vector<uint8_t> blob = valid;
    const uint32_t total = load_u32(blob.data() + 0x0C);
    store_u32(blob, 0x3C, total);  // strings_off at the end, strings_len unchanged
    fix_crc(blob);
    Catalog cat;
    CHECK(!cat.open(blob.data(), blob.size()));
  }
}

TEST(uds_catalog_rejects_string_pool_not_nul_framed) {
  const std::vector<uint8_t> valid = make_mini_blob();
  const uint32_t strings_off = load_u32(valid.data() + 0x3C);
  const uint32_t strings_len = load_u32(valid.data() + 0x40);
  {
    std::vector<uint8_t> blob = valid;
    blob[strings_off] = 'x';  // first byte: ref 0 would no longer read as ""
    fix_crc(blob);
    Catalog cat;
    CHECK(!cat.open(blob.data(), blob.size()));
  }
  {
    std::vector<uint8_t> blob = valid;
    blob[strings_off + strings_len - 1] = 'x';  // trailing NUL: the last string would run off the pool
    fix_crc(blob);
    Catalog cat;
    CHECK(!cat.open(blob.data(), blob.size()));
  }
  {
    std::vector<uint8_t> blob = valid;
    store_u32(blob, 0x40, 0);  // an empty pool cannot hold either NUL
    fix_crc(blob);
    Catalog cat;
    CHECK(!cat.open(blob.data(), blob.size()));
  }
}

TEST(uds_catalog_rejects_null_and_tiny_inputs) {
  const std::vector<uint8_t> blob = make_mini_blob();
  Catalog cat;
  CHECK(!cat.open(nullptr, blob.size()));
  CHECK(!cat.open(blob.data(), 0));
  CHECK(!cat.open(blob.data(), 0x4F));  // one byte short of header + extension
}

TEST(uds_catalog_ignores_reserved_fields_and_unknown_flags) {
  // §1/§9, the forward-compatibility hinge: reserved fields and unknown flag bits must not make
  // a reader reject the blob. This is what lets a v1-compatible catalog gain tables later.
  std::vector<uint8_t> blob = make_mini_blob();
  store_u16(blob, 0x1A, 0xBEEF);                                    // header reserved0
  store_u32(blob, 0x4C, 0x12345678u);                               // extension reserved4
  store_u16(blob, 0x0A, HDR_FLAG_NAMES | HDR_FLAG_TEXTS | 0x8000);  // an undefined header flag bit
  fix_crc(blob);
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  walk_everything(cat);
}

TEST(uds_catalog_crc_matches_ieee_check_value) {
  // Pins crc32_ieee to the standard, independent of the builder that reuses it: the classic
  // check value for CRC-32/IEEE (zlib) is crc32("123456789") == 0xCBF43926.
  CHECK_EQ(crc32_ieee(reinterpret_cast<const uint8_t *>("123456789"), 9), 0xCBF43926u);
}

// ---------------------------------------------------------------------------
// Name index (§7)
// ---------------------------------------------------------------------------

TEST(uds_catalog_find_resolves_each_kind) {
  std::vector<uint8_t> blob = make_mini_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  uint32_t idx = 0xAAAA;
  CHECK(cat.find("DEMO", NameKind::ECU, &idx));
  CHECK_EQ(idx, 0u);
  CHECK(cat.find("DT_Demo_Contactor", NameKind::GROUP, &idx));
  CHECK_EQ(idx, 1u);
  CHECK(cat.find("PRES_Demo_Voltage_Minimum_2Byte", NameKind::FIELD, &idx));
  CHECK_EQ(idx, 2u);
  // Wrong kind is a miss even though the hash is present.
  CHECK(!cat.find("DEMO", NameKind::GROUP, &idx));
  CHECK(!cat.find("DT_Demo_Contactor", NameKind::FIELD, &idx));
  // Case-sensitive (§7), and unknown names miss.
  CHECK(!cat.find("demo", NameKind::ECU, &idx));
  CHECK(!cat.find("PRES_Demo_Voltage", NameKind::FIELD, &idx));
}

TEST(uds_catalog_find_collision_resolves_to_miss) {
  // "costarring" and "liquid" genuinely collide under FNV-1a 32; assert that first so this case
  // can never silently stop testing what it is named for.
  CHECK_EQ(fnv1a("costarring"), fnv1a("liquid"));
  BlobBuilder b;
  b.add_field({"costarring", "", 24, 8, 0, 0, 0, 1, 0, 0});
  const std::vector<uint8_t> blob = b.build();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  uint32_t idx;
  CHECK(cat.find("costarring", NameKind::FIELD, &idx));
  CHECK_EQ(idx, 0u);
  // The hash hits, the strcmp does not: a miss, never the wrong field.
  CHECK(!cat.find("liquid", NameKind::FIELD, &idx));
}

TEST(uds_catalog_find_walks_equal_hashes_to_the_right_record) {
  BlobBuilder b;
  b.add_field({"costarring", "", 24, 8, 0, 0, 0, 1, 0, 0});
  b.add_field({"liquid", "", 32, 8, 0, 0, 0, 1, 0, 0});
  const std::vector<uint8_t> blob = b.build();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  uint32_t idx;
  CHECK(cat.find("costarring", NameKind::FIELD, &idx));
  CHECK_EQ(idx, 0u);
  CHECK(cat.find("liquid", NameKind::FIELD, &idx));
  CHECK_EQ(idx, 1u);
}

TEST(uds_catalog_find_same_name_different_kinds) {
  BlobBuilder b;
  b.add_group({"Shared_Name", {0x22, 0x01, 0x00}, GROUP_FLAG_SAFE_READ, 0, 1, 0x0100, 4});
  b.add_field({"Shared_Name", "", 24, 8, 0, 0, 0, 1, 0, 0});
  const std::vector<uint8_t> blob = b.build();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  uint32_t idx;
  CHECK(cat.find("Shared_Name", NameKind::GROUP, &idx));
  CHECK_EQ(idx, 0u);
  CHECK(cat.find("Shared_Name", NameKind::FIELD, &idx));
  CHECK_EQ(idx, 0u);
  CHECK(!cat.find("Shared_Name", NameKind::ECU, &idx));
}

TEST(uds_catalog_find_resolves_a_merged_group_alias) {
  // §7's promise, and the reason the entry carries its own name_ref: a merged group keeps one
  // canonical name, and every original service qualifier folded into it resolves to that same
  // group. Verifying against the *record's* name (the format's first version) could not do this —
  // an alias never equals the canonical name, so every alias lookup was a miss.
  const std::vector<uint8_t> blob = make_mini_blob();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  uint32_t idx = 0xAAAA;
  CHECK(cat.find("DT_Demo_Voltage_Maximum_Cell_Voltage", NameKind::GROUP, &idx));
  CHECK_EQ(idx, 0u);
  // The canonical name finds the same group, and a name that is neither still misses.
  idx = 0xAAAA;
  CHECK(cat.find("DT_Demo_Voltage", NameKind::GROUP, &idx));
  CHECK_EQ(idx, 0u);
  CHECK(!cat.find("DT_Demo_Voltage_Maximum", NameKind::GROUP, &idx));
}

TEST(uds_catalog_find_rejects_an_out_of_range_target) {
  // The entry's own name verifies, but its target index does not exist. A CRC pass proves the
  // blob is intact, not that its target words are in range, so this must be a miss rather than an
  // accessor call that fails later — or worse, an index handed to a caller that trusts it.
  BlobBuilder b;
  b.add_field({"real_field", "", 24, 8, 0, 0, 0, 1, 0, 0});
  b.add_name("ghost_field", NameKind::FIELD, 9);  // field 9 of a 1-field catalog
  const std::vector<uint8_t> blob = b.build();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  uint32_t idx = 0xAAAA;
  CHECK(!cat.find("ghost_field", NameKind::FIELD, &idx));
  CHECK_EQ(idx, 0xAAAAu);  // untouched on a miss
  CHECK(cat.find("real_field", NameKind::FIELD, &idx));
  CHECK_EQ(idx, 0u);
}

TEST(uds_catalog_find_entry_with_an_empty_name_ref_misses) {
  // An entry whose name_ref is 0 reads as "" (§1) and cannot equal any real query — a
  // name_ref-less entry from an older compiler must not resolve on its hash alone.
  BlobBuilder b;
  b.add_field({"target_field", "", 24, 8, 0, 0, 0, 1, 0, 0});
  const std::vector<uint8_t> blob = b.build();
  const uint32_t index_off = load_u32(blob.data() + 0x44);
  std::vector<uint8_t> broken = blob;
  store_u32(broken, index_off + 4, 0);  // drop the entry's name_ref
  fix_crc(broken);
  Catalog cat;
  CHECK(cat.open(broken.data(), broken.size()));
  uint32_t idx;
  CHECK(!cat.find("target_field", NameKind::FIELD, &idx));
  // And the empty string does not resolve to it either.
  CHECK(!cat.find("", NameKind::FIELD, &idx));
}

TEST(uds_catalog_index_entry_is_twelve_bytes_at_the_documented_offsets) {
  // §7 offset by offset, read out of the blob by hand: {hash u32, name_ref u32, target u32}.
  // A reader and a builder that agreed on a *wrong* record size would still pass find(), so the
  // geometry is pinned against the document rather than against either side.
  CHECK_EQ(INDEX_RECORD_SIZE, static_cast<size_t>(12));
  BlobBuilder b;
  b.add_ecu({"DEMO", 0x7E7, 0x7EF, 0, 8, 0, 500, 5000, 0, 0});
  const std::vector<uint8_t> blob = b.build();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  CHECK_EQ(cat.index_count(), 1u);
  const uint32_t index_off = load_u32(blob.data() + 0x44);
  CHECK_EQ(index_off % 4, 0u);
  const uint8_t *e = blob.data() + index_off;
  CHECK_EQ(load_u32(e + 0), fnv1a("DEMO"));
  CHECK(std::strcmp(cat.string(load_u32(e + 4)), "DEMO") == 0);
  const uint32_t target = load_u32(e + 8);
  CHECK_EQ(target >> 28, static_cast<uint32_t>(NameKind::ECU));
  CHECK_EQ(target & 0x0FFFFFFF, 0u);
  // The whole index fits inside the blob at 12 bytes an entry.
  CHECK(index_off + 12 <= cat.total_size());
}

TEST(uds_catalog_find_on_empty_index) {
  BlobBuilder b;
  b.add_scale({0, 1, 1.0f, 0.0f, "", 0});  // a blob with no named records at all
  const std::vector<uint8_t> blob = b.build();
  Catalog cat;
  CHECK(cat.open(blob.data(), blob.size()));
  CHECK_EQ(cat.index_count(), 0u);
  uint32_t idx;
  CHECK(!cat.find("anything", NameKind::FIELD, &idx));
}
