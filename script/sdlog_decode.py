#!/usr/bin/env python3
"""Load archived sd_logger chunks into a column store (docs/sdlog-decode-spec.md).

``sdlog_fleet.py`` leaves sealed, verified chunks in ``<archive>/<device>/``. This
puts every one of them into three tables, and keeps them in step with the archive
and with the decode rules:

    frames    every record, lossless — the raw bus, queryable without any DBC
    signals   every CAN frame a DBC message matches, decoded to physical values
    chunks    one row per archive file: what was loaded, under which catalog, and
              whether it was ok or a defect. Written last, so it is the commit marker.

The archive is the truth and the database is derived from it. A chunk is pending
when it has no ``chunks`` row or one written under a different catalog (a sha256
over the device's DBC files); a pending chunk is replaced, never appended to:
delete its rows, insert frames, insert signals, then the marker. A run that dies
anywhere in there leaves no marker, so the next run does the chunk again.

Usage:
    script/sdlog_decode.py init   --config decode.toml   # create the tables
    script/sdlog_decode.py run    --config decode.toml   # every pending chunk
    script/sdlog_decode.py status --config decode.toml   # per device: ok / defect / pending

Exit status of ``run``: 0 every pending chunk loaded; 1 at least one defect was
found in this run; 2 usage or config error; 75 the database is unreachable
(nothing marked, the next run retries).

Only this tool needs ``cantools`` (https://pypi.org/project/cantools/). ``sdlog.py``,
the collector and the fleet stay stdlib-only.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import importlib.util
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlencode

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - the host is too old, say so plainly
    sys.exit("sdlog_decode.py needs Python >= 3.11 (tomllib)")

EX_DEFECT = 1
EX_CONFIG = 2
EX_TEMPFAIL = 75

# script/ is a tool directory, not a package: the reader is loaded by path, and has
# to be registered before exec_module because @dataclass looks its module up there.
_SDLOG_PATH = Path(__file__).absolute().parent / "sdlog.py"
if "sdlog" in sys.modules:
    sdlog = sys.modules["sdlog"]
else:
    _spec = importlib.util.spec_from_file_location("sdlog", _SDLOG_PATH)
    sdlog = importlib.util.module_from_spec(_spec)
    sys.modules["sdlog"] = sdlog
    _spec.loader.exec_module(sdlog)

_SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
_TOP_KEYS = {"archive", "sink", "clickhouse_url", "database", "lock", "batch_rows", "device"}
_DEVICE_KEYS = {"name", "vehicle", "dbc"}
_SINKS = {"clickhouse", "fake"}

FRAME_COLUMNS = ("device", "chunk", "kind", "label", "t_us", "utc_us", "id", "flags", "dlc", "data")
SIGNAL_COLUMNS = ("vehicle", "device", "chunk", "signal", "t_us", "utc_us", "value")
CHUNK_COLUMNS = (
    "device", "chunk", "file", "seq", "sha256", "bytes", "frames", "signals",
    "utc_min", "utc_max", "gaps", "catalog", "result", "detail", "decoded_at",
)


class ConfigError(Exception):
    """The config cannot be trusted to mean what it says. Exit 2, before any work."""


class SinkUnavailable(Exception):
    """The database did not answer. Nothing is marked; the next run retries (75)."""


class SinkError(Exception):
    """The database answered with an error: a bug or a schema problem, not weather."""


# ------------------------------------------------------------------------ config


@dataclass(frozen=True)
class Device:
    name: str
    vehicle: str
    dbc: tuple[tuple[str, Path], ...]  # (bus label, file), sorted by label


@dataclass(frozen=True)
class Config:
    archive: Path
    sink: str
    devices: tuple[Device, ...]
    lock: Path
    clickhouse_url: str = "http://localhost:8123"
    database: str = "sdlog"
    batch_rows: int = 200_000


def load_config(path: Path) -> Config:
    try:
        raw = tomllib.loads(path.read_text())
    except (OSError, tomllib.TOMLDecodeError) as err:
        raise ConfigError(f"{path}: {err}") from None
    unknown = set(raw) - _TOP_KEYS
    if unknown:
        raise ConfigError(f"unknown key(s): {', '.join(sorted(unknown))}")
    if "archive" not in raw:
        raise ConfigError("archive is required")
    sink = raw.get("sink", "clickhouse")
    if sink not in _SINKS:
        raise ConfigError(f"unknown sink {sink!r} (use {' or '.join(sorted(_SINKS))})")
    database = raw.get("database", "sdlog")
    if not isinstance(database, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,63}", database):
        raise ConfigError(f"database must be a plain identifier, not {database!r}")
    batch_rows = raw.get("batch_rows", 200_000)
    if isinstance(batch_rows, bool) or not isinstance(batch_rows, int) or batch_rows < 1:
        raise ConfigError(f"batch_rows must be a positive integer, not {batch_rows!r}")
    devices_raw = raw.get("device", [])
    if not devices_raw:
        raise ConfigError("at least one [[device]] is required")
    devices: list[Device] = []
    seen: set[str] = set()
    for entry in devices_raw:
        unknown = set(entry) - _DEVICE_KEYS
        if unknown:
            raise ConfigError(f"device: unknown key(s): {', '.join(sorted(unknown))}")
        name = entry.get("name")
        if not isinstance(name, str) or not _SAFE_NAME.match(name):
            raise ConfigError(f"device name {name!r} is not a safe path component")
        if name in seen:
            raise ConfigError(f"duplicate device {name!r}")
        seen.add(name)
        vehicle = entry.get("vehicle")
        if not isinstance(vehicle, str) or not vehicle:
            raise ConfigError(f"device {name!r}: vehicle is required")
        dbc_raw = entry.get("dbc", {})
        if not isinstance(dbc_raw, dict):
            raise ConfigError(f"device {name!r}: dbc must be a table of label = path")
        dbc: list[tuple[str, Path]] = []
        for label, file in sorted(dbc_raw.items()):
            dbc_path = Path(file)
            if not dbc_path.is_file():
                raise ConfigError(f"device {name!r}: DBC for {label!r} not found: {dbc_path}")
            dbc.append((label, dbc_path))
        devices.append(Device(name=name, vehicle=vehicle, dbc=tuple(dbc)))
    return Config(
        archive=Path(raw["archive"]),
        sink=sink,
        devices=tuple(devices),
        lock=Path(raw.get("lock", str(path) + ".lock")),
        clickhouse_url=raw.get("clickhouse_url", "http://localhost:8123").rstrip("/"),
        database=database,
        batch_rows=batch_rows,
    )


# ------------------------------------------------------------------------ decode


def catalog_hash(device: Device) -> str:
    """sha256 over the device's DBC files, by label. A content hash: any edit
    to a DBC is a new catalog, and no git is needed to say so."""
    digest = hashlib.sha256()
    for label, path in device.dbc:
        digest.update(label.encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


class Decoder:
    """DBC decode for one device: bus label -> cantools database."""

    def __init__(self, device: Device) -> None:
        import cantools  # only this tool needs it (spec D13)

        self.dbs = {}
        for label, path in device.dbc:
            # DBC files in the wild are ISO-8859-1, not UTF-8 (see dbc2yaml.py), and
            # they carry mistakes: a signal that does not fit its message rejects the
            # whole file under strict=True. Non-strict keeps every other message; the
            # broken signal fails only its own frames, counted in the chunk's detail.
            try:
                self.dbs[label] = cantools.database.load_file(
                    str(path), database_format="dbc", encoding="latin-1", strict=False
                )
            except Exception as err:  # noqa: BLE001 - cantools raises several types
                raise ConfigError(f"device {device.name!r}: DBC for {label!r} does not load: {err}") from None
        self._cache: dict[tuple[str, int, bool, str], list[tuple[str, float]]] = {}
        self.errors = 0

    def reset_cache(self) -> None:
        self._cache.clear()

    def decode(self, label: str, can_id: int, extended: bool, data_hex: str) -> list[tuple[str, float]]:
        db = self.dbs.get(label)
        if db is None:
            return []
        key = (label, can_id, extended, data_hex)
        hit = self._cache.get(key)
        if hit is not None:
            return hit
        out: list[tuple[str, float]] = []
        try:
            message = db.get_message_by_frame_id(can_id)
        except KeyError:
            message = None
        if message is not None and bool(message.is_extended_frame) == extended:
            try:
                values = message.decode(
                    bytes.fromhex(data_hex), decode_choices=False, scaling=True, allow_truncated=True
                )
                out = [(f"{message.name}.{name}", float(value)) for name, value in values.items()]
            except Exception:  # noqa: BLE001 - one undecodable frame must not stop the chunk
                self.errors += 1
        self._cache[key] = out
        return out


# ------------------------------------------------------------------------ sinks


class FakeSink:
    """In-memory tables with the same behavior the spec asks of a real one."""

    def __init__(self, fail_on: str | None = None, unavailable: bool = False) -> None:
        self.tables: dict[str, list[dict]] = {"frames": [], "signals": [], "chunks": []}
        self.fail_on = fail_on  # table name whose next insert raises (crash injection)
        self.unavailable = unavailable
        self.initialized = False

    def _check(self) -> None:
        if self.unavailable:
            raise SinkUnavailable("fake sink is down")

    def init(self) -> None:
        self._check()
        self.initialized = True

    def chunk_state(self, device: str) -> dict[str, tuple[str, str]]:
        self._check()
        latest: dict[str, dict] = {}
        for row in self.tables["chunks"]:
            if row["device"] != device:
                continue
            prev = latest.get(row["file"])
            if prev is None or row["decoded_at"] >= prev["decoded_at"]:
                latest[row["file"]] = row
        return {file: (row["result"], row["catalog"]) for file, row in latest.items()}

    def delete_chunk(self, device: str, chunk: str) -> None:
        self._check()
        for table in ("frames", "signals"):
            self.tables[table] = [
                row for row in self.tables[table] if not (row["device"] == device and row["chunk"] == chunk)
            ]

    def insert(self, table: str, columns: tuple[str, ...], rows: list[tuple]) -> None:
        self._check()
        if self.fail_on == table:
            self.fail_on = None
            raise SinkUnavailable(f"injected failure on {table}")
        self.tables[table].extend(dict(zip(columns, row)) for row in rows)


def _tsv_field(value) -> str:
    if value is None:
        return "\\N"
    if isinstance(value, str):
        return value.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
    if isinstance(value, float):
        return repr(value)
    return str(value)


class ClickHouseSink:
    """ClickHouse over its HTTP interface, with urllib (spec O2): no driver."""

    def __init__(self, url: str, database: str, timeout_s: float = 300.0) -> None:
        self.url = url
        self.db = database
        self.timeout_s = timeout_s

    def query(self, sql: str, params: dict | None = None) -> str:
        """A read query, for tools built on these tables (sdlog_publish.py)."""
        return self._query(sql, params=params)

    def _query(self, sql: str, body: bytes | None = None, params: dict | None = None) -> str:
        query = {"query": sql}
        for key, value in (params or {}).items():
            query[f"param_{key}"] = value
        request = urllib.request.Request(f"{self.url}/?{urlencode(query)}", data=body if body is not None else b"")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_s) as response:
                return response.read().decode()
        except urllib.error.HTTPError as err:
            detail = err.read().decode(errors="replace")[:300].strip()
            if err.code == 503:
                raise SinkUnavailable(f"ClickHouse 503: {detail}") from None
            raise SinkError(f"ClickHouse {err.code}: {detail}") from None
        except (urllib.error.URLError, ConnectionError, TimeoutError) as err:
            raise SinkUnavailable(f"ClickHouse unreachable at {self.url}: {err}") from None

    def init(self) -> None:
        for sql in clickhouse_ddl(self.db):
            self._query(sql)

    def chunk_state(self, device: str) -> dict[str, tuple[str, str]]:
        text = self._query(
            f"SELECT file, argMax(result, decoded_at), argMax(catalog, decoded_at) FROM {self.db}.chunks"
            " WHERE device = {device:String} GROUP BY file FORMAT TSV",
            params={"device": device},
        )
        state = {}
        for line in text.splitlines():
            file, result, catalog = line.split("\t")
            state[file] = (result, catalog)
        return state

    def delete_chunk(self, device: str, chunk: str) -> None:
        for table in ("frames", "signals"):
            self._query(
                f"DELETE FROM {self.db}.{table} WHERE device = {{device:String}} AND chunk = {{chunk:String}}",
                params={"device": device, "chunk": chunk},
            )

    def insert(self, table: str, columns: tuple[str, ...], rows: list[tuple]) -> None:
        if not rows:
            return
        body = "".join("\t".join(_tsv_field(v) for v in row) + "\n" for row in rows).encode()
        self._query(f"INSERT INTO {self.db}.{table} ({', '.join(columns)}) FORMAT TSV", body=body)


def clickhouse_ddl(db: str) -> list[str]:
    """The three tables (spec §2). Codecs as measured: timestamps delta-coded,
    payloads ZSTD, ids T64."""
    utc_alias = "Nullable(DateTime64(6, 'UTC')) ALIAS fromUnixTimestamp64Micro(toInt64(utc_us), 'UTC')"
    return [
        f"CREATE DATABASE IF NOT EXISTS {db}",
        f"""CREATE TABLE IF NOT EXISTS {db}.frames (
            device LowCardinality(String),
            chunk String CODEC(ZSTD(3)),
            kind LowCardinality(String),
            label LowCardinality(String),
            t_us UInt64 CODEC(DoubleDelta, ZSTD(3)),
            utc_us Nullable(UInt64) CODEC(DoubleDelta, ZSTD(3)),
            id UInt32 CODEC(T64, ZSTD(3)),
            flags LowCardinality(String),
            dlc UInt8 CODEC(ZSTD(3)),
            data UInt64 CODEC(ZSTD(3)),
            utc {utc_alias}
        ) ENGINE = MergeTree PARTITION BY device ORDER BY (device, label, id, t_us)""",
        f"""CREATE TABLE IF NOT EXISTS {db}.signals (
            vehicle LowCardinality(String),
            device LowCardinality(String),
            chunk String CODEC(ZSTD(3)),
            signal LowCardinality(String),
            t_us UInt64 CODEC(DoubleDelta, ZSTD(3)),
            utc_us Nullable(UInt64) CODEC(DoubleDelta, ZSTD(3)),
            value Float64 CODEC(Gorilla, ZSTD(3)),
            utc {utc_alias}
        ) ENGINE = MergeTree PARTITION BY vehicle ORDER BY (vehicle, signal, t_us)""",
        f"""CREATE TABLE IF NOT EXISTS {db}.chunks (
            device LowCardinality(String),
            chunk String,
            file String,
            seq Nullable(UInt32),
            sha256 String,
            bytes UInt64,
            frames UInt64,
            signals UInt64,
            utc_min Nullable(UInt64),
            utc_max Nullable(UInt64),
            gaps UInt32,
            catalog String,
            result LowCardinality(String),
            detail String,
            decoded_at DateTime64(3, 'UTC')
        ) ENGINE = ReplacingMergeTree(decoded_at) ORDER BY (device, file)""",
    ]


def make_sink(config: Config):
    if config.sink == "fake":
        return FakeSink()
    return ClickHouseSink(config.clickhouse_url, config.database)


# ------------------------------------------------------------------------ one chunk


@dataclass
class ChunkResult:
    file: str
    result: str  # ok | defect
    detail: str = ""
    frames: int = 0
    signals: int = 0


def _defect_reason(report, device: Device) -> str | None:
    if report.legacy:
        return "pre-v1 file (no #sdlog header)"
    if report.unsupported_version:
        return f"write format v{report.format_version}, which this reader does not know"
    if report.bad_lines:
        number, what = report.bad_lines[0]
        return f"malformed line {number}: {what}"
    if report.close_reason is None:
        return "no #close: not a sealed chunk"
    if report.origin is not None and report.origin != device.name:
        return f"#origin {report.origin!r} is not device {device.name!r}"
    return None


def _now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def load_chunk(path: Path, device: Device, catalog: str, decoder: Decoder | None, sink, batch_rows: int) -> ChunkResult:
    """Replace one chunk in the sink. Order is the contract (D5): delete → frames →
    signals → marker. Raises SinkUnavailable/SinkError from the sink untouched."""
    raw = path.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    report = sdlog.parse_file(path)
    seq = report.seq
    chunk = f"{seq if seq is not None else 'x'}-{sha[:12]}"
    marker = {
        "device": device.name, "chunk": chunk, "file": path.name, "seq": seq, "sha256": sha,
        "bytes": len(raw), "frames": 0, "signals": 0, "utc_min": report.first_utc_us,
        "utc_max": report.last_utc_us, "gaps": len(report.gaps), "catalog": catalog,
        "result": "ok", "detail": "",
    }
    sink.delete_chunk(device.name, chunk)
    reason = _defect_reason(report, device)
    frames = signals = 0
    if reason is None:
        reader = sdlog.StreamReader(path)
        frame_rows: list[tuple] = []
        signal_rows: list[tuple] = []
        if decoder is not None:
            decoder.reset_cache()
            errors_before = decoder.errors
        try:
            for stream in reader:
                if stream.kind not in sdlog.RECORD_KINDS or len(stream.fields) != sdlog.RECORD_FIELDS:
                    continue
                _, tag, flags = sdlog.split_token(stream.fields[0])
                label = reader.label(tag, stream.fields[0])
                id_hex, data_hex = stream.fields[2], stream.fields[3]
                can_id = int(id_hex, 16)
                frame_rows.append((
                    device.name, chunk, stream.kind, label, stream.t_us, stream.utc_us, can_id,
                    flags or "-", len(data_hex) // 2, int(data_hex, 16) if data_hex else 0,
                ))
                if decoder is not None and stream.kind == "C":
                    for name, value in decoder.decode(label, can_id, "x" in flags, data_hex):
                        signal_rows.append((device.vehicle, device.name, chunk, name, stream.t_us, stream.utc_us, value))
                if len(frame_rows) >= batch_rows:
                    sink.insert("frames", FRAME_COLUMNS, frame_rows)
                    frames += len(frame_rows)
                    frame_rows = []
                if len(signal_rows) >= batch_rows:
                    sink.insert("signals", SIGNAL_COLUMNS, signal_rows)
                    signals += len(signal_rows)
                    signal_rows = []
        except (sdlog.ReadError, ValueError) as err:
            reason = f"unreadable: {err}"
        if reason is None:
            sink.insert("frames", FRAME_COLUMNS, frame_rows)
            frames += len(frame_rows)
            sink.insert("signals", SIGNAL_COLUMNS, signal_rows)
            signals += len(signal_rows)
            if decoder is not None and decoder.errors > errors_before:
                marker["detail"] = f"{decoder.errors - errors_before} frames failed to decode"
        else:
            # A defect writes nothing but its marker: drop whatever a batch already put in.
            sink.delete_chunk(device.name, chunk)
            frames = signals = 0
    if reason is not None:
        marker["result"] = "defect"
        marker["detail"] = reason
    marker["frames"], marker["signals"] = frames, signals
    marker["decoded_at"] = _now()
    sink.insert("chunks", CHUNK_COLUMNS, [tuple(marker[c] for c in CHUNK_COLUMNS)])
    return ChunkResult(path.name, marker["result"], marker["detail"], frames, signals)


def pending_files(config: Config, device: Device, state: dict[str, tuple[str, str]], catalog: str) -> list[Path]:
    directory = config.archive / device.name
    if not directory.is_dir():
        return []
    files = sorted(p for p in directory.glob("*.LOG") if p.is_file())
    return [p for p in files if p.name not in state or state[p.name][1] != catalog]


# ------------------------------------------------------------------------ commands


def cmd_init(config: Config, sink) -> int:
    sink.init()
    print(f"tables ready in {config.database}")
    return 0


def cmd_run(config: Config, sink) -> int:
    lock_handle = config.lock.open("a")
    try:
        fcntl.flock(lock_handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("busy: another run holds the lock")
        return 0
    defects = 0
    try:
        for device in config.devices:
            catalog = catalog_hash(device)
            decoder = Decoder(device) if device.dbc else None
            state = sink.chunk_state(device.name)
            todo = pending_files(config, device, state, catalog)
            loaded = 0
            for path in todo:
                result = load_chunk(path, device, catalog, decoder, sink, config.batch_rows)
                if result.result == "defect":
                    defects += 1
                    print(f"{device.name}: {path.name}: defect — {result.detail}")
                loaded += 1
            print(f"{device.name}: {loaded} chunk(s) loaded")
    except SinkUnavailable as err:
        print(f"database unavailable: {err}", file=sys.stderr)
        return EX_TEMPFAIL
    finally:
        fcntl.flock(lock_handle, fcntl.LOCK_UN)
        lock_handle.close()
    return EX_DEFECT if defects else 0


def device_status(config: Config, sink, device: Device) -> tuple[int, int, int]:
    """(ok, defect, pending) for one device — what `status` prints, and what
    sdlog_publish.py reports, from one place."""
    catalog = catalog_hash(device)
    state = sink.chunk_state(device.name)
    pending = pending_files(config, device, state, catalog)
    ok = sum(1 for result, cat in state.values() if result == "ok" and cat == catalog)
    defect = sum(1 for result, _ in state.values() if result == "defect")
    return ok, defect, len(pending)


def cmd_status(config: Config, sink) -> int:
    any_defect = False
    for device in config.devices:
        ok, defect, pending = device_status(config, sink, device)
        any_defect |= defect > 0
        print(f"{device.name}: {ok} ok, {defect} defect, {pending} pending")
    return EX_DEFECT if any_defect else 0


def main(argv: list[str] | None = None, sink=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("init", "run", "status"):
        sub.add_parser(name).add_argument("--config", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        config = load_config(args.config)
    except ConfigError as err:
        print(f"config: {err}", file=sys.stderr)
        return EX_CONFIG
    sink = sink if sink is not None else make_sink(config)
    command = {"init": cmd_init, "run": cmd_run, "status": cmd_status}[args.command]
    try:
        return command(config, sink)
    except ConfigError as err:
        print(f"config: {err}", file=sys.stderr)
        return EX_CONFIG
    except SinkUnavailable as err:
        print(f"database unavailable: {err}", file=sys.stderr)
        return EX_TEMPFAIL
    except SinkError as err:
        print(f"database error: {err}", file=sys.stderr)
        return EX_DEFECT


if __name__ == "__main__":
    sys.exit(main())
