#!/usr/bin/env python3
"""Collect sd_logger chunks from many devices, unattended (docs/sdlog-fleet-spec.md).

``sdlog_collect.py`` is one pass against one device, run by hand. This is what
runs on a home server every few minutes against every device in a config file,
and says something only when something is actually wrong:

    a car that is away      is silent. The collector exits 75 (EX_TEMPFAIL),
                            nothing is recorded but the attempt.
    a defect                is loud at once: a chunk that fails verification,
                            a broken index, the wrong device at the URL, a disk
                            that is full. It stays loud until a pass succeeds.
    a collection that       is loud after ``stale_after_days`` (spec E4), even
    stops happening         if every pass in between was only "offline".

WHAT IT DOES PER DEVICE, EACH PASS

    flock <state>/<name>.lock      held → "busy", skip quietly
    disk pre-flight                under min_free_mb → error, no pull
    sdlog_collect.py pull <url> --into <archive> --expect <name>
    <state>/<name>.json            heartbeat, replaced atomically
    GET push_url?status=up|down&msg=…   optional, e.g. an Uptime Kuma push monitor

The push goes out on every pass, not only on success, and ``down`` means "in
alarm". So the E4 threshold lives in this config, and the push monitor's own
interval only has to cover "is this host still running".

WHAT IT NEVER DOES

    It never deletes, renames or rewrites anything in the archive. Only the
    collector writes there, and only new files: the archive is append-only so
    an external backup can snapshot it at any time.

Usage:
    script/sdlog_fleet.py run    --config fleet.toml              # one pass, e.g. from a systemd timer
    script/sdlog_fleet.py run    --config fleet.toml --every 300  # container loop
    script/sdlog_fleet.py status --config fleet.toml              # exit 1 if any device is in alarm

Exit status of ``run``: 0 no device had an error in this pass (offline is not
an error), 1 at least one did, 2 a usage or config error.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import parse_qsl, urlencode, urlparse, urlunparse

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - the host is too old, say so plainly
    sys.exit("sdlog_fleet.py needs Python >= 3.11 (tomllib)")

EX_TEMPFAIL = 75  # sdlog_collect.py: only the network failed
EX_CONFIG = 2

COLLECTOR = Path(__file__).absolute().parent / "sdlog_collect.py"
PUSH_MSG_MAX = 200
PUSH_TIMEOUT_S = 10.0
MIB = 1024 * 1024

# A device name becomes a path component twice — the archive directory (via the
# collector) and the heartbeat file — so it gets the collector's chunk-name rule.
_SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")

_TOP_KEYS = {
    "archive",
    "state",
    "stale_after_days",
    "min_free_mb",
    "timeout_s",
    "retries",
    "backoff",
    "device",
}
_DEVICE_KEYS = {"name", "url", "push_url"}


class ConfigError(Exception):
    """The config cannot be trusted to mean what it says. Exit 2, before any pass."""


@dataclass(frozen=True)
class Device:
    name: str
    url: str
    push_url: str | None = None


@dataclass(frozen=True)
class Config:
    archive: Path
    state: Path
    devices: tuple[Device, ...]
    stale_after_days: float = 3.0
    min_free_mb: float = 1024.0
    timeout_s: float = 3600.0
    retries: int | None = None
    backoff: float | None = None


@dataclass
class PassResult:
    """One device, one pass. ``result`` is ok, offline, error or busy."""

    name: str
    result: str
    exit: int | None = None
    reason: str | None = None
    summary: str | None = None
    torn: list[tuple[str, str]] = field(default_factory=list)  # (name, reason), collector C12


@dataclass
class DeviceStatus:
    name: str
    state: dict | None  # the heartbeat file, None if never run
    alarm: bool
    age_days: float | None  # since the last success (or since first seen)


# ------------------------------------------------------------------------ config


def _number(raw: dict, key: str, default: float, *, minimum: float, strict: bool) -> float:
    value = raw.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(f"{key} must be a number, not {value!r}")
    if value < minimum or (strict and value == minimum):
        relation = ">" if strict else ">="
        raise ConfigError(f"{key} must be {relation} {minimum:g}, not {value!r}")
    return float(value)


def _http_url(value, key: str, where: str) -> str:
    if not isinstance(value, str):
        raise ConfigError(f"{where}: {key} must be a string, not {value!r}")
    parsed = urlparse(value)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise ConfigError(f"{where}: {key} must be an http(s) URL, not {value!r}")
    return value


def load_config(path: Path | str) -> Config:
    """Parse and validate. Strict on purpose: an unknown key is an error, because
    a misspelt ``push_url`` would otherwise just switch an alarm off."""
    path = Path(path)
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except OSError as err:
        raise ConfigError(f"cannot read {path}: {err}") from err
    except tomllib.TOMLDecodeError as err:
        raise ConfigError(f"{path} is not valid TOML: {err}") from err

    unknown = sorted(set(raw) - _TOP_KEYS)
    if unknown:
        raise ConfigError(f"unknown key(s): {', '.join(unknown)}")
    for key in ("archive", "state"):
        if not isinstance(raw.get(key), str) or not raw[key]:
            raise ConfigError(f"{key} must be set to a directory path")

    entries = raw.get("device")
    if not isinstance(entries, list) or not entries:
        raise ConfigError("at least one [[device]] is required")
    devices = []
    seen = set()
    for number, entry in enumerate(entries, start=1):
        where = f"device #{number}"
        if not isinstance(entry, dict):
            raise ConfigError(f"{where} is not a table")
        unknown = sorted(set(entry) - _DEVICE_KEYS)
        if unknown:
            raise ConfigError(f"{where}: unknown key(s): {', '.join(unknown)}")
        name = entry.get("name")
        if not isinstance(name, str) or not _SAFE_NAME.fullmatch(name):
            raise ConfigError(
                f"{where}: name {name!r} must be 1-64 of [A-Za-z0-9._-], not starting with . _ -"
            )
        where = f"device {name!r}"
        if name in seen:
            raise ConfigError(f"{where}: duplicate name")
        seen.add(name)
        if "url" not in entry:
            raise ConfigError(f"{where}: url is required")
        url = _http_url(entry["url"], "url", where)
        push = entry.get("push_url")
        if push is not None:
            push = _http_url(push, "push_url", where)
        devices.append(Device(name=name, url=url, push_url=push))

    retries = raw.get("retries")
    if retries is not None and (isinstance(retries, bool) or not isinstance(retries, int) or retries < 1):
        raise ConfigError(f"retries must be an integer >= 1, not {retries!r}")
    backoff = None
    if "backoff" in raw:
        backoff = _number(raw, "backoff", 0, minimum=0, strict=False)

    return Config(
        archive=Path(raw["archive"]),
        state=Path(raw["state"]),
        devices=tuple(devices),
        stale_after_days=_number(raw, "stale_after_days", 3, minimum=0, strict=True),
        min_free_mb=_number(raw, "min_free_mb", 1024, minimum=0, strict=False),
        timeout_s=_number(raw, "timeout_s", 3600, minimum=0, strict=True),
        retries=retries,
        backoff=backoff,
    )


# ----------------------------------------------------------------------- plumbing


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _parse_time(value) -> datetime | None:
    if not isinstance(value, str):
        return None
    try:
        return datetime.fromisoformat(value)
    except ValueError:
        return None


def _compact(text: str, limit: int = 500) -> str:
    text = " ".join(text.split())
    return text if len(text) <= limit else text[: limit - 1] + "…"


def _last_line(text: str) -> str:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return lines[-1] if lines else ""


_TORN_LINE = re.compile(r"^TORN (\S+): (.+)$")


def _torn_lines(text: str) -> list[tuple[str, str]]:
    """The collector's `TORN <name>: <reason>` lines (sdlog-fleet-spec C12, F17)."""
    found = []
    for line in text.splitlines():
        match = _TORN_LINE.match(line.strip())
        if match:
            found.append((match.group(1), match.group(2)))
    return found


def _human_age(days: float) -> str:
    seconds = days * 86400
    if seconds < 3600:
        return f"{seconds / 60:.0f} min"
    if seconds < 86400:
        return f"{seconds / 3600:.1f} h"
    return f"{days:.1f} d"


def run_subprocess(argv: list[str], timeout: float) -> tuple[int, str]:
    """The collector as a child: a crash or a hang in one pass cannot take the
    fleet with it, and ``timeout`` is enforced by killing the child."""
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout + proc.stderr


def http_push(url: str) -> None:
    """GET the push URL. A non-2xx (Kuma answers 404 for an unknown token)
    raises, as an ``OSError`` — urllib's errors are."""
    with urllib.request.urlopen(url, timeout=PUSH_TIMEOUT_S) as response:
        response.read()


def push_url_with(base: str, status: str, msg: str) -> str:
    """``base`` with ``status`` and ``msg`` set. Kuma's copy-paste push URL
    already carries ``?status=up&msg=OK&ping=``; those are replaced, not
    appended a second time."""
    parsed = urlparse(base)
    query = [(k, v) for k, v in parse_qsl(parsed.query, keep_blank_values=True)
             if k not in ("status", "msg", "ping")]
    query += [("status", status), ("msg", msg[:PUSH_MSG_MAX])]
    return urlunparse(parsed._replace(query=urlencode(query)))


def disk_free_bytes(path: Path) -> int:
    return shutil.disk_usage(path).free


# -------------------------------------------------------------------------- fleet


class Fleet:
    """Every device in a config, one :meth:`run_once` per pass.

    The runner, clock, pusher and disk probe are parameters so the policy can be
    tested without processes, waiting or a network; the defaults are the real
    ones.
    """

    def __init__(
        self,
        config: Config,
        *,
        runner: Callable[[list[str], float], tuple[int, str]] | None = None,
        collector: Path | None = None,
        now: Callable[[], datetime] = _utcnow,
        pusher: Callable[[str], None] = http_push,
        disk_free: Callable[[Path], int] = disk_free_bytes,
    ) -> None:
        self.config = config
        self.runner = runner or run_subprocess
        self.collector = Path(collector) if collector is not None else COLLECTOR
        self.now = now
        self.pusher = pusher
        self.disk_free = disk_free

    # ----------------------------------------------------------------- the pass

    def run_once(self) -> dict[str, PassResult]:
        """One pass over every device, concurrently: a slow transfer on one must
        not starve the others until the next timer tick."""
        self.config.state.mkdir(parents=True, exist_ok=True)
        blocked = self._disk_problem()
        with ThreadPoolExecutor(max_workers=len(self.config.devices)) as pool:
            futures = {
                device.name: pool.submit(self._pass, device, blocked)
                for device in self.config.devices
            }
            return {name: future.result() for name, future in futures.items()}

    def _disk_problem(self) -> str | None:
        """Pre-flight: refuse to pull onto a disk that is (nearly) full. The
        collector would stop on its first failed write too (exit 74), but this
        says so before a car's worth of chunks went over the WiFi for nothing."""
        try:
            self.config.archive.mkdir(parents=True, exist_ok=True)
            free = self.disk_free(self.config.archive)
        except OSError as err:
            return f"archive not usable: {err}"
        need = self.config.min_free_mb * MIB
        if free < need:
            return (
                f"archive low on space: {free / MIB:.0f} MB free,"
                f" min_free_mb is {self.config.min_free_mb:g}"
            )
        return None

    def _pass(self, device: Device, blocked: str | None) -> PassResult:
        lock_path = self.config.state / f"{device.name}.lock"
        with open(lock_path, "a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                # Another pass for this device is still running (a long transfer,
                # a manual run). It will write the heartbeat and push; we don't.
                return PassResult(device.name, "busy", reason="another pass holds the lock")
            # The lock is released when the file closes — and by the kernel if
            # this process dies, so a crash never leaves a device locked out.
            at = self.now()
            if blocked is not None:
                result = PassResult(device.name, "error", reason=blocked)
            else:
                result = self._collect(device)
            state = self._record(device, result, at)
            self._push(device, state, result, at)
            return result

    def _argv(self, device: Device) -> list[str]:
        argv = [
            sys.executable,
            str(self.collector),
            "pull",
            device.url,
            "--into",
            str(self.config.archive),
            "--expect",
            device.name,
        ]
        if self.config.retries is not None:
            argv += ["--retries", str(self.config.retries)]
        if self.config.backoff is not None:
            argv += ["--backoff", f"{self.config.backoff:g}"]
        return argv

    def _collect(self, device: Device) -> PassResult:
        timeout = self.config.timeout_s
        try:
            code, output = self.runner(self._argv(device), timeout)
        except subprocess.TimeoutExpired:
            # Killed. A partial it left resumes next run, but a pass that never
            # ends is not the car driving away: somebody should look.
            return PassResult(
                device.name, "error", reason=f"timeout: pass exceeded {timeout:g} s and was killed"
            )
        except Exception as err:  # noqa: BLE001 - any failure to run is an error, never "offline"
            return PassResult(device.name, "error", reason=f"collector could not run: {err}")
        if code == 0:
            # A torn chunk the collector accepted is a warning: the pass stays ok (F17).
            return PassResult(device.name, "ok", exit=0, summary=_last_line(output), torn=_torn_lines(output))
        if code == EX_TEMPFAIL:
            return PassResult(device.name, "offline", exit=code, reason=_last_line(output))
        return PassResult(
            device.name, "error", exit=code, reason=f"exit {code}: {_compact(output) or 'no output'}"
        )

    # ---------------------------------------------------------------- heartbeat

    def _state_path(self, name: str) -> Path:
        return self.config.state / f"{name}.json"

    def _load(self, name: str) -> dict | None:
        try:
            state = json.loads(self._state_path(name).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return state if isinstance(state, dict) else None

    def _record(self, device: Device, result: PassResult, at: datetime) -> dict:
        state = self._load(device.name) or {
            "device": device.name,
            "created": at.isoformat(),
            "last_success": None,
            "verdict": "ok",
            "last_error": None,
            "summary": None,
        }
        state["url"] = device.url
        state["last_run"] = at.isoformat()
        state["last_result"] = result.result
        state["last_exit"] = result.exit
        if result.result == "ok":
            state["last_success"] = at.isoformat()
            state["verdict"] = "ok"
            state["summary"] = result.summary
        if result.torn:
            state["torn_accepted"] = int(state.get("torn_accepted") or 0) + len(result.torn)
            name, reason = result.torn[-1]
            state["last_torn"] = {"at": at.isoformat(), "name": name, "reason": reason}
        elif result.result == "error":
            state["verdict"] = "error"
            state["last_error"] = {"at": at.isoformat(), "exit": result.exit, "reason": result.reason}
        # "offline" changes nothing else: the car being away says nothing about
        # whether a defect seen earlier is gone.
        self._write(device.name, state)
        return state

    def _write(self, name: str, state: dict) -> None:
        """Write-then-rename, so a reader (or a crash) never sees half a file."""
        final = self._state_path(name)
        temp = final.with_name(f".{final.name}.tmp")
        try:
            temp.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
            os.replace(temp, final)
        except OSError as err:
            print(f"{name}: cannot write heartbeat {final}: {err}", file=sys.stderr)
            try:
                temp.unlink(missing_ok=True)
            except OSError:
                pass

    def _status_of(self, name: str, state: dict | None, at: datetime) -> DeviceStatus:
        if state is None:
            return DeviceStatus(name, None, alarm=False, age_days=None)
        since = _parse_time(state.get("last_success")) or _parse_time(state.get("created")) or at
        age = (at - since).total_seconds() / 86400
        alarm = state.get("verdict") == "error" or age > self.config.stale_after_days
        return DeviceStatus(name, state, alarm=alarm, age_days=age)

    def statuses(self) -> list[DeviceStatus]:
        at = self.now()
        return [self._status_of(d.name, self._load(d.name), at) for d in self.config.devices]

    # --------------------------------------------------------------------- push

    def _message(self, status: DeviceStatus, result: PassResult) -> str:
        parts = []
        limit = self.config.stale_after_days
        if status.age_days is not None and status.age_days > limit:
            parts.append(f"no collection for {status.age_days:.1f} d (limit {limit:g} d)")
        if result.result == "ok":
            parts.append(f"ok: {result.summary or 'collected'}")
        elif result.result == "error":
            parts.append(f"error: {result.reason}")
        else:
            parts.append(f"offline; last collection {_human_age(status.age_days or 0)} ago")
            last_error = (status.state or {}).get("last_error") or {}
            if status.state and status.state.get("verdict") == "error":
                parts.append(f"unresolved error: {last_error.get('reason')}")
        return _compact("; ".join(parts), PUSH_MSG_MAX)

    def _push(self, device: Device, state: dict, result: PassResult, at: datetime) -> None:
        if device.push_url is None:
            return
        status = self._status_of(device.name, state, at)
        url = push_url_with(
            device.push_url, "down" if status.alarm else "up", self._message(status, result)
        )
        try:
            self.pusher(url)
        except Exception as err:  # noqa: BLE001 - a dead monitor must not fail a collection
            print(f"{device.name}: push failed: {err}", file=sys.stderr)


def exit_code(results: dict[str, PassResult]) -> int:
    """0 unless a device had an error in this pass. Staleness alone is not a
    failed run; ``status`` (and the push) report that."""
    return 1 if any(r.result == "error" for r in results.values()) else 0


def print_results(results: dict[str, PassResult]) -> None:
    for name, result in results.items():
        detail = result.summary if result.result == "ok" else result.reason
        print(f"{name}: {result.result}" + (f" — {detail}" if detail else ""))


def run_forever(fleet: Fleet, every: float, sleeper: Callable[[float], None] = time.sleep) -> None:
    """Pass, sleep, pass — for a container without a timer. A pass that raises
    is reported and the loop goes on; only an interrupt ends it."""
    while True:
        try:
            print_results(fleet.run_once())
        except KeyboardInterrupt:
            return
        except Exception as err:  # noqa: BLE001 - one bad pass must not end the service
            print(f"pass failed: {err}", file=sys.stderr)
        sys.stdout.flush()
        try:
            sleeper(every)
        except KeyboardInterrupt:
            return


def print_status(fleet: Fleet) -> int:
    alarm = False
    for status in fleet.statuses():
        if status.state is None:
            print(f"{status.name}: never run")
            continue
        alarm |= status.alarm
        state = status.state
        since = "never collected" if state.get("last_success") is None else "last success"
        line = (
            f"{status.name}: {'ALARM' if status.alarm else 'ok'}"
            f" — last pass {state.get('last_result')} at {state.get('last_run')},"
            f" {since} {_human_age(status.age_days or 0)} ago"
        )
        if state.get("verdict") == "error":
            line += f"; error: {(state.get('last_error') or {}).get('reason')}"
        print(line)
    return 1 if alarm else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Collect sd_logger chunks from many devices, unattended.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="one pass over every device")
    run.add_argument("--config", required=True, type=Path)
    run.add_argument("--every", type=float, metavar="SECONDS", help="repeat until interrupted")
    status = sub.add_parser("status", help="heartbeat table; exit 1 if any device is in alarm")
    status.add_argument("--config", required=True, type=Path)
    args = parser.parse_args(argv)

    try:
        config = load_config(args.config)
    except ConfigError as err:
        print(f"config: {err}", file=sys.stderr)
        return EX_CONFIG
    fleet = Fleet(config)

    if args.command == "status":
        return print_status(fleet)
    if args.every is not None:
        if args.every <= 0:
            print("--every must be > 0", file=sys.stderr)
            return EX_CONFIG
        run_forever(fleet, args.every)
        return 0
    results = fleet.run_once()
    print_results(results)
    return exit_code(results)


if __name__ == "__main__":
    sys.exit(main())
