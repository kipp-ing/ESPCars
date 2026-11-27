"""Schema and table-file validation that is not one of the named refusals.

Spec: "everything in it starts off" (enabled false, all faults off, s3_timeout 5s), the required
keys of the YAML block, and the `diag-sim/1` table format with its ECU selection.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from esphome import config_validation as cv

from .common import (
    accept,
    ecu,
    entry,
    final_validate,
    full_config,
    isotp_block,
    refusal,
    server,
    setup_c6,
    table_doc,
    validate,
    write_table,
)

MINI = (
    Path(__file__).resolve().parent.parent / "components" / "diag_server" / "mini.json"
)


def test_everything_starts_off(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    config = validate(server(write_table(tmp_path, table_doc())))
    assert config["enabled"] is False
    assert config["s3_timeout"].total_milliseconds == 5000
    assert config["faults"]["delay"].total_milliseconds == 0
    assert config["faults"]["pending_78"] is False
    assert config["faults"]["drop_every"] == 0


def test_faults_accept_the_documented_values(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    config = validate(
        server(
            write_table(tmp_path, table_doc()),
            enabled=True,
            s3_timeout="2s",
            faults={"delay": "250ms", "pending_78": True, "drop_every": 3},
        )
    )
    assert config["enabled"] is True
    assert config["s3_timeout"].total_milliseconds == 2000
    assert config["faults"]["delay"].total_milliseconds == 250
    assert config["faults"]["pending_78"] is True
    assert config["faults"]["drop_every"] == 3


@pytest.mark.parametrize("key", ["isotp_id", "table", "ecu"])
def test_required_keys(set_core_config, tmp_path, key) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match=key):
        block = server(write_table(tmp_path, table_doc()))
        del block[key]
        validate(block)


@pytest.mark.parametrize(
    "faults",
    [{"drop_every": -1}, {"delay": "-5ms"}, {"pending_78": "maybe"}, {"colour": 1}],
)
def test_bad_fault_values_refused(set_core_config, tmp_path, faults) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(server(write_table(tmp_path, table_doc()), faults=faults))


def test_missing_table_file_refused(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(server(str(tmp_path / "absent.json")))


def test_unknown_ecu_name_is_named(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    assert "transmission" in refusal(tmp_path, table_doc(), ecu="transmission")


def test_second_ecu_of_the_table_is_selectable(set_core_config, tmp_path) -> None:
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

    block = validate(server(write_table(tmp_path, doc), ecu="gearbox"))
    config = full_config([block], isotps=[isotp_block(tx_id=0x7E9, rx_id=0x7E1)])
    assert final_validate(block, config) is block


@pytest.mark.parametrize(
    "text",
    ["{not json", "[]", '{"format": "diag-sim/1"}', '{"ecus": []}', ""],
    ids=["syntax", "not-an-object", "no-ecus", "no-format", "empty-file"],
)
def test_unreadable_or_incomplete_table_refused(
    set_core_config, tmp_path, text
) -> None:
    setup_c6(set_core_config)
    assert refusal(tmp_path, text)


def test_other_format_version_refused_and_named(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    assert "diag-sim/2" in refusal(tmp_path, table_doc(format="diag-sim/2"))


def test_entry_without_resp_or_req_refused(set_core_config, tmp_path) -> None:
    setup_c6(set_core_config)
    assert "resp" in refusal(tmp_path, table_doc([{"req": "22 F1 90"}]))
    assert "req" in refusal(tmp_path, table_doc([{"resp": "62 F1 90"}]))


def test_the_checked_in_fictional_fixture_validates(set_core_config, tmp_path) -> None:
    """tests/components/diag_server/mini.json with its documented isotp pair (rx 0x651, tx 0x659)."""
    setup_c6(set_core_config)

    block = validate(server(str(MINI), ecu="MINI"))
    config = full_config(
        [block], isotps=[isotp_block(tx_id=0x659, rx_id=0x651, max_message_size=64)]
    )
    assert final_validate(block, config) is block
    assert json.loads(MINI.read_text(encoding="utf-8"))["format"] == "diag-sim/1"


def test_accepts_entries_with_the_spec_example_shapes(
    set_core_config, tmp_path
) -> None:
    """The three entry forms of the spec example: exact read, session control, prefix NRC."""
    setup_c6(set_core_config)
    doc = table_doc(
        [
            entry("22 F1 90", "62 F1 90 57 44 44 41 42 43", src="run 2026-01-01"),
            entry("10 03", "50 03 00 32 01 F4", src="synthetic"),
            entry("22 01 *", "7F 22 31", src="synthetic"),
        ]
    )
    assert accept(tmp_path, doc)
