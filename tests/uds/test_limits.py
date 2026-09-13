"""Spec §8: the compiler fails loudly on any limit breach — no clamping,
no silent truncation, because a catalog that half-fits decodes half-wrong."""

import pytest

from .common import load_catalog_module

catalog = load_catalog_module()


def one_group_catalog(fields):
    return catalog.Catalog(
        ecus=(
            catalog.Ecu(
                "E", 1, 2, groups=(catalog.Group("G", b"\x22\x00\x01", fields=fields),)
            ),
        )
    )


def expect_limit(cat, why: str):
    with pytest.raises(catalog.LimitError):
        catalog.write_catalog(cat), why


def test_numeric_bit_size_over_64():
    expect_limit(
        one_group_catalog((catalog.Field("F", 24, 65),)),
        "the extraction accumulator is 64 bits",
    )


def test_bit_size_zero():
    expect_limit(one_group_catalog((catalog.Field("F", 24, 0),)), "empty field")


def test_bit_end_over_u16():
    expect_limit(
        one_group_catalog((catalog.Field("F", 65528, 16),)),
        "bit_pos + bit_size beyond u16",
    )


def test_ascii_over_248_bits():
    expect_limit(
        one_group_catalog((catalog.Field("F", 24, 256, ascii=True),)),
        "u8 bit_size ceiling for blocks",
    )


def test_ascii_not_byte_multiple():
    expect_limit(
        one_group_catalog((catalog.Field("F", 24, 12, ascii=True),)),
        "blocks are whole bytes",
    )


def test_ascii_unaligned_bit_pos():
    expect_limit(
        one_group_catalog((catalog.Field("F", 25, 16, ascii=True),)),
        "blocks must start on a byte boundary",
    )


def test_hexdump_beyond_248_bits():
    expect_limit(
        one_group_catalog((catalog.Field("F", 24, 3200, hexdump=True),)),
        "the 400-byte hexdump case",
    )


def test_req_len_zero_and_over_255():
    with pytest.raises(catalog.LimitError):
        catalog.write_catalog(
            catalog.Catalog(
                ecus=(catalog.Ecu("E", 1, 2, groups=(catalog.Group("G", b""),)),)
            )
        )
    expect_limit(
        catalog.Catalog(
            ecus=(
                catalog.Ecu("E", 1, 2, groups=(catalog.Group("G", b"\x22" * 256),)),
            )
        ),
        "u8 req_len",
    )


def test_scale_rows_per_field_over_u8():
    rows = tuple(catalog.Scale(i, i, 1.0, 0.0) for i in range(256))
    expect_limit(
        one_group_catalog((catalog.Field("F", 24, 8, scales=rows),)),
        "u8 scale_count",
    )


def test_scale_bound_outside_i32():
    expect_limit(
        one_group_catalog(
            (catalog.Field("F", 24, 8, scales=(catalog.Scale(0, 1 << 31, 1.0, 0.0),)),)
        ),
        "i32 bounds",
    )


def test_array_is_accepted():
    # §5.1b: arrays are the normal case now, not a future one.
    catalog.write_catalog(
        one_group_catalog(
            (catalog.Field("F", 24, 16, repeat_count=12, repeat_stride=16),)
        )
    )


def test_repeat_count_over_u8():
    expect_limit(
        one_group_catalog(
            (catalog.Field("F", 24, 8, repeat_count=256, repeat_stride=8),)
        ),
        "u8 repeat_count",
    )


def test_stride_on_a_scalar_refused():
    expect_limit(
        one_group_catalog((catalog.Field("F", 24, 16, repeat_stride=16),)),
        "a scalar has no stride",
    )


def test_overlapping_array_elements_refused():
    # Elements at a stride below their own width would decode the same bits
    # under two element indices, which no database ever means.
    expect_limit(
        one_group_catalog(
            (catalog.Field("F", 24, 16, repeat_count=4, repeat_stride=8),)
        ),
        "elements overlap",
    )


def test_array_end_beyond_u16():
    expect_limit(
        one_group_catalog(
            (catalog.Field("F", 24, 16, repeat_count=255, repeat_stride=300),)
        ),
        "the array's last bit (76240) exceeds u16",
    )


def test_duplicate_field_names_in_a_group_refused():
    # §5.1a: YAML addresses a field by group plus name, so a duplicate makes
    # that address meaningless. The writer is the enforcement point.
    expect_limit(
        one_group_catalog(
            (catalog.Field("F", 24, 8), catalog.Field("F", 32, 8))
        ),
        "duplicate name within a group",
    )


def test_same_name_in_two_groups_is_fine():
    # Cross-group duplication is legal and real; `service:` disambiguates.
    catalog.write_catalog(
        catalog.Catalog(
            ecus=(
                catalog.Ecu(
                    "E", 1, 2,
                    groups=(
                        catalog.Group(
                            "G1", b"\x22\x00\x01", fields=(catalog.Field("F", 24, 8),)
                        ),
                        catalog.Group(
                            "G2", b"\x22\x00\x02", fields=(catalog.Field("F", 24, 8),)
                        ),
                    ),
                ),
            )
        )
    )


def test_fields_out_of_bit_order_refused():
    # Ascending bit_pos is normative (§5.1b) so element k means the same thing
    # in both implementations.
    expect_limit(
        one_group_catalog(
            (catalog.Field("B", 32, 8), catalog.Field("A", 24, 8))
        ),
        "fields must be ordered by ascending bit_pos",
    )


def test_field_count_over_u16():
    fields = tuple(
        catalog.Field("F", 24, 1) for _ in range(catalog.MAX_RECORDS + 1)
    )
    expect_limit(one_group_catalog(fields), "u16 field indices")


def test_security_level_over_nibble():
    with pytest.raises(catalog.LimitError):
        catalog.write_catalog(
            catalog.Catalog(
                ecus=(
                    catalog.Ecu(
                        "E",
                        1,
                        2,
                        groups=(
                            catalog.Group(
                                "G", b"\x22\x00\x01", security_level=16
                            ),
                        ),
                    ),
                )
            )
        )
