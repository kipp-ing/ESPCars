"""script/dbc2yaml.py — DBC signal definitions into can_gateway sensor YAML.

The fixture below is a hand-written miniature DBC rather than the real vehicle
file: the real one lives outside the repo and cannot be a test dependency, and
a fixture we control is the only way to pin the two cases that matter most —
a signal whose all-ones value is *not* "not available", and a Motorola signal
that must be refused rather than mistranslated.

Every number here was computed by hand from the DBC spec, not read back out of
the implementation.
"""

import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def _load():
    """Import script/dbc2yaml.py by path — script/ is not a package."""
    spec = importlib.util.spec_from_file_location(
        "dbc2yaml", REPO / "script" / "dbc2yaml.py"
    )
    mod = importlib.util.module_from_spec(spec)
    sys.modules["dbc2yaml"] = mod
    spec.loader.exec_module(mod)
    return mod


dbc2yaml = _load()


# 0x076 = 118, 0x043 = 67. Signal layouts echo the shapes seen in real vehicle
# DBC files, trimmed to what each case needs.
MINI_DBC = """VERSION "test"

BO_ 118 CellMsg: 8 BMS
 SG_ CellVolt_Max : 0|12@1+ (0.001,1.5) [0|4.5] "V" Vector__XXX
 SG_ CellVolt_Min : 12|12@1+ (0.001,1.5) [0|4.5] "V" Vector__XXX
 SG_ Pls_Rq : 48|1@1+ (1,0) [0|0] "" Vector__XXX

BO_ 67 ErrMsg: 8 BMS
 SG_ CutSw_Err : 5|3@1+ (1,0) [0|7] "" Vector__XXX

BO_ 789 BigEndianMsg: 8 BMS
 SG_ PackVolt : 7|13@0+ (0.1,0) [0|800] "V" Vector__XXX
 SG_ ByteAligned : 31|16@0+ (1,0) [0|65535] "A" Vector__XXX

BA_ "GenSigSNA" SG_ 118 CellVolt_Max "FFFh";
BA_ "GenSigSNA" SG_ 118 CellVolt_Min "FFFh";
BA_ "GenSigSNA" SG_ 118 Pls_Rq "0h";
BA_ "GenSigSNA" SG_ 67 CutSw_Err "7h";
BA_ "GenSigSNA" SG_ 789 PackVolt "1FFFh";

VAL_ 67 CutSw_Err 7 "ALL_ERR" 0 "NO_ERR" ;
"""


@pytest.fixture
def dbc(tmp_path):
    p = tmp_path / "mini.dbc"
    # latin-1 with CRLF, exactly like the vehicle files this parses.
    p.write_bytes(MINI_DBC.replace("\n", "\r\n").encode("latin-1"))
    return p


@pytest.fixture
def parsed(dbc):
    text = dbc.read_text(encoding="latin-1")
    return (
        dbc2yaml.parse(text),
        dbc2yaml.parse_sna(text),
        dbc2yaml.parse_val(text),
    )


def test_parse_reads_messages_and_signal_geometry(parsed):
    msgs, _, _ = parsed
    assert set(msgs) == {0x076, 0x043, 0x315}
    cell = msgs[0x076]
    assert cell["name"] == "CellMsg" and cell["dlc"] == 8
    sig = cell["signals"][0]
    assert sig["name"] == "CellVolt_Max"
    assert (sig["start"], sig["length"]) == (0, 12)
    assert sig["little_endian"] is True and sig["signed"] is False
    assert sig["factor"] == 0.001 and sig["offset"] == 1.5
    assert sig["unit"] == "V"


def test_latin1_crlf_file_parses(parsed):
    """A stray 0xB0 must not abort the parse — these files are never UTF-8."""
    msgs, _, _ = parsed
    assert msgs, "CRLF/latin-1 file produced no messages"


def test_sna_map_reads_hex_values_and_drops_zero(parsed):
    _, sna, _ = parsed
    assert sna[(0x076, "CellVolt_Max")] == 0xFFF
    assert sna[(0x043, "CutSw_Err")] == 7
    # "0h" means the signal has no SNA at all, not that raw 0 is unavailable.
    assert (0x076, "Pls_Rq") not in sna


def test_intel_signal_maps_straight_to_bit_offsets(parsed):
    msgs, _, _ = parsed
    sig = msgs[0x076]["signals"][1]  # CellVolt_Min, start 12 len 12
    kind, kw, reason = dbc2yaml.position(sig)
    assert kind == "bit" and reason == ""
    assert kw == {"bit_offset": 12, "bit_length": 12}


def test_motorola_non_byte_aligned_is_refused(parsed):
    """The component has no bit-level big-endian form; emitting one anyway
    would read the wrong bits and look plausible. PackVolt is 13 bits — the
    same odd width a real Motorola pack-voltage signal uses in the wild."""
    msgs, _, _ = parsed
    sig = next(s for s in msgs[0x315]["signals"] if s["name"] == "PackVolt")
    kind, _, reason = dbc2yaml.position(sig)
    assert kind is None
    assert "Motorola" in reason


def test_motorola_byte_aligned_uses_the_byte_form(parsed):
    """A DBC Motorola start bit is the field's MSB in sawtooth numbering, so a
    byte-aligned field starts at 7, 15, 23, 31... — start % 8 == 7. Reading it
    as % 8 == 0 refuses every real big-endian signal in the file."""
    msgs, _, _ = parsed
    sig = next(s for s in msgs[0x315]["signals"] if s["name"] == "ByteAligned")
    kind, kw, _ = dbc2yaml.position(sig)
    assert kind == "byte"
    assert kw == {"offset": 3, "length": 2, "byte_order": "big"}


def test_sna_all_ones_is_reported_as_all_ones(parsed):
    msgs, sna, val = parsed
    sig = msgs[0x076]["signals"][0]
    assert dbc2yaml.sna_for(msgs[0x076], sig, sna, val, True) == "all_ones"


def test_sna_is_omitted_when_all_ones_is_a_real_enum_value(parsed):
    """CutSw_Err raw 7 is ALL_ERR — a genuine contactor fault. Turning
    that into NaN would erase the one event worth seeing."""
    msgs, sna, val = parsed
    sig = msgs[0x043]["signals"][0]
    assert dbc2yaml.sna_for(msgs[0x043], sig, sna, val, True) is None


def test_sna_is_omitted_for_a_signal_with_no_sna(parsed):
    msgs, sna, val = parsed
    sig = next(s for s in msgs[0x076]["signals"] if s["name"] == "Pls_Rq")
    assert dbc2yaml.sna_for(msgs[0x076], sig, sna, val, True) is None


def test_sna_can_be_disabled_entirely(parsed):
    msgs, sna, val = parsed
    sig = msgs[0x076]["signals"][0]
    assert dbc2yaml.sna_for(msgs[0x076], sig, sna, val, False) is None


def test_generated_yaml_is_parseable_and_carries_the_geometry(dbc, capsys):
    yaml = pytest.importorskip("yaml")
    rc = dbc2yaml.main([str(dbc), "--id", "0x076", "--port", "seg2"])
    assert rc == 0
    doc = yaml.safe_load(capsys.readouterr().out)
    sensors = doc["sensor"]
    by_name = {s["name"].split()[-1]: s for s in sensors}
    vmax = by_name["CellVolt_Max"]
    assert vmax["can_id"] == 0x076
    assert vmax["bit_offset"] == 0 and vmax["bit_length"] == 12
    assert vmax["port_id"] == "seg2"
    # Scaling stays in ESPHome filters so NaN propagates untouched.
    assert {"multiply": 0.001} in vmax["filters"]
    assert {"offset": 1.5} in vmax["filters"]


def test_undecodable_signals_are_absent_from_the_output_not_silently_wrong(dbc, capsys):
    rc = dbc2yaml.main([str(dbc), "--id", "0x315"])
    assert rc == 0
    out = capsys.readouterr()
    assert "PackVolt" not in out.out, "a refused signal must not be emitted"
    assert "PackVolt" in out.err, "a refused signal must be reported"
