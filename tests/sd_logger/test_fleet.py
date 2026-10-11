"""script/sdlog_fleet.py — unattended collection from many devices.

Requirements and their IDs are in ``docs/sdlog-fleet-spec.md`` §3; each case
names the one it proves. Two kinds of case live here:

- **unit** cases drive :class:`Fleet` with an injected runner, clock, pusher and
  disk probe. They pin the policy — what is an alarm, what is silent, what is
  written where — without spawning a process.
- **end-to-end** cases run the real ``sdlog_collect.py`` as a subprocess against
  the same in-process ``FakeDevice`` the collector's own suite uses. They are
  the proof that the exit contract the policy relies on is the one the
  collector actually emits.
"""

from __future__ import annotations

import fcntl
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest

from sd_logger.test_collect import FakeDevice, devices, sealed_chunk  # noqa: F401 - devices is a fixture

_ROOT = Path(__file__).parent.parent.parent
_SCRIPT = _ROOT / "script"
EXAMPLE = _ROOT / "deploy" / "sdlog-fleet" / "fleet.example.toml"


def _load(name: str):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, _SCRIPT / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sdlog_fleet = _load("sdlog_fleet")

T0 = datetime(2026, 10, 10, 12, 0, tzinfo=timezone.utc)


# ----------------------------------------------------------------- helpers


class Clock:
    def __init__(self, at: datetime = T0) -> None:
        self.at = at

    def __call__(self) -> datetime:
        return self.at

    def advance(self, **delta) -> None:
        self.at += timedelta(**delta)


class Runner:
    """Stands in for the collector subprocess: records argv, answers by device."""

    def __init__(self, codes: dict[str, int] | None = None, default: int = 0) -> None:
        self.codes = codes or {}
        self.default = default
        self.calls: list[list[str]] = []
        self.delay: dict[str, float] = {}
        self._lock = threading.Lock()

    def __call__(self, argv: list[str], timeout: float) -> tuple[int, str]:
        name = argv[argv.index("--expect") + 1]
        with self._lock:
            self.calls.append(argv)
        if name in self.delay:
            time.sleep(self.delay[name])
        code = self.codes.get(name, self.default)
        if isinstance(code, BaseException):
            raise code
        return code, f"{name}: 1 fetched (1.0 KB), 1 confirmed\n"

    def names(self) -> list[str]:
        return sorted(argv[argv.index("--expect") + 1] for argv in self.calls)


class Pusher:
    def __init__(self, fail: bool = False) -> None:
        self.urls: list[str] = []
        self.fail = fail

    def __call__(self, url: str) -> None:
        self.urls.append(url)
        if self.fail:
            raise OSError("kuma is down")

    def last(self) -> dict[str, str]:
        query = parse_qs(urlparse(self.urls[-1]).query)
        return {key: values[-1] for key, values in query.items()}


def write_config(tmp_path: Path, devices_toml: str, **top) -> Path:
    values = {
        "archive": str(tmp_path / "archive"),
        "state": str(tmp_path / "state"),
        "stale_after_days": 3,
        "min_free_mb": 1,
        "timeout_s": 60,
        "retries": 1,
        "backoff": 0,
    }
    values.update(top)
    lines = []
    for key, value in values.items():
        if value is None:
            continue
        lines.append(f"{key} = {json.dumps(value)}")
    path = tmp_path / "fleet.toml"
    path.write_text("\n".join(lines) + "\n\n" + devices_toml)
    return path


def device_toml(name: str, url: str = "http://192.0.2.10:8080", push: str | None = None) -> str:
    text = f'[[device]]\nname = "{name}"\nurl = "{url}"\n'
    if push is not None:
        text += f'push_url = "{push}"\n'
    return text + "\n"


def make_fleet(tmp_path: Path, names=("mr-orange",), *, runner=None, clock=None, pusher=None,
               free_mb: float = 10_000, push: bool = True, **top):
    toml = "".join(
        device_toml(name, push=f"http://kuma.example:3001/api/push/{name}" if push else None)
        for name in names
    )
    config = sdlog_fleet.load_config(write_config(tmp_path, toml, **top))
    fleet = sdlog_fleet.Fleet(
        config,
        runner=runner or Runner(),
        now=clock or Clock(),
        pusher=pusher or Pusher(),
        disk_free=lambda _path: int(free_mb * 1024 * 1024),
    )
    return fleet


def heartbeat(tmp_path: Path, name: str = "mr-orange") -> dict:
    return json.loads((tmp_path / "state" / f"{name}.json").read_text())


# ------------------------------------------------------------------ F1 config


def test_config_loads_the_example() -> None:
    """F1, F16. The shipped example is a config that actually loads."""
    config = sdlog_fleet.load_config(EXAMPLE)
    assert [d.name for d in config.devices]
    assert config.stale_after_days > 0


@pytest.mark.parametrize(
    ("top", "devices_toml", "needle"),
    [
        ({"pushurl": "x"}, device_toml("a"), "pushurl"),
        ({}, device_toml("a") + device_toml("a"), "duplicate"),
        ({}, device_toml("../a"), "name"),
        ({}, device_toml(".hidden"), "name"),
        ({}, device_toml("a", url="ftp://192.0.2.1/"), "url"),
        ({}, device_toml("a", url="192.0.2.1:8080"), "url"),
        ({"stale_after_days": 0}, device_toml("a"), "stale_after_days"),
        ({"timeout_s": -1}, device_toml("a"), "timeout_s"),
        ({"min_free_mb": -5}, device_toml("a"), "min_free_mb"),
        ({}, "", "device"),
        ({}, '[[device]]\nname = "a"\nurl = "http://192.0.2.1"\npushurl = "x"\n', "pushurl"),
        ({}, '[[device]]\nname = "a"\n', "url"),
        ({"archive": None}, device_toml("a"), "archive"),
        ({}, device_toml("a", push="not a url"), "push_url"),
    ],
)
def test_config_rejects(tmp_path: Path, top: dict, devices_toml: str, needle: str) -> None:
    """F1. A typo must not quietly disable an alarm."""
    path = write_config(tmp_path, devices_toml, **top)
    with pytest.raises(sdlog_fleet.ConfigError, match=needle):
        sdlog_fleet.load_config(path)


def test_a_bad_config_exits_two(tmp_path: Path, capsys) -> None:
    """F1, F13."""
    path = write_config(tmp_path, device_toml("a"), bogus=1)
    assert sdlog_fleet.main(["run", "--config", str(path)]) == 2
    assert "bogus" in capsys.readouterr().err
    assert sdlog_fleet.main(["run", "--config", str(tmp_path / "missing.toml")]) == 2
    (tmp_path / "broken.toml").write_text("archive = [")
    assert sdlog_fleet.main(["status", "--config", str(tmp_path / "broken.toml")]) == 2


# --------------------------------------------------------- F2, F3 one pass each


def test_each_device_gets_one_pull_with_expect(tmp_path: Path) -> None:
    """F2. The collector next to the fleet, the fleet's own interpreter, and the
    URL pinned to the configured name."""
    runner = Runner()
    fleet = make_fleet(tmp_path, ("mr-orange", "mr-blue"), runner=runner)

    fleet.run_once()

    assert runner.names() == ["mr-blue", "mr-orange"]
    argv = next(a for a in runner.calls if "mr-orange" in a)
    assert argv[0] == sys.executable
    assert Path(argv[1]) == _SCRIPT / "sdlog_collect.py"
    assert argv[2:4] == ["pull", "http://192.0.2.10:8080"]
    assert argv[argv.index("--into") + 1] == str(tmp_path / "archive")
    assert argv[argv.index("--retries") + 1] == "1"
    assert argv[argv.index("--backoff") + 1] == "0"


@pytest.mark.parametrize(
    ("code", "result"),
    [
        (0, "ok"),
        (75, "offline"),
        (1, "error"),
        (2, "error"),
        (3, "error"),
        (74, "error"),
        (-9, "error"),
        (127, "error"),
    ],
)
def test_exit_status_mapping(tmp_path: Path, code: int, result: str) -> None:
    """F3. Only 75 is silent, only 0 is a success."""
    fleet = make_fleet(tmp_path, runner=Runner(default=code))
    results = fleet.run_once()
    assert results["mr-orange"].result == result
    assert results["mr-orange"].exit == code


def test_a_hung_pass_is_killed_and_is_an_error(tmp_path: Path) -> None:
    """F3. A real child that outlives ``timeout_s`` is killed; the partial it
    left resumes next run, but a pass that never ends needs a look."""
    hang = tmp_path / "hang.py"
    hang.write_text("import time\ntime.sleep(30)\n")
    config = sdlog_fleet.load_config(
        write_config(tmp_path, device_toml("mr-orange"), timeout_s=0.5)
    )
    fleet = sdlog_fleet.Fleet(config, collector=hang, now=Clock(), pusher=Pusher(),
                              disk_free=lambda _p: 1 << 40)

    started = time.monotonic()
    results = fleet.run_once()

    assert time.monotonic() - started < 10
    assert results["mr-orange"].result == "error"
    assert "timeout" in results["mr-orange"].reason


def test_a_missing_collector_is_an_error_not_offline(tmp_path: Path) -> None:
    """F3. ``python missing.py`` exits 2. Under the old contract that was
    'unreachable', and a broken deploy would have been silent forever."""
    config = sdlog_fleet.load_config(write_config(tmp_path, device_toml("mr-orange")))
    fleet = sdlog_fleet.Fleet(config, collector=tmp_path / "nope.py", now=Clock(),
                              pusher=Pusher(), disk_free=lambda _p: 1 << 40)
    assert fleet.run_once()["mr-orange"].result == "error"


# ------------------------------------------------------- F4, F8, F9 heartbeat


def test_an_offline_device_is_quiet(tmp_path: Path) -> None:
    """F4. The car is away: no success recorded, no verdict change, push stays up."""
    clock = Clock()
    runner = Runner()
    pusher = Pusher()
    fleet = make_fleet(tmp_path, runner=runner, clock=clock, pusher=pusher)
    fleet.run_once()
    clock.advance(hours=5)
    runner.default = 75

    results = fleet.run_once()

    state = heartbeat(tmp_path)
    assert results["mr-orange"].result == "offline"
    assert state["last_result"] == "offline"
    assert state["last_success"] == T0.isoformat()
    assert state["verdict"] == "ok"
    assert pusher.last()["status"] == "up"
    assert sdlog_fleet.exit_code(results) == 0


def test_the_heartbeat_file_records_each_pass(tmp_path: Path) -> None:
    """F8."""
    clock = Clock()
    runner = Runner(default=1)
    fleet = make_fleet(tmp_path, runner=runner, clock=clock)

    fleet.run_once()
    state = heartbeat(tmp_path)
    assert state["device"] == "mr-orange"
    assert state["created"] == T0.isoformat()
    assert state["last_run"] == T0.isoformat()
    assert state["last_result"] == "error"
    assert state["last_success"] is None
    assert state["verdict"] == "error"
    assert state["last_error"]["exit"] == 1
    assert state["last_error"]["at"] == T0.isoformat()

    clock.advance(minutes=5)
    runner.default = 0
    fleet.run_once()
    state = heartbeat(tmp_path)
    assert state["last_result"] == "ok"
    assert state["last_success"] == clock.at.isoformat()
    assert state["verdict"] == "ok"
    assert state["created"] == T0.isoformat()
    # The last error is history, kept for whoever comes to look.
    assert state["last_error"]["exit"] == 1


def test_a_torn_chunk_is_a_warning_not_an_alarm(tmp_path: Path) -> None:
    """F17. The collector's TORN line keeps the pass ok and the push up; the heartbeat
    counts it and remembers the newest one, and the summary names it."""
    output = (
        "TORN L0000012.LOG: torn on the card: no #close, 1536 B stable over two full fetches\n"
        "mr-orange: 1 fetched (1.5 KB), 1 confirmed, 1 torn accepted (L0000012.LOG)\n"
    )
    clock = Clock()
    pusher = Pusher()
    fleet = make_fleet(tmp_path, runner=lambda argv, timeout: (0, output), clock=clock, pusher=pusher)

    results = fleet.run_once()

    assert results["mr-orange"].result == "ok"
    assert sdlog_fleet.exit_code(results) == 0
    state = heartbeat(tmp_path)
    assert (state["verdict"], state["torn_accepted"]) == ("ok", 1)
    assert state["last_torn"] == {
        "at": T0.isoformat(),
        "name": "L0000012.LOG",
        "reason": "torn on the card: no #close, 1536 B stable over two full fetches",
    }
    assert "1 torn accepted (L0000012.LOG)" in state["summary"]
    query = parse_qs(urlparse(pusher.urls[-1]).query)
    assert query["status"] == ["up"] and "L0000012.LOG" in query["msg"][0]
    fleet.run_once()
    assert heartbeat(tmp_path)["torn_accepted"] == 2


def test_the_heartbeat_file_is_replaced_atomically(tmp_path: Path, monkeypatch) -> None:
    """F8. A crash mid-write leaves the previous heartbeat, never half a file."""
    clock = Clock()
    fleet = make_fleet(tmp_path, clock=clock)
    fleet.run_once()
    before = (tmp_path / "state" / "mr-orange.json").read_text()

    def broken_replace(*_args, **_kwargs):
        raise OSError("power cut")

    monkeypatch.setattr(sdlog_fleet.os, "replace", broken_replace)
    clock.advance(minutes=5)
    fleet.run_once()

    assert (tmp_path / "state" / "mr-orange.json").read_text() == before
    assert sorted(p.name for p in (tmp_path / "state").iterdir() if p.suffix != ".lock") == [
        "mr-orange.json"
    ]


def test_staleness_raises_the_alarm(tmp_path: Path) -> None:
    """F9 — E4. Three days without a success: the push turns red, even though
    every single pass was only 'offline'."""
    clock = Clock()
    runner = Runner()
    pusher = Pusher()
    fleet = make_fleet(tmp_path, runner=runner, clock=clock, pusher=pusher)
    fleet.run_once()
    runner.default = 75

    clock.advance(days=2, hours=23)
    fleet.run_once()
    assert pusher.last()["status"] == "up"

    clock.advance(hours=2)
    results = fleet.run_once()
    assert pusher.last()["status"] == "down"
    assert "no collection for 3.0 d" in pusher.last()["msg"]
    assert fleet.statuses()[0].alarm is True
    # Staleness is the status command's business, not a failed run (F13).
    assert sdlog_fleet.exit_code(results) == 0


def test_a_never_collected_device_ages_from_creation(tmp_path: Path) -> None:
    """F9. A fresh deploy with the car away is not an alarm on day one."""
    clock = Clock()
    pusher = Pusher()
    fleet = make_fleet(tmp_path, runner=Runner(default=75), clock=clock, pusher=pusher)

    fleet.run_once()
    assert pusher.last()["status"] == "up"
    assert fleet.statuses()[0].alarm is False

    clock.advance(days=4)
    fleet.run_once()
    assert pusher.last()["status"] == "down"


def test_an_error_survives_offline_passes_until_a_success(tmp_path: Path) -> None:
    """F9. A defect seen once stays red while the car is away — 'offline' says
    nothing about whether the defect is gone. Only a success clears it."""
    clock = Clock()
    runner = Runner(default=1)
    pusher = Pusher()
    fleet = make_fleet(tmp_path, runner=runner, clock=clock, pusher=pusher)

    fleet.run_once()
    assert pusher.last()["status"] == "down"

    runner.default = 75
    clock.advance(minutes=5)
    fleet.run_once()
    assert pusher.last()["status"] == "down"
    assert heartbeat(tmp_path)["verdict"] == "error"

    runner.default = 0
    clock.advance(minutes=5)
    fleet.run_once()
    assert pusher.last()["status"] == "up"
    assert heartbeat(tmp_path)["verdict"] == "ok"


# -------------------------------------------------------------------- F7 disk


def test_a_nearly_full_disk_is_an_error_and_pulls_nothing(tmp_path: Path) -> None:
    """F7."""
    runner = Runner()
    pusher = Pusher()
    fleet = make_fleet(tmp_path, ("mr-orange", "mr-blue"), runner=runner, pusher=pusher,
                       free_mb=10, min_free_mb=100)

    results = fleet.run_once()

    assert runner.calls == []
    assert {r.result for r in results.values()} == {"error"}
    assert "space" in results["mr-orange"].reason
    assert pusher.last()["status"] == "down"
    assert sdlog_fleet.exit_code(results) == 1


# -------------------------------------------------------------------- F10 push


def test_push_reports_up_and_down(tmp_path: Path) -> None:
    """F10. Every non-busy pass pushes; ``down`` iff in alarm."""
    runner = Runner()
    pusher = Pusher()
    fleet = make_fleet(tmp_path, runner=runner, pusher=pusher)

    fleet.run_once()
    assert urlparse(pusher.urls[-1]).path == "/api/push/mr-orange"
    assert pusher.last()["status"] == "up"
    assert "confirmed" in pusher.last()["msg"]

    runner.default = 3
    fleet.run_once()
    assert pusher.last()["status"] == "down"
    assert "exit 3" in pusher.last()["msg"]
    assert len(pusher.urls) == 2


def test_a_device_without_push_url_is_not_pushed(tmp_path: Path) -> None:
    """F10. push_url is optional."""
    pusher = Pusher()
    fleet = make_fleet(tmp_path, pusher=pusher, push=False)
    fleet.run_once()
    assert pusher.urls == []


def test_push_failure_does_not_fail_the_pass(tmp_path: Path, capsys) -> None:
    """F10. Kuma down is Kuma's problem; the collection still counts."""
    fleet = make_fleet(tmp_path, pusher=Pusher(fail=True))

    results = fleet.run_once()

    assert results["mr-orange"].result == "ok"
    assert heartbeat(tmp_path)["last_result"] == "ok"
    assert sdlog_fleet.exit_code(results) == 0
    assert "push" in capsys.readouterr().err


def test_push_message_is_bounded_and_encoded(tmp_path: Path) -> None:
    """F10. Kuma's copy-paste URL already carries ``?status=up&msg=OK&ping=``;
    those are replaced, not duplicated, and a long collector report is cut."""
    toml = device_toml(
        "mr-orange", push="http://kuma.example:3001/api/push/TOKEN?status=up&msg=OK&ping="
    )
    config = sdlog_fleet.load_config(write_config(tmp_path, toml))

    class Chatty(Runner):
        def __call__(self, argv, timeout):
            super().__call__(argv, timeout)
            return 1, "LEFT  " + "x & y = z " * 100

    pusher = Pusher()
    fleet = sdlog_fleet.Fleet(config, runner=Chatty(), now=Clock(), pusher=pusher,
                              disk_free=lambda _p: 1 << 40)
    fleet.run_once()

    query = parse_qs(urlparse(pusher.urls[-1]).query, keep_blank_values=True)
    assert query["status"] == ["down"]
    assert len(query["msg"]) == 1
    assert len(query["msg"][0]) <= sdlog_fleet.PUSH_MSG_MAX
    assert "x & y = z" in query["msg"][0]
    assert urlparse(pusher.urls[-1]).path == "/api/push/TOKEN"


def test_the_default_pusher_speaks_http(tmp_path: Path) -> None:
    """F10. The real pusher against a tiny local endpoint, incl. a 404 for an
    unknown token raising rather than passing for a success."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    seen: list[str] = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):  # noqa: N802
            seen.append(self.path)
            ok = self.path.startswith("/api/push/good")
            body = b'{"ok": true}' if ok else b'{"ok": false}'
            self.send_response(200 if ok else 404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(
        target=server.serve_forever, kwargs={"poll_interval": 0.005}, daemon=True
    )
    thread.start()
    try:
        base = f"http://127.0.0.1:{server.server_address[1]}"
        sdlog_fleet.http_push(f"{base}/api/push/good?status=up&msg=hi")
        with pytest.raises(OSError):
            sdlog_fleet.http_push(f"{base}/api/push/bad?status=up&msg=hi")
    finally:
        server.shutdown()
        server.server_close()
    assert seen[0] == "/api/push/good?status=up&msg=hi"


# -------------------------------------------------------------------- F11 lock


def test_a_held_lock_skips_the_device_quietly(tmp_path: Path) -> None:
    """F11. Another pass for this device is still running: no collector, no
    heartbeat write, no push — the running pass will do all three."""
    runner = Runner()
    pusher = Pusher()
    fleet = make_fleet(tmp_path, ("mr-orange", "mr-blue"), runner=runner, pusher=pusher)
    state = tmp_path / "state"
    state.mkdir(parents=True, exist_ok=True)
    with open(state / "mr-orange.lock", "a") as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        results = fleet.run_once()

    assert results["mr-orange"].result == "busy"
    assert runner.names() == ["mr-blue"]
    assert not (state / "mr-orange.json").exists()
    assert all("mr-orange" not in url for url in pusher.urls)
    assert sdlog_fleet.exit_code(results) == 0


def test_the_lock_is_released_after_a_pass(tmp_path: Path) -> None:
    """F11. Including after a pass that raised inside the runner."""
    runner = Runner(default=RuntimeError("runner blew up"))
    fleet = make_fleet(tmp_path, runner=runner)
    assert fleet.run_once()["mr-orange"].result == "error"

    with open(tmp_path / "state" / "mr-orange.lock", "a") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)  # would raise if still held


# -------------------------------------------------------------- F12, F13, F15


def test_a_slow_device_does_not_delay_another(tmp_path: Path) -> None:
    """F12."""
    finished: dict[str, float] = {}
    runner = Runner()
    runner.delay["mr-orange"] = 0.6

    class Timed(Runner):
        def __call__(self, argv, timeout):
            out = runner(argv, timeout)
            finished[argv[argv.index("--expect") + 1]] = time.monotonic()
            return out

    fleet = make_fleet(tmp_path, ("mr-orange", "mr-blue"), runner=Timed())
    started = time.monotonic()
    fleet.run_once()

    assert finished["mr-blue"] - started < 0.4
    assert finished["mr-orange"] - started >= 0.6


@pytest.mark.parametrize(
    ("codes", "expected"),
    [({"a": 0, "b": 75}, 0), ({"a": 0, "b": 1}, 1), ({"a": 75, "b": 74}, 1)],
)
def test_run_exit_status(tmp_path: Path, codes: dict, expected: int) -> None:
    """F13."""
    fleet = make_fleet(tmp_path, ("a", "b"), runner=Runner(codes))
    assert sdlog_fleet.exit_code(fleet.run_once()) == expected


def test_every_repeats_the_pass(tmp_path: Path) -> None:
    """F15. The container loop: pass, sleep, pass, until interrupted."""
    runner = Runner()
    fleet = make_fleet(tmp_path, runner=runner)
    sleeps: list[float] = []

    def sleeper(seconds: float) -> None:
        sleeps.append(seconds)
        if len(sleeps) == 2:
            raise KeyboardInterrupt

    sdlog_fleet.run_forever(fleet, every=300, sleeper=sleeper)

    # pass, sleep, pass, interrupted in the second sleep
    assert len(runner.calls) == 2
    assert sleeps == [300, 300]


def test_every_survives_a_pass_that_raised(tmp_path: Path, monkeypatch) -> None:
    """F15. One bad pass must not end the container."""
    fleet = make_fleet(tmp_path)
    calls = []

    def flaky():
        calls.append(1)
        if len(calls) == 1:
            raise RuntimeError("boom")
        return {}

    monkeypatch.setattr(fleet, "run_once", flaky)
    sleeps = []

    def sleeper(seconds):
        sleeps.append(seconds)
        if len(sleeps) == 2:
            raise KeyboardInterrupt

    sdlog_fleet.run_forever(fleet, every=1, sleeper=sleeper)
    assert len(calls) == 2


# ------------------------------------------------------------------- F14 status


def test_status_reports_alarm_and_exits_one(tmp_path: Path, capsys) -> None:
    """F14. Reads the heartbeat files only; contacts no device."""
    path = write_config(tmp_path, device_toml("mr-orange") + device_toml("mr-blue"))
    config = sdlog_fleet.load_config(path)
    clock = Clock(datetime.now(timezone.utc))
    fleet = sdlog_fleet.Fleet(config, runner=Runner({"mr-orange": 0, "mr-blue": 1}),
                              now=clock, pusher=Pusher(), disk_free=lambda _p: 1 << 40)
    fleet.run_once()

    assert sdlog_fleet.main(["status", "--config", str(path)]) == 1
    out = capsys.readouterr().out
    orange = next(line for line in out.splitlines() if "mr-orange" in line)
    blue = next(line for line in out.splitlines() if "mr-blue" in line)
    assert "ok" in orange and "ALARM" not in orange
    assert "ALARM" in blue and "error" in blue


def test_status_of_a_device_never_run_is_not_an_alarm(tmp_path: Path, capsys) -> None:
    """F14. No heartbeat file yet: reported as such, not as a fault."""
    path = write_config(tmp_path, device_toml("mr-orange"))
    assert sdlog_fleet.main(["status", "--config", str(path)]) == 0
    assert "never run" in capsys.readouterr().out


# ----------------------------------------------- end to end, real collector


def e2e_fleet(tmp_path: Path, fakes: list[FakeDevice], pusher: Pusher | None = None, **top):
    toml = "".join(
        device_toml(fake.device, url=fake.url, push=f"http://kuma.example/api/push/{fake.device}")
        for fake in fakes
    )
    config = sdlog_fleet.load_config(write_config(tmp_path, toml, **top))
    return sdlog_fleet.Fleet(config, pusher=pusher or Pusher())


def archived(tmp_path: Path, name: str) -> list[str]:
    directory = tmp_path / "archive" / name
    return sorted(p.name for p in directory.iterdir()) if directory.is_dir() else []


def test_end_to_end_two_devices_are_archived_and_confirmed(devices, tmp_path: Path) -> None:
    """F2, end to end."""
    orange = devices("mr-orange")
    blue = devices("mr-blue")
    orange.add(431)
    orange.add(432)
    blue.add(431)

    results = e2e_fleet(tmp_path, [orange, blue]).run_once()

    assert {name: r.result for name, r in results.items()} == {"mr-orange": "ok", "mr-blue": "ok"}
    assert archived(tmp_path, "mr-orange") == ["L0000431.LOG", "L0000432.LOG"]
    assert archived(tmp_path, "mr-blue") == ["L0000431.LOG"]
    assert sorted(orange.confirmed) == ["L0000431.LOG", "L0000432.LOG"]
    assert heartbeat(tmp_path, "mr-blue")["last_success"] is not None


def test_end_to_end_an_absent_device_is_quiet(devices, tmp_path: Path) -> None:
    """F4 — E3 'car away'."""
    gone = devices("mr-orange")
    pusher = Pusher()
    fleet = e2e_fleet(tmp_path, [gone], pusher=pusher)
    gone.stop()

    results = fleet.run_once()

    assert results["mr-orange"].result == "offline"
    assert results["mr-orange"].exit == 75
    assert pusher.last()["status"] == "up"
    assert sdlog_fleet.exit_code(results) == 0


def test_end_to_end_the_wrong_device_at_the_url_is_an_error(devices, tmp_path: Path) -> None:
    """F2/C4. A DHCP lease moved: mr-blue answers where mr-orange is configured."""
    blue = devices("mr-blue")
    blue.add(431)
    toml = device_toml("mr-orange", url=blue.url)
    fleet = sdlog_fleet.Fleet(sdlog_fleet.load_config(write_config(tmp_path, toml)),
                              pusher=Pusher())

    results = fleet.run_once()

    assert results["mr-orange"].result == "error"
    assert results["mr-orange"].exit == 3
    assert blue.confirms() == []
    assert archived(tmp_path, "mr-blue") == []


def test_end_to_end_a_cut_transfer_resumes_on_the_next_run(devices, tmp_path: Path) -> None:
    """F5 — E3. WiFi gone mid-transfer: silent, partial kept; the next run
    resumes, stores the chunk once and confirms it once."""
    fake = devices("mr-orange")
    name = fake.add(431, sealed_chunk(431, records=200))
    fake.cut(name, 1500, once=False)
    fleet = e2e_fleet(tmp_path, [fake])

    first = fleet.run_once()
    assert first["mr-orange"].result == "offline"
    assert (tmp_path / "archive" / "mr-orange" / f"{name}.part").is_file()
    assert fake.confirms() == []

    fake.cut_after.pop(name)
    second = fleet.run_once()

    assert second["mr-orange"].result == "ok"
    assert archived(tmp_path, "mr-orange") == [name]
    assert (tmp_path / "archive" / "mr-orange" / name).read_bytes() == fake.chunks[name]
    assert len(fake.confirms(name)) == 1
    resumed = [r for r in fake.bodies(name) if r.range and r.range != "bytes=0-"]
    assert resumed, "the second run must resume with a Range, not refetch from 0"


def test_end_to_end_the_archive_is_append_only(devices, tmp_path: Path) -> None:
    """F6. A file already in the archive is never rewritten — not by a normal
    run, and not when the device offers a different file under its name (a new
    card that reused the seq). That case is an error for a human, not a fix."""
    fake = devices("mr-orange")
    fake.add(431)
    clash = fake.add(432)
    directory = tmp_path / "archive" / "mr-orange"
    directory.mkdir(parents=True)
    old = sealed_chunk(432, stamp=777_000)
    (directory / clash).write_bytes(old)
    past = time.time() - 3600
    os.utime(directory / clash, (past, past))
    fleet = e2e_fleet(tmp_path, [fake])

    first = fleet.run_once()
    snapshot = {p.name: (p.read_bytes(), p.stat().st_mtime) for p in directory.iterdir()}
    second = fleet.run_once()

    assert first["mr-orange"].result == "error"
    assert second["mr-orange"].result == "error"
    assert (directory / clash).read_bytes() == old
    assert (directory / clash).stat().st_mtime == pytest.approx(past)
    assert fake.confirms(clash) == []
    for path in directory.iterdir():
        assert (path.read_bytes(), path.stat().st_mtime) == snapshot[path.name]
    assert archived(tmp_path, "mr-orange") == ["L0000431.LOG", clash]


def test_end_to_end_cli_run(devices, tmp_path: Path, capsys) -> None:
    """F13 through ``main``: one ok device, one away."""
    here = devices("mr-orange")
    away = devices("mr-blue")
    here.add(431)
    toml = device_toml("mr-orange", url=here.url) + device_toml("mr-blue", url=away.url)
    path = write_config(tmp_path, toml)
    away.stop()

    assert sdlog_fleet.main(["run", "--config", str(path)]) == 0
    out = capsys.readouterr().out
    assert "mr-orange" in out and "ok" in out
    assert "mr-blue" in out and "offline" in out


def test_the_fleet_runs_as_a_script(tmp_path: Path) -> None:
    """The shebang path a systemd unit uses, not just the import."""
    path = write_config(tmp_path, device_toml("mr-orange"))
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT / "sdlog_fleet.py"), "status", "--config", str(path)],
        capture_output=True, text=True, timeout=30,
    )
    assert proc.returncode == 0, proc.stderr
    assert "mr-orange" in proc.stdout
