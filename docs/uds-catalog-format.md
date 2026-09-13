# The `.dcat` diagnostic catalog — normative binary format v1

A `.dcat` file describes **what a diagnostic ECU can be asked and how to decode
its answers**: request bytes, response field geometry, scaling, units, enum and
sentinel texts. It is produced on a workstation by `script/uds_catalog.py` from a
factory diagnostic database, and consumed on the device by `components/uds`.

**Why a binary blob and not YAML.** The catalog is read from **memory-mapped
flash**: `esp_partition_mmap()` makes the partition directly addressable, so
lookups run against flash with **zero heap** and no parse step at boot. Records
are therefore fixed-width, 4-byte aligned and little-endian — the exact in-memory
layout of the C structs on an ESP32-C6, so the reader casts rather than copies.
A 4 MB C6 spends 128 KB of each app slot per 256 KB of catalog; firmware is
~230 KB in a 1.8 MB slot, so this is affordable.

**Why this document is normative.** Two independent implementations must agree:
the Python compiler/validator (`components/uds/catalog.py`, imported by both the
CLI and ESPHome codegen) and the C++ reader (`components/uds/uds_catalog.h`).
They meet on one checked-in artifact — `tests/uds/fixtures/mini.dcat` — which
pytest asserts byte-identical to a fresh compile and `tests/host` reads back.
If those two ever disagree, this file is the tiebreaker.

## 1. Conventions

- **Endianness:** all multi-byte integers little-endian. Floats are IEEE-754
  binary32, little-endian.
- **Alignment:** every table starts at a 4-byte aligned absolute offset; every
  record size is a multiple of 4. Pad bytes are zero.
- **Offsets** are absolute from byte 0 of the blob, `uint32_t`.
- **Indices** are `uint16_t` into their table. `0xFFFF` means "none".
- **String refs** are `uint32_t` offsets **relative to `strings_off`**, pointing
  at a NUL-terminated UTF-8 byte string. Ref 0 is therefore the pool's first
  byte, which is always `\0`, so a ref of 0 reads as the empty string and means
  "none". A reader must never need a length to use a string ref.

  *Corrected 2026-07-30, before either implementation shipped.* This clause
  originally said "absolute offsets" **and** that ref 0 reads as empty because
  the pool starts with `\0`. Those cannot both hold: an absolute offset of 0 is
  the magic bytes, not the pool. The C++ reader raised it during
  implementation. Pool-relative is the version that makes the reserved-zero rule
  true, so that is the rule.
- **Reserved fields** are written zero and ignored on read. A reader must not
  reject a blob because a reserved field is non-zero — that is the forward
  compatibility hinge.

## 2. Header — 64 bytes at offset 0

| off | type | field | meaning |
|---|---|---|---|
| 0x00 | u8[8] | `magic` | `ESPDCAT\0` exactly |
| 0x08 | u16 | `format_version` | 1. A reader refuses a version it does not know |
| 0x0A | u16 | `flags` | bit0 `NAMES` (string pool holds qualifiers), bit1 `TEXTS` (enum/sentinel texts present). Others reserved |
| 0x0C | u32 | `total_size` | size of the whole blob in bytes, header included |
| 0x10 | u32 | `crc32` | CRC-32 (IEEE 802.3, zlib polynomial, init `0xFFFFFFFF`, final xor, reflected) over bytes `[64, total_size)` |
| 0x14 | u32 | `ecus_off` | |
| 0x18 | u16 | `ecu_count` | |
| 0x1A | u16 | `reserved0` | |
| 0x1C | u32 | `groups_off` | |
| 0x20 | u16 | `group_count` | |
| 0x22 | u16 | `reserved1` | |
| 0x24 | u32 | `fields_off` | |
| 0x28 | u16 | `field_count` | |
| 0x2A | u16 | `reserved2` | |
| 0x2C | u32 | `scales_off` | |
| 0x30 | u16 | `scale_count` | |
| 0x32 | u16 | `reserved3` | |
| 0x34 | u32 | `reqbytes_off` | request-byte pool |
| 0x38 | u32 | `reqbytes_len` | |
| 0x3C | u32 | `strings_off` | |

The remaining two pool descriptors and the index descriptor follow the header in
a fixed 16-byte **header extension** at offset 0x40, kept separate only so the
header itself stays exactly 64 bytes:

| off | type | field |
|---|---|---|
| 0x40 | u32 | `strings_len` |
| 0x44 | u32 | `index_off` |
| 0x48 | u32 | `index_count` |
| 0x4C | u32 | `reserved4` |

So the first table may start at 0x50 or later. **`crc32` covers `[64, total_size)`**,
which includes the extension — the CRC is computed over everything except the
first 64 bytes, whose only mutable field is the CRC itself.

**Validation a reader MUST perform before trusting any record** (all cheap, all
at `open()`):

1. `magic` matches and `format_version == 1`.
2. `total_size` is between 0x50 and the mapped region size.
3. Every table's `[off, off + count * record_size)` lies inside
   `[0x50, total_size)` and `off % 4 == 0`. A table with `count == 0` is vacuous:
   its offset is not checked, because there is nothing to address.
4. Both pools lie inside `[0x50, total_size)`; the string pool's first byte is
   `\0` and its last byte is `\0` (so every string ref is NUL-terminated inside
   the pool — this is what makes `const char *` returns safe without a length).
5. `crc32` matches.

A failure at any step means the catalog is unavailable: the component logs once,
publishes nothing, and keeps running. **It must never crash, and must never read
outside the mapped region** — `tests/host` truncates a valid blob at every
possible length and asserts exactly that under ASan.

## 3. Ecu — 24 bytes

One per diagnostic ECU. Carries the transport parameters straight out of the
factory database, so YAML does not have to restate CAN identifiers.

| off | type | field | meaning |
|---|---|---|---|
| 0x00 | u32 | `name_ref` | qualifier, e.g. `MINI` |
| 0x04 | u32 | `request_id` | CAN id the tester sends on, e.g. `0x7E7` |
| 0x08 | u32 | `response_id` | CAN id the ECU answers on, e.g. `0x7EF` |
| 0x0C | u16 | `flags` | bit0 `EXTENDED_ID` (29-bit) |
| 0x0E | u8 | `block_size` | ISO-TP BS we should grant, from `CP_BLOCKSIZE_SUG` |
| 0x0F | u8 | `st_min_raw` | STmin in raw ISO encoding, from `CP_STMIN_SUG` |
| 0x10 | u16 | `p2_ms` | response deadline, from `CP_P2_TIMEOUT` |
| 0x12 | u16 | `p2_ext_ms` | extended deadline after NRC 0x78, from `CP_P2_EXT_TIMEOUT_7F_78` |
| 0x14 | u16 | `group_first` | first request group belonging to this ECU |
| 0x16 | u16 | `group_count` | |

Groups belonging to one ECU are contiguous, so an ECU's groups are a range and
never a list.

## 4. Group — 20 bytes

**A group is one request and every field that can be decoded from its response.**
This is the load-bearing idea of the format, and it comes from the data: in a
real factory database, `22 01 02` is *three* separate services
(`DT_Mini_Temperature_Averaged`, `…Maximum…`, `…Minimum…`, see the `mini`
fixture's DID `0x0102`) with identical request bytes. Keyed by service, one
poll of "temperatures" would be three requests on the wire for the same three
bytes. Keyed by request, it is one request and three fields. Deduplicating this
way can collapse a factory database's thousand-odd read services into a few
dozen pollable groups.

| off | type | field | meaning |
|---|---|---|---|
| 0x00 | u32 | `name_ref` | canonical name (see below) |
| 0x04 | u32 | `req_off` | offset into the request-byte pool |
| 0x08 | u8 | `req_len` | request length in bytes, e.g. 3 for `22 01 01` |
| 0x09 | u8 | `sid` | service identifier, the first request byte |
| 0x0A | u16 | `flags` | bit0 `SAFE_READ`, bit1 `NEEDS_SESSION`, bit2 `NEEDS_SECURITY`; bits 8–11 security level |
| 0x0C | u16 | `field_first` | |
| 0x0E | u16 | `field_count` | |
| 0x10 | u16 | `did` | the 16-bit identifier for SID 0x22, else `0xFFFF` |
| 0x12 | u16 | `resp_min_len` | smallest positive-response length, in **bytes**, from which every field of this group is fully extractable |

`resp_min_len` is precomputed by the compiler as
`ceil(max(bit_pos + bit_size) / 8)` over the group's fields.

**It is a fast-path hint, not a gate** — and saying otherwise was a defect in this
document, caught while writing the bench config. A response at least
`resp_min_len` long needs no per-field bounds check at all: every field is
covered, so the loop can skip the arithmetic. A **shorter** response is
`PARTIAL`, not rejected: the fields it does cover are decoded, the fields it does
not are skipped and counted, and the response still counts as a success.

The reason this matters is not hypothetical. A factory database can declare
more array elements than a particular unit actually has wired — mini's DID
`0x0208` models four cells; a real ECU of that family with only two installed
would answer with a shorter response. Treated as a gate, that would reject the
entire response and every cell voltage reads unavailable against a perfectly
healthy unit — the exact class of silent, plausible failure this format is
written to avoid. An ECU answering with fewer elements than its database
declares is ordinary, not malformed.

The only length that genuinely rejects a response is the one that makes it
unreadable as a response at all: fewer than `1 + echo_len` bytes, so the service
identifier and its echo cannot be checked (design §4). Everything above that is
data, however little of it there is.

`SAFE_READ` is set **only** for service identifiers that cannot change ECU state:
`0x22` ReadDataByIdentifier, `0x1A` ReadEcuIdentification, `0x21`
ReadDataByLocalIdentifier, `0x19` ReadDTCInformation, `0x03`/`0x07`/`0x17`
(read/report DTC). Everything else — sessions, routines, IO control, writes,
security access, reset — is left clear. The component's scheduler polls
`SAFE_READ` groups only; see the design doc for the gate on the rest.

**Canonical group name.** When several services share request bytes, the
compiler emits the longest common `_`-separated prefix of their qualifiers, with
trailing separators stripped (`DT_Mini_Temperature`, from `_Maximum`/`_Minimum`/
`_Averaged`). If that is empty it emits the first qualifier in file order.
Every original service qualifier is still resolvable: each one gets its own
name-index entry pointing at the group (§7). So `service:
DT_Mini_Temperature_Maximum` and `service: 0x0102` both find the same group,
and neither name is invented.

## 5. Field — 20 bytes

| off | type | field | meaning |
|---|---|---|---|
| 0x00 | u32 | `name_ref` | presentation qualifier, e.g. `PRES_Mini_Temp_2Byte` |
| 0x04 | u32 | `unit_ref` | displayed unit, e.g. `V`; 0 when unitless |
| 0x08 | u16 | `bit_pos` | MSB-first bit position **within the whole positive response, header included** |
| 0x0A | u8 | `bit_size` | 1–64 |
| 0x0B | u8 | `flags` | bit0 `SIGNED` (two's complement), bit1 `BYTESWAP`, bit2 `ASCII`, bit3 `HEXDUMP`, bit4 `ENUM` |
| 0x0C | u16 | `scale_first` | |
| 0x0E | u8 | `scale_count` | 0 means "raw value, no scaling" |
| 0x0F | u8 | `repeat_count` | 1 for a scalar; N for an array of N elements |
| 0x10 | u16 | `repeat_stride` | bit distance between array elements; 0 when `repeat_count == 1` |
| 0x12 | u16 | `group_index` | the group this field belongs to — a field alone is addressable |

`bit_pos` counts from the first byte of the positive response, so for a
`0x22` read the first payload byte begins at bit 24 (after `62 <DIDhi> <DIDlo>`).
This is deliberately the same origin the factory database uses, so no rebasing
happens anywhere in the toolchain — rebasing is how off-by-one-byte decode bugs
get in.

### 5.1a Field names are unique within a group — the compiler's obligation

**A group's field names MUST be distinct.** This is a compiler obligation, not a
property of the source database, and the format does not carry a disambiguator:
YAML addresses a field by group plus name, and a duplicate name inside one group
makes that address meaningless.

A factory database does produce duplicates, which is how this rule was found.
In the `mini` fixture, DID `0x0101`'s maximum and minimum cell voltage are
**both** called `PRES_Mini_Voltage_2Byte`, in the same group, because they came
from two different services that each name their one output the same way.
`service:` cannot separate them either, since both services merged into the
same request group.

The compiler picks each field's name by the first of these that is unique within
the group, so a name is invented only when nothing factual is left:

1. the presentation qualifier (e.g. `PRES_Mini_Voltage_2Byte`);
2. the **originating service qualifier** — the service whose response
   preparation contributed this field (`DT_Mini_Voltage_Maximum_Cell`).
   Still a factory-style name, and unique whenever each service contributes one
   field, which covers the interesting scalar cases;
3. the presentation qualifier with `@<byte>.<bit>` appended
   (`PRES_Mini_Temp_2Byte@2.0`), for the case where one service
   contributes several identically named fields.

Every field's final name is listed in the `.manifest.json`, which is what a YAML
author reads. A compiler that cannot make a group's names unique must fail rather
than emit the group.

### 5.1b Arrays are collapsed, and that is how a cell is addressed

**A run of three or more fields with the same name, identical geometry and a
constant bit stride MUST be collapsed** into one record with `repeat_count` and
`repeat_stride` set — **subject to the three guards below, without which this rule
corrupts real data.** Mini's DID `0x0208` is four cell voltages sharing one
presentation qualifier; emitting four records would be four duplicate names in
one group, which §5.1a forbids, and disambiguating them positionally would give
a YAML author names differing only by an offset.

**The guards.** Each was found by applying the rule literally to a real factory
database and reproducing the damage; none of them ever *forces* a collapse, so
everything this rule intends still collapses.

1. **The originating service names must match once digits are stripped.**
   Mini's DID `0x0102` is deliberately this trap: three services —
   `DT_Mini_Temperature_Maximum`, `_Minimum`, `_Averaged` — share one
   presentation, `PRES_Mini_Temp_2Byte`, at a constant stride of 16 bits: a
   textbook array by geometry, and three completely different measurements.
   Unguarded they fuse into a bogus 3-element array and the distinction is
   gone. This shape recurs anywhere a factory database groups several
   differently-named measurements under one shared presentation type.
2. **Elements must be adjacent in bit order.** A pack-wide average stored
   immediately before a per-cell array, at the same presentation and the same
   geometry, is a real shape a factory database can produce — without
   adjacency it would be silently absorbed as element 0, a whole-pack average
   masquerading as the first cell.
3. **A majority of trailing numbers must equal their ordinal.** A factory
   database can also name a run of fields by a threshold rather than a
   position — `..._Class_0_2A`, `..._Class_5_0A`, `..._Class_50_0A` for current
   thresholds, not indices. Collapsing them trades self-describing factory
   names for `element: N` on a name ending in "ClassA". *Majority* and not
   unanimity, because a factory database can break its own naming pattern
   once — a single element ending in a number that does not match its
   position — and one such slip must not un-array an otherwise-clean run.

**Element numbering is normative: element 0 is the factory's element 1.** The
database numbers cells from 1, so `element: 0` is cell 1. Two compilers could
legitimately have differed here, so it is pinned; the manifest records the first
and last elements' originating service names so an author can confirm it.

The `@<byte>.<bit>` suffix of §5.1a counts **from the first byte of the response,
header included** — the same origin as `bit_pos`, so a field at bit 40 is `@5.0`
and never `@2.0`. The suffix is a user-visible string, and the toolchain rebases
nothing anywhere else.

§5.1a's uniqueness rule is enforced by the **writer**, not the reader: it is not in
§2's validation list, because rejecting a blob for a duplicate name would make a
reader stricter than the format and turn a cosmetic fault into an inert component.
`uds_catalog.py verify` catches it by re-running the writer over what it read.

Collapsed, the array is one name plus an element index, which is what a config
actually wants to say — cell 42 rather than the field at bit 696. Fields are
ordered by ascending `bit_pos` within a group, so element *k* is element *k* in
both implementations.

This reverses the original v1 instruction ("emit `repeat_count == 1`, collapsing
is for later"), which was written to protect name fidelity and turned out to cost
it: uncollapsed arrays are precisely the case where fidelity is unachievable.
Readers already had to implement the stride, so no reader changes.

### 5.1 Extraction — normative algorithm

```
raw = 0
for i in 0 .. bit_size-1:
    bit = (response[(bit_pos + k*repeat_stride + i) / 8] >> (7 - ((bit_pos + k*repeat_stride + i) % 8))) & 1
    raw = (raw << 1) | bit
if BYTESWAP:  raw = byteswap(raw, ceil(bit_size/8))
if SIGNED and bit_size < 64 and (raw >> (bit_size-1)) & 1:
    raw -= (1 << bit_size)          # sign-extend
```

Bit numbering is MSB-first within each byte, and bits run across byte boundaries
without gaps. `BYTESWAP` covers the database's little-endian exception (the
factory `byte_order` field; 0 means MSB-first, which is the common case and the
flag's cleared state).

`ASCII` fields are not numeric: the reader copies `bit_size / 8` bytes verbatim
and the component publishes them to a text sensor. `HEXDUMP` fields are
formatted as uppercase hex pairs. Both ignore `scale_*`. **Both require byte
alignment** — `bit_pos % 8 == 0` and `bit_size % 8 == 0`; a reader refuses any
other geometry rather than inventing a byte boundary, and a compiler must not
emit one.

## 6. Scale — 24 bytes

A field's scale rows are tried **in table order**; the first row whose bounds
contain `raw` wins. This one mechanism expresses linear scaling, enumerations
and "signal not available" sentinels, exactly as the factory database does.

| off | type | field | meaning |
|---|---|---|---|
| 0x00 | i32 | `low` | inclusive lower bound on the **raw** value |
| 0x04 | i32 | `high` | inclusive upper bound |
| 0x08 | f32 | `factor` | multiplier |
| 0x0C | f32 | `offset` | added constant |
| 0x10 | u32 | `text_ref` | enum/sentinel text, 0 when none |
| 0x14 | u16 | `flags` | bit0 `TEXT`, bit1 `INVALID` |
| 0x16 | u16 | `reserved` | |

Evaluation:

- row with `TEXT` and `INVALID` → **the value is not available**. A numeric
  sensor publishes `NaN`; a text sensor publishes the text.
- row with `TEXT` only → an enumerated state. Text sensor publishes the text; a
  numeric sensor publishes `raw * factor + offset`, which for a pure enum row
  (`factor == 0`) is the offset, usually the raw code itself when the compiler
  sets `factor = 1, offset = 0`.
- row without `TEXT` → `raw * factor + offset`.
- **no row matches** → not available: `NaN`, and a counter increments. A raw
  value outside every declared range is a decode we do not understand, and
  publishing a confident number for it is the failure mode this rule exists to
  prevent.

`INVALID` is **authoritative on its own**: a row with `INVALID` and no `TEXT`
still means not-available, and yields `NaN` with no text. The flag says what the
value *is*; `TEXT` only says whether there is a string to show for it.

`INVALID` is set by the **compiler**, not the runtime, from the row's text
against a documented pattern list (`not available` including the factory's
`not asvailable` typo, `nicht verfügbar`, `invalid`, `SNA`, `no signal`), and any
profile may override it per row. This is deliberate, and it is the lesson from
`can_gateway`'s SNA work: an all-ones raw value is *usually* "not available" but
sometimes a real state — a bus signal's all-ones code can just as well be a
genuine fault enum as a sentinel. A per-row decision made where the database is
readable, and reviewable in the profile, is the only form of this rule that
does not erase real faults.

## 7. Name index — 12 bytes per entry

Lookup by name must not need RAM, so names are found by binary search over a
sorted table of hashes and verified against the mmap'd string pool.

| off | type | field |
|---|---|---|
| 0x00 | u32 | `hash` — FNV-1a 32 over the name's bytes, **case-sensitive**, NUL excluded |
| 0x04 | u32 | `name_ref` — the exact name this entry was hashed from |
| 0x08 | u32 | `target` — kind in bits 28–31 (1 = ECU, 2 = GROUP, 3 = FIELD), index in bits 0–27 |

Entries are sorted ascending by `hash`, ties broken by ascending `target`, so a
binary search plus a linear walk over equal hashes is deterministic. On a hash
match the reader **must** `strcmp` **the entry's own `name_ref`** against the
query; FNV-1a collisions are rare, not impossible, and a collision must resolve
to a miss rather than to the wrong field.

*`name_ref` added 2026-07-30, before either implementation shipped.* The entry
was originally `{hash, target}` and verification was specified against the
*record's* name. The C++ reader proved that unimplementable for the alias
entries this same section requires: a merged group carries one canonical name, so
an alias — an original service qualifier folded into that group — could never
match, and every alias lookup became a verified miss. Either the aliases go or
the entry carries the name it was built from. The aliases are the whole reason
`service: DT_Mini_Temperature_Maximum` resolves without inventing
names, so the entry grew by four bytes: a few hundred entries is well under a
kilobyte, against a promise the format would otherwise have documented and
broken.

The index contains, for every ECU: its qualifier. For every group: its canonical
name **and every original service qualifier that mapped into it**. For every
field: its qualifier. A field qualifier that occurs in more than one group is
ambiguous by name alone; the Python validator rejects such a reference at
`esphome config` time and lists the candidate groups, so `field:` + `service:`
disambiguates before a build ever runs.

## 7a. Rules the compiler follows when the database is ambiguous

Every one of these came from compiling a real factory database. They are
written down because the alternative is two compilers guessing differently, and
because each one silently changes decoded values.

- **`bit_size` above 64 is legal only for `ASCII` and `HEXDUMP`,** up to the
  `uint8_t` ceiling of 248 bits, byte-aligned. The 64-bit limit in §8 bounds the
  *numeric accumulator*; a 17-character VIN is a 136-bit block and is not a
  number. Numeric fields above 64 bits are a hard error.
- **Blocks above 248 bits are dropped, loudly** — a warning plus a
  `skipped_fields` entry in the manifest. A factory database can carry
  whole-payload hexdump conveniences hundreds of bytes long that duplicate
  per-field data byte for byte, so nothing is lost; what must not happen is a
  silent omission.
- **A scale row with `(low, high) == (0, 0)` means "bounds absent", not "raw 0
  only".** The database writes it for plain linear conversions, and reading it
  literally makes every real value fall through to not-available for that
  field. The compiler widens such a row to the field's full range and records
  it in the manifest.
- **A sentinel that an `i32` bound cannot express is dropped, not clamped.** A
  full-range `u32` field declares its SNA at 2³²−1, which does not fit `low`/
  `high`. Clamping it onto the linear row's end hides it behind that row; dropping
  it is correct because §6's final rule already covers the value — a raw outside
  every row is not-available, which is exactly what the sentinel meant. The
  no-match rule is the safety net that makes this lossless.
- **`NEEDS_SESSION` is derived from `client_access_level > 1`, `NEEDS_SECURITY`
  from `security_access_level > 0`,** and the security level is stored as
  declared. Mini's `DT_Mini_Guarded_Block` (DID `0x0318`, `client_access_level:
  5`, `security_access_level: 3`) exercises exactly this: one flagged group
  among otherwise-plain reads, matching how a richer ECU variant typically
  gates just its non-default-session services.

## 8. Limits, and what the compiler must enforce

| quantity | limit | why |
|---|---|---|
| ECUs, groups, fields, scales | 65534 each | `u16` indices, `0xFFFF` reserved |
| `bit_size` | 64 | the extraction accumulator |
| `bit_pos + bit_size` | 65535 | `u16 bit_pos`; a 512-byte response reaches 4096 |
| `req_len` | 255 | `u8` |
| total size | ≤ partition size | the flasher refuses otherwise |

The compiler fails loudly on any breach. Sizes measured on a real factory
database we compiled: a curated few-dozen-DID live-value profile is
single-digit KB of JSON before packing and lands in single-digit KB packed; a
full multi-hundred-service variant is on the order of a few hundred KB of JSON
and still fits a 256 KB partition packed. Both are inside budget, so the
profile is a readability choice rather than a capacity one.

**The one place it does not fail loudly, and it has already cost a session:** a
group appended to the profile *after* the `invalid_overrides:` block is
**silently ignored**. A catalog built that way compiles byte-identical to the one
without the addition — the same size, the same CRC, no warning — and the only
symptom is that the DID you added never appears. Moving the entry inside
`groups:` fixes it. A validator that rejected foreign keys inside
`invalid_overrides:` would have said so; until one exists, `dump` the catalog and
count the groups after editing a profile.

## 9. Versioning

`format_version` is bumped only for a change that a v1 reader would
mis-interpret. Appending a table (with its descriptor in the reserved header
words), or defining a reserved flag bit whose cleared state is the current
behaviour, is a v1-compatible change: readers ignore what they do not know, and
must not reject a blob for containing it.
