"""One test per refusal in docs/diag_server-spec.md, each asserting the message names the values.

Spec lines: "Codegen checks them against the isotp instance's rx_id/tx_id and refuses a mismatch,
naming both values", "Unknown keys are refused. Duplicate req strings are refused", "Limits are
refused at config time with the actual numbers: a reply longer than the isotp max_message_size, or
a table larger than 64 KB of flash", and "one consumer per instance, the same rule as uds U13".
"""

from __future__ import annotations

import re

import pytest

from esphome import config_validation as cv

from .common import (
    ISOTP_ID,
    REQ_ID,
    RESP_ID,
    accept,
    ecu,
    entry,
    final_validate,
    full_config,
    hex_bytes,
    isotp_block,
    names_number,
    names_size,
    refusal,
    server,
    setup_c6,
    table_doc,
    uds_hub,
    validate,
    write_table,
)

# --------------------------------------------------------------- CAN ID mismatch: both values


def test_matching_ids_accepted(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    assert accept(tmp_path, table_doc())["ecu"] == "engine"


@pytest.mark.parametrize(
    ("isotp_overrides", "table_value", "isotp_value"),
    [
        ({"rx_id": 0x7E1}, REQ_ID, 0x7E1),  # the request ID differs
        ({"tx_id": 0x7E9}, RESP_ID, 0x7E9),  # the reply ID differs
    ],
)
def test_can_id_mismatch_names_both_values(
    set_core_config, tmp_path, isotp_overrides, table_value, isotp_value
) -> None:
    setup_c6(set_core_config)
    message = refusal(tmp_path, table_doc(), isotp=isotp_block(**isotp_overrides))
    assert names_number(message, table_value), message
    assert names_number(message, isotp_value), message


def test_swapped_ids_are_a_mismatch(set_core_config, tmp_path) -> None:
    """A tester-side isotp instance (tx 0x7E0, rx 0x7E8) is the wrong way round for a server."""
    setup_c6(set_core_config)
    message = refusal(
        tmp_path, table_doc(), isotp=isotp_block(tx_id=REQ_ID, rx_id=RESP_ID)
    )
    assert names_number(message, REQ_ID) and names_number(message, RESP_ID), message


# ------------------------------------------------- reply longer than the isotp max_message_size


def test_reply_longer_than_max_message_size_names_both_numbers(
    set_core_config, tmp_path
) -> None:
    setup_c6(set_core_config)
    doc = table_doc([entry("22 F1 90", "62 F1 90 " + hex_bytes(34))])  # 37 bytes
    message = refusal(tmp_path, doc, isotp=isotp_block(max_message_size=16))
    assert names_size(message, 37), message
    assert names_size(message, 16), message


def test_reply_exactly_max_message_size_accepted(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    doc = table_doc([entry("22 F1 90", hex_bytes(16))])
    assert accept(tmp_path, doc, isotp=isotp_block(max_message_size=16))


def test_one_byte_over_max_message_size_refused(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    doc = table_doc([entry("22 F1 90", hex_bytes(17))])
    message = refusal(tmp_path, doc, isotp=isotp_block(max_message_size=16))
    assert names_size(message, 17) and names_size(message, 16), message


def test_oversized_reply_is_found_in_a_prefix_entry_too(
    set_core_config, tmp_path
) -> None:
    setup_c6(set_core_config)
    doc = table_doc([entry("22 F1 90", "01"), entry("22 01 *", hex_bytes(41))])
    message = refusal(tmp_path, doc, isotp=isotp_block(max_message_size=40))
    assert names_size(message, 41) and names_size(message, 40), message


# ----------------------------------------------------------------------- table larger than 64 KB


def _big_table(entries: int, reply_bytes: int) -> dict:
    reply = hex_bytes(reply_bytes)
    return table_doc(
        [entry(f"22 {i >> 8:02X} {i & 0xFF:02X}", reply) for i in range(entries)]
    )


def test_table_over_64kb_names_actual_size_and_limit(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    # 300 x 250 reply bytes = 75000 bytes of payload alone, over 64 KB under any way of counting.
    message = refusal(tmp_path, _big_table(300, 250))
    numbers = [
        int(n) for n in re.findall(r"(?<![0-9A-Za-z.])\d+(?![0-9A-Za-z])", message)
    ]
    assert any(75000 <= n <= 120000 for n in numbers), message  # the actual size
    assert any(n in (65536, 64) for n in numbers) or "64 KB" in message, (
        message
    )  # the limit


def test_table_just_under_64kb_accepted(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    # ~50 KB of payload plus requests and bookkeeping stays well under 64 KB.
    assert accept(tmp_path, _big_table(200, 250))


# ----------------------------------------------------------------------------------- unknown key


@pytest.mark.parametrize(
    "doc",
    [
        table_doc([entry("22 F1 90", "62 F1 90 01", colour="red")]),
        table_doc(extra_top="x"),
        {
            "format": "diag-sim/1",
            "ecus": [ecu([entry("22 F1 90", "62 F1 90 01")], voltage="12")],
        },
    ],
    ids=["entry", "top-level", "ecu"],
)
def test_unknown_table_key_refused_and_named(set_core_config, tmp_path, doc) -> None:
    setup_c6(set_core_config)
    message = refusal(tmp_path, doc)
    assert re.search(r"colour|extra_top|voltage", message), message


def test_known_keys_src_and_session_accepted(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    doc = table_doc([entry("22 F1 90", "62 F1 90 01", src="synthetic", session="03")])
    assert accept(tmp_path, doc)


def test_unknown_yaml_key_refused_and_named(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="colour"):
        validate(server(write_table(tmp_path, table_doc()), colour="red"))


# ---------------------------------------------------------------------------- duplicate req


def test_duplicate_req_refused_and_named(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    doc = table_doc(
        [entry("22 F1 90", "62 F1 90 01"), entry("22 F1 90", "62 F1 90 02")]
    )
    message = refusal(tmp_path, doc)
    assert "22 F1 90" in message, message


def test_duplicate_prefix_req_refused(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    doc = table_doc([entry("22 01 *", "7F 22 31"), entry("22 01 *", "7F 22 11")])
    assert "22 01 *" in refusal(tmp_path, doc)


def test_exact_and_prefix_with_the_same_bytes_are_not_duplicates(
    set_core_config, tmp_path
) -> None:
    """The spec lets an exact entry beat a prefix entry, so the two must be able to coexist."""
    setup_c6(set_core_config)
    doc = table_doc([entry("22 01 02", "62 01 02 AA"), entry("22 01 02 *", "7F 22 31")])
    assert accept(tmp_path, doc)


def test_same_req_in_two_ecus_is_not_a_duplicate(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    doc = table_doc()
    doc["ecus"].append(
        ecu(
            [entry("22 F1 90", "62 F1 90 09")],
            name="gearbox",
            req="0x7E1",
            resp="0x7E9",
        )
    )
    assert accept(tmp_path, doc)


# --------------------------------------------------------------------------------------- bad hex


@pytest.mark.parametrize(
    ("req", "resp", "offender"),
    [
        ("22 GG", "62 01", "GG"),
        ("22 F1 9", "62 01", "9"),
        ("22F190", "62 01", "22F190"),
        ("0x22 F1", "62 01", "0x22"),
        ("22 F1 90", "62 01 ZZ", "ZZ"),
        ("22 F1 90", "62,01", "62,01"),
    ],
)
def test_bad_hex_refused_and_names_the_offender(
    set_core_config, tmp_path, req, resp, offender
) -> None:
    setup_c6(set_core_config)
    message = refusal(tmp_path, table_doc([entry(req, resp)]))
    assert offender in message, message


@pytest.mark.parametrize(
    ("req", "resp"),
    [("", "62 01"), ("22 F1", ""), ("22 * 01", "62 01"), ("22 F1", "62 *")],
    ids=["empty-req", "empty-resp", "star-not-trailing", "star-in-resp"],
)
def test_malformed_req_or_resp_refused(set_core_config, tmp_path, req, resp) -> None:
    setup_c6(set_core_config)
    assert refusal(tmp_path, table_doc([entry(req, resp)]))


def test_bad_session_byte_refused_and_named(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    doc = table_doc([entry("22 F1 90", "62 F1 90 01", session="3G")])
    assert "3G" in refusal(tmp_path, doc)


# ----------------------------------------------------------------------- isotp consumer already taken


def test_isotp_taken_by_a_uds_hub_refused_naming_both(
    set_core_config, tmp_path
) -> None:
    setup_c6(set_core_config)
    block = validate(server(write_table(tmp_path, table_doc())))
    hub = uds_hub()
    with pytest.raises(cv.Invalid) as excinfo:
        final_validate(block, full_config([block], hubs=[hub]))
    message = str(excinfo.value)
    assert ISOTP_ID in message, message
    assert "bench_hub" in message, message
    assert "uds" in message, message


def test_isotp_taken_by_another_diag_server_refused_naming_both(
    set_core_config, tmp_path
) -> None:
    setup_c6(set_core_config)
    path = write_table(tmp_path, table_doc())
    first = validate(server(path, id="sim_first"))
    second = validate(server(path, id="sim_second"))
    config = full_config([first, second])
    with pytest.raises(cv.Invalid) as excinfo:
        final_validate(first, config)
    message = str(excinfo.value)
    assert ISOTP_ID in message and "sim_second" in message, message
    with pytest.raises(cv.Invalid) as excinfo:
        final_validate(second, config)
    message = str(excinfo.value)
    assert ISOTP_ID in message and "sim_first" in message, message


def test_servers_on_separate_isotp_instances_accepted(
    set_core_config, tmp_path
) -> None:
    setup_c6(set_core_config)
    first = validate(server(write_table(tmp_path, table_doc(), "a.json"), id="sim_a"))
    gearbox = table_doc()
    gearbox["ecus"] = [
        ecu(
            [entry("22 F1 90", "62 F1 90 09")],
            name="gearbox",
            req="0x7E1",
            resp="0x7E9",
        )
    ]
    second = validate(
        server(
            write_table(tmp_path, gearbox, "b.json"),
            id="sim_b",
            isotp_id="iso_gearbox",
            ecu="gearbox",
        )
    )
    isotps = [isotp_block(), isotp_block(id="iso_gearbox", tx_id=0x7E9, rx_id=0x7E1)]
    config = full_config([first, second], isotps=isotps)
    assert final_validate(first, config) is first
    assert final_validate(second, config) is second
