"""script/sdlog_decode.py — archived chunks into frames/signals/chunks (docs/sdlog-decode-spec.md).

Everything runs against the in-process ``FakeSink`` and the fictional
``fixtures/mini.dbc``. The expected physical values are computed by hand in the
comments, not by the DBC library under test, so a byte-order or sign mistake in
how the decoder drives it shows up here. A ClickHouse round trip runs only when
``SDLOG_CLICKHOUSE_URL`` is set.
"""

from __future__ import annotations

import ast
import fcntl
import importlib.util
import os
import sys
import uuid
from pathlib import Path

import pytest

pytest.importorskip("cantools")

_SCRIPT = Path(__file__).parent.parent.parent / "script"
_FIXTURES = Path(__file__).parent / "fixtures"
MINI_DBC = _FIXTURES / "mini.dbc"


def _load(name: str):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, _SCRIPT / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sdlog = _load("sdlog")
dec = _load("sdlog_decode")


# ------------------------------------------------------------------ fixtures

# One chunk with every kind of line the decoder has to get right. Hand-computed:
#   0x100 E803F603…  Speed = 0x03E8*0.01 = 10.0, Temp = int8(0xF6)*0.5 = -5.0, Gear = 3
#   0x200 0FA0FF9C…  Motorola: PackVoltage = 0x0FA0*0.1 = 400.0, PackCurrent = int16(0xFF9C)*0.1 = -10.0
#   0x18FF0100 x 2A  extended: Counter = 42
#   0x300 00D007     mux 0: CellA = 0x07D0*0.001 = 2.0
#   0x300 01B80B     mux 1: CellB = 0x0BB8*0.001 = 3.0
#   0x7FF (empty)    no message: frame only
FIXTURE = [
    "#sdlog,2,7,@0,2026.7.0",
    "#origin,mr-orange",
    "#src,C,EV,1,can_gateway,500000",
    "#src,L,lin,5,linbus,19200",
    "C1,@1000,100,E803F60300000000",
    "#utc,1010,F4240",
    "C1,20,200,0FA0FF9C00000000",
    "XI,5,sd_logger,records=3, dropped=0",
    "L5,6,3C,55AA",
    "C1x,7,18FF0100,2A",
    "C1s,8,300,00D007",
    "#utc,1100,1E8480",
    "C1,9,300,01B80B",
    "C1,A,7FF,",
    "#close,@2000,rotate",
]

EXPECTED_SIGNALS = {
    "DEMO_STATUS.Speed": 10.0,
    "DEMO_STATUS.Temp": -5.0,
    "DEMO_STATUS.Gear": 3.0,
    "DEMO_PACK.PackVoltage": 400.0,
    "DEMO_PACK.PackCurrent": -10.0,
    "DEMO_EXT.Counter": 42.0,
    "DEMO_MUX.CellA": 2.0,
    "DEMO_MUX.CellB": 3.0,
}


def write_chunk(directory: Path, name: str, lines: list[str]) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_bytes(("\n".join(lines) + "\n").encode("latin-1"))
    return path


def with_seq(lines: list[str], seq: int) -> list[str]:
    return [f"#sdlog,2,{seq},@0,2026.7.0", *lines[1:]]


@pytest.fixture
def setup(tmp_path: Path):
    archive = tmp_path / "archive"
    dbc = tmp_path / "mini.dbc"
    dbc.write_bytes(MINI_DBC.read_bytes())
    write_chunk(archive / "mr-orange", "L0000007.LOG", FIXTURE)
    config = tmp_path / "decode.toml"
    config.write_text(
        f'archive = "{archive}"\nsink = "fake"\n\n'
        f'[[device]]\nname = "mr-orange"\nvehicle = "demo-car"\n[device.dbc]\nEV = "{dbc}"\n'
    )
    return archive, dbc, config


def run(config: Path, sink, command: str = "run") -> int:
    return dec.main([command, "--config", str(config)], sink=sink)


# ------------------------------------------------------------------ D1 config


@pytest.mark.parametrize(
    "body, why",
    [
        ('archive = "/a"\nsink = "fake"\nbogus = 1\n[[device]]\nname = "d"\nvehicle = "v"\n', "unknown key"),
        ('archive = "/a"\nsink = "fake"\n[[device]]\nname = "d"\nvehicle = "v"\n[[device]]\nname = "d"\nvehicle = "v"\n', "duplicate"),
        ('archive = "/a"\nsink = "fake"\n[[device]]\nname = "d"\nvehicle = "v"\n[device.dbc]\nEV = "/no/such.dbc"\n', "not found"),
        ('archive = "/a"\nsink = "mongo"\n[[device]]\nname = "d"\nvehicle = "v"\n', "unknown sink"),
        ('archive = "/a"\nsink = "fake"\n', "at least one"),
        ('archive = "/a"\nsink = "fake"\n[[device]]\nname = "../x"\nvehicle = "v"\n', "safe path"),
        ('archive = "/a"\nsink = "fake"\n[[device]]\nname = "d"\n', "vehicle is required"),
    ],
)
def test_config_rejects(tmp_path: Path, capsys, body: str, why: str) -> None:
    config = tmp_path / "decode.toml"
    config.write_text(body)
    assert run(config, dec.FakeSink()) == 2
    assert why in capsys.readouterr().err


# ------------------------------------------------------------------ D2 shared reader


def test_extract_unchanged_after_refactor(tmp_path: Path, capsys) -> None:
    """Output of `extract` captured from the reader before it was split into
    `StreamReader` (and checked byte-identical on two real chunks at the time)."""
    path = write_chunk(tmp_path, "L0000007.LOG", FIXTURE)
    golden = {
        "C": [
            "t_us,label,id_hex,flags,dlc,data_hex,utc_us",
            "4096,EV,100,-,8,E803F60300000000,",
            "4128,EV,200,-,8,0FA0FF9C00000000,1000016",
            "4146,EV,18FF0100,x,1,2A,1000034",
            "4154,EV,300,s,3,00D007,1000042",
            "4163,EV,300,-,3,01B80B,1999811",
            "4173,EV,7FF,-,0,,1999821",
        ],
        "L": ["t_us,label,id_hex,flags,dlc,data_hex,utc_us", "4139,lin,3C,-,2,55AA,1000027"],
        "X": ["t_us,level,tag,message,utc_us", "4133,I,sd_logger,records=3, dropped=0,1000021"],
    }
    for kind, expected in golden.items():
        assert sdlog.main(["extract", str(path), "--type", kind]) == 0
        assert capsys.readouterr().out.splitlines() == expected


def test_reader_yields_frames_with_utc(tmp_path: Path) -> None:
    path = write_chunk(tmp_path, "L0000007.LOG", FIXTURE)
    lines = [(s.kind, s.t_us, s.utc_us) for s in sdlog.StreamReader(path)]
    assert lines[0] == ("C", 4096, None)  # before the first #utc
    assert lines[1] == ("C", 4128, 1000016)
    assert [k for k, _, _ in lines] == ["C", "C", "X", "L", "C", "C", "C", "C"]


# FIXTURE as a writer with `#ident` (spec F1j) and `#clock` (clock spec) writes it: the
# identity right after `#sdlog`, a card floor at boot, and the reading next to `#utc`.
# Fictional values throughout.
STATED = [
    FIXTURE[0],
    "#ident,id=mr-orange,mac=02:00:5e:10:20:30,date=1970-01-01T00:00:01Z,clock=est/card:6,"
    "build=2026-10-04T09:12:33Z,elf=1a2b3c4d5e6f7a8b,cfg=5eed1234",
    "#clock,@0,F4240,est,card,6",
    *FIXTURE[1:6],
    "#clock,@1010,F4240,utc,sync",
    *FIXTURE[6:],
]


def _rows(sink, table: str) -> list[dict]:
    # The chunk key carries the file's sha, which the extra header lines change.
    return [{k: v for k, v in row.items() if k != "chunk"} for row in sink.tables[table]]


def test_ident_and_clock_lines_change_no_frame(setup) -> None:
    """`#ident` passes through, a `#clock … utc` reading equals its `#utc`, and a card
    floor (a lower bound) never places a row: the rows are those of the plain chunk."""
    archive, _, config = setup
    plain = dec.FakeSink()
    assert run(config, plain) == 0
    write_chunk(archive / "mr-orange", "L0000007.LOG", STATED)
    stated = dec.FakeSink()
    assert run(config, stated) == 0
    (marker,) = stated.tables["chunks"]
    assert marker["result"] == "ok", marker["detail"]
    assert _rows(stated, "frames") == _rows(plain, "frames")
    assert _rows(stated, "signals") == _rows(plain, "signals")
    assert stated.tables["frames"][0]["utc"] == 999  # from the `#utc`, not from the card floor


def test_reader_estimates_are_opt_in(tmp_path: Path) -> None:
    """`utc_best` (extract --estimates) only when asked for; a reader after readings is
    not stopped by a `#clock` line it never uses, one that asks for it is."""
    path = write_chunk(tmp_path, "L0000007.LOG", STATED)
    reference = write_chunk(tmp_path / "plain", "L0000007.LOG", FIXTURE)
    plain = list(sdlog.StreamReader(path))
    assert {s.utc_best for s in plain} == {None}
    assert [(s.t_us, s.utc_us) for s in plain] == [(s.t_us, s.utc_us) for s in sdlog.StreamReader(reference)]
    best = list(sdlog.StreamReader(path, estimates=True))
    assert (best[0].utc_us, best[0].utc_best) == (None, (1_004_096, "est"))  # floor 1 s + boot 4096 µs
    assert (best[1].utc_us, best[1].utc_best) == (1_000_016, (1_000_016, "utc"))
    bad = write_chunk(tmp_path / "bad", "L0000007.LOG", [*STATED[:2], "#clock,@0,F4240,utc,card,6", *STATED[3:]])
    assert len(list(sdlog.StreamReader(bad))) == len(plain)
    with pytest.raises(sdlog.ReadError, match="#clock"):
        list(sdlog.StreamReader(bad, estimates=True))


# ------------------------------------------------------------------ D3/D4 frames and decode


def test_frames_match_extract(setup) -> None:
    _, _, config = setup
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    frames = sink.tables["frames"]
    # utc in ms: boot µs 4096 lies before the first `#utc` (1010 = 4112 µs -> 1 000 000 µs),
    # so it resolves backwards to 999 984 µs; 4163/4173 sit under the second anchor.
    assert [(f["kind"], f["label"], f["utc"], f["id"], f["flags"], f["dlc"]) for f in frames] == [
        ("C", "EV", 999, 0x100, "-", 8),
        ("C", "EV", 1000, 0x200, "-", 8),
        ("L", "lin", 1000, 0x3C, "-", 2),
        ("C", "EV", 1000, 0x18FF0100, "x", 1),
        ("C", "EV", 1000, 0x300, "s", 3),
        ("C", "EV", 1999, 0x300, "-", 3),
        ("C", "EV", 1999, 0x7FF, "-", 0),
    ]
    assert set(frames[0]) == set(dec.FRAME_COLUMNS)  # one time column, no boot µs
    assert frames[0]["data"] == 0xE803F60300000000
    assert frames[-1]["data"] == 0


def test_utc_before_the_first_anchor_is_resolved_backwards(setup) -> None:
    """A chunk never spans a boot: the first anchor places the records before it (spec §2)."""
    _, _, config = setup
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    first = sink.tables["frames"][0]
    assert first["utc"] == (1_000_000 - (0x1010 - 0x1000)) // 1000
    assert [s["utc"] for s in sink.tables["signals"] if s["signal"].startswith("DEMO_STATUS.")] == [999] * 3
    (marker,) = sink.tables["chunks"]
    assert (marker["utc_min"], marker["utc_max"]) == (999, 1999)


def test_a_chunk_without_anchor_has_null_utc(setup) -> None:
    """No UTC anchor at all: `utc` NULL. A `drift`/`est` `#clock` never fills it."""
    archive, _, config = setup
    lines = [line for line in FIXTURE if not line.startswith("#utc")]
    lines.insert(1, "#clock,@0,F4240,est,card,6")
    lines.insert(2, "#clock,@1,F4240,drift,sleep,403")
    write_chunk(archive / "mr-orange", "L0000007.LOG", lines)
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    assert {f["utc"] for f in sink.tables["frames"]} == {None}
    assert {s["utc"] for s in sink.tables["signals"]} == {None}
    (marker,) = sink.tables["chunks"]
    assert (marker["result"], marker["utc_min"], marker["utc_max"]) == ("ok", None, None)


def test_a_clock_line_with_basis_utc_is_an_anchor(setup) -> None:
    """`#clock … utc,warm` (a clock that kept running through a reset) places the chunk
    like a `#utc` does, backwards included."""
    archive, _, config = setup
    lines = [line for line in FIXTURE if not line.startswith("#utc")]
    lines.insert(5, "#clock,@1010,F4240,utc,warm")
    write_chunk(archive / "mr-orange", "L0000007.LOG", lines)
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    assert [f["utc"] for f in sink.tables["frames"]][:2] == [999, 1000]


def test_utc_is_truncated_to_ms(setup) -> None:
    archive, _, config = setup
    # anchor 1 999 999 µs at boot 0x1000: the first frame (same µs) is 1999 ms, not 2000
    lines = [*FIXTURE[:4], "#utc,1000,1E847F", *FIXTURE[4:5], "#close,@2000,rotate"]
    write_chunk(archive / "mr-orange", "L0000007.LOG", lines)
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    assert [f["utc"] for f in sink.tables["frames"]] == [1999]


def test_matched_counts_frames_with_a_message(setup) -> None:
    """D16: matched = CAN frames whose label has a DBC and whose id matched; the extended
    mismatch and an unknown id do not count, LIN is not a CAN frame."""
    archive, _, config = setup
    lines = [*FIXTURE[:-1], "C1x,B,100,E803F60300000000", "#close,@2000,rotate"]
    write_chunk(archive / "mr-orange", "L0000007.LOG", lines)
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    (marker,) = sink.tables["chunks"]
    assert (marker["can_frames"], marker["matched"]) == (7, 5)


def test_decode_against_reference(setup) -> None:
    _, _, config = setup
    sink = dec.FakeSink()
    run(config, sink)
    got = {s["signal"]: s["value"] for s in sink.tables["signals"] if s["signal"] != "DEMO_MUX.Mux"}
    assert got == pytest.approx(EXPECTED_SIGNALS)
    assert {s["vehicle"] for s in sink.tables["signals"]} == {"demo-car"}


def test_unknown_id_is_frames_only(setup) -> None:
    _, _, config = setup
    sink = dec.FakeSink()
    run(config, sink)
    # 0x7FF has no message and LIN is not DBC-decoded: every signal comes from one of the
    # five matched CAN frames.
    assert {s["signal"].split(".")[0] for s in sink.tables["signals"]} == {"DEMO_STATUS", "DEMO_PACK", "DEMO_EXT", "DEMO_MUX"}
    (marker,) = sink.tables["chunks"]
    assert (marker["can_frames"], marker["matched"]) == (6, 5)


def test_an_extended_flag_mismatch_does_not_match(tmp_path: Path, setup) -> None:
    archive, _, config = setup
    write_chunk(archive / "mr-orange", "L0000007.LOG", [*FIXTURE[:4], "C1x,@1000,100,E803F60300000000", "#close,@2000,rotate"])
    sink = dec.FakeSink()
    run(config, sink)
    assert sink.tables["signals"] == []


def test_a_signal_that_does_not_fit_spares_the_rest_of_the_dbc(setup) -> None:
    """Real DBCs carry mistakes. One signal past the message end must not cost the file."""
    _, dbc, config = setup
    dbc.write_text(dbc.read_text().replace(' SG_ Gear : 24|4@1+', ' SG_ Broken : 60|16@1+ (1,0) [0|1] "" Vector__XXX\n SG_ Gear : 24|4@1+'))
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    got = {s["signal"] for s in sink.tables["signals"]}
    assert "DEMO_PACK.PackVoltage" in got


def test_decode_failures_are_counted_per_frame(setup) -> None:
    """D4a counts failed frames, not distinct payloads: the decode cache must not hide repeats."""
    archive, dbc, config = setup
    dbc.write_text(dbc.read_text().replace(' SG_ Gear : 24|4@1+', ' SG_ Broken : 60|16@1+ (1,0) [0|1] "" Vector__XXX\n SG_ Gear : 24|4@1+'))
    write_chunk(archive / "mr-orange", "L0000007.LOG", [*FIXTURE[:5], "C1,10,100,E803F60300000000", "#close,@2000,rotate"])
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    (marker,) = sink.tables["chunks"]
    assert marker["detail"] == "2 frames failed to decode"


def test_an_unloadable_dbc_exits_two(setup, capsys) -> None:
    _, dbc, config = setup
    dbc.write_text("this is not a DBC {")
    assert run(config, dec.FakeSink()) == 2
    assert "does not load" in capsys.readouterr().err


# ------------------------------------------------------------------ D5–D7 marker, idempotence, catalog


def test_a_crash_before_the_marker_redoes_the_chunk(setup) -> None:
    _, _, config = setup
    sink = dec.FakeSink(fail_on="chunks")
    assert run(config, sink) == 75
    assert sink.tables["chunks"] == []
    assert sink.tables["frames"]  # the rows went in, the marker did not
    assert run(config, sink) == 0
    assert len(sink.tables["frames"]) == 7  # replaced, not appended
    assert len(sink.tables["chunks"]) == 1


def test_second_run_is_a_noop(setup) -> None:
    _, _, config = setup
    sink = dec.FakeSink()
    run(config, sink)
    counts = {t: len(rows) for t, rows in sink.tables.items()}
    run(config, sink)
    assert {t: len(rows) for t, rows in sink.tables.items()} == counts


def test_dbc_edit_redecodes_without_duplicates(setup) -> None:
    _, dbc, config = setup
    sink = dec.FakeSink()
    run(config, sink)
    frames_before = len(sink.tables["frames"])
    # Change the Speed factor: same signals, new values, new catalog.
    dbc.write_text(dbc.read_text().replace("Speed : 0|16@1+ (0.01,0)", "Speed : 0|16@1+ (0.1,0)"))
    assert run(config, sink) == 0
    speeds = [s["value"] for s in sink.tables["signals"] if s["signal"] == "DEMO_STATUS.Speed"]
    assert speeds == [pytest.approx(100.0)]
    assert len(sink.tables["frames"]) == frames_before
    state = sink.chunk_state("mr-orange")
    assert state["L0000007.LOG"][1] == dec.catalog_hash(dec.load_config(config).devices[0])


# ------------------------------------------------------------------ D8/D9 defects and gaps


@pytest.mark.parametrize(
    "lines, why",
    [
        (FIXTURE[:-1], "no #close"),
        ([*FIXTURE[:5], "C1,ZZ,100,00", *FIXTURE[5:]], "malformed"),
        (["#sdlog,1,7,412000,2026.7.0", *FIXTURE[1:]], "v1"),
        (["#sdlog,2,7,@0,2026.7.0", "#origin,mr-blue", *FIXTURE[2:]], "#origin"),
    ],
)
def test_a_bad_chunk_is_a_defect_and_writes_nothing(setup, lines: list[str], why: str) -> None:
    archive, _, config = setup
    write_chunk(archive / "mr-orange", "L0000007.LOG", lines)
    sink = dec.FakeSink()
    assert run(config, sink) == 1
    assert sink.tables["frames"] == [] and sink.tables["signals"] == []
    (marker,) = sink.tables["chunks"]
    assert marker["result"] == "defect" and why in marker["detail"]


def test_a_torn_last_line_is_not_a_defect(setup) -> None:
    archive, _, config = setup
    path = archive / "mr-orange" / "L0000007.LOG"
    path.write_bytes(path.read_bytes() + b"C1,5,100,E8")  # power cut mid-line, after #close
    sink = dec.FakeSink()
    assert run(config, sink) == 0


def test_gaps_are_recorded(setup) -> None:
    archive, _, config = setup
    write_chunk(archive / "mr-orange", "L0000007.LOG", [*FIXTURE[:5], "#gap,@1005,3,12000,2,4", *FIXTURE[5:]])
    sink = dec.FakeSink()
    assert run(config, sink) == 0
    assert sink.tables["chunks"][0]["gaps"] == 1
    assert len(sink.tables["frames"]) == 7


def test_a_reused_seq_is_a_new_chunk_key(setup) -> None:
    """After a card erase, seq restarts: same seq, different content → different key."""
    archive, _, config = setup
    write_chunk(archive / "mr-orange", "L0000008.LOG", with_seq([*FIXTURE[:4], "C1,@1000,100,0000000000000000", "#close,@2000,rotate"], 7))
    sink = dec.FakeSink()
    run(config, sink)
    keys = {c["chunk"] for c in sink.tables["chunks"]}
    assert len(keys) == 2 and all(k.startswith("7-") for k in keys)


# ------------------------------------------------------------------ D10–D13


def test_archive_is_untouched(setup) -> None:
    archive, _, config = setup
    path = archive / "mr-orange" / "L0000007.LOG"
    before = (path.read_bytes(), path.stat().st_mtime_ns)
    run(config, dec.FakeSink())
    assert (path.read_bytes(), path.stat().st_mtime_ns) == before
    assert sorted(p.name for p in (archive / "mr-orange").iterdir()) == ["L0000007.LOG"]


def test_partials_are_not_loaded(setup) -> None:
    archive, _, config = setup
    (archive / "mr-orange" / "L0000008.LOG.part").write_bytes(b"#sdlog,2,8,@0,2026.7.0\n")
    sink = dec.FakeSink()
    run(config, sink)
    assert [c["file"] for c in sink.tables["chunks"]] == ["L0000007.LOG"]


@pytest.mark.parametrize("case, code", [("ok", 0), ("defect", 1), ("down", 75)])
def test_exit_status(setup, case: str, code: int) -> None:
    archive, _, config = setup
    if case == "defect":
        write_chunk(archive / "mr-orange", "L0000007.LOG", FIXTURE[:-1])
    sink = dec.FakeSink(unavailable=case == "down")
    assert run(config, sink) == code


def test_a_held_lock_skips(setup, capsys) -> None:
    _, _, config = setup
    with open(str(config) + ".lock", "a") as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        sink = dec.FakeSink()
        assert run(config, sink) == 0
        assert "busy" in capsys.readouterr().out
        assert sink.tables["chunks"] == []


def test_status_counts(setup, capsys) -> None:
    archive, _, config = setup
    sink = dec.FakeSink()
    assert run(config, sink, "status") == 0
    assert "0 ok, 0 defect, 1 pending" in capsys.readouterr().out
    run(config, sink)
    capsys.readouterr()
    assert run(config, sink, "status") == 0
    assert "1 ok, 0 defect, 0 pending" in capsys.readouterr().out


def test_status_prints_coverage(setup, capsys) -> None:
    _, _, config = setup
    sink = dec.FakeSink()
    assert run(config, sink, "status") == 0
    assert "coverage -" in capsys.readouterr().out  # nothing decoded yet
    run(config, sink)
    capsys.readouterr()
    assert run(config, sink, "status") == 0
    assert "coverage 83.3 %" in capsys.readouterr().out  # 5 of 6 CAN frames


def test_reader_imports_stdlib_only() -> None:
    """sdlog.py, the collector and the fleet stay stdlib-only (D13)."""
    stdlib = set(sys.stdlib_module_names)
    for name in ("sdlog.py", "sdlog_collect.py", "sdlog_fleet.py"):
        tree = ast.parse((_SCRIPT / name).read_text())
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                roots = [alias.name.split(".")[0] for alias in node.names]
            elif isinstance(node, ast.ImportFrom) and node.level == 0:
                roots = [node.module.split(".")[0]]
            else:
                continue
            for root in roots:
                assert root in stdlib or root == "__future__", f"{name} imports {root}"


def test_the_example_config_loads(tmp_path: Path) -> None:
    example = Path(__file__).parent.parent.parent / "deploy" / "sdlog-decode" / "decode.example.toml"
    text = example.read_text().replace("/path/to/consuming-project/vehicles/demo/dbc/ev.dbc", str(MINI_DBC))
    config = tmp_path / "decode.toml"
    config.write_text(text)
    assert dec.load_config(config).devices[0].name == "mr-orange"


# ------------------------------------------------------------------ ClickHouse round trip (optional)


@pytest.mark.skipif(not os.environ.get("SDLOG_CLICKHOUSE_URL"), reason="SDLOG_CLICKHOUSE_URL not set")
def test_clickhouse_round_trip(setup) -> None:
    _, dbc, config = setup
    url = os.environ["SDLOG_CLICKHOUSE_URL"]
    database = f"sdlog_test_{uuid.uuid4().hex[:8]}"
    sink = dec.ClickHouseSink(url, database)
    try:
        sink.init()
        assert run(config, sink) == 0
        assert run(config, sink) == 0  # idempotent
        rows = sink._query(f"SELECT count() FROM {database}.frames").strip()
        assert rows == "7"
        values = dict(
            line.split("\t")
            for line in sink._query(
                f"SELECT signal, toString(value) FROM {database}.signals WHERE signal != 'DEMO_MUX.Mux' FORMAT TSV"
            ).splitlines()
        )
        assert {k: float(v) for k, v in values.items()} == pytest.approx(EXPECTED_SIGNALS)
        utc = sink._query(f"SELECT groupArray(toString(utc)) FROM (SELECT utc FROM {database}.frames WHERE id IN (256, 512) ORDER BY utc)").strip()
        assert utc == "['1970-01-01 00:00:00.999','1970-01-01 00:00:01.000']"
        assert sink.coverage("mr-orange", dec.catalog_hash(dec.load_config(config).devices[0])) == (5, 6)
        bounds = sink._query(f"SELECT toString(utc_min), toString(utc_max) FROM {database}.chunks FORMAT TSV").strip()
        assert bounds == "1970-01-01 00:00:00.999\t1970-01-01 00:00:01.999"
        dbc.write_text(dbc.read_text().replace("(0.01,0)", "(0.1,0)"))
        assert run(config, sink) == 0
        speed = sink._query(f"SELECT groupArray(value) FROM {database}.signals WHERE signal = 'DEMO_STATUS.Speed'").strip()
        assert speed == "[100]"
    finally:
        sink._query(f"DROP DATABASE IF EXISTS {database}")
