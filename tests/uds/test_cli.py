"""script/uds_catalog.py — the toolchain around the shared module.

The flash path is the safety-relevant one: offsets are auto-placed by
gen_esp32part.py, so they are read back from partition-table.bin, never
assumed, and a blob larger than its partition is refused before esptool ever
runs. Both behaviours are tested here against a synthetic table.
"""

import json
import struct

import pytest

from .common import FIXTURES, load_catalog_module, load_cli_module

catalog = load_catalog_module()
cli = load_cli_module()


def make_table(diag_size: int = 0x40000) -> bytes:
    """A partition-table.bin the way gen_esp32part.py writes it: 32-byte
    entries, magic AA 50, md5 marker EB EB, 0xFF terminator."""

    def entry(ptype, subtype, offset, size, label):
        return struct.pack(
            "<2sBBII16sI", b"\xaa\x50", ptype, subtype, offset, size,
            label.encode(), 0
        )

    table = b"".join(
        [
            entry(1, 0x02, 0x9000, 0x5000, "nvs"),
            entry(1, 0x00, 0xE000, 0x2000, "otadata"),
            entry(0, 0x10, 0x10000, 0x1C0000, "app0"),
            entry(0, 0x11, 0x1D0000, 0x1C0000, "app1"),
            entry(1, 0x06, 0x390000, diag_size, "diag"),
            b"\xeb\xeb" + b"\x00" * 14 + b"\xa5" * 16,  # md5 checksum entry
            b"\xff" * 32,  # terminator
        ]
    )
    return table


def test_parse_partition_table():
    parts = cli.parse_partition_table(make_table())
    labels = [p["label"] for p in parts]
    assert labels == ["nvs", "otadata", "app0", "app1", "diag"]
    diag = parts[-1]
    assert diag["offset"] == 0x390000
    assert diag["size"] == 0x40000
    assert diag["subtype"] == 0x06


def test_flash_dry_run_reads_offset_back(tmp_path, capsys):
    table = tmp_path / "partition-table.bin"
    table.write_bytes(make_table())
    rc = cli.main(
        [
            "flash", str(FIXTURES / "mini.dcat"),
            "--table", str(table), "--dry-run",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "0x390000" in out  # the offset came from the table, not a guess
    assert "write_flash" in out


def test_flash_refuses_oversized_blob(tmp_path, capsys):
    table = tmp_path / "partition-table.bin"
    table.write_bytes(make_table(diag_size=1024))  # smaller than mini.dcat
    rc = cli.main(
        ["flash", str(FIXTURES / "mini.dcat"), "--table", str(table), "--dry-run"]
    )
    assert rc == 1
    assert "refusing" in capsys.readouterr().err


def test_flash_refuses_unknown_partition(tmp_path, capsys):
    table = tmp_path / "partition-table.bin"
    table.write_bytes(make_table())
    rc = cli.main(
        ["flash", str(FIXTURES / "mini.dcat"), "--table", str(table),
         "--partition", "diagg", "--dry-run"]
    )
    assert rc == 1
    err = capsys.readouterr().err
    assert "diag" in err  # the real labels are listed


def test_flash_refuses_invalid_catalog(tmp_path, capsys):
    bad = tmp_path / "bad.dcat"
    bad.write_bytes(b"NOTDCAT\0" + b"\x00" * 100)
    table = tmp_path / "partition-table.bin"
    table.write_bytes(make_table())
    rc = cli.main(["flash", str(bad), "--table", str(table), "--dry-run"])
    assert rc == 1
    assert "invalid" in capsys.readouterr().err


def test_verify_exit_codes(tmp_path, capsys):
    assert cli.main(["verify", str(FIXTURES / "mini.dcat")]) == 0
    blob = bytearray((FIXTURES / "mini.dcat").read_bytes())
    blob[-1] ^= 0xFF
    bad = tmp_path / "corrupt.dcat"
    bad.write_bytes(bytes(blob))
    assert cli.main(["verify", str(bad)]) == 1
    assert "INVALID" in capsys.readouterr().err


def test_compile_cli_end_to_end(tmp_path):
    out = tmp_path / "mini.dcat"
    rc = cli.main(
        [
            "compile", str(FIXTURES / "mini-source.json"),
            "--ecu", "MINI", "--variant", "Mini_Var", "--all",
            "-o", str(out),
        ]
    )
    assert rc == 0
    assert out.read_bytes() == (FIXTURES / "mini.dcat").read_bytes()
    manifest = json.loads((tmp_path / "mini.manifest.json").read_text())
    assert manifest["totals"]["size_bytes"] == out.stat().st_size


def test_dump_runs(capsys):
    assert cli.main(["dump", str(FIXTURES / "mini.dcat"), "--fields"]) == 0
    out = capsys.readouterr().out
    assert "DT_Mini_Voltage" in out
    assert "SAFE_READ" in out


@pytest.fixture()
def mini_db():
    return json.loads((FIXTURES / "mini-source.json").read_text(encoding="utf-8"))


def test_profile_unknown_did_lists_known(mini_db):
    with pytest.raises(cli.CompileError) as exc:
        cli.compile_catalog(
            mini_db, "MINI", "Mini_Var",
            profile={"groups": [{"did": 0x0202}]},
        )
    msg = str(exc.value)
    assert "0x0202" in msg and "0x0101" in msg  # what it knows, listed


def test_profile_unknown_service_suggests(mini_db):
    with pytest.raises(cli.CompileError) as exc:
        cli.compile_catalog(
            mini_db, "MINI", "Mini_Var",
            profile={"groups": [{"service": "DT_Mini_Temperatur"}]},
        )
    assert "DT_Mini_Temperature" in str(exc.value)


def test_profile_selection_and_invalid_override(mini_db):
    cat, manifest, _ = cli.compile_catalog(
        mini_db, "MINI", "Mini_Var",
        profile={
            "groups": [{"did": 0xD000, "interval": "5s"}],
            "invalid_overrides": [{"text": "closed", "invalid": True}],
        },
    )
    assert len(cat.ecus[0].groups) == 1
    g = cat.ecus[0].groups[0]
    assert g.did == 0xD000
    closed = [s for s in g.fields[0].scales if s.text == "closed"][0]
    assert closed.invalid  # the profile overrode the pattern verdict
    assert manifest["groups"][0]["interval"] == "5s"


def test_unknown_ecu_and_variant_diagnostics(mini_db):
    with pytest.raises(cli.CompileError) as exc:
        cli.compile_catalog(mini_db, "NOPE", "Mini_Var", include_all=True)
    assert "MINI" in str(exc.value)
    with pytest.raises(cli.CompileError) as exc:
        cli.compile_catalog(mini_db, "MINI", "NOPE", include_all=True)
    assert "Mini_Var" in str(exc.value)
