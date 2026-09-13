"""The `uds:` hub schema — U1, U2, U3, U4 and the defaults that must not widen.

The catalog source and the partition sizing are checked here, in CONFIG_SCHEMA, rather than in
`to_code`: `esphome config` never reaches codegen, so a rule enforced only there would let CI pass
a config that cannot be built. U4 (`ecu:`) needs the catalog but not the full config, so it is
checked at final validation and lives in test_gates.py with the other cross-config rules.
"""

from __future__ import annotations

import pytest

import esphome.config_validation as cv

from .glue import MINI, MINI_PATH, catalog, hub, partition_catalog, setup_c6, validate

# --------------------------------------------------------------------------- U1: one source


def test_embedded_source_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    assert config["catalog"]["embed"] is True
    assert "partition" not in config["catalog"]


def test_partition_source_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=partition_catalog(MINI)))
    assert config["catalog"]["partition"] == "diag"
    # 256 KiB by default: enough for a real factory database, 128 KiB out of each app slot.
    assert config["catalog"]["size"] == 0x40000


def test_no_source_rejected(set_core_config) -> None:
    """Neither key is not "the default one": there is no default place for a catalog to live."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="exactly one source"):
        validate(hub(catalog={"file": MINI}))


def test_both_sources_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="exactly one source"):
        validate(hub(catalog={"file": MINI, "partition": "diag", "embed": True}))


def test_size_without_partition_rejected(set_core_config) -> None:
    """An embedded catalog is exactly as big as its file; a size would be a fiction."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="belongs to 'partition:'"):
        validate(hub(catalog={"file": MINI, "embed": True, "size": "256KB"}))


# ------------------------------------------------------------------- U2: a valid catalog file


def test_missing_file_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(hub(catalog=catalog("fixtures/nope.dcat")))


def test_corrupt_catalog_rejected(set_core_config, tmp_path) -> None:
    """A blob that fails a §2 check fails the build, naming the tool that would explain it.

    The device's own reader would refuse the same bytes and go inert — but silently accepting a
    broken catalog at config time would mean discovering it only after a flash.
    """
    setup_c6(set_core_config)
    blob = bytearray(MINI_PATH.read_bytes())
    blob[0x08] = 0x63  # format_version 99: a layout this reader would mis-interpret
    broken = tmp_path / "broken.dcat"
    broken.write_bytes(blob)
    with pytest.raises(cv.Invalid, match="not a usable catalog"):
        validate(hub(catalog=catalog(str(broken))))


def test_truncated_catalog_rejected(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    short = tmp_path / "short.dcat"
    short.write_bytes(MINI_PATH.read_bytes()[:0x30])
    with pytest.raises(cv.Invalid, match="not a usable catalog"):
        validate(hub(catalog=catalog(str(short))))


def test_crc_is_read_from_the_file(set_core_config) -> None:
    """Codegen emits the file's own CRC, not a recomputed one, so `verify` and the build agree."""
    import struct

    from esphome.components.uds import load_catalog

    setup_c6(set_core_config)
    validate(hub(catalog=catalog(MINI)))
    _cat, crc, size = load_catalog(MINI_PATH)
    (header_crc,) = struct.unpack_from("<I", MINI_PATH.read_bytes(), 0x10)
    assert crc == header_crc
    assert size == MINI_PATH.stat().st_size


# ------------------------------------------------------------------------ U3: partition sizing


@pytest.mark.parametrize(
    ("given", "expected"),
    [
        (0x40000, 0x40000),
        ("256KB", 0x40000),
        ("256KiB", 0x40000),
        ("64kb", 0x10000),
        ("1MB", 0x100000),
        ("4096", 0x1000),
    ],
)
def test_partition_size_forms(set_core_config, given, expected: int) -> None:
    """KB means 1024 here: a partition table is binary, and 256 * 1000 is not a legal size."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=partition_catalog(MINI, size=given)))
    assert config["catalog"]["size"] == expected


@pytest.mark.parametrize("size", [0, 0x800, 0x1001, "5000", "3KB"])
def test_unaligned_partition_size_rejected(set_core_config, size) -> None:
    """gen_esp32part.py needs 4 KB alignment; an unaligned size fails at build time instead."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="multiple of 4 KB"):
        validate(hub(catalog=partition_catalog(MINI, size=size)))


# "Partition too small for the blob" cannot be exercised against `mini`: it is ~1.8 KB, so
# every 4 KB-aligned size the U2 test above allows already fits it — a small-enough-to-reject
# blob has to be real-world sized. That case lives in private/tests/test_bms_extra.py
# (docs/CONVENTIONS.md), against the real bench catalog.


def test_partition_exactly_big_enough_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    from esphome.components.uds import load_catalog

    from .glue import MINI_PATH

    _cat, _crc, blob = load_catalog(MINI_PATH)
    rounded = -(-blob // 0x1000) * 0x1000
    assert validate(hub(catalog=partition_catalog(MINI, size=rounded)))["catalog"]["size"] == rounded


@pytest.mark.parametrize("name", ["diag", "d", "a_b_9", "sixteencharacter"])
def test_partition_names_accepted(set_core_config, name: str) -> None:
    setup_c6(set_core_config)
    assert validate(hub(catalog=partition_catalog(MINI, partition=name)))["catalog"]["partition"] == name


@pytest.mark.parametrize("name", ["", "seventeencharacter", "with space", "dash-ed", "dot.ted"])
def test_partition_names_rejected(set_core_config, name: str) -> None:
    """The label goes into a 16-byte field in the partition table, verbatim."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(hub(catalog=partition_catalog(MINI, partition=name)))


# ------------------------------------------------------------------------------ defaults


def test_defaults(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub())
    # Never widen this one: everything that can change ECU state is behind it (design §6).
    assert config["allow_active_services"] is False
    assert config["max_consecutive_failures"] == 3
    assert "ecu" not in config


def test_allow_active_services_is_explicit(set_core_config) -> None:
    setup_c6(set_core_config)
    assert validate(hub(allow_active_services=True))["allow_active_services"] is True


@pytest.mark.parametrize("value", [0, 256])
def test_max_consecutive_failures_range(set_core_config, value: int) -> None:
    """Zero would publish unavailable before the first answer could arrive."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(hub(max_consecutive_failures=value))


def test_isotp_id_required(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(hub(isotp_id=None))


def test_catalog_required(set_core_config) -> None:
    """There is no built-in decode table to fall back on — that is the whole design."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(hub(catalog=None))
