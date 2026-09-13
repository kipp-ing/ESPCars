"""Reader/writer for the ``.dcat`` diagnostic catalog (docs/uds-catalog-format.md).

This module is the *Python half* of the two implementations the format doc
binds together: the C++ reader (``uds_catalog.h``) casts mmap'd flash, this
module compiles and validates the same bytes on the workstation. Both are held
to the normative spec, and they meet on ``tests/uds/fixtures/mini.dcat`` —
if this module and the C++ reader ever disagree, the format doc wins.

Why one shared module and not code in the CLI: ESPHome codegen must resolve
``field:`` names at ``esphome config`` time against the same blob the CLI
compiled, and a second implementation of the layout would drift. So the CLI
(``script/uds_catalog.py``) and codegen both import this file. It is therefore
**stdlib only** — codegen runs inside stock esphome and must not grow a
dependency.

The dataclasses here are the semantic model (names, not offsets): ``Ecu`` owns
``Group`` owns ``Field`` owns ``Scale``. ``write_catalog`` derives everything
positional — table offsets, string/request pools, the sorted name index, the
CRC — so nothing offset-shaped can be constructed wrong by a caller.
``read_catalog`` reverses that and performs every validation the spec's §2
demands *before* trusting a single record, because on the device this blob is
mmap'd flash that may be stale, truncated or garbage, and the failure mode has
to be "catalog unavailable", never a wrong decode.
"""

from __future__ import annotations

import difflib
import struct
import zlib
from dataclasses import dataclass, field
from functools import cached_property

# --- format constants (spec §1, §2) ---------------------------------------

MAGIC = b"ESPDCAT\0"
FORMAT_VERSION = 1
HEADER_SIZE = 0x40  # fixed header; the 16-byte extension follows at 0x40
EXT_SIZE = 0x10
FIRST_TABLE = 0x50  # first byte any table or pool may occupy

# header flags
FLAG_NAMES = 1 << 0  # string pool holds qualifiers
FLAG_TEXTS = 1 << 1  # enum/sentinel texts present

# Ecu.flags
ECU_FLAG_EXTENDED_ID = 1 << 0

# Group.flags
GROUP_FLAG_SAFE_READ = 1 << 0
GROUP_FLAG_NEEDS_SESSION = 1 << 1
GROUP_FLAG_NEEDS_SECURITY = 1 << 2
GROUP_SECURITY_SHIFT = 8  # bits 8-11: security level

# SAFE_READ is set only for service identifiers that cannot change ECU state
# (spec §4). This set is the entire safety argument of the polling scheduler,
# so it is defined once, here, and tested — not repeated in the compiler.
SAFE_READ_SIDS = frozenset({0x22, 0x1A, 0x21, 0x19, 0x03, 0x07, 0x17})

# Field.flags
FIELD_FLAG_SIGNED = 1 << 0
FIELD_FLAG_BYTESWAP = 1 << 1
FIELD_FLAG_ASCII = 1 << 2
FIELD_FLAG_HEXDUMP = 1 << 3
FIELD_FLAG_ENUM = 1 << 4

# Scale.flags
SCALE_FLAG_TEXT = 1 << 0
SCALE_FLAG_INVALID = 1 << 1

# name-index target kinds (bits 28-31 of `target`)
KIND_ECU = 1
KIND_GROUP = 2
KIND_FIELD = 3
_KIND_NAMES = {KIND_ECU: "ecu", KIND_GROUP: "group", KIND_FIELD: "field"}
_KIND_BY_NAME = {v: k for k, v in _KIND_NAMES.items()}

NO_INDEX = 0xFFFF  # u16 "none"

# record sizes (spec §3-§7)
ECU_SIZE = 24
GROUP_SIZE = 20
FIELD_SIZE = 20
SCALE_SIZE = 24
INDEX_ENTRY_SIZE = 12  # {hash, name_ref, target} — name_ref added 2026-07-30

_HEADER = struct.Struct("<8sHHIIIHHIHHIHHIHHIIIIIII")  # header + extension, 0x50 bytes
_ECU = struct.Struct("<IIIHBBHHHH")
_GROUP = struct.Struct("<IIBBHHHHH")
_FIELD = struct.Struct("<IIHBBHBBHH")
_SCALE = struct.Struct("<iiffIHH")
_INDEX = struct.Struct("<III")
assert _HEADER.size == HEADER_SIZE + EXT_SIZE
assert _ECU.size == ECU_SIZE and _GROUP.size == GROUP_SIZE
assert _FIELD.size == FIELD_SIZE and _SCALE.size == SCALE_SIZE
assert _INDEX.size == INDEX_ENTRY_SIZE

# limits (spec §8)
MAX_RECORDS = 65534  # u16 indices, 0xFFFF reserved
MAX_NUMERIC_BITS = 64  # the extraction accumulator
# ASCII/HEXDUMP fields bypass the accumulator (the reader copies bytes
# verbatim, spec §5.1), so the 64-bit cap does not apply to them; the u8
# `bit_size` caps them at 248 bits = 31 bytes. Normative per §7a: above 64
# bits is legal for blocks only, byte-aligned; numeric is a hard error.
MAX_BLOCK_BITS = 248
MAX_BIT_END = 65535  # u16 bit_pos
MAX_REQ_LEN = 255  # u8 req_len
MAX_REPEAT_COUNT = 255  # u8 repeat_count (spec §5)

FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193
I32_MIN = -(1 << 31)
I32_MAX = (1 << 31) - 1


def fnv1a32(name: str | bytes) -> int:
    """FNV-1a 32 over the name's bytes, case-sensitive, NUL excluded (§7)."""
    if isinstance(name, str):
        name = name.encode("utf-8")
    h = FNV_OFFSET
    for b in name:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


# --- errors -----------------------------------------------------------------


class CatalogError(Exception):
    """Base for everything this module raises deliberately."""


class FormatError(CatalogError):
    """The blob fails a spec §2 validation. The catalog is unavailable."""


class LimitError(CatalogError):
    """A spec §8 limit is breached. The compiler fails loudly, by design."""


class UnknownNameError(CatalogError):
    """A name resolves to nothing. Carries close matches so a YAML typo is a
    one-edit fix instead of a trip to `dump`."""

    def __init__(self, name: str, kind: str | None, suggestions: list[str]):
        self.name = name
        self.kind = kind
        self.suggestions = suggestions
        what = f"{kind} " if kind else ""
        msg = f"unknown {what}name {name!r}"
        if suggestions:
            msg += " — did you mean: " + ", ".join(suggestions)
        super().__init__(msg)


class AmbiguousNameError(CatalogError):
    """A name resolves to several records. Lists every candidate with its
    group, because a factory database really can reuse field qualifiers —
    even within one group (a real one has been seen naming a max and a min
    identically)."""

    def __init__(self, name: str, candidates: list[str]):
        self.name = name
        self.candidates = candidates
        super().__init__(
            f"name {name!r} is ambiguous — candidates: " + "; ".join(candidates)
        )


# --- the semantic model ------------------------------------------------------


@dataclass(frozen=True)
class Scale:
    """One row of a field's conversion table (spec §6).

    ``text == ""`` means a plain linear row (TEXT flag clear, text_ref 0).
    ``invalid`` is a *compiler* decision reviewable in the profile — the
    runtime never guesses what "not available" looks like (spec §6, and the
    lesson from can_gateway's SNA work).

    ``factor``/``offset`` are quantized to IEEE-754 binary32 on construction
    because that is all the record can hold (spec §6) — the dataclass then
    states exactly what the device will compute with, and ``read(write(cat))
    == cat`` holds without a tolerance.
    """

    low: int  # inclusive lower bound on the raw value
    high: int  # inclusive upper bound
    factor: float = 1.0
    offset: float = 0.0
    text: str = ""
    invalid: bool = False

    def __post_init__(self) -> None:
        for name in ("factor", "offset"):
            v = getattr(self, name)
            object.__setattr__(self, name, struct.unpack("<f", struct.pack("<f", v))[0])


@dataclass(frozen=True)
class Field:
    """One decodable value inside a group's response (spec §5).

    ``bit_pos`` counts MSB-first from the first byte of the positive response,
    header included — the same origin the factory database uses, so nothing is
    ever rebased (rebasing is how off-by-one-byte decode bugs get in).

    ``repeat_count`` > 1 makes this one record stand for an array of N
    equally-spaced elements (spec §5.1b): 0x0208's cell voltages are one
    field addressed by ``element:``, not one name per cell differing by an
    offset. ``name`` is unique within its group — a compiler obligation,
    §5.1a.
    """

    name: str
    bit_pos: int
    bit_size: int
    unit: str = ""
    signed: bool = False
    byteswap: bool = False
    ascii: bool = False
    hexdump: bool = False
    scales: tuple[Scale, ...] = ()
    repeat_count: int = 1
    repeat_stride: int = 0

    @property
    def bit_end(self) -> int:
        """One past the last bit this field (array included) can occupy."""
        return (
            self.bit_pos
            + (self.repeat_count - 1) * self.repeat_stride
            + self.bit_size
        )

    @property
    def is_enum(self) -> bool:
        """True when at least one valid enumerated state exists — the hint the
        codegen uses to steer a field toward a text sensor."""
        return any(s.text and not s.invalid for s in self.scales)

    @property
    def flags(self) -> int:
        f = 0
        if self.signed:
            f |= FIELD_FLAG_SIGNED
        if self.byteswap:
            f |= FIELD_FLAG_BYTESWAP
        if self.ascii:
            f |= FIELD_FLAG_ASCII
        if self.hexdump:
            f |= FIELD_FLAG_HEXDUMP
        if self.is_enum:
            f |= FIELD_FLAG_ENUM
        return f


@dataclass(frozen=True)
class Group:
    """One request and every field decodable from its response (spec §4).

    ``aliases`` are the original service qualifiers that merged into this
    group. They live only in the name index — the blob has no alias table —
    so they are normalised (sorted, deduplicated, canonical name removed) to
    keep `write(read(blob)) == blob` byte-exact.
    """

    name: str
    request: bytes
    fields: tuple[Field, ...] = ()
    aliases: tuple[str, ...] = ()
    needs_session: bool = False
    needs_security: bool = False
    security_level: int = 0

    def __post_init__(self) -> None:
        norm = tuple(sorted(set(self.aliases) - {self.name}))
        object.__setattr__(self, "aliases", norm)
        if isinstance(self.fields, list):
            object.__setattr__(self, "fields", tuple(self.fields))

    @property
    def sid(self) -> int:
        return self.request[0]

    @property
    def did(self) -> int:
        """The 16-bit identifier for SID 0x22, else 0xFFFF (spec §4)."""
        if self.sid == 0x22 and len(self.request) >= 3:
            return (self.request[1] << 8) | self.request[2]
        return NO_INDEX

    @property
    def safe_read(self) -> bool:
        """Derived, never stored on the dataclass: the SAFE_READ policy is the
        spec's, not the caller's, and must not be widenable from a profile."""
        return self.sid in SAFE_READ_SIDS

    @property
    def resp_min_len(self) -> int:
        """Smallest positive-response length covering every field, in bytes.

        Precomputed so the runtime's short-response guard is one comparison
        (spec §4)."""
        end = max((f.bit_end for f in self.fields), default=0)
        return (end + 7) // 8

    @property
    def flags(self) -> int:
        f = 0
        if self.safe_read:
            f |= GROUP_FLAG_SAFE_READ
        if self.needs_session:
            f |= GROUP_FLAG_NEEDS_SESSION
        if self.needs_security:
            f |= GROUP_FLAG_NEEDS_SECURITY
        f |= (self.security_level & 0xF) << GROUP_SECURITY_SHIFT
        return f


@dataclass(frozen=True)
class Ecu:
    """One diagnostic ECU with its transport parameters (spec §3), carried
    straight out of the factory database so YAML never restates CAN ids."""

    name: str
    request_id: int
    response_id: int
    extended_id: bool = False
    block_size: int = 0
    st_min_raw: int = 0
    p2_ms: int = 0
    p2_ext_ms: int = 0
    groups: tuple[Group, ...] = ()

    def __post_init__(self) -> None:
        if isinstance(self.groups, list):
            object.__setattr__(self, "groups", tuple(self.groups))

    @property
    def flags(self) -> int:
        return ECU_FLAG_EXTENDED_ID if self.extended_id else 0


@dataclass(frozen=True)
class Resolution:
    """What `Catalog.resolve` hands back: enough to reach the record and the
    structure around it without a second lookup."""

    kind: str  # "ecu" | "group" | "field"
    index: int  # index into the flat table of that kind
    obj: object  # the Ecu / Group / Field dataclass
    group: Group | None = None  # for fields: the group that answers for it
    ecu: Ecu | None = None


@dataclass(frozen=True)
class Catalog:
    ecus: tuple[Ecu, ...]

    def __post_init__(self) -> None:
        if isinstance(self.ecus, list):
            object.__setattr__(self, "ecus", tuple(self.ecus))

    @property
    def groups(self) -> tuple[Group, ...]:
        return tuple(g for e in self.ecus for g in e.groups)

    @property
    def fields(self) -> tuple[Field, ...]:
        return tuple(f for e in self.ecus for g in e.groups for f in g.fields)

    @cached_property
    def _targets(self) -> dict[str, list[tuple[int, int]]]:
        """name -> [(kind, flat index)], exactly the name index's content."""
        out: dict[str, list[tuple[int, int]]] = {}

        def add(name: str, kind: int, idx: int) -> None:
            if name:
                lst = out.setdefault(name, [])
                if (kind, idx) not in lst:
                    lst.append((kind, idx))

        gi = fi = 0
        for ei, ecu in enumerate(self.ecus):
            add(ecu.name, KIND_ECU, ei)
            for group in ecu.groups:
                add(group.name, KIND_GROUP, gi)
                for alias in group.aliases:
                    add(alias, KIND_GROUP, gi)
                for f in group.fields:
                    add(f.name, KIND_FIELD, fi)
                    fi += 1
                gi += 1
        return out

    def _locate(self, kind: int, index: int) -> Resolution:
        gi = fi = 0
        for ecu_i, ecu in enumerate(self.ecus):
            if kind == KIND_ECU and index == ecu_i:
                return Resolution("ecu", index, ecu, ecu=ecu)
            for group in ecu.groups:
                if kind == KIND_GROUP and index == gi:
                    return Resolution("group", index, group, group=group, ecu=ecu)
                if kind == KIND_FIELD and fi <= index < fi + len(group.fields):
                    return Resolution(
                        "field", index, group.fields[index - fi], group=group, ecu=ecu
                    )
                fi += len(group.fields)
                gi += 1
        raise CatalogError(f"dangling index ({kind}, {index})")  # pragma: no cover

    def resolve(self, name: str, kind: str | None = None) -> Resolution:
        """Find a record by name, the way the on-device reader will.

        This runs at `esphome config` time so a typo cannot survive to
        runtime: an unknown name fails with the closest matches listed, an
        ambiguous one fails naming every candidate and its group — the same
        contract the design doc promises for `field:` (§3 there).
        """
        want = _KIND_BY_NAME[kind] if kind else None
        hits = [
            (k, i)
            for (k, i) in self._targets.get(name, [])
            if want is None or k == want
        ]
        if not hits:
            universe = [
                n
                for n, tgts in self._targets.items()
                if want is None or any(k == want for k, _ in tgts)
            ]
            raise UnknownNameError(
                name, kind, difflib.get_close_matches(name, universe, n=5, cutoff=0.5)
            )
        if len(hits) > 1:
            candidates = []
            for k, i in hits:
                r = self._locate(k, i)
                if r.kind == "field":
                    where = f"in group {r.group.name!r} at bit {r.obj.bit_pos}"
                    candidates.append(f"field {name!r} {where}")
                else:
                    candidates.append(f"{r.kind} {name!r}")
            # A field named by §5.1a rule (b) carries its originating service
            # qualifier, which is also that group's alias — so this name
            # legitimately denotes both a group and a field, and the caller's
            # `kind` is what separates them. Say so rather than making the
            # caller guess; resolving to either one silently is how a config
            # ends up bound to the wrong record.
            if len({k for k, _ in hits}) > 1 and kind is None:
                candidates.append(
                    "pass kind='field' (YAML `field:`) or kind='group' "
                    "(YAML `service:`) to disambiguate a name that is both"
                )
            raise AmbiguousNameError(name, candidates)
        return self._locate(*hits[0])


# --- extraction and evaluation (spec §5.1, §6) -------------------------------
#
# The device does this in C++; this Python twin exists so tests can assert the
# *semantics* of a compiled catalog (golden decodes), not just its bytes.


def extract(f: Field, response: bytes, k: int = 0) -> int:
    """Normative §5.1 bit extraction: MSB-first, cross-byte, sign-extended."""
    if f.ascii or f.hexdump:
        raise CatalogError("extract() is for numeric fields; copy bytes instead")
    base = f.bit_pos + k * f.repeat_stride
    raw = 0
    for i in range(f.bit_size):
        pos = base + i
        bit = (response[pos // 8] >> (7 - (pos % 8))) & 1
        raw = (raw << 1) | bit
    if f.byteswap:
        nbytes = (f.bit_size + 7) // 8
        raw = int.from_bytes(raw.to_bytes(nbytes, "big"), "little")
    if f.signed and f.bit_size < 64 and (raw >> (f.bit_size - 1)) & 1:
        raw -= 1 << f.bit_size
    return raw


def evaluate(f: Field, raw: int) -> tuple[float | None, str | None]:
    """Normative §6 scale evaluation: first row whose bounds contain raw wins.

    Returns (numeric value or None, text or None). A None numeric value means
    "not available" — either an INVALID sentinel row matched, or no row did.
    INVALID is authoritative on its own (a textless INVALID row still yields
    NaN); publishing a confident number for a raw outside every declared range
    is the failure mode this rule exists to prevent.
    """
    if not f.scales:
        return float(raw), None
    for s in f.scales:
        if s.low <= raw <= s.high:
            if s.invalid:
                return None, s.text or None
            if s.text:
                return raw * s.factor + s.offset, s.text
            return raw * s.factor + s.offset, None
    return None, None


# --- limits (spec §8) ---------------------------------------------------------


def _check_limits(cat: Catalog) -> None:
    """Fail loudly on any §8 breach — before a single byte is laid out."""
    ecus, groups = cat.ecus, cat.groups
    fields = cat.fields
    n_scales = 0
    if len(ecus) > MAX_RECORDS:
        raise LimitError(f"{len(ecus)} ECUs exceeds the u16 limit {MAX_RECORDS}")
    if len(groups) > MAX_RECORDS:
        raise LimitError(f"{len(groups)} groups exceeds the u16 limit {MAX_RECORDS}")
    if len(fields) > MAX_RECORDS:
        raise LimitError(f"{len(fields)} fields exceeds the u16 limit {MAX_RECORDS}")
    for g in groups:
        if not 1 <= len(g.request) <= MAX_REQ_LEN:
            raise LimitError(
                f"group {g.name!r}: request length {len(g.request)} outside 1..{MAX_REQ_LEN}"
            )
        if not 0 <= g.security_level <= 0xF:
            raise LimitError(
                f"group {g.name!r}: security level {g.security_level} outside 0..15"
            )
        # §5.1a: a group's field names MUST be distinct. YAML addresses a field
        # by group plus name, so a duplicate makes that address meaningless —
        # and this is the compiler's obligation, not the database's property.
        seen_names: dict[str, int] = {}
        for f in g.fields:
            if f.name in seen_names:
                raise LimitError(
                    f"group {g.name!r}: field name {f.name!r} is used twice "
                    f"(bits {seen_names[f.name]} and {f.bit_pos}) — §5.1a requires "
                    f"unique names within a group"
                )
            seen_names[f.name] = f.bit_pos
        # Fields ordered by ascending bit_pos is normative (§5.1b), so element
        # k means the same thing in both implementations.
        positions = [f.bit_pos for f in g.fields]
        if positions != sorted(positions):
            raise LimitError(
                f"group {g.name!r}: fields are not ordered by ascending bit_pos"
            )
        for f in g.fields:
            if not 1 <= f.repeat_count <= MAX_REPEAT_COUNT:
                raise LimitError(
                    f"field {f.name!r}: repeat_count {f.repeat_count} outside "
                    f"1..{MAX_REPEAT_COUNT} (u8)"
                )
            if f.repeat_count == 1:
                if f.repeat_stride:
                    raise LimitError(
                        f"field {f.name!r}: repeat_stride {f.repeat_stride} on a "
                        f"scalar — must be 0 when repeat_count == 1"
                    )
            elif f.repeat_stride < f.bit_size:
                # Overlapping elements would decode the same bits twice under
                # two element indices, which is never what a database means.
                raise LimitError(
                    f"field {f.name!r}: repeat_stride {f.repeat_stride} is smaller "
                    f"than bit_size {f.bit_size} — array elements would overlap"
                )
            if f.ascii or f.hexdump:
                if f.bit_size % 8 or not 8 <= f.bit_size <= MAX_BLOCK_BITS:
                    raise LimitError(
                        f"field {f.name!r}: ASCII/HEXDUMP size {f.bit_size} bits — "
                        f"must be a byte multiple in 8..{MAX_BLOCK_BITS}"
                    )
                if f.bit_pos % 8:
                    # Spec §5.1: block fields require byte alignment; a reader
                    # refuses rather than inventing a byte boundary, so the
                    # compiler must never emit one.
                    raise LimitError(
                        f"field {f.name!r}: ASCII/HEXDUMP at bit {f.bit_pos} — "
                        f"bit_pos must be byte-aligned"
                    )
            elif not 1 <= f.bit_size <= MAX_NUMERIC_BITS:
                raise LimitError(
                    f"field {f.name!r}: bit_size {f.bit_size} outside 1..{MAX_NUMERIC_BITS}"
                )
            if f.bit_pos < 0 or f.bit_end > MAX_BIT_END:
                raise LimitError(
                    f"field {f.name!r}: last bit {f.bit_end} (bit_pos {f.bit_pos}, "
                    f"{f.repeat_count} x {f.bit_size} bits at stride "
                    f"{f.repeat_stride}) exceeds the u16 limit {MAX_BIT_END}"
                )
            if len(f.scales) > 255:
                raise LimitError(
                    f"field {f.name!r}: {len(f.scales)} scale rows exceeds the u8 limit 255"
                )
            for s in f.scales:
                for v, what in ((s.low, "low"), (s.high, "high")):
                    if not I32_MIN <= v <= I32_MAX:
                        raise LimitError(
                            f"field {f.name!r}: scale {what} {v} outside i32"
                        )
            n_scales += len(f.scales)
    if n_scales > MAX_RECORDS:
        raise LimitError(f"{n_scales} scale rows exceeds the u16 limit {MAX_RECORDS}")


# --- writer -------------------------------------------------------------------


def write_catalog(cat: Catalog) -> bytes:
    """Serialise a Catalog to the normative v1 blob.

    Deterministic by construction: identical Catalogs produce identical bytes
    (string interning walks the structure in one fixed order, scale runs are
    deduplicated first-seen-wins, the name index is sorted). That determinism
    is what makes the checked-in golden `mini.dcat` a meaningful contract
    between this writer and the C++ reader.
    """
    _check_limits(cat)

    # String pool. Refs are offsets relative to strings_off (spec §1), so the
    # interned pool-local offset IS the ref; the first byte is \0, so ref 0
    # reads as the empty string and doubles as "none" (unit_ref, text_ref).
    pool = bytearray(b"\0")
    interned: dict[str, int] = {"": 0}

    def intern(s: str) -> int:
        off = interned.get(s)
        if off is None:
            off = len(pool)
            interned[s] = off
            pool.extend(s.encode("utf-8"))
            pool.append(0)
        return off

    # Request-byte pool, deduplicated by content.
    reqpool = bytearray()
    req_offs: dict[bytes, int] = {}

    def intern_req(b: bytes) -> int:
        off = req_offs.get(b)
        if off is None:
            off = len(reqpool)
            req_offs[b] = off
            reqpool.extend(b)
        return off

    # Scale runs, deduplicated by content: fields sharing a presentation share
    # rows (a real factory database can reuse one temperature presentation
    # across several fields), and the record's scale_first/scale_count makes
    # that free.
    scale_rows: list[Scale] = []
    scale_runs: dict[tuple[Scale, ...], int] = {}

    def intern_scales(run: tuple[Scale, ...]) -> int:
        if not run:
            return 0
        first = scale_runs.get(run)
        if first is None:
            first = len(scale_rows)
            scale_runs[run] = first
            scale_rows.extend(run)
        return first

    # One walk assigns pool offsets and flattens records, in structure order.
    ecu_recs: list[tuple] = []
    group_recs: list[tuple] = []
    field_recs: list[tuple] = []
    index_names: list[tuple[str, int, int]] = []  # (name, kind, flat index)
    any_text = False

    gi = fi = 0
    for ei, ecu in enumerate(cat.ecus):
        ecu_name = intern(ecu.name)
        index_names.append((ecu.name, KIND_ECU, ei))
        group_first = gi
        for group in ecu.groups:
            g_name = intern(group.name)
            index_names.append((group.name, KIND_GROUP, gi))
            for alias in group.aliases:
                intern(alias)
                index_names.append((alias, KIND_GROUP, gi))
            req_off = intern_req(group.request)
            field_first = fi
            for f in group.fields:
                f_name = intern(f.name)
                index_names.append((f.name, KIND_FIELD, fi))
                unit_ref = intern(f.unit)
                for s in f.scales:
                    if s.text:
                        any_text = True
                    intern(s.text)
                scale_first = intern_scales(f.scales)
                field_recs.append(
                    (
                        f_name,
                        unit_ref,
                        f.bit_pos,
                        f.bit_size,
                        f.flags,
                        scale_first if f.scales else 0,
                        len(f.scales),
                        f.repeat_count,
                        f.repeat_stride,
                        gi,
                    )
                )
                fi += 1
            group_recs.append(
                (
                    g_name,
                    req_off,
                    len(group.request),
                    group.sid,
                    group.flags,
                    field_first,
                    len(group.fields),
                    group.did,
                    group.resp_min_len,
                )
            )
            gi += 1
        ecu_recs.append(
            (
                ecu_name,
                ecu.request_id,
                ecu.response_id,
                ecu.flags,
                ecu.block_size,
                ecu.st_min_raw,
                ecu.p2_ms,
                ecu.p2_ext_ms,
                group_first,
                len(ecu.groups),
            )
        )

    # Deduplicate identical (name -> target) pairs: a single-service group's
    # canonical name equals its only qualifier, and one entry is enough. Each
    # entry carries its own name_ref — that is what lets an alias verify on
    # lookup (spec §7, corrected 2026-07-30).
    seen: set[tuple[str, int, int]] = set()
    entries: list[tuple[int, int, int]] = []  # (hash, name_ref, target)
    for name, kind, idx in index_names:
        key = (name, kind, idx)
        if key in seen:
            continue
        seen.add(key)
        entries.append((fnv1a32(name), interned[name], (kind << 28) | idx))
    entries.sort(key=lambda e: (e[0], e[2], e[1]))

    # Layout: header+ext, then tables in spec order, then pools, strings last.
    off = FIRST_TABLE
    ecus_off = off
    off += len(ecu_recs) * ECU_SIZE
    groups_off = off
    off += len(group_recs) * GROUP_SIZE
    fields_off = off
    off += len(field_recs) * FIELD_SIZE
    scales_off = off
    off += len(scale_rows) * SCALE_SIZE
    reqbytes_off = off
    off += len(reqpool)
    off += -off % 4  # pad bytes are zero (spec §1)
    index_off = off
    off += len(entries) * INDEX_ENTRY_SIZE
    strings_off = off
    total_size = strings_off + len(pool)

    blob = bytearray(total_size)
    flags = FLAG_NAMES | (FLAG_TEXTS if any_text else 0)
    _HEADER.pack_into(
        blob,
        0,
        MAGIC,
        FORMAT_VERSION,
        flags,
        total_size,
        0,  # crc32, patched below
        ecus_off,
        len(ecu_recs),
        0,
        groups_off,
        len(group_recs),
        0,
        fields_off,
        len(field_recs),
        0,
        scales_off,
        len(scale_rows),
        0,
        reqbytes_off,
        len(reqpool),
        strings_off,
        len(pool),
        index_off,
        len(entries),
        0,
    )

    o = ecus_off
    for r in ecu_recs:
        _ECU.pack_into(blob, o, *r)
        o += ECU_SIZE
    o = groups_off
    for r in group_recs:
        _GROUP.pack_into(blob, o, r[0], reqbytes_off + r[1], *r[2:])
        o += GROUP_SIZE
    o = fields_off
    for r in field_recs:
        _FIELD.pack_into(blob, o, *r)
        o += FIELD_SIZE
    o = scales_off
    for s in scale_rows:
        sf = (SCALE_FLAG_TEXT if s.text else 0) | (
            SCALE_FLAG_INVALID if s.invalid else 0
        )
        _SCALE.pack_into(
            blob, o, s.low, s.high, s.factor, s.offset, interned[s.text], sf, 0
        )
        o += SCALE_SIZE
    blob[reqbytes_off : reqbytes_off + len(reqpool)] = reqpool
    o = index_off
    for h, name_ref, target in entries:
        _INDEX.pack_into(blob, o, h, name_ref, target)
        o += INDEX_ENTRY_SIZE
    blob[strings_off:total_size] = pool

    crc = zlib.crc32(bytes(blob[HEADER_SIZE:])) & 0xFFFFFFFF
    struct.pack_into("<I", blob, 0x10, crc)
    return bytes(blob)


# --- reader -------------------------------------------------------------------


def read_catalog(data: bytes | bytearray | memoryview) -> Catalog:
    """Parse and validate a blob, refusing anything the spec's §2 refuses.

    Every check happens before the corresponding bytes are trusted, in the
    §2 order, so a truncated or corrupted blob raises FormatError — never an
    IndexError, never a struct.error, and never a silently wrong Catalog.
    This mirrors what the C++ reader must do against mmap'd flash.

    Deliberately *not* checked here: §5.1a's within-group name uniqueness.
    That is a compiler obligation and §2 does not list it, so rejecting a blob
    for it would make this reader stricter than the format — `verify` catches
    it instead by re-running the writer over what it read.
    """
    data = bytes(data)
    n = len(data)
    if n < FIRST_TABLE:
        raise FormatError(f"blob is {n} bytes; header + extension need 0x50")
    (
        magic,
        version,
        _flags,
        total_size,
        crc32,
        ecus_off,
        ecu_count,
        _r0,
        groups_off,
        group_count,
        _r1,
        fields_off,
        field_count,
        _r2,
        scales_off,
        scale_count,
        _r3,
        reqbytes_off,
        reqbytes_len,
        strings_off,
        strings_len,
        index_off,
        index_count,
        _r4,
    ) = _HEADER.unpack_from(data, 0)

    # 1. magic and version
    if magic != MAGIC:
        raise FormatError(f"bad magic {magic!r}")
    if version != FORMAT_VERSION:
        raise FormatError(f"format_version {version}; this reader knows {FORMAT_VERSION}")
    # 2. total_size within the mapped region
    if not FIRST_TABLE <= total_size <= n:
        raise FormatError(f"total_size {total_size} outside [0x50, {n}]")

    # 3. every table inside [0x50, total_size), 4-byte aligned. A table with
    #    count == 0 is vacuous: nothing to address, offset unchecked.
    def check_table(name: str, off: int, count: int, size: int) -> None:
        if count == 0:
            return
        if off % 4:
            raise FormatError(f"{name} table at 0x{off:X} is not 4-byte aligned")
        end = off + count * size
        if not (FIRST_TABLE <= off and end <= total_size):
            raise FormatError(
                f"{name} table [0x{off:X}, 0x{end:X}) outside [0x50, 0x{total_size:X})"
            )

    check_table("ecu", ecus_off, ecu_count, ECU_SIZE)
    check_table("group", groups_off, group_count, GROUP_SIZE)
    check_table("field", fields_off, field_count, FIELD_SIZE)
    check_table("scale", scales_off, scale_count, SCALE_SIZE)
    check_table("index", index_off, index_count, INDEX_ENTRY_SIZE)

    # 4. pools inside the blob; string pool NUL at both ends, so every string
    #    ref is NUL-terminated inside the pool and needs no length to use.
    for name, off, length in (
        ("request-byte", reqbytes_off, reqbytes_len),
        ("string", strings_off, strings_len),
    ):
        if not (FIRST_TABLE <= off and off + length <= total_size):
            raise FormatError(f"{name} pool outside [0x50, 0x{total_size:X})")
    if strings_len < 1:
        raise FormatError("string pool is empty; it must hold at least a NUL")
    if data[strings_off] != 0:
        raise FormatError("string pool's first byte is not NUL")
    if data[strings_off + strings_len - 1] != 0:
        raise FormatError("string pool is not NUL-terminated")

    # 5. CRC over [64, total_size)
    actual = zlib.crc32(data[HEADER_SIZE:total_size]) & 0xFFFFFFFF
    if actual != crc32:
        raise FormatError(f"crc32 mismatch: header 0x{crc32:08X}, blob 0x{actual:08X}")

    # From here every fixed-size read is inside the validated tables; string
    # refs still get bounds-checked individually because a corrupt-but-CRC-
    # consistent blob is exactly what a compiler bug would produce. Refs are
    # relative to strings_off (spec §1): ref 0 is the pool's leading NUL.
    def read_str(ref: int, what: str) -> str:
        if ref >= strings_len:
            raise FormatError(f"{what}: string ref 0x{ref:X} outside the pool")
        start = strings_off + ref
        end = data.index(0, start)  # exists: pool's last byte is NUL
        return data[start:end].decode("utf-8")

    scales_flat: list[Scale] = []
    for i in range(scale_count):
        low, high, factor, offset, text_ref, sflags, _res = _SCALE.unpack_from(
            data, scales_off + i * SCALE_SIZE
        )
        scales_flat.append(
            Scale(
                low=low,
                high=high,
                factor=factor,
                offset=offset,
                text=read_str(text_ref, f"scale {i}"),
                invalid=bool(sflags & SCALE_FLAG_INVALID),
            )
        )

    fields_flat: list[Field] = []
    fields_group: list[int] = []
    for i in range(field_count):
        (
            name_ref,
            unit_ref,
            bit_pos,
            bit_size,
            fflags,
            scale_first,
            n_scales,
            repeat_count,
            repeat_stride,
            group_index,
        ) = _FIELD.unpack_from(data, fields_off + i * FIELD_SIZE)
        if scale_first + n_scales > scale_count:
            raise FormatError(f"field {i}: scale run outside the scale table")
        if group_index >= group_count:
            raise FormatError(f"field {i}: group_index {group_index} out of range")
        is_block = bool(fflags & (FIELD_FLAG_ASCII | FIELD_FLAG_HEXDUMP))
        if is_block:
            # Spec §5.1: a reader refuses unaligned block geometry rather
            # than inventing a byte boundary.
            if bit_size % 8 or not 8 <= bit_size <= MAX_BLOCK_BITS:
                raise FormatError(f"field {i}: block size {bit_size} bits invalid")
            if bit_pos % 8:
                raise FormatError(f"field {i}: block at bit {bit_pos} is unaligned")
        elif not 1 <= bit_size <= MAX_NUMERIC_BITS:
            raise FormatError(f"field {i}: bit_size {bit_size} outside 1..64")
        fields_flat.append(
            Field(
                name=read_str(name_ref, f"field {i}"),
                unit=read_str(unit_ref, f"field {i} unit"),
                bit_pos=bit_pos,
                bit_size=bit_size,
                signed=bool(fflags & FIELD_FLAG_SIGNED),
                byteswap=bool(fflags & FIELD_FLAG_BYTESWAP),
                ascii=bool(fflags & FIELD_FLAG_ASCII),
                hexdump=bool(fflags & FIELD_FLAG_HEXDUMP),
                scales=tuple(scales_flat[scale_first : scale_first + n_scales]),
                repeat_count=max(repeat_count, 1),
                repeat_stride=repeat_stride,
            )
        )
        fields_group.append(group_index)

    # The name index: needed here to recover group aliases (they live nowhere
    # else) and to validate sortedness, which the device's binary search
    # depends on. Each entry carries its own name_ref — that is what lets an
    # alias verify on lookup (spec §7, corrected 2026-07-30) — so alias
    # recovery is a direct read, and the hash can be cross-checked against it.
    group_aliases: list[set[str]] = [set() for _ in range(group_count)]
    prev = (-1, -1)
    for i in range(index_count):
        h, name_ref, target = _INDEX.unpack_from(
            data, index_off + i * INDEX_ENTRY_SIZE
        )
        if (h, target) < prev:
            raise FormatError(f"name index not sorted at entry {i}")
        prev = (h, target)
        name = read_str(name_ref, f"index entry {i}")
        if fnv1a32(name) != h:
            raise FormatError(f"index entry {i}: hash does not match {name!r}")
        kind, idx = target >> 28, target & 0x0FFFFFFF
        if kind == KIND_ECU and idx >= ecu_count:
            raise FormatError(f"index entry {i}: ecu {idx} out of range")
        if kind == KIND_GROUP:
            if idx >= group_count:
                raise FormatError(f"index entry {i}: group {idx} out of range")
            group_aliases[idx].add(name)
        if kind == KIND_FIELD and idx >= field_count:
            raise FormatError(f"index entry {i}: field {idx} out of range")
        # Unknown kinds are ignored, not rejected: that is the forward
        # compatibility hinge (spec §1, reserved semantics).

    raw_groups = [
        _GROUP.unpack_from(data, groups_off + i * GROUP_SIZE)
        for i in range(group_count)
    ]

    groups_flat: list[Group] = []
    for i, raw in enumerate(raw_groups):
        (
            name_ref,
            req_off,
            req_len,
            sid,
            gflags,
            field_first,
            n_fields,
            did,
            _resp_min_len,  # derived; recomputed from the fields
        ) = raw
        if field_first + n_fields > field_count:
            raise FormatError(f"group {i}: field run outside the field table")
        if not (reqbytes_off <= req_off and req_off + req_len <= reqbytes_off + reqbytes_len):
            raise FormatError(f"group {i}: request bytes outside the pool")
        request = data[req_off : req_off + req_len]
        if req_len < 1:
            raise FormatError(f"group {i}: empty request")
        if request[0] != sid:
            raise FormatError(f"group {i}: sid 0x{sid:02X} != first request byte")
        name = read_str(name_ref, f"group {i}")
        groups_flat.append(
            Group(
                name=name,
                request=request,
                fields=tuple(
                    fields_flat[field_first : field_first + n_fields]
                ),
                aliases=tuple(sorted(group_aliases[i] - {name})),
                needs_session=bool(gflags & GROUP_FLAG_NEEDS_SESSION),
                needs_security=bool(gflags & GROUP_FLAG_NEEDS_SECURITY),
                security_level=(gflags >> GROUP_SECURITY_SHIFT) & 0xF,
            )
        )
        for fi in range(field_first, field_first + n_fields):
            if fields_group[fi] != i:
                raise FormatError(
                    f"field {fi}: group_index {fields_group[fi]} disagrees with group {i}"
                )

    ecus: list[Ecu] = []
    for i in range(ecu_count):
        (
            name_ref,
            request_id,
            response_id,
            eflags,
            block_size,
            st_min_raw,
            p2_ms,
            p2_ext_ms,
            group_first,
            n_groups,
        ) = _ECU.unpack_from(data, ecus_off + i * ECU_SIZE)
        if group_first + n_groups > group_count:
            raise FormatError(f"ecu {i}: group range outside the group table")
        ecus.append(
            Ecu(
                name=read_str(name_ref, f"ecu {i}"),
                request_id=request_id,
                response_id=response_id,
                extended_id=bool(eflags & ECU_FLAG_EXTENDED_ID),
                block_size=block_size,
                st_min_raw=st_min_raw,
                p2_ms=p2_ms,
                p2_ext_ms=p2_ext_ms,
                groups=tuple(groups_flat[group_first : group_first + n_groups]),
            )
        )

    return Catalog(ecus=tuple(ecus))
