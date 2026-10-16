"""script/sdlog_uds.py via sdlog_decode.py — diagnostic answers into signals and events
(docs/sdlog-decode-spec.md §7, U1–U16).

The catalog is the fictional ``MINI`` fixture (``tests/uds/fixtures/mini.dcat``, request
``0x7E7``, response ``0x7EF``). Expected values are computed by hand from the answer bytes in
the comments, not by ``catalog.py``, so a wrong field offset, sign or scale in how the decoder
drives the catalog shows up here. Scale factors are binary32 in the catalog (format §6), hence
``approx``.
"""

from __future__ import annotations

import ast
import hashlib
import importlib.util
import math
import os
import sys
import uuid
from pathlib import Path

import pytest

pytest.importorskip("cantools")

_ROOT = Path(__file__).parent.parent.parent
_SCRIPT = _ROOT / "script"
MINI_DBC = Path(__file__).parent / "fixtures" / "mini.dbc"
MINI_DCAT = _ROOT / "tests" / "uds" / "fixtures" / "mini.dcat"



def _load(name: str):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, _SCRIPT / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


dec = _load("sdlog_decode")
uds = _load("sdlog_uds")
lib = uds.lib

RX, TX = 0x7EF, 0x7E7

NAMES = """[fields]
DT_Mini_Voltage_Maximum_Cell = "cell_voltage_max"
"0x0208/PRES_Mini_Cell_2Byte" = "cell_voltage"
PRES_Mini_State = "contactor"
"""

DTC_TEXTS = """# fictional fault texts
P056200\tSupply voltage too low
"""


# ------------------------------------------------------------------ chunk writing


def tp(payload: bytes, can_id: int = RX, pad: int = 0xAA) -> list[tuple[int, bytes]]:
    """ISO 15765-2 frames for one message (classic CAN, padded to 8 bytes)."""
    if len(payload) <= 7:
        return [(can_id, bytes([len(payload)]) + payload + bytes([pad]) * (7 - len(payload)))]
    frames = [(can_id, bytes([0x10 | len(payload) >> 8, len(payload) & 0xFF]) + payload[:6])]
    rest, sn = payload[6:], 1
    while rest:
        frames.append((can_id, bytes([0x20 | sn]) + rest[:7].ljust(7, bytes([pad]))))
        rest, sn = rest[7:], (sn + 1) & 0xF
    return frames


def chunk_lines(frames: list[tuple]) -> list[str]:
    """Frame i sits at UTC 1 000 000 µs + i ms (anchor at boot 0x1000, 1 ms apart): utc = 1000 + i."""
    lines = [
        "#sdlog,2,7,@0,2026.7.0",
        "#origin,mr-orange",
        "#src,C,EV,1,can_gateway,500000",
        "#src,C,AUX,2,can_gateway,500000",
        "#utc,1000,F4240",
    ]
    for i, (can_id, data, *rest) in enumerate(frames):
        tag = rest[0] if rest else "1"
        flags = rest[1] if len(rest) > 1 else ""
        lines.append(f"C{tag}{flags},{'@1000' if i == 0 else '3E8'},{can_id:X},{data.hex().upper()}")
    lines.append("#close,@FFFFFF,rotate")
    return lines


def write_chunk(archive: Path, frames: list[tuple], name: str = "L0000007.LOG") -> Path:
    directory = archive / "mr-orange"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_bytes(("\n".join(chunk_lines(frames)) + "\n").encode("latin-1"))
    return path


# The reference answers, by hand:
#   62 01 01 0BB8 FFFF        max cell 3000*0.001+1.5 = 4.5 V; min SNA -> no row
#   62 01 02 0640 FF80 8000   max 1600/64 = 25.0, min int16(FF80)=-128/64 = -2.0, mean SNA (raw -32768)
#   62 D0 00 02               contactor: row (2,2) factor 0 offset 0 -> value 0.0, text "closed"
#   62 F1 90 44454D4F         VIN, ASCII: "DEMO"
#   62 F1 8C 12AB             serial, HEXDUMP: "12AB"
#   62 02 08 0D00 0D40 0CC0 FFFF   cells /1024: 3.25, 3.3125, 3.1875, SNA (11 bytes: FF + 1 CF)
REFERENCE = [
    bytes.fromhex("6201010BB8FFFF"),
    bytes.fromhex("620102 0640 FF80 8000"),
    bytes.fromhex("62D00002"),
    bytes.fromhex("62F19044454D4F"),
    bytes.fromhex("62F18C12AB"),
    bytes.fromhex("620208 0D00 0D40 0CC0 FFFF"),
]

EXPECTED = {
    "MINI.cell_voltage_max": (4.5, ""),
    "MINI.DT_Mini_Temperature.DT_Mini_Temperature_Maximum": (25.0, ""),
    "MINI.DT_Mini_Temperature.DT_Mini_Temperature_Minimum": (-2.0, ""),
    "MINI.contactor": (0.0, "closed"),
    "MINI.DT_Mini_VIN.PRES_Mini_VIN_4Byte": (math.nan, "DEMO"),
    "MINI.DT_Mini_Serial.PRES_Mini_Hex_2Byte": (math.nan, "12AB"),
    "MINI.cell_voltage.1": (3.25, ""),
    "MINI.cell_voltage.2": (3.3125, ""),
    "MINI.cell_voltage.3": (3.1875, ""),
}


def frames_of(*payloads: bytes) -> list[tuple[int, bytes]]:
    return [f for p in payloads for f in tp(p)]


@pytest.fixture
def setup(tmp_path: Path):
    archive = tmp_path / "archive"
    files = tmp_path / "files"
    files.mkdir()
    (files / "mini.dbc").write_bytes(MINI_DBC.read_bytes())
    (files / "mini.dcat").write_bytes(MINI_DCAT.read_bytes())
    (files / "mini.names.toml").write_text(NAMES)
    (files / "mini-dtc.txt").write_text(DTC_TEXTS)
    write_chunk(archive, frames_of(*REFERENCE))
    config = tmp_path / "decode.toml"
    config.write_text(config_text(archive, files))
    return archive, files, config


def config_text(archive: Path, files: Path, uds_body: str | None = None) -> str:
    if uds_body is None:
        uds_body = (
            f'label = "EV"\ndcat = "{files / "mini.dcat"}"\nnames = "{files / "mini.names.toml"}"\n'
            f'dtc = "{files / "mini-dtc.txt"}"\n'
        )
    return (
        f'archive = "{archive}"\nsink = "fake"\n\n[[device]]\nname = "mr-orange"\nvehicle = "demo-car"\n'
        f'[device.dbc]\nEV = "{files / "mini.dbc"}"\n\n[[device.uds]]\n{uds_body}'
    )


def run(config: Path, sink) -> int:
    return dec.main(["run", "--config", str(config)], sink=sink)


def decode(setup, *frames) -> dec.FakeSink:
    archive, _, config = setup
    write_chunk(archive, list(frames))
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    (marker,) = sink.tables["chunks"]
    assert marker["result"] == "ok", marker["detail"]
    return sink


def signals(sink) -> dict[str, tuple[float, str]]:
    return {s["signal"]: (s["value"], s["text"]) for s in sink.tables["signals"]}


def events(sink, kind: str | None = None) -> list[tuple]:
    return [
        (e["kind"], e["name"], e["value"], e["text"], e["detail"])
        for e in sink.tables["events"]
        if kind is None or e["kind"] == kind
    ]


def assert_values(got: dict, expected: dict) -> None:
    assert set(got) == set(expected)
    for name, (value, text) in expected.items():
        assert got[name][1] == text, name
        if math.isnan(value):
            assert math.isnan(got[name][0]), name
        else:
            assert got[name][0] == pytest.approx(value, rel=1e-5), name


def write_catalog(path: Path, ecus) -> Path:
    path.write_bytes(lib.write_catalog(lib.Catalog(ecus=tuple(ecus))))
    return path


def ecu(name: str, groups, request_id: int = TX, response_id: int = RX):
    return lib.Ecu(name=name, request_id=request_id, response_id=response_id, groups=tuple(groups))


# ------------------------------------------------------------------ U1 config


def _uds(files: Path, **over) -> str:
    entry = {"label": '"EV"', "dcat": f'"{files / "mini.dcat"}"'}
    entry.update(over)
    return "".join(f"{k} = {v}\n" for k, v in entry.items() if v is not None)


@pytest.mark.parametrize(
    "case, why",
    [
        ("unknown key", "unknown key"),
        ("no label", "label is required"),
        ("no dcat", "dcat is required"),
        ("missing dcat", "dcat file not found"),
        ("garbage dcat", "is not a catalog"),
        ("unknown ecu", "is not in"),
        ("several ecus", "several ECUs"),
        ("missing names", "names file not found"),
        ("unknown field", "no such field"),
        ("ambiguous field", "is ambiguous"),
        ("bad name", "is not a name"),
        ("duplicate name", "is given to both"),
        ("one signal twice", "two fields end up as signal"),
        ("same id twice", "both use id 0x7E"),
        ("bad dtc line", "expected CODE<TAB>text"),
    ],
)
def test_uds_config_rejects(tmp_path: Path, setup, capsys, case: str, why: str) -> None:
    archive, files, config = setup
    body = _uds(files)
    extra = ""
    names = files / "names.toml"
    if case == "unknown key":
        body = _uds(files, bogus="1")
    elif case == "no label":
        body = _uds(files, label=None)
    elif case == "no dcat":
        body = _uds(files, dcat=None)
    elif case == "missing dcat":
        body = _uds(files, dcat='"/no/such.dcat"')
    elif case == "garbage dcat":
        (files / "bad.dcat").write_bytes(b"not a catalog at all, not even close" * 4)
        body = _uds(files, dcat=f'"{files / "bad.dcat"}"')
    elif case == "unknown ecu":
        body = _uds(files, ecu='"NOPE"')
    elif case == "several ecus":
        group = lib.Group("G", b"\x22\x00\x01", fields=(lib.Field("F", 24, 8),))
        two = write_catalog(files / "two.dcat", [ecu("A", [group]), ecu("B", [group], 0x7E0, 0x7E8)])
        body = _uds(files, dcat=f'"{two}"')
    elif case == "missing names":
        body = _uds(files, names='"/no/such.toml"')
    elif case in ("unknown field", "ambiguous field", "bad name", "duplicate name", "one signal twice"):
        dcat = files / "mini.dcat"
        text = {
            "unknown field": '[fields]\nPRES_Mini_Nope = "x"\n',
            "ambiguous field": '[fields]\nF = "x"\n',
            "bad name": '[fields]\nPRES_Mini_State = "9lives"\n',
            "duplicate name": '[fields]\nPRES_Mini_State = "x"\nDT_Mini_Voltage_Maximum_Cell = "x"\n',
            "one signal twice": '[fields]\n"G/F" = "cells"\n',
        }[case]
        if case == "ambiguous field":
            dcat = write_catalog(files / "amb.dcat", [ecu("A", [
                lib.Group("G1", b"\x22\x00\x01", fields=(lib.Field("F", 24, 8),)),
                lib.Group("G2", b"\x22\x00\x02", fields=(lib.Field("F", 24, 8),)),
            ])])
        if case == "one signal twice":
            # "cells" (an array of 2) becomes A.cells.1/.2; the unnamed field "1" of group "cells" is A.cells.1 too
            dcat = write_catalog(files / "twice.dcat", [ecu("A", [
                lib.Group("G", b"\x22\x00\x01", fields=(lib.Field("F", 24, 8, repeat_count=2, repeat_stride=8),)),
                lib.Group("cells", b"\x22\x00\x02", fields=(lib.Field("1", 24, 8),)),
            ])])
        names.write_text(text)
        body = _uds(files, dcat=f'"{dcat}"', names=f'"{names}"')
    elif case == "same id twice":
        extra = f"\n[[device.uds]]\n{_uds(files)}"
    elif case == "bad dtc line":
        (files / "bad-dtc.txt").write_text("P0562\ttoo short a code\n")
        body = _uds(files, dtc=f'"{files / "bad-dtc.txt"}"')
    config.write_text(config_text(archive, files, body + extra))
    assert run(config, dec.FakeSink()) == 2
    assert why in capsys.readouterr().err


def test_an_ecu_name_that_is_a_dbc_message_exits_two(setup, capsys) -> None:
    archive, files, config = setup
    group = lib.Group("G", b"\x22\x00\x01", fields=(lib.Field("F", 24, 8),))
    clash = write_catalog(files / "clash.dcat", [ecu("DEMO_STATUS", [group])])
    config.write_text(config_text(archive, files, _uds(files, dcat=f'"{clash}"')))
    assert run(config, dec.FakeSink()) == 2
    assert "also a DBC message name" in capsys.readouterr().err


# ------------------------------------------------------------------ U2–U6 reassembly, values, names


def test_values_against_hand_computed_reference(setup) -> None:
    _, _, config = setup
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    assert_values(signals(sink), EXPECTED)
    assert {s["vehicle"] for s in sink.tables["signals"]} == {"demo-car"}
    (marker,) = sink.tables["chunks"]
    # 6 answers; unavailable: min cell, mean temp, cell 4; the contactor's first state is one event
    assert marker["detail"] == "uds: 6 answers, 3 unavailable values, 1 events"


def test_single_and_multi_frame_answers_decode(setup) -> None:
    """U2: the 11-byte cell answer is a first frame + 1 consecutive frame; its utc is the last one's."""
    sink = decode(setup, *frames_of(REFERENCE[2], REFERENCE[5]))
    got = {s["signal"]: s["utc"] for s in sink.tables["signals"]}
    assert got["MINI.contactor"] == 1000  # frame 0, a single frame
    assert got["MINI.cell_voltage.1"] == 1002  # frames 1..2, the last at +2 ms
    assert len(tp(REFERENCE[5])) == 2


def test_other_ids_and_labels_are_ignored(setup) -> None:
    """U2: the same answer on another label, another id or as an extended frame is not the ECU's."""
    contactor = tp(REFERENCE[2])[0][1]
    sink = decode(setup, (RX, contactor, "2"), (0x7EE, contactor), (RX, contactor, "1", "x"))
    assert sink.tables["signals"] == [] and sink.tables["events"] == []
    (marker,) = sink.tables["chunks"]
    assert marker["matched"] == 0


def test_an_answer_decodes_against_the_echoed_group(setup) -> None:
    """U3: 62 01 01 and 62 01 02 share a length; the echoed DID picks the group."""
    sink = decode(setup, *frames_of(bytes.fromhex("6201020640FF808000")))
    assert set(signals(sink)) == {
        "MINI.DT_Mini_Temperature.DT_Mini_Temperature_Maximum",
        "MINI.DT_Mini_Temperature.DT_Mini_Temperature_Minimum",
    }


def test_an_unknown_answer_writes_nothing(setup) -> None:
    """U3: tester present (7E 00) and a DID the catalog does not hold."""
    sink = decode(setup, *frames_of(bytes.fromhex("7E00"), bytes.fromhex("62ABCD01")))
    assert sink.tables["signals"] == [] and sink.tables["events"] == []
    assert sink.tables["chunks"][0]["detail"] == "uds: 2 answers, 2 unknown answers, 0 events"


def test_a_short_answer_decodes_what_it_covers(setup) -> None:
    """U5: an ECU with two cells wired answers 7 bytes; cells 3 and 4 are skipped and counted."""
    sink = decode(setup, *frames_of(bytes.fromhex("6202080D000D40")))
    assert_values(signals(sink), {"MINI.cell_voltage.1": (3.25, ""), "MINI.cell_voltage.2": (3.3125, "")})
    assert sink.tables["chunks"][0]["detail"] == (
        "uds: 1 answers, 1 partial answers, 2 values past the answer's end, 0 events"
    )


def test_readable_names_and_array_elements(setup) -> None:
    """U6: without a names file every field is <ECU>.<group>.<field>; an array of 4 has width 1."""
    archive, files, config = setup
    config.write_text(config_text(archive, files, _uds(files)))
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    got = set(signals(sink))
    assert "MINI.DT_Mini_Voltage.DT_Mini_Voltage_Maximum_Cell" in got
    assert "MINI.DT_Mini_Cells_Voltage_Cell.PRES_Mini_Cell_2Byte.3" in got
    assert "MINI.DT_Mini_Contactor_State.PRES_Mini_State" in got


def test_array_elements_are_zero_padded_to_the_count(setup) -> None:
    """U6: 12 elements sort as .01 … .12."""
    archive, files, config = setup
    cells = lib.Field("C", 24, 8, repeat_count=12, repeat_stride=8)
    dcat = write_catalog(files / "twelve.dcat", [ecu("A", [lib.Group("G", b"\x22\x00\x01", fields=(cells,))])])
    config.write_text(config_text(archive, files, _uds(files, dcat=f'"{dcat}"')))
    write_chunk(archive, frames_of(bytes([0x62, 0x00, 0x01, *range(12)])))
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    got = signals(sink)
    assert sorted(got) == [f"A.G.C.{n:02d}" for n in range(1, 13)]
    assert got["A.G.C.12"] == (11.0, "")  # no scale rows: the raw value


# ------------------------------------------------------------------ U7–U11 events


def contactor(raw: int) -> bytes:
    return bytes([0x62, 0xD0, 0x00, raw])


def test_state_events(setup) -> None:
    """U7: open, open (no event), closed, SNA (the sentinel's text), 1 (no row: unavailable, no text)."""
    sink = decode(setup, *frames_of(*(contactor(r) for r in (0, 0, 2, 255, 1))))
    got = events(sink, "state")
    assert [(k, n, t, d) for k, n, _, t, d in got] == [
        ("state", "MINI.contactor", "open", "initial"),
        ("state", "MINI.contactor", "closed", "from open"),
        ("state", "MINI.contactor", "SNA", "from closed"),
        ("state", "MINI.contactor", "", "from SNA"),
    ]
    assert got[0][2] == 0.0 and math.isnan(got[2][2]) and math.isnan(got[3][2])
    assert [e["utc"] for e in sink.tables["events"] if e["kind"] == "state"] == [1000, 1002, 1003, 1004]
    # SNA and no row leave the group's one numeric field unavailable: U11 reports each answer too
    assert events(sink, "unavailable") == [("unavailable", "DT_Mini_Contactor_State", 1.0, "", "")] * 2
    assert [s["text"] for s in sink.tables["signals"]] == ["open", "open", "closed"]  # SNA and no row: no signal


def test_a_measurement_with_a_text_row_is_not_a_state(setup) -> None:
    """U7: a linear temperature with a valid "value not defined" row beside it is a measurement;
    its readings are signals (the text row's too), never state events."""
    archive, files, config = setup
    temp = lib.Field(
        "T", 24, 8,
        scales=(lib.Scale(0, 99, 1.0, -40.0), lib.Scale(100, 254, 1.0, 0.0, "value not defined"),
                lib.Scale(255, 255, 0.0, 0.0, "SNA", True)),
    )
    dcat = write_catalog(files / "temp.dcat", [ecu("A", [lib.Group("G", b"\x22\x00\x01", fields=(temp,))])])
    config.write_text(config_text(archive, files, _uds(files, dcat=f'"{dcat}"')))
    write_chunk(archive, frames_of(*(bytes([0x62, 0x00, 0x01, raw]) for raw in (60, 61, 120))))
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    assert events(sink) == []
    assert [(s["value"], s["text"]) for s in sink.tables["signals"]] == [(20.0, ""), (21.0, ""), (120.0, "value not defined")]


def dtc_answer(*records: str) -> bytes:
    return bytes.fromhex("5902FF" + "".join(records))


def test_dtc_events(setup) -> None:
    """U8: 05 62 00 = P0 5 62 + FTB 00 -> P056200 (known text); 92 1F 20: 0x92 = 10 01 0010 -> B1 2 1F + 20 -> B121F20."""
    sink = decode(
        setup,
        *frames_of(
            dtc_answer("0562002F"),  # initial
            dtc_answer("0562002F"),  # unchanged: no event
            dtc_answer("0562002E", "921F2008"),  # status change, new code
            dtc_answer("921F2008"),  # P056200 cleared
        ),
    )
    assert events(sink, "dtc") == [
        ("dtc", "P056200", 0x2F, "Supply voltage too low", "initial"),
        ("dtc", "P056200", 0x2E, "Supply voltage too low", "status 0x2F->0x2E"),
        ("dtc", "B121F20", 0x08, "", "new"),
        ("dtc", "P056200", 0.0, "Supply voltage too low", "cleared"),
    ]
    assert {e["source"] for e in sink.tables["events"]} == {"MINI"}
    assert sink.tables["signals"] == []


def test_dtc_answers_to_different_logged_requests_are_followed_apart(setup) -> None:
    """U8: with the requests logged, a mask-08 read and a mask-FF read are two histories."""
    confirmed = [*tp(bytes.fromhex("190208"), TX), *frames_of(dtc_answer("0562002F"))]
    everything = [*tp(bytes.fromhex("1902FF"), TX), *frames_of(dtc_answer("0562002F", "921F2008"))]
    sink = decode(setup, *confirmed, *everything, *confirmed)
    assert [(n, d) for _, n, _, _, d in events(sink, "dtc")] == [
        ("P056200", "initial"), ("P056200", "initial"), ("B121F20", "initial"),
    ]


def test_nrc_events(setup) -> None:
    """U9: 7F 22 31 without a logged request names the SID; with one, the request; 78 is counted only."""
    sink = decode(
        setup,
        *frames_of(bytes.fromhex("7F2231")),
        *tp(bytes.fromhex("220208"), TX),
        *frames_of(bytes.fromhex("7F2278"), bytes.fromhex("7F2231")),
    )
    assert events(sink) == [
        ("nrc", "SID 22", 0x31, "requestOutOfRange", ""),
        ("nrc", "22 02 08", 0x31, "requestOutOfRange", ""),
    ]
    assert "1 pending" in sink.tables["chunks"][0]["detail"]


def test_isotp_events_inside_but_not_at_the_edges(setup) -> None:
    """U10. The chunk opens with the tail of a message cut by the previous chunk (counted), then:
    a wrong SN (the rest of that message is not reported again), a good answer, two orphaned CFs
    (one event), a first frame broken off by a single frame, and a first frame still open at the
    end (counted)."""
    cells = tp(REFERENCE[5])  # FF, CF1
    temps = tp(REFERENCE[1])  # FF, CF1
    frames = [
        cells[1],  # cut at the start
        temps[0], (RX, bytes([0x22]) + temps[1][1][1:]),  # SN 2 where 1 belongs -> sequence
        *tp(REFERENCE[2]),
        cells[1], cells[1],  # CFs without a FF -> one orphan event
        cells[0], *tp(REFERENCE[2]),  # FF then a SF -> incomplete; the SF still decodes
        cells[0],  # open at the end
    ]
    sink = decode(setup, *frames)
    assert [(k, n, v, d) for k, n, v, _, d in events(sink, "isotp")] == [
        ("isotp", "sequence", 6.0, "expected SN 1, got 2"),
        ("isotp", "orphan", 7.0, "consecutive frame without a first frame"),
        ("isotp", "incomplete", 6.0, "6 of 11 bytes, then a new message"),
    ]
    assert "MINI.contactor" in signals(sink)
    detail = sink.tables["chunks"][0]["detail"]
    assert "1 frames cut at the start" in detail and "1 frames cut at the end" in detail


def test_an_all_unavailable_answer_is_an_event(setup) -> None:
    """U11: both cell voltages SNA -> one event for the group; one available value -> none."""
    sink = decode(setup, *frames_of(bytes.fromhex("620101FFFFFFFF"), bytes.fromhex("6201010BB8FFFF")))
    assert events(sink) == [("unavailable", "DT_Mini_Voltage", 2.0, "", "")]


# ------------------------------------------------------------------ U12–U15 coverage, catalog, schema


def test_diag_frames_count_as_matched(setup) -> None:
    """U12: requests and answers of the ECU are covered by the catalog; another id is not."""
    sink = decode(setup, *tp(bytes.fromhex("220208"), TX), *frames_of(REFERENCE[5]), (0x7EE, bytes(8)))
    (marker,) = sink.tables["chunks"]
    assert (marker["can_frames"], marker["matched"]) == (4, 3)


def test_a_names_edit_redecodes_without_duplicates(setup) -> None:
    """U13: the names file is part of the catalog hash; a rename replaces the rows."""
    _, files, config = setup
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    frames_before, events_before = len(sink.tables["frames"]), len(sink.tables["events"])
    (files / "mini.names.toml").write_text(NAMES.replace('"contactor"', '"main_contactor"'))
    assert run(config, sink) == 0
    got = signals(sink)
    assert "MINI.main_contactor" in got and "MINI.contactor" not in got
    assert len(sink.tables["signals"]) == len(EXPECTED)
    assert len(sink.tables["frames"]) == frames_before
    assert [e["name"] for e in sink.tables["events"]] == ["MINI.main_contactor"] * events_before


@pytest.mark.parametrize("which", ["dcat", "dtc"])
def test_dcat_and_dtc_edits_are_a_new_catalog(setup, which: str) -> None:
    _, files, config = setup
    device = dec.load_config(config).devices[0]
    before = dec.catalog_hash(device)
    path = files / ("mini.dcat" if which == "dcat" else "mini-dtc.txt")
    path.write_bytes(path.read_bytes() + (b"\0" if which == "dcat" else b"P000100\tx\n"))
    assert dec.catalog_hash(device) != before


def test_a_device_without_uds_keeps_its_catalog_hash(tmp_path: Path) -> None:
    """Upgrading does not re-decode a device that has no [[device.uds]]."""
    dbc = tmp_path / "mini.dbc"
    dbc.write_bytes(MINI_DBC.read_bytes())
    device = dec.Device(name="d", vehicle="v", dbc=(("EV", dbc),))
    expected = hashlib.sha256(b"EV\0" + dbc.read_bytes() + b"\0").hexdigest()
    assert dec.catalog_hash(device) == expected


def test_a_defect_writes_no_events(setup) -> None:
    """U14: a defect chunk deletes what a batch wrote and leaves only its marker."""
    archive, _, config = setup
    path = write_chunk(archive, frames_of(contactor(0)))
    path.write_bytes(path.read_bytes().replace(b"#close,@FFFFFF,rotate\n", b""))
    sink = dec.FakeSink()
    assert run(config, sink) == 1
    assert sink.tables["events"] == [] and sink.tables["signals"] == []


def test_a_crash_redoes_the_chunk_events_included(setup) -> None:
    """U14: a crash before the marker; the rerun replaces signals and events, no duplicates."""
    _, _, config = setup
    sink = dec.FakeSink(fail_on="chunks")
    assert run(config, sink) == 75
    assert sink.tables["events"]
    assert run(config, sink) == 0
    assert len(sink.tables["events"]) == 1
    assert len(sink.tables["signals"]) == len(EXPECTED)


def test_run_initializes_the_sink(setup) -> None:
    """U15: `run` brings the schema up to date first; DBC signals carry an empty text."""
    archive, _, config = setup
    write_chunk(archive, [(0x100, bytes.fromhex("E803F60300000000"))])
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    assert sink.initialized
    assert {s["text"] for s in sink.tables["signals"]} == {""}
    assert set(sink.tables["signals"][0]) == set(dec.SIGNAL_COLUMNS)


def test_uds_decode_imports_stdlib_only() -> None:
    """U16: sdlog_uds.py and the catalog reader it loads import only the standard library."""
    stdlib = set(sys.stdlib_module_names)
    for path in (_SCRIPT / "sdlog_uds.py", _ROOT / "components" / "uds" / "catalog.py"):
        for node in ast.walk(ast.parse(path.read_text())):
            if isinstance(node, ast.Import):
                roots = [alias.name.split(".")[0] for alias in node.names]
            elif isinstance(node, ast.ImportFrom) and node.level == 0:
                roots = [node.module.split(".")[0]]
            else:
                continue
            for root in roots:
                assert root in stdlib or root == "__future__", f"{path.name} imports {root}"


@pytest.mark.skipif(not os.environ.get("SDLOG_CLICKHOUSE_URL"), reason="SDLOG_CLICKHOUSE_URL not set")
def test_clickhouse_round_trip_with_uds(setup) -> None:
    """U15 against a real ClickHouse: an older database (no events, no text) is upgraded by `run`,
    NaN and text survive TSV, events load and a rerun is idempotent."""
    _, _, config = setup
    url = os.environ["SDLOG_CLICKHOUSE_URL"]
    database = f"sdlog_test_{uuid.uuid4().hex[:8]}"
    sink = dec.ClickHouseSink(url, database)
    try:
        old = [sql for sql in dec.clickhouse_ddl(database) if "events" not in sql and "ALTER" not in sql]
        for sql in old:
            sink._query(sql.replace(",\n            text LowCardinality(String) DEFAULT ''", ""))
        assert "text" not in sink._query(f"SELECT name FROM system.columns WHERE database='{database}' AND table='signals'")
        assert run(config, sink) == 0
        assert run(config, sink) == 0  # idempotent
        rows = sink._query(
            f"SELECT signal, toString(value), text FROM {database}.signals ORDER BY signal FORMAT TSV"
        ).splitlines()
        got = {name: (float(value), text) for name, value, text in (line.split("\t") for line in rows)}
        assert_values(got, EXPECTED)
        ev = sink._query(f"SELECT kind, name, text, detail, toString(utc) FROM {database}.events FORMAT TSV").strip()
        assert ev == "state\tMINI.contactor\tclosed\tinitial\t1970-01-01 00:00:01.003"  # frame 3
    finally:
        sink._query(f"DROP DATABASE IF EXISTS {database}")
