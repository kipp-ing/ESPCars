#pragma once
//
// Test-only builder for spec-conformant `.dcat` blobs (docs/uds-catalog-format.md), so host cases
// can construct exact geometries: a minimal valid catalog, colliding index names, a blob to
// truncate at every length. Host scaffolding in the harness.h sense — std::vector is fine here,
// none of this compiles into the component, and the real compiler is components/uds/catalog.py.
//
// This is deliberately a second, independent encoding of the format: the builder *stores* at the
// offsets the spec names, the reader *loads* at the offsets it names, and a disagreement between
// the two is a failing case rather than a shared bug. (The checked-in golden fixture plays the
// same role against the Python compiler.) The only pieces borrowed from the reader are fnv1a()
// and crc32_ieee(), because index hashing and the CRC must match bit-for-bit by definition —
// crc32_ieee itself is pinned to the standard by the "123456789" check value in the catalog
// suite, not by self-agreement.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "uds_catalog.h"

namespace udstest {

// Little-endian stores, mirroring the reader's loads.
inline void store_u16(std::vector<uint8_t> &v, size_t off, uint16_t x) {
  v[off] = static_cast<uint8_t>(x & 0xFF);
  v[off + 1] = static_cast<uint8_t>(x >> 8);
}

inline void store_u32(std::vector<uint8_t> &v, size_t off, uint32_t x) {
  v[off] = static_cast<uint8_t>(x & 0xFF);
  v[off + 1] = static_cast<uint8_t>((x >> 8) & 0xFF);
  v[off + 2] = static_cast<uint8_t>((x >> 16) & 0xFF);
  v[off + 3] = static_cast<uint8_t>((x >> 24) & 0xFF);
}

inline void append_u16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>(x >> 8));
}

inline void append_u32(std::vector<uint8_t> &v, uint32_t x) {
  append_u16(v, static_cast<uint16_t>(x & 0xFFFF));
  append_u16(v, static_cast<uint16_t>(x >> 16));
}

inline void append_i32(std::vector<uint8_t> &v, int32_t x) { append_u32(v, static_cast<uint32_t>(x)); }

inline void append_f32(std::vector<uint8_t> &v, float x) {
  uint32_t bits;
  std::memcpy(&bits, &x, sizeof(bits));
  append_u32(v, bits);
}

/// Recompute the header CRC over [64, total_size) after a test mutated blob content. Clamped to
/// the buffer so a case that also corrupted total_size cannot make the *test scaffolding* read
/// out of bounds.
inline void fix_crc(std::vector<uint8_t> &blob) {
  if (blob.size() < 0x50)
    return;
  uint64_t total = esphome::uds::load_u32(blob.data() + 0x0C);
  if (total > blob.size())
    total = blob.size();
  if (total < 64)
    return;
  store_u32(blob, 0x10, esphome::uds::crc32_ieee(blob.data() + 64, static_cast<size_t>(total) - 64));
}

class BlobBuilder {
 public:
  BlobBuilder() {
    // §1: the pool's first byte is '\0' so ref 0 always reads as the empty string.
    strings_.push_back('\0');
  }

  /// Intern a string; returns its pool-relative ref (0 for empty, per §1). Deduplicating, like a
  /// real compiler: an index entry's name_ref and its record's name_ref then share one copy, which
  /// is what keeps the §7 entry's four extra bytes the whole cost of carrying the name.
  uint32_t add_string(const char *s) {
    if (s == nullptr || *s == '\0')
      return 0;
    const std::string key(s);
    auto it = interned_.find(key);
    if (it != interned_.end())
      return it->second;
    const uint32_t ref = static_cast<uint32_t>(strings_.size());
    strings_.insert(strings_.end(), s, s + key.size() + 1);
    interned_.emplace(key, ref);
    return ref;
  }

  /// Add an index entry by hand — for group aliases (§7) and for crafting collision geometries.
  /// The entry carries the name it was hashed from, which is what the reader verifies against.
  void add_name(const char *name, esphome::uds::NameKind kind, uint32_t index) {
    index_.push_back(
        {esphome::uds::fnv1a(name), add_string(name), (static_cast<uint32_t>(kind) << 28) | (index & 0x0FFFFFFF)});
  }

  struct Ecu {
    const char *name;
    uint32_t request_id;
    uint32_t response_id;
    uint16_t flags;
    uint8_t block_size;
    uint8_t st_min_raw;
    uint16_t p2_ms;
    uint16_t p2_ext_ms;
    uint16_t group_first;
    uint16_t group_count;
  };

  uint16_t add_ecu(const Ecu &e) {  // §3, 24 bytes
    const uint16_t idx = ecu_count_++;
    append_u32(ecus_, add_string(e.name));
    append_u32(ecus_, e.request_id);
    append_u32(ecus_, e.response_id);
    append_u16(ecus_, e.flags);
    ecus_.push_back(e.block_size);
    ecus_.push_back(e.st_min_raw);
    append_u16(ecus_, e.p2_ms);
    append_u16(ecus_, e.p2_ext_ms);
    append_u16(ecus_, e.group_first);
    append_u16(ecus_, e.group_count);
    add_name(e.name, esphome::uds::NameKind::ECU, idx);
    return idx;
  }

  struct Group {
    const char *name;
    std::vector<uint8_t> req;
    uint16_t flags;
    uint16_t field_first;
    uint16_t field_count;
    uint16_t did;
    uint16_t resp_min_len;
  };

  uint16_t add_group(const Group &g) {  // §4, 20 bytes; sid is by definition the first request byte
    const uint16_t idx = group_count_++;
    // Stored POOL-RELATIVE here and rewritten to an absolute blob offset in build(), once the pool
    // base is known. `req_off` is an absolute offset per §1 — only string refs are pool-relative —
    // and this builder emitted it relative, matching a reader bug instead of the format. Because
    // both sides were wrong the same way, every host case passed and the disagreement only appeared
    // when the C6 read a catalog the *Python* compiler had written. Hence the offsets recorded
    // below, and test_uds_fixture.cpp.
    req_off_patch_.push_back(static_cast<uint32_t>(groups_.size()) + 4);
    const uint32_t req_rel = static_cast<uint32_t>(reqbytes_.size());
    reqbytes_.insert(reqbytes_.end(), g.req.begin(), g.req.end());
    append_u32(groups_, add_string(g.name));
    append_u32(groups_, req_rel);
    groups_.push_back(static_cast<uint8_t>(g.req.size()));
    groups_.push_back(g.req.empty() ? 0 : g.req[0]);
    append_u16(groups_, g.flags);
    append_u16(groups_, g.field_first);
    append_u16(groups_, g.field_count);
    append_u16(groups_, g.did);
    append_u16(groups_, g.resp_min_len);
    add_name(g.name, esphome::uds::NameKind::GROUP, idx);
    return idx;
  }

  struct Field {
    const char *name;
    const char *unit;
    uint16_t bit_pos;
    uint8_t bit_size;
    uint8_t flags;
    uint16_t scale_first;
    uint8_t scale_count;
    uint8_t repeat_count;
    uint16_t repeat_stride;
    uint16_t group_index;
  };

  uint16_t add_field(const Field &f) {  // §5, 20 bytes
    const uint16_t idx = field_count_++;
    append_u32(fields_, add_string(f.name));
    append_u32(fields_, add_string(f.unit));
    append_u16(fields_, f.bit_pos);
    fields_.push_back(f.bit_size);
    fields_.push_back(f.flags);
    append_u16(fields_, f.scale_first);
    fields_.push_back(f.scale_count);
    fields_.push_back(f.repeat_count);
    append_u16(fields_, f.repeat_stride);
    append_u16(fields_, f.group_index);
    add_name(f.name, esphome::uds::NameKind::FIELD, idx);
    return idx;
  }

  struct Scale {
    int32_t low;
    int32_t high;
    float factor;
    float offset;
    const char *text;
    uint16_t flags;
  };

  uint16_t add_scale(const Scale &s) {  // §6, 24 bytes
    const uint16_t idx = scale_count_++;
    append_i32(scales_, s.low);
    append_i32(scales_, s.high);
    append_f32(scales_, s.factor);
    append_f32(scales_, s.offset);
    append_u32(scales_, add_string(s.text));
    append_u16(scales_, s.flags);
    append_u16(scales_, 0);  // reserved
    return idx;
  }

  uint16_t ecu_count() const { return ecu_count_; }
  uint16_t group_count() const { return group_count_; }
  uint16_t field_count() const { return field_count_; }
  uint16_t scale_count() const { return scale_count_; }

  /// Assemble the blob: header, extension, tables (every record size is a multiple of 4, so the
  /// tables stay 4-aligned back to back), request-byte pool, string pool; then index sort and
  /// CRC. The layout order is a builder choice — the reader must take its geometry from the
  /// descriptors, never from this convention.
  std::vector<uint8_t> build() const {
    std::vector<Entry> idx = index_;
    std::sort(idx.begin(), idx.end(),
              [](const Entry &a, const Entry &b) { return a.hash != b.hash ? a.hash < b.hash : a.target < b.target; });

    size_t off = 0x50;
    const size_t ecus_off = off;
    off += ecus_.size();
    const size_t groups_off = off;
    off += groups_.size();
    const size_t fields_off = off;
    off += fields_.size();
    const size_t scales_off = off;
    off += scales_.size();
    const size_t index_off = off;
    off += idx.size() * esphome::uds::INDEX_RECORD_SIZE;
    const size_t reqbytes_off = off;
    off += reqbytes_.size();
    const size_t strings_off = off;
    off += strings_.size();
    const size_t total = off;

    std::vector<uint8_t> blob(total, 0);
    static const uint8_t MAGIC[8] = {'E', 'S', 'P', 'D', 'C', 'A', 'T', '\0'};
    std::memcpy(blob.data(), MAGIC, sizeof(MAGIC));
    store_u16(blob, 0x08, esphome::uds::CATALOG_FORMAT_VERSION);
    store_u16(blob, 0x0A, header_flags_);
    store_u32(blob, 0x0C, static_cast<uint32_t>(total));
    store_u32(blob, 0x14, static_cast<uint32_t>(ecus_off));
    store_u16(blob, 0x18, ecu_count_);
    store_u32(blob, 0x1C, static_cast<uint32_t>(groups_off));
    store_u16(blob, 0x20, group_count_);
    store_u32(blob, 0x24, static_cast<uint32_t>(fields_off));
    store_u16(blob, 0x28, field_count_);
    store_u32(blob, 0x2C, static_cast<uint32_t>(scales_off));
    store_u16(blob, 0x30, scale_count_);
    store_u32(blob, 0x34, static_cast<uint32_t>(reqbytes_off));
    store_u32(blob, 0x38, static_cast<uint32_t>(reqbytes_.size()));
    store_u32(blob, 0x3C, static_cast<uint32_t>(strings_off));
    store_u32(blob, 0x40, static_cast<uint32_t>(strings_.size()));
    store_u32(blob, 0x44, static_cast<uint32_t>(index_off));
    store_u32(blob, 0x48, static_cast<uint32_t>(idx.size()));

    // memcpy(dst, nullptr, 0) is a UBSan finding, and an empty table is a legal geometry here.
    copy_into_(blob, ecus_off, ecus_);
    copy_into_(blob, groups_off, groups_);
    // Rewrite every group's req_off from pool-relative to absolute, now that the pool base is
    // known. Doing it here rather than at add_group() time keeps the layout a build() decision.
    for (const uint32_t patch : req_off_patch_) {
      const uint32_t rel = esphome::uds::load_u32(blob.data() + groups_off + patch);
      store_u32(blob, groups_off + patch, static_cast<uint32_t>(reqbytes_off) + rel);
    }
    copy_into_(blob, fields_off, fields_);
    copy_into_(blob, scales_off, scales_);
    for (size_t i = 0; i < idx.size(); i++) {
      const size_t at = index_off + i * esphome::uds::INDEX_RECORD_SIZE;
      store_u32(blob, at, idx[i].hash);
      store_u32(blob, at + 4, idx[i].name_ref);
      store_u32(blob, at + 8, idx[i].target);
    }
    copy_into_(blob, reqbytes_off, reqbytes_);
    copy_into_(blob, strings_off, strings_);

    fix_crc(blob);
    return blob;
  }

  void set_header_flags(uint16_t flags) { header_flags_ = flags; }

 private:
  struct Entry {
    uint32_t hash;
    uint32_t name_ref;
    uint32_t target;
  };

  static void copy_into_(std::vector<uint8_t> &blob, size_t off, const std::vector<uint8_t> &src) {
    if (!src.empty())
      std::memcpy(blob.data() + off, src.data(), src.size());
  }

  std::vector<uint8_t> ecus_;
  std::vector<uint8_t> groups_;
  std::vector<uint8_t> fields_;
  std::vector<uint8_t> scales_;
  std::vector<uint8_t> reqbytes_;
  std::vector<uint8_t> strings_;
  std::map<std::string, uint32_t> interned_;
  std::vector<Entry> index_;
  /// Byte offsets inside `groups_` of each group's `req_off` word, so build() can rewrite the
  /// pool-relative value it was collected with into the absolute offset §1 requires.
  std::vector<uint32_t> req_off_patch_;
  uint16_t ecu_count_{0};
  uint16_t group_count_{0};
  uint16_t field_count_{0};
  uint16_t scale_count_{0};
  uint16_t header_flags_{esphome::uds::HDR_FLAG_NAMES | esphome::uds::HDR_FLAG_TEXTS};
};

}  // namespace udstest
