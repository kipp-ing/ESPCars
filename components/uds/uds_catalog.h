#pragma once

/// Freestanding reader for the `.dcat` diagnostic catalog. docs/uds-catalog-format.md is the
/// normative layout; this file implements it offset by offset. No ESPHome, no ESP-IDF, no
/// allocation, no exceptions — the same bytes-in, views-out code runs against
/// `esp_partition_mmap()`ed flash on the ESP32-C6 and against plain heap buffers in tests/host,
/// where ASan proves the "never read outside the mapped region" contract at every truncation
/// length.
///
/// Records are read through explicit little-endian loads, NOT by casting struct pointers over the
/// mapped bytes. The format doc's preamble says "the reader casts rather than copies" — that
/// sentence motivates why records are fixed-width and 4-byte aligned; it is not a license to skip
/// the preconditions a cast would need: a packed struct whose every member offset is
/// static_asserted against the spec, a base pointer that is provably 4-aligned (true for
/// esp_partition_mmap, not for an arbitrary test buffer), and a little-endian host. Explicit
/// loads have none of those preconditions and read the exact offsets this file names, so a wrong
/// offset is a failing test vector instead of a silent struct-padding accident. The cost is nil:
/// open() runs once, accessors copy ~20 bytes of scalars into a view, and strings stay
/// `const char *` pointers into flash.
///
/// String refs: format §1 calls them "absolute offsets into the string pool" and in the same
/// breath reserves ref 0 as the empty string *because the pool's first byte is `\0`*. Those two
/// statements agree only if refs count from the pool's start — a blob-absolute ref 0 would point
/// at the magic, not at a NUL. This reader takes the pool-relative reading, the one in which the
/// ref-0 rule is mechanical rather than a special case (and in which `unit_ref`/`text_ref` "0
/// when none" needs no sentinel test either). The blob builder in tests/host writes the same
/// reading; the checked-in golden fixture is where a compiler that read it the other way would
/// collide with us.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::uds {

// -------------------------------------------------------------------------------------------
// Format constants (docs/uds-catalog-format.md §2, §8)
// -------------------------------------------------------------------------------------------

/// Header (64 bytes) plus the fixed header extension (16 bytes): the first table may start at
/// 0x50 or later, and nothing smaller can be a catalog.
static constexpr uint32_t CATALOG_MIN_SIZE = 0x50;
static constexpr uint16_t CATALOG_FORMAT_VERSION = 1;

static constexpr size_t ECU_RECORD_SIZE = 24;
static constexpr size_t GROUP_RECORD_SIZE = 20;
static constexpr size_t FIELD_RECORD_SIZE = 20;
static constexpr size_t SCALE_RECORD_SIZE = 24;
/// §7: {hash u32, name_ref u32, target u32}. The name_ref was added 2026-07-30 — see find().
static constexpr size_t INDEX_RECORD_SIZE = 12;

/// Header flags (§2).
static constexpr uint16_t HDR_FLAG_NAMES = 1 << 0;
static constexpr uint16_t HDR_FLAG_TEXTS = 1 << 1;

/// Ecu flags (§3).
static constexpr uint16_t ECU_FLAG_EXTENDED_ID = 1 << 0;

/// Group flags (§4). Bits 8-11 carry the security level; see group_security_level().
static constexpr uint16_t GROUP_FLAG_SAFE_READ = 1 << 0;
static constexpr uint16_t GROUP_FLAG_NEEDS_SESSION = 1 << 1;
static constexpr uint16_t GROUP_FLAG_NEEDS_SECURITY = 1 << 2;

inline uint8_t group_security_level(uint16_t group_flags) { return (group_flags >> 8) & 0x0F; }

/// Field flags (§5).
static constexpr uint8_t FIELD_FLAG_SIGNED = 1 << 0;
static constexpr uint8_t FIELD_FLAG_BYTESWAP = 1 << 1;
static constexpr uint8_t FIELD_FLAG_ASCII = 1 << 2;
static constexpr uint8_t FIELD_FLAG_HEXDUMP = 1 << 3;
static constexpr uint8_t FIELD_FLAG_ENUM = 1 << 4;

/// Scale row flags (§6).
static constexpr uint16_t SCALE_FLAG_TEXT = 1 << 0;
static constexpr uint16_t SCALE_FLAG_INVALID = 1 << 1;

/// Name-index target kinds (§7), bits 28-31 of the target word.
enum class NameKind : uint8_t {
  ECU = 1,
  GROUP = 2,
  FIELD = 3,
};

// -------------------------------------------------------------------------------------------
// Little-endian loads
// -------------------------------------------------------------------------------------------

inline uint16_t load_u16(const uint8_t *p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t load_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

inline int32_t load_i32(const uint8_t *p) { return static_cast<int32_t>(load_u32(p)); }

inline float load_f32(const uint8_t *p) {
  const uint32_t bits = load_u32(p);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

/// FNV-1a 32 over a NUL-terminated name, case-sensitive, NUL excluded (§7). Shared with the test
/// blob builder so both sides of the index hash identically.
inline uint32_t fnv1a(const char *s) {
  uint32_t h = 0x811C9DC5u;
  while (*s != '\0') {
    h ^= static_cast<uint8_t>(*s++);
    h *= 0x01000193u;
  }
  return h;
}

/// CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor) — the zlib polynomial the format
/// header names. Bitwise on purpose: it runs once per open() over at most a few hundred KB, and
/// a table would spend flash on a cold path.
inline uint32_t crc32_ieee(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

// -------------------------------------------------------------------------------------------
// Record views
// -------------------------------------------------------------------------------------------
// Small POD copies of one record's scalars; the strings stay pointers into the mapped blob and
// remain valid for as long as the mapping (a view outliving the catalog is the caller's bug).

struct EcuView {
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

struct GroupView {
  const char *name;
  uint32_t req_off;  ///< into the request-byte pool; resolve via Catalog::request_bytes()
  uint8_t req_len;
  uint8_t sid;
  uint16_t flags;
  uint16_t field_first;
  uint16_t field_count;
  uint16_t did;           ///< 0xFFFF when the group is not a SID 0x22 read
  uint16_t resp_min_len;  ///< bytes, whole response including the header (§4)
};

struct FieldView {
  const char *name;
  const char *unit;  ///< "" when unitless
  uint16_t bit_pos;  ///< MSB-first, from the first byte of the positive response (§5)
  uint8_t bit_size;
  uint8_t flags;
  uint16_t scale_first;
  uint8_t scale_count;     ///< 0 means raw value, no scaling
  uint8_t repeat_count;    ///< 1 for a scalar
  uint16_t repeat_stride;  ///< bits between array elements
  uint16_t group_index;
};

struct ScaleView {
  int32_t low;  ///< inclusive bounds on the raw value
  int32_t high;
  float factor;
  float offset;
  const char *text;  ///< "" when text_ref == 0; SCALE_FLAG_TEXT says whether the row is textual
  uint16_t flags;
};

// -------------------------------------------------------------------------------------------
// Catalog
// -------------------------------------------------------------------------------------------

/// The reader. open() performs every validation format §2 lists and trusts nothing until all of
/// them pass; a failure leaves the catalog closed and the caller with `false`, never with a
/// crash. Accessors bounds-check on top of that, because a matching CRC proves the blob arrived
/// intact — not that its record contents are honest — and an out-of-range index must cost a
/// `false`, not a read outside the mapping.
class Catalog {
 public:
  /// Validate and adopt a mapped blob. `base` stays borrowed: the catalog holds the pointer for
  /// its lifetime and copies nothing, which is the whole point of the mmap design.
  bool open(const uint8_t *base, size_t size) {
    this->base_ = nullptr;
    if (base == nullptr || size < CATALOG_MIN_SIZE)
      return false;
    // §2 step 1: magic and version. An unknown version is refused outright — by definition it is
    // a layout this reader would mis-interpret (§9).
    static const uint8_t MAGIC[8] = {'E', 'S', 'P', 'D', 'C', 'A', 'T', '\0'};
    if (std::memcmp(base, MAGIC, sizeof(MAGIC)) != 0)
      return false;
    if (load_u16(base + 0x08) != CATALOG_FORMAT_VERSION)
      return false;
    // §2 step 2: total_size within [header+extension, mapped region].
    const uint32_t total = load_u32(base + 0x0C);
    if (total < CATALOG_MIN_SIZE || static_cast<uint64_t>(total) > size)
      return false;
    this->flags_ = load_u16(base + 0x0A);
    this->crc_ = load_u32(base + 0x10);
    this->ecus_off_ = load_u32(base + 0x14);
    this->ecu_count_ = load_u16(base + 0x18);
    this->groups_off_ = load_u32(base + 0x1C);
    this->group_count_ = load_u16(base + 0x20);
    this->fields_off_ = load_u32(base + 0x24);
    this->field_count_ = load_u16(base + 0x28);
    this->scales_off_ = load_u32(base + 0x2C);
    this->scale_count_ = load_u16(base + 0x30);
    this->reqbytes_off_ = load_u32(base + 0x34);
    this->reqbytes_len_ = load_u32(base + 0x38);
    this->strings_off_ = load_u32(base + 0x3C);
    this->strings_len_ = load_u32(base + 0x40);
    this->index_off_ = load_u32(base + 0x44);
    this->index_count_ = load_u32(base + 0x48);
    // §2 step 3: every table inside [0x50, total_size), 4-aligned. All arithmetic in 64 bits so a
    // hostile offset cannot wrap its way past the check.
    if (!table_ok_(this->ecus_off_, this->ecu_count_, ECU_RECORD_SIZE, total))
      return false;
    if (!table_ok_(this->groups_off_, this->group_count_, GROUP_RECORD_SIZE, total))
      return false;
    if (!table_ok_(this->fields_off_, this->field_count_, FIELD_RECORD_SIZE, total))
      return false;
    if (!table_ok_(this->scales_off_, this->scale_count_, SCALE_RECORD_SIZE, total))
      return false;
    if (!table_ok_(this->index_off_, this->index_count_, INDEX_RECORD_SIZE, total))
      return false;
    // §2 step 4: both pools inside [0x50, total_size); the string pool NUL-framed at both ends,
    // which is the invariant that makes every in-pool ref a safe `const char *` without a length.
    if (!range_ok_(this->reqbytes_off_, this->reqbytes_len_, total))
      return false;
    if (!range_ok_(this->strings_off_, this->strings_len_, total))
      return false;
    if (this->strings_len_ == 0)
      return false;  // cannot hold the mandatory leading and trailing NUL
    if (base[this->strings_off_] != 0 || base[this->strings_off_ + this->strings_len_ - 1] != 0)
      return false;
    // §2 step 5: CRC over [64, total_size) — everything except the header whose only mutable
    // field is the CRC itself.
    if (crc32_ieee(base + 64, total - 64) != this->crc_)
      return false;
    this->total_size_ = total;
    this->base_ = base;
    return true;
  }

  bool is_open() const { return this->base_ != nullptr; }
  void close() { this->base_ = nullptr; }

  uint16_t ecu_count() const { return this->ecu_count_; }
  uint16_t group_count() const { return this->group_count_; }
  uint16_t field_count() const { return this->field_count_; }
  uint16_t scale_count() const { return this->scale_count_; }
  uint32_t index_count() const { return this->index_count_; }
  uint16_t header_flags() const { return this->flags_; }
  uint32_t total_size() const { return this->total_size_; }
  /// The header's CRC — what codegen recorded at `esphome config` time is compared against this,
  /// so a catalog newer than the firmware is a logged warning rather than a mystery.
  uint32_t crc32() const { return this->crc_; }

  bool ecu(uint32_t i, EcuView *out) const {
    if (this->base_ == nullptr || i >= this->ecu_count_)
      return false;
    const uint8_t *r = this->base_ + this->ecus_off_ + static_cast<size_t>(i) * ECU_RECORD_SIZE;
    out->name = this->string(load_u32(r + 0x00));
    out->request_id = load_u32(r + 0x04);
    out->response_id = load_u32(r + 0x08);
    out->flags = load_u16(r + 0x0C);
    out->block_size = r[0x0E];
    out->st_min_raw = r[0x0F];
    out->p2_ms = load_u16(r + 0x10);
    out->p2_ext_ms = load_u16(r + 0x12);
    out->group_first = load_u16(r + 0x14);
    out->group_count = load_u16(r + 0x16);
    return true;
  }

  bool group(uint32_t i, GroupView *out) const {
    if (this->base_ == nullptr || i >= this->group_count_)
      return false;
    const uint8_t *r = this->base_ + this->groups_off_ + static_cast<size_t>(i) * GROUP_RECORD_SIZE;
    out->name = this->string(load_u32(r + 0x00));
    out->req_off = load_u32(r + 0x04);
    out->req_len = r[0x08];
    out->sid = r[0x09];
    out->flags = load_u16(r + 0x0A);
    out->field_first = load_u16(r + 0x0C);
    out->field_count = load_u16(r + 0x0E);
    out->did = load_u16(r + 0x10);
    out->resp_min_len = load_u16(r + 0x12);
    return true;
  }

  bool field(uint32_t i, FieldView *out) const {
    if (this->base_ == nullptr || i >= this->field_count_)
      return false;
    const uint8_t *r = this->base_ + this->fields_off_ + static_cast<size_t>(i) * FIELD_RECORD_SIZE;
    out->name = this->string(load_u32(r + 0x00));
    out->unit = this->string(load_u32(r + 0x04));
    out->bit_pos = load_u16(r + 0x08);
    out->bit_size = r[0x0A];
    out->flags = r[0x0B];
    out->scale_first = load_u16(r + 0x0C);
    out->scale_count = r[0x0E];
    out->repeat_count = r[0x0F];
    out->repeat_stride = load_u16(r + 0x10);
    out->group_index = load_u16(r + 0x12);
    return true;
  }

  bool scale(uint32_t i, ScaleView *out) const {
    if (this->base_ == nullptr || i >= this->scale_count_)
      return false;
    const uint8_t *r = this->base_ + this->scales_off_ + static_cast<size_t>(i) * SCALE_RECORD_SIZE;
    out->low = load_i32(r + 0x00);
    out->high = load_i32(r + 0x04);
    out->factor = load_f32(r + 0x08);
    out->offset = load_f32(r + 0x0C);
    out->text = this->string(load_u32(r + 0x10));
    out->flags = load_u16(r + 0x14);
    return true;
  }

  /// The group's request bytes, or nullptr when its pool reference does not fit the pool. A CRC
  /// pass does not make record contents honest, so this is checked here and not assumed.
  /// Resolve a group's request bytes.
  ///
  /// `req_off` is an **absolute** blob offset, not pool-relative — §1's general rule is that an
  /// "offset" counts from byte 0, and only *string refs* are relative to their pool. Adding
  /// `reqbytes_off_` here instead landed inside the string pool: every request read back as ASCII
  /// text, `req[0] != sid`, and `build_request()` refused every group on the bench while the
  /// catalog itself opened cleanly. Host tests could not see it because they only ever read blobs
  /// this same code's sibling builder had produced; `test_uds_fixture.cpp` now reads the Python
  /// compiler's own artifact, which is the only place a disagreement like this can surface.
  const uint8_t *request_bytes(const GroupView &g) const {
    if (this->base_ == nullptr || g.req_len == 0)
      return nullptr;
    // Must lie wholly inside the request-byte pool, which is what makes the absolute offset safe.
    if (g.req_off < this->reqbytes_off_)
      return nullptr;
    const uint64_t rel = static_cast<uint64_t>(g.req_off) - this->reqbytes_off_;
    if (rel + g.req_len > this->reqbytes_len_)
      return nullptr;
    return this->base_ + g.req_off;
  }

  /// Resolve a string ref (pool-relative; see the header comment). Always returns a valid
  /// NUL-terminated pointer: in-pool refs terminate before the pool's mandatory trailing NUL, and
  /// an out-of-pool ref degrades to "" rather than to a read outside the mapping.
  const char *string(uint32_t ref) const {
    if (this->base_ == nullptr || ref >= this->strings_len_)
      return "";
    return reinterpret_cast<const char *>(this->base_ + this->strings_off_ + ref);
  }

  /// Look up a name of the given kind: binary search over the hash index (§7), then a linear walk
  /// over equal hashes, each candidate verified by strcmp against **the entry's own `name_ref`** —
  /// an FNV-1a collision must resolve to a miss, never to the wrong record.
  ///
  /// Verifying against the entry rather than the target record is what makes alias lookups work:
  /// a merged group carries one canonical name, so an original service qualifier folded into it
  /// (§7 gives every one of them its own entry) can never match the *record's* name. That was the
  /// first version of this format, and it made every alias a verified miss — the entry now
  /// carries the name it was hashed from, and `service: DT_Cell_Voltage_Maximum_Cell_Voltage`
  /// resolves to the group without anyone inventing a name for it.
  bool find(const char *name, NameKind kind, uint32_t *index_out) const {
    if (this->base_ == nullptr || name == nullptr)
      return false;
    const uint32_t h = fnv1a(name);
    // Lower bound over the hash column. Entries are sorted ascending by hash, ties by target.
    uint32_t lo = 0;
    uint32_t hi = this->index_count_;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (load_u32(this->base_ + this->index_off_ + static_cast<size_t>(mid) * INDEX_RECORD_SIZE) < h) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    for (uint32_t i = lo; i < this->index_count_; i++) {
      const uint8_t *e = this->base_ + this->index_off_ + static_cast<size_t>(i) * INDEX_RECORD_SIZE;
      if (load_u32(e) != h)
        break;
      const uint32_t target = load_u32(e + 8);
      if (static_cast<uint8_t>(target >> 28) != static_cast<uint8_t>(kind))
        continue;
      if (std::strcmp(this->string(load_u32(e + 4)), name) != 0)
        continue;  // hash hit, name mismatch: a collision, and it must not resolve
      const uint32_t idx = target & 0x0FFFFFFF;
      // The entry's own name verified, but the index it points at still has to exist — a CRC pass
      // proves the blob is intact, not that its target words are in range.
      if (!this->index_in_range_(kind, idx))
        continue;
      *index_out = idx;
      return true;
    }
    return false;
  }

 protected:
  /// §2 step 3 for one table. An empty table is accepted whatever its offset says: the range is
  /// empty, the accessors never dereference it, and rejecting a compiler's choice of offset for
  /// zero records would be strictness without safety.
  static bool table_ok_(uint32_t off, uint32_t count, size_t record_size, uint32_t total) {
    if (count == 0)
      return true;
    if (off % 4 != 0)
      return false;
    const uint64_t end = static_cast<uint64_t>(off) + static_cast<uint64_t>(count) * record_size;
    return off >= CATALOG_MIN_SIZE && end <= total;
  }

  /// §2 step 4 for one pool. Pools are byte pools — no alignment requirement in the spec, none
  /// imposed here.
  static bool range_ok_(uint32_t off, uint32_t len, uint32_t total) {
    if (len == 0)
      return true;
    return off >= CATALOG_MIN_SIZE && static_cast<uint64_t>(off) + len <= total;
  }

  bool index_in_range_(NameKind kind, uint32_t idx) const {
    switch (kind) {
      case NameKind::ECU:
        return idx < this->ecu_count_;
      case NameKind::GROUP:
        return idx < this->group_count_;
      case NameKind::FIELD:
        return idx < this->field_count_;
    }
    return false;
  }

  const uint8_t *base_{nullptr};
  uint32_t total_size_{0};
  uint32_t ecus_off_{0};
  uint32_t groups_off_{0};
  uint32_t fields_off_{0};
  uint32_t scales_off_{0};
  uint32_t reqbytes_off_{0};
  uint32_t reqbytes_len_{0};
  uint32_t strings_off_{0};
  uint32_t strings_len_{0};
  uint32_t index_off_{0};
  uint32_t index_count_{0};
  uint32_t crc_{0};
  uint16_t ecu_count_{0};
  uint16_t group_count_{0};
  uint16_t field_count_{0};
  uint16_t scale_count_{0};
  uint16_t flags_{0};
};

}  // namespace esphome::uds
