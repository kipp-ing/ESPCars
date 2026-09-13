"""write -> read -> write must be lossless and byte-stable.

Byte determinism is not a nicety: the checked-in golden `mini.dcat` is the
meeting point between this writer and the C++ reader, and it only means
something if identical input always produces identical bytes.
"""

import struct

from .common import load_catalog_module, tiny_catalog

catalog = load_catalog_module()


def test_roundtrip_structural_equality():
    cat = tiny_catalog(catalog)
    blob = catalog.write_catalog(cat)
    assert catalog.read_catalog(blob) == cat


def test_roundtrip_byte_identity():
    cat = tiny_catalog(catalog)
    blob = catalog.write_catalog(cat)
    assert catalog.write_catalog(catalog.read_catalog(blob)) == blob


def test_write_is_deterministic():
    a = catalog.write_catalog(tiny_catalog(catalog))
    b = catalog.write_catalog(tiny_catalog(catalog))
    assert a == b


def test_header_geometry():
    blob = catalog.write_catalog(tiny_catalog(catalog))
    assert blob[:8] == b"ESPDCAT\0"
    version, flags = struct.unpack_from("<HH", blob, 8)
    assert version == 1
    assert flags & catalog.FLAG_NAMES
    assert flags & catalog.FLAG_TEXTS  # tiny catalog has enum texts
    (total_size,) = struct.unpack_from("<I", blob, 0x0C)
    assert total_size == len(blob)
    # every table 4-byte aligned
    for off_at in (0x14, 0x1C, 0x24, 0x2C, 0x44):
        (off,) = struct.unpack_from("<I", blob, off_at)
        assert off % 4 == 0
        assert off >= 0x50


def test_string_pool_framing():
    blob = catalog.write_catalog(tiny_catalog(catalog))
    (strings_off,) = struct.unpack_from("<I", blob, 0x3C)
    (strings_len,) = struct.unpack_from("<I", blob, 0x40)
    assert blob[strings_off] == 0  # ref 0 reads as the empty string
    assert blob[strings_off + strings_len - 1] == 0  # every ref NUL-terminated


def test_mapped_region_may_exceed_total_size():
    # esp_partition_mmap hands the reader the whole partition; trailing 0xFF
    # erased-flash bytes after total_size must not matter.
    cat = tiny_catalog(catalog)
    blob = catalog.write_catalog(cat)
    assert catalog.read_catalog(blob + b"\xff" * 1024) == cat


def test_aliases_survive_and_normalise():
    cat = tiny_catalog(catalog)
    got = catalog.read_catalog(catalog.write_catalog(cat))
    g1 = got.ecus[0].groups[0]
    assert g1.aliases == ("DT_Volt_Averaged", "DT_Volt_Maximum", "DT_Volt_Minimum")


def test_group_flags_roundtrip():
    got = catalog.read_catalog(catalog.write_catalog(tiny_catalog(catalog)))
    g_read, g_state, g_job, _g_cells = got.ecus[0].groups
    assert g_read.safe_read and g_state.safe_read
    assert not g_job.safe_read  # 0x31 RoutineControl can change ECU state
    assert g_job.needs_session and g_job.needs_security
    assert g_job.security_level == 3
    assert g_read.did == 0x0207 and g_state.did == 0xD000
    assert g_job.did == catalog.NO_INDEX  # not a 0x22 read


def test_resp_min_len_is_derived():
    got = catalog.read_catalog(catalog.write_catalog(tiny_catalog(catalog)))
    assert got.ecus[0].groups[0].resp_min_len == 9  # 3B header + 3 words
    assert got.ecus[0].groups[1].resp_min_len == 4


def test_scale_runs_are_shared():
    # Three fields share one scale tuple: the table must hold it once.
    blob = catalog.write_catalog(tiny_catalog(catalog))
    (scale_count,) = struct.unpack_from("<H", blob, 0x30)
    # volt run (2 rows, shared by three fields) + state run (3 rows) = 5
    assert scale_count == 5


def test_float_quantisation_is_f32():
    s = catalog.Scale(0, 100, 0.001, 0.0)
    assert s.factor == struct.unpack("<f", struct.pack("<f", 0.001))[0]


def test_extract_and_evaluate_golden():
    # The §14.2 worked example from the factory database docs:
    # 62 02 07 0A 28 -> 2600 -> 2600 * 0.001 + 1.5 = 4.1 V (f32 precision).
    cat = tiny_catalog(catalog)
    f = cat.ecus[0].groups[0].fields[0]
    raw = catalog.extract(f, bytes([0x62, 0x02, 0x07, 0x0A, 0x28]))
    assert raw == 2600
    value, text = catalog.evaluate(f, raw)
    assert abs(value - 4.1) < 1e-6
    assert text is None
    # the SNA sentinel row
    value, text = catalog.evaluate(f, 0xFFFF)
    assert value is None and text == "Signal not available"
    # a raw outside every declared range is "we do not understand this"
    f_state = cat.ecus[0].groups[1].fields[0]
    assert catalog.evaluate(f_state, 7) == (None, None)


def test_array_roundtrip_and_element_extraction():
    cat = tiny_catalog(catalog)
    got = catalog.read_catalog(catalog.write_catalog(cat))
    arr = got.ecus[0].groups[3].fields[0]
    assert arr.repeat_count == 4 and arr.repeat_stride == 16
    # element k reads the k-th 16-bit word after the 3-byte response header
    resp = bytes([0x62, 0x02, 0x08]) + b"".join(
        (1000 + k).to_bytes(2, "big") for k in range(4)
    )
    assert [catalog.extract(arr, resp, k) for k in range(4)] == [
        1000, 1001, 1002, 1003
    ]
    # and the group's short-response guard covers the whole array
    assert got.ecus[0].groups[3].resp_min_len == 13


def test_extract_signed_and_crossbyte():
    f = catalog.Field("t", 12, 8, signed=True)
    # bits 12..19 of 0x0F 0xF8 0x00 -> 0x80 -> -128
    assert catalog.extract(f, bytes([0x0F, 0xF8, 0x00])) == -128
    f2 = catalog.Field("u", 12, 8)
    assert catalog.extract(f2, bytes([0x0F, 0xF8, 0x00])) == 128


def test_extract_byteswap():
    f = catalog.Field("w", 24, 16, byteswap=True)
    assert catalog.extract(f, bytes([0x62, 0, 0, 0x34, 0x12])) == 0x1234


def test_invalid_without_text_is_authoritative():
    # Spec §6: INVALID alone means not-available; TEXT only adds a string.
    f = catalog.Field(
        "x", 24, 8, scales=(catalog.Scale(255, 255, invalid=True),)
    )
    assert catalog.evaluate(f, 255) == (None, None)
    got = catalog.read_catalog(
        catalog.write_catalog(
            catalog.Catalog(
                ecus=(
                    catalog.Ecu(
                        "E", 1, 2,
                        groups=(catalog.Group("G", b"\x22\x00\x01", fields=(f,)),),
                    ),
                )
            )
        )
    )
    s = got.ecus[0].groups[0].fields[0].scales[0]
    assert s.invalid and s.text == ""
