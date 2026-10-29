"""sdlog_relay core (docs/sdlog-relay-spec.md), without Home Assistant.

The relay runs in-process on its own event loop thread behind a minimal aiohttp
app that stands in for the HA views: the views only look up the entry and the
user, everything else is ``relay_core``. Upstream is the ``FakeDevice`` from the
collector suite, so the wire contract is the one the collector is tested against,
plus small stubs where a case needs an upstream the fake cannot be (slow, huge,
answering 401, echoing headers).
"""

from __future__ import annotations

import ast
import asyncio
import hashlib
import http.client
import importlib.util
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import sys
import threading
import tracemalloc
import urllib.error
import urllib.parse
import urllib.request

import aiohttp
from aiohttp import web
import pytest

from sd_logger.test_collect import HEAD, SEAM, FakeDevice, sdlog_collect

_REPO = Path(__file__).parent.parent.parent
_PKG = _REPO / "custom_components" / "sdlog_relay"
_CORE = _PKG / "relay_core.py"


def _load_core():
    # The package __init__ imports homeassistant; the core must load without it.
    spec = importlib.util.spec_from_file_location("sdlog_relay_core", _CORE)
    module = importlib.util.module_from_spec(spec)
    sys.modules["sdlog_relay_core"] = module
    spec.loader.exec_module(module)
    return module


core = _load_core()


# ------------------------------------------------------------------ the relay


class RelayServer:
    """The core behind a stand-in for the HA views. ``X-Test-Admin: 1`` plays
    the admin user; HA's own auth layer is tested in tests_ha/."""

    def __init__(self, targets: dict, store_root: Path) -> None:
        self.targets = targets
        self.store = core.FirmwareStore(store_root)
        self.url = ""
        self._loop = asyncio.new_event_loop()

    def start(self) -> RelayServer:
        ready = threading.Event()

        def run() -> None:
            asyncio.set_event_loop(self._loop)
            self._loop.run_until_complete(self._start())
            ready.set()
            self._loop.run_forever()

        threading.Thread(target=run, daemon=True).start()
        assert ready.wait(5)
        return self

    async def _start(self) -> None:
        self._session = aiohttp.ClientSession(cookie_jar=aiohttp.DummyCookieJar())
        self.relay = core.Relay(self._session)
        app = web.Application()
        app.router.add_route("*", "/r/{logger_id}/sdlog/{tail:.+}", self._pass)
        app.router.add_get("/r/{logger_id}/fw/{name}", self._fw)
        app.router.add_put("/r/{logger_id}/fw", self._upload)
        self._runner = web.AppRunner(app)
        await self._runner.setup()
        await web.TCPSite(self._runner, "127.0.0.1", 0).start()
        self.url = f"http://127.0.0.1:{self._runner.addresses[0][1]}/r"

    async def _pass(self, request: web.Request) -> web.StreamResponse:
        m = request.match_info
        return await self.relay.forward(request, self.targets, m["logger_id"], m["tail"])

    async def _fw(self, request: web.Request) -> web.StreamResponse:
        m = request.match_info
        return await core.serve_firmware(request, self.store, self.targets, m["logger_id"], m["name"])

    async def _upload(self, request: web.Request) -> web.Response:
        return await core.upload_firmware(
            request,
            self.store,
            self.targets,
            request.match_info["logger_id"],
            is_admin=request.headers.get("X-Test-Admin") == "1",
        )

    def stop(self) -> None:
        async def _stop() -> None:
            await self._runner.cleanup()
            await self._session.close()

        asyncio.run_coroutine_threadsafe(_stop(), self._loop).result(5)
        self._loop.call_soon_threadsafe(self._loop.stop)


def target_of(url: str) -> core.Target:
    host, port = url.removeprefix("http://").split(":")
    return core.Target(host, int(port))


@pytest.fixture
def device():
    fake = FakeDevice("mr-orange").start()
    yield fake
    fake.stop()


@pytest.fixture
def relay(device, tmp_path):
    server = RelayServer({"car": target_of(device.url)}, tmp_path / "fw").start()
    yield server
    server.stop()


def call(url: str, *, method: str = "GET", headers: dict | None = None, data: bytes | None = None):
    """(status, headers, body) without urllib raising on 4xx/5xx."""
    request = urllib.request.Request(url, method=method, headers=headers or {}, data=data)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, response.headers, response.read()
    except urllib.error.HTTPError as err:
        return err.code, err.headers, err.read()


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# --------------------------------------------------------------- stub upstreams


class Stub:
    """An upstream that is not an sd_logger: it records what arrived and answers
    with whatever the case needs."""

    def __init__(self, handler) -> None:
        self.handler = handler
        self.seen: list[dict] = []
        self.release = threading.Event()
        stub = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args) -> None:
                pass

            def do_GET(self) -> None:  # noqa: N802
                stub.seen.append(dict(self.headers))
                stub.handler(self, stub)

            do_POST = do_GET  # noqa: N815

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        threading.Thread(target=self._server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        self.target = core.Target("127.0.0.1", self._server.server_address[1])

    def stop(self) -> None:
        self.release.set()
        self._server.shutdown()
        self._server.server_close()


def _answer(h: BaseHTTPRequestHandler, status: int, body: bytes, extra: dict | None = None) -> None:
    h.send_response(status)
    h.send_header("Content-Type", "application/json")
    h.send_header("Content-Length", str(len(body)))
    for k, v in (extra or {}).items():
        h.send_header(k, v)
    h.end_headers()
    h.wfile.write(body)


@pytest.fixture
def stubs(tmp_path):
    made: list[Stub] = []
    servers: list[RelayServer] = []

    def factory(handler) -> tuple[Stub, RelayServer]:
        stub = Stub(handler)
        made.append(stub)
        server = RelayServer({"stub": stub.target}, tmp_path / f"fw{len(made)}").start()
        servers.append(server)
        return stub, server

    yield factory
    for server in servers:
        server.stop()
    for stub in made:
        stub.stop()


# ----------------------------------------------------------- R1, R2: pass-through


def test_relay_passes_status_and_body(relay: RelayServer, device: FakeDevice) -> None:
    name = device.add(431)
    data = device.chunks[name]
    base = f"{relay.url}/car/sdlog"

    status, _, body = call(f"{base}/index")
    assert status == 200 and json.loads(body) == {"device": "mr-orange", "chunks": device.index_entries()}

    status, _, body = call(f"{base}/status")
    assert status == 200 and json.loads(body) == device.status_payload()

    status, headers, body = call(f"{base}/f/{name}")
    assert (status, body, headers["Content-Length"]) == (200, data, str(len(data)))

    status, headers, body = call(f"{base}/f/{name}", headers={"Range": "bytes=10-19"})
    assert (status, body, headers["Content-Range"]) == (206, data[10:20], f"bytes 10-19/{len(data)}")

    status, headers, _ = call(f"{base}/f/{name}", headers={"Range": f"bytes={len(data)}-"})
    assert (status, headers["Content-Range"]) == (416, f"bytes */{len(data)}")

    device.open_file = device.add(432)
    assert call(f"{base}/f/{device.open_file}")[0] == 409
    assert call(f"{base}/f/L9999999.LOG")[0] == 404

    status, _, body = call(f"{base}/done/{name}", method="POST", data=b"")
    assert status == 200 and json.loads(body) == {"confirmed": name}
    assert device.confirmed == [name]
    assert call(f"{base}/done/L9999999.LOG", method="POST", data=b"")[0] == 404


def test_relay_strips_headers(stubs) -> None:
    def handler(h, stub):
        _answer(h, 200, b"{}", {"Set-Cookie": "s=1", "X-Logger-Secret": "x", "Accept-Ranges": "bytes"})

    stub, server = stubs(handler)
    status, headers, _ = call(
        f"{server.url}/stub/sdlog/index",
        headers={"Authorization": "Bearer t", "Cookie": "c=1", "Range": "bytes=0-1", "X-Custom": "1"},
    )

    assert status == 200
    seen = {k.lower(): v for k, v in stub.seen[0].items()}
    assert "authorization" not in seen and "cookie" not in seen and "x-custom" not in seen
    assert seen["range"] == "bytes=0-1"
    assert seen["accept-encoding"] == "identity"
    assert headers.get("Set-Cookie") is None and headers.get("X-Logger-Secret") is None
    assert headers["Accept-Ranges"] == "bytes"


# ------------------------------------------------------------------ R3: memory


def test_relay_streams_32mib_with_bounded_memory(stubs) -> None:
    block = bytes(range(256)) * 256  # 64 KiB, allocated once
    total = 32 * 1024 * 1024

    def handler(h, stub):
        h.send_response(200)
        h.send_header("Content-Type", "text/csv")
        h.send_header("Content-Length", str(total))
        h.end_headers()
        for _ in range(total // len(block)):
            h.wfile.write(block)

    _, server = stubs(handler)
    tracemalloc.start()
    try:
        received = 0
        digest = hashlib.sha256()
        with urllib.request.urlopen(f"{server.url}/stub/sdlog/f/L0000001.LOG", timeout=30) as response:
            while chunk := response.read(64 * 1024):
                received += len(chunk)
                digest.update(chunk)
        _, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()

    assert received == total
    assert digest.hexdigest() == hashlib.sha256(block * (total // len(block))).hexdigest()
    assert peak < 4 * 1024 * 1024, f"peak {peak} B: the relay buffered the chunk"


# ------------------------------------------------------------- R4: paths, names


@pytest.mark.parametrize(
    ("tail", "method", "status"),
    [
        ("f/..", "GET", 400),
        ("f/.hidden", "GET", 400),
        ("f/a..b", "GET", 400),
        ("f/a/b", "GET", 400),
        ("f/%2e%2e", "GET", 400),
        ("f/" + "x" * 33, "GET", 400),
        ("done/..", "POST", 400),
        ("config", "GET", 404),
        ("f", "GET", 404),
        ("../index", "GET", 404),
        ("f/L0000001.LOG", "POST", 405),
        ("done/L0000001.LOG", "GET", 405),
        ("index", "POST", 405),
    ],
)
def test_relay_rejects_bad_names(relay: RelayServer, device: FakeDevice, tail, method, status) -> None:
    got = call(f"{relay.url}/car/sdlog/{tail}", method=method, data=b"" if method == "POST" else None)
    assert got[0] == status
    assert device.requests == [], "the logger must never be contacted"


def test_relay_unknown_logger(relay: RelayServer, device: FakeDevice) -> None:
    status, _, body = call(f"{relay.url}/nope/sdlog/index")
    assert status == 404 and json.loads(body) == {"reason": "unknown_logger"}
    assert device.requests == []


# --------------------------------------------------------------- R5: failures


def test_relay_502_when_offline(tmp_path) -> None:
    server = RelayServer({"gone": core.Target("127.0.0.1", free_port())}, tmp_path).start()
    try:
        status, _, body = call(f"{server.url}/gone/sdlog/index")
    finally:
        server.stop()
    assert status == 502 and json.loads(body) == {"reason": "logger_unreachable"}


def test_relay_aborts_on_upstream_drop(relay: RelayServer, device: FakeDevice) -> None:
    name = device.add(431)
    device.cut(name, 400, once=False)
    with pytest.raises(http.client.IncompleteRead) as caught:
        with urllib.request.urlopen(f"{relay.url}/car/sdlog/f/{name}", timeout=10) as response:
            response.read()
    assert len(caught.value.partial) == 400


# ------------------------------------------------------------ R6: concurrency


def test_relay_one_upstream_per_logger(stubs, device: FakeDevice, tmp_path) -> None:
    def handler(h, stub):
        stub.release.wait(10)
        _answer(h, 200, b"{}")

    stub, server = stubs(handler)
    server.targets["car"] = target_of(device.url)
    first: list = []
    worker = threading.Thread(target=lambda: first.append(call(f"{server.url}/stub/sdlog/index")))
    worker.start()
    for _ in range(200):
        if stub.seen:
            break
        threading.Event().wait(0.01)

    status, _, body = call(f"{server.url}/stub/sdlog/status")
    assert status == 503 and json.loads(body) == {"reason": "busy"}
    # Another logger is not held up.
    assert call(f"{server.url}/car/sdlog/index")[0] == 200

    stub.release.set()
    worker.join(10)
    assert first[0][0] == 200
    assert call(f"{server.url}/stub/sdlog/index")[0] == 200


# -------------------------------------------------------------------- R8: 401


def test_relay_never_emits_401(stubs) -> None:
    _, server = stubs(lambda h, stub: _answer(h, 401, b"{}", {"WWW-Authenticate": "Basic"}))
    status, headers, body = call(f"{server.url}/stub/sdlog/index")
    assert status == 502 and json.loads(body) == {"reason": "logger_answered_401"}
    assert headers.get("WWW-Authenticate") is None


# ----------------------------------------------------------- R9-R12: firmware


def upload(server: RelayServer, data: bytes, *, version="1.0.0", chip="ESP32-C6", admin=True, logger="car"):
    return call(
        f"{server.url}/{logger}/fw?" + urllib.parse.urlencode({"version": version, "chip_family": chip}),
        method="PUT",
        data=data,
        headers={"X-Test-Admin": "1"} if admin else {},
    )


@pytest.fixture
def fw_relay(tmp_path):
    # The tests talk from 127.0.0.1, so that is "the logger".
    server = RelayServer({"car": core.Target("127.0.0.1"), "other": core.Target("192.0.2.10")}, tmp_path).start()
    yield server
    server.stop()


def test_fw_manifest_format(fw_relay: RelayServer) -> None:
    data = b"\xe9" + b"firmware" * 1000
    md5 = hashlib.md5(data).hexdigest()  # noqa: S324

    status, _, body = upload(fw_relay, data)

    assert status == 200
    manifest = json.loads(body)
    assert manifest == {
        "name": "car",
        "version": "1.0.0",
        "builds": [{"chipFamily": "ESP32-C6", "ota": {"md5": md5, "path": f"firmware-{md5}.ota.bin"}}],
    }
    status, headers, served = call(f"{fw_relay.url}/car/fw/manifest.json")
    assert status == 200 and json.loads(served) == manifest
    assert headers["Cache-Control"] == "no-store"
    # The relative path resolves next to the manifest, as the update platform does.
    assert call(f"{fw_relay.url}/car/fw/firmware-{md5}.ota.bin")[2] == data


def test_fw_only_for_logger_ip(fw_relay: RelayServer) -> None:
    upload(fw_relay, b"car-image")
    upload(fw_relay, b"other-image", logger="other")
    assert call(f"{fw_relay.url}/car/fw/manifest.json")[0] == 200
    assert call(f"{fw_relay.url}/other/fw/manifest.json")[0] == 404


def test_fw_other_source_404(fw_relay: RelayServer) -> None:
    upload(fw_relay, b"other-image", logger="other")
    md5 = hashlib.md5(b"other-image").hexdigest()  # noqa: S324
    for name in ("manifest.json", f"firmware-{md5}.ota.bin"):
        status, _, body = call(f"{fw_relay.url}/other/fw/{name}")
        assert status == 404 and json.loads(body) == {"reason": "not_found"}


def test_fw_upload_admin_only(fw_relay: RelayServer, tmp_path: Path) -> None:
    status, _, body = upload(fw_relay, b"image", admin=False)
    assert status == 403 and json.loads(body) == {"reason": "admin_required"}
    assert not (tmp_path / "car").exists()


@pytest.mark.parametrize(
    ("kwargs", "status", "reason"),
    [
        ({"version": "1.0 beta"}, 400, "bad_version"),
        ({"version": ""}, 400, "bad_version"),
        ({"chip": "ESP8266"}, 400, "bad_chip_family"),
        ({"data": b""}, 400, "empty_firmware"),
        ({"data": b"x" * (core.MAX_FIRMWARE + 1)}, 413, "firmware_too_large"),
        ({"logger": "nope"}, 404, "unknown_logger"),
    ],
)
def test_fw_upload_rejects(fw_relay: RelayServer, kwargs, status, reason) -> None:
    data = kwargs.pop("data", b"image")
    got = upload(fw_relay, data, **kwargs)
    assert got[0] == status and json.loads(got[2]) == {"reason": reason}


def test_fw_atomic_replace(fw_relay: RelayServer, tmp_path: Path) -> None:
    images = [b"one" * 100, b"two" * 100, b"three" * 100]
    md5s = [hashlib.md5(i).hexdigest() for i in images]  # noqa: S324

    upload(fw_relay, images[0], version="1")
    old_manifest = json.loads(call(f"{fw_relay.url}/car/fw/manifest.json")[2])
    upload(fw_relay, images[1], version="2")

    # A logger holding the old manifest still gets the binary it names.
    old_path = old_manifest["builds"][0]["ota"]["path"]
    assert call(f"{fw_relay.url}/car/fw/{old_path}")[2] == images[0]
    assert json.loads(call(f"{fw_relay.url}/car/fw/manifest.json")[2])["version"] == "2"

    upload(fw_relay, images[2], version="3")
    files = sorted(p.name for p in (tmp_path / "car").iterdir())
    assert files == sorted(["manifest.json", f"firmware-{md5s[1]}.ota.bin", f"firmware-{md5s[2]}.ota.bin"])


def test_fw_absent_404(fw_relay: RelayServer) -> None:
    assert call(f"{fw_relay.url}/car/fw/manifest.json")[0] == 404


@pytest.mark.parametrize("name", ["../manifest.json", "firmware.ota.bin", "firmware-XYZ.ota.bin", ".tmp-abc"])
def test_fw_serves_nothing_but_manifest_and_binaries(fw_relay: RelayServer, name) -> None:
    upload(fw_relay, b"image")
    assert call(f"{fw_relay.url}/car/fw/{name}")[0] == 404


@pytest.mark.parametrize(
    ("remote", "host", "same"),
    [
        ("192.0.2.5", "192.0.2.5", True),
        ("::ffff:192.0.2.5", "192.0.2.5", True),
        ("192.0.2.6", "192.0.2.5", False),
        (None, "192.0.2.5", False),
        ("not-an-ip", "192.0.2.5", False),
    ],
)
def test_same_address(remote, host, same) -> None:
    assert core.same_address(remote, host) is same


# --------------------------------------------------------- R13-R15: shape


def test_core_has_no_ha_import() -> None:
    tree = ast.parse(_CORE.read_text())
    roots = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            roots |= {alias.name.split(".")[0] for alias in node.names}
        elif isinstance(node, ast.ImportFrom) and node.level == 0:
            roots.add(node.module.split(".")[0])
    assert roots - set(sys.stdlib_module_names) == {"aiohttp"}


def test_collect_through_relay(relay: RelayServer, device: FakeDevice, tmp_path: Path) -> None:
    """The real collector, unchanged, through the relay: a clean chunk, a cut one
    that resumes with Range, and the open file's 409."""
    clean = device.add(431)
    cut = device.add(432)
    device.cut(cut, 400)
    device.open_file = device.add(433)
    device.list_open = True
    dest = tmp_path / "archive"

    outcome = sdlog_collect.Collector(f"{relay.url}/car", dest, retries=3, backoff=0).collect()

    assert outcome.device == "mr-orange"
    assert outcome.confirmed == [clean, cut]
    assert outcome.resumed == [cut]
    for name in (clean, cut):
        assert (dest / "mr-orange" / name).read_bytes() == device.chunks[name]
    assert device.confirmed == [clean, cut]
    assert [r.range for r in device.gets(cut)] == [None, f"bytes=0-{HEAD - 1}", f"bytes={400 - SEAM}-"]


def test_hacs_layout() -> None:
    manifest = json.loads((_PKG / "manifest.json").read_text())
    for key in ("domain", "name", "version", "documentation", "issue_tracker", "codeowners"):
        assert manifest.get(key), key
    assert manifest["domain"] == "sdlog_relay"
    assert json.loads((_REPO / "hacs.json").read_text())["name"]
    assert (_PKG / "brand" / "icon.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert json.loads((_PKG / "strings.json").read_text()) == json.loads(
        (_PKG / "translations" / "en.json").read_text()
    )
