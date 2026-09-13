"""Every spec §2 validation, exercised with a deliberately corrupted blob.

On the device this blob is mmap'd flash that may be stale, truncated or
garbage; the contract is FormatError — never a crash, never a silently wrong
Catalog. `tests/host` proves the same for the C++ reader under ASan; this file
proves the Python reader refuses the same bytes.
"""

import struct
import zlib

import pytest

from .common import load_catalog_module, tiny_catalog

catalog = load_catalog_module()


def make_blob() -> bytes:
    return catalog.write_catalog(tiny_catalog(catalog))


def refix_crc(blob: bytearray) -> bytes:
    """Re-seal a corrupted blob so the corruption, not the CRC, is what the
    reader has to catch."""
    crc = zlib.crc32(bytes(blob[0x40:])) & 0xFFFFFFFF
    struct.pack_into("<I", blob, 0x10, crc)
    return bytes(blob)


def expect_reject(blob: bytes, why: str):
    with pytest.raises(catalog.FormatError):
        catalog.read_catalog(blob), why


def test_bad_magic():
    blob = bytearray(make_blob())
    blob[0:8] = b"NOTDCAT\0"
    expect_reject(refix_crc(blob), "magic")


def test_wrong_version():
    blob = bytearray(make_blob())
    struct.pack_into("<H", blob, 0x08, 2)
    expect_reject(refix_crc(blob), "version 2")


def test_bad_crc():
    blob = bytearray(make_blob())
    blob[-1] ^= 0xFF  # flip a string-pool byte, leave the header CRC stale
    expect_reject(bytes(blob), "crc")


def test_total_size_too_small():
    blob = bytearray(make_blob())
    struct.pack_into("<I", blob, 0x0C, 0x4F)
    expect_reject(refix_crc(blob), "total_size below header+extension")


def test_total_size_beyond_region():
    blob = bytearray(make_blob())
    struct.pack_into("<I", blob, 0x0C, len(blob) + 4)
    expect_reject(refix_crc(blob), "total_size beyond the mapped region")


@pytest.mark.parametrize(
    "off_at",
    [0x14, 0x1C, 0x24, 0x2C, 0x44],
    ids=["ecus", "groups", "fields", "scales", "index"],
)
def test_table_out_of_bounds(off_at):
    blob = bytearray(make_blob())
    (total,) = struct.unpack_from("<I", blob, 0x0C)
    struct.pack_into("<I", blob, off_at, total - 4)  # end lands past total_size
    expect_reject(refix_crc(blob), "table outside the blob")


@pytest.mark.parametrize(
    "off_at",
    [0x14, 0x1C, 0x24, 0x2C, 0x44],
    ids=["ecus", "groups", "fields", "scales", "index"],
)
def test_table_misaligned(off_at):
    blob = bytearray(make_blob())
    (off,) = struct.unpack_from("<I", blob, off_at)
    struct.pack_into("<I", blob, off_at, off + 2)
    expect_reject(refix_crc(blob), "table misaligned")


def test_vacuous_table_offset_is_not_checked():
    # Spec §2 item 3: count == 0 means there is nothing to address. Zero the
    # scale table's count and point its offset at garbage; must still read.
    stripped = catalog.Catalog(
        ecus=(
            catalog.Ecu(
                "E",
                1,
                2,
                groups=(
                    catalog.Group(
                        "G",
                        b"\x22\x00\x01",
                        fields=(catalog.Field("F", 24, 8),),
                    ),
                ),
            ),
        )
    )
    blob = bytearray(catalog.write_catalog(stripped))
    (scale_count,) = struct.unpack_from("<H", blob, 0x30)
    assert scale_count == 0
    struct.pack_into("<I", blob, 0x2C, 0xFFFFFFF1)  # absurd and misaligned
    catalog.read_catalog(refix_crc(blob))  # must not raise


def test_string_pool_first_byte_not_nul():
    blob = bytearray(make_blob())
    (strings_off,) = struct.unpack_from("<I", blob, 0x3C)
    blob[strings_off] = ord("x")
    expect_reject(refix_crc(blob), "pool must start with NUL")


def test_string_pool_unterminated():
    blob = bytearray(make_blob())
    (strings_off,) = struct.unpack_from("<I", blob, 0x3C)
    (strings_len,) = struct.unpack_from("<I", blob, 0x40)
    blob[strings_off + strings_len - 1] = ord("x")
    expect_reject(refix_crc(blob), "pool must end with NUL")


def test_string_ref_outside_pool():
    blob = bytearray(make_blob())
    (ecus_off,) = struct.unpack_from("<I", blob, 0x14)
    (strings_len,) = struct.unpack_from("<I", blob, 0x40)
    struct.pack_into("<I", blob, ecus_off, strings_len + 7)  # ecu name_ref
    expect_reject(refix_crc(blob), "ref beyond the pool")


def test_index_hash_mismatch_rejected():
    blob = bytearray(make_blob())
    (index_off,) = struct.unpack_from("<I", blob, 0x44)
    h, name_ref, target = struct.unpack_from("<III", blob, index_off)
    struct.pack_into("<III", blob, index_off, h ^ 1, name_ref, target)
    expect_reject(refix_crc(blob), "hash does not match its name")


def test_index_unsorted_rejected():
    blob = bytearray(make_blob())
    (index_off,) = struct.unpack_from("<I", blob, 0x44)
    (index_count,) = struct.unpack_from("<I", blob, 0x48)
    assert index_count >= 2
    e0 = bytes(blob[index_off : index_off + 12])
    e1 = bytes(blob[index_off + 12 : index_off + 24])
    blob[index_off : index_off + 12] = e1
    blob[index_off + 12 : index_off + 24] = e0
    expect_reject(refix_crc(blob), "binary search needs sorted entries")


def test_index_dangling_target_rejected():
    blob = bytearray(make_blob())
    (index_off,) = struct.unpack_from("<I", blob, 0x44)
    h, name_ref, target = struct.unpack_from("<III", blob, index_off)
    kind = target >> 28
    struct.pack_into("<III", blob, index_off, h, name_ref, (kind << 28) | 0x0FFFFFF)
    expect_reject(refix_crc(blob), "target index out of range")


def test_unknown_index_kind_is_ignored():
    # Forward compatibility hinge: an unknown target kind must not reject the
    # blob — a later format may add kinds a v1 reader has no business judging.
    blob = bytearray(make_blob())
    (index_off,) = struct.unpack_from("<I", blob, 0x44)
    (index_count,) = struct.unpack_from("<I", blob, 0x48)
    last = index_off + (index_count - 1) * 12
    h, name_ref, target = struct.unpack_from("<III", blob, last)
    struct.pack_into("<III", blob, last, h, name_ref, (0x7 << 28) | 0x123456)
    # keep sortedness: the hash is unchanged and target grew, so order holds
    catalog.read_catalog(refix_crc(blob))


def test_reserved_header_fields_are_ignored():
    # Spec §1: reserved fields are written zero and ignored on read.
    blob = bytearray(make_blob())
    for off in (0x1A, 0x22, 0x2A, 0x32, 0x4C):
        struct.pack_into("<H", blob, off, 0xBEEF & 0xFFFF)
    got = catalog.read_catalog(refix_crc(blob))
    assert got == tiny_catalog(catalog)


def test_group_field_run_out_of_bounds():
    blob = bytearray(make_blob())
    (groups_off,) = struct.unpack_from("<I", blob, 0x1C)
    # field_first at offset 0x0C in the group record; point it past the table
    struct.pack_into("<H", blob, groups_off + 0x0C, 9999)
    expect_reject(refix_crc(blob), "field run outside the field table")


def test_field_scale_run_out_of_bounds():
    blob = bytearray(make_blob())
    (fields_off,) = struct.unpack_from("<I", blob, 0x24)
    struct.pack_into("<H", blob, fields_off + 0x0C, 9999)  # scale_first
    expect_reject(refix_crc(blob), "scale run outside the scale table")


def test_truncated_at_every_length():
    # The Python twin of the tests/host ASan sweep: every possible truncation
    # must be a clean FormatError, never any other exception.
    blob = make_blob()
    for length in range(len(blob)):
        with pytest.raises(catalog.FormatError):
            catalog.read_catalog(blob[:length])
