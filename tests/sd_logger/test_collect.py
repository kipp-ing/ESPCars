"""script/sdlog_collect.py — the host half of the pull model, against a fake ECU.

The protocol in ``docs/sdlog-collection-design.md`` §5b is four HTTP routes, and
every interesting property of it is a property of *ordering under failure*: what
the collector may do before the bytes are safe, what it must do again after a
crash, and what it must never do twice. None of that is observable by pointing
the collector at a healthy device and watching files appear.

So the valuable half of this file is :class:`FakeDevice` — an in-process
``ThreadingHTTPServer`` that speaks the §5b contract and can be told to misbehave
on demand: drop a connection mid-body, refuse a confirm, 409 its open file, keep
re-serving a chunk it already acknowledged, 500 its own index, ignore a Range,
hand out an index entry with a hole in it. It makes the protocol testable with no
card, no WiFi and no board, in about a millisecond per case, and it records every
request so the *order* of them can be asserted rather than assumed.

The one rule the whole design rests on (§3):

    serve bytes  →  verify locally  →  confirm  →  device renames to .UPL

A confirm is a licence to delete. Confirming before the bytes are on disk and
parse-checked turns a failed transfer into permanent data loss, while the inverse
mistake — re-serving a chunk that was already collected — costs one retry. That
asymmetry is why several cases below assert what did *not* happen.

The second rule, which has the same consequence and is easier to miss: **a
filename is not an identity.** ``seq`` restarts at 0 on a fresh card and
`sd_logger.cpp` rescans it on every remount ("the card may be a different one"),
so ``L0000431.LOG`` on disk and ``L0000431.LOG`` in today's index can be two
unrelated files. Two paths would otherwise trust the name — the dedup skip and a
resumed ``.part`` — and both end in a confirm. The cases under "a filename is not
an identity" are those two, and they are the reason the collector re-reads a
small window off the device before it confirms anything it did not just fetch.

Five cases at the top test the fixture itself. They pass today, on purpose: a
fault-injecting fake that is silently broken would make every case below it
green for the wrong reason.
"""

from __future__ import annotations

import http.client
import importlib.util
import json
import re
import sys
import threading
import urllib.error
import urllib.request
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

import pytest

# script/ is a tool directory, not a package, so both tools are loaded by path.
# They have to land in sys.modules before exec_module: @dataclass resolves the
# defining module out of there and fails on a module that is not registered.
_SCRIPT = Path(__file__).parent.parent.parent / "script"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, _SCRIPT / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sdlog = _load("sdlog")
sdlog_collect = _load("sdlog_collect")

HEAD = sdlog_collect.HEAD_WINDOW
SEAM = sdlog_collect.SEAM_WINDOW


# --------------------------------------------------------------- chunk fixtures

# A sealed chunk is a card file: the same header block and the same record lines
# tests/host/test_log_format.cpp pins the C++ formatter to, plus the `#close` that
# makes it SEALED. The collector must accept nothing less — the whole point of
# verifying before confirming is that a short transfer looks exactly like a file
# that is missing its tail.
HEADER = [
    "#src,C,C1,1,can_gateway,500000",
    "#types,C=can,L=lin,U=user,I=isotp,X=esphome-log,#=meta",
    "#flags,x=extended,r=rtr,t=tx,s=shed,~=truncated,none=absent",
]


def sealed_chunk(
    seq: int,
    *,
    records: int = 20,
    close: bool = True,
    stamp: int | None = None,
    vary_from: int | None = None,
    mark: str = "FF",
    version: int = 2,
) -> bytes:
    """A complete SEALED chunk, ~700 B — big enough that a cut lands mid-body.

    Every chunk anchors its own first stream line and steps from there, which is
    what makes it verifiable **on its own**: the collector fetches chunks one at a
    time, so one that needed its predecessor to decode could never be checked.

    ``stamp`` moves every timestamp, which is what a *different* file with the
    same seq looks like (a swapped card, a reformatted one). ``vary_from`` keeps
    the header and the first records identical and changes the payload from that
    record on, which is the harder case: two files that agree on their first
    bytes and diverge later, exactly where a resume would splice them.
    ``version`` is a parameter because a format this collector cannot read must not
    read as a confirmable chunk — the bytes may be perfect and it still cannot say.
    """
    base = seq * 1000 if stamp is None else stamp
    lines = [f"#sdlog,{version},{seq},@{base:X},2026.7.0", *HEADER]
    for i in range(records):
        data = f"00112233445566{i:02X}"
        if vary_from is not None and i >= vary_from:
            data = f"{mark}112233445566{i:02X}"
        # The first line anchors, the rest step 1 µs — the shape a real chunk has.
        lines.append(f"C1x,{f'@{base:X}' if i == 0 else '1'},1A2,{data}")
    if close:
        lines.append(f"#close,@{base + records:X},rotate")
    return ("\n".join(lines) + "\n").encode("latin-1")


def v1_chunk(seq: int) -> bytes:
    """A chunk in the format this collector no longer reads. Well-formed for what it
    is — which is the point: only the `#sdlog` version says so."""
    base = seq * 1000
    lines = [f"#sdlog,1,{seq},{base},2026.7.0", *HEADER]
    for i in range(20):
        lines.append(f"C,{base + i},seg1,1A2,x,8,00112233445566{i:02X}")
    lines.append(f"#close,{base + 20},rotate")
    return ("\n".join(lines) + "\n").encode("latin-1")


def corrupt_chunk(seq: int) -> bytes:
    """Intact framing, one line the reader rejects: an odd number of hex characters
    in the payload, i.e. half a byte. This is a card that returned garbage, not a
    power cut."""
    good = sealed_chunk(seq).decode("latin-1").split("\n")
    good[len(HEADER) + 2] = "C1x,1,1A2,0011223"
    return "\n".join(good).encode("latin-1")


def unclosed_chunk(seq: int) -> bytes:
    """No `#close`. On a card that is a power cut; arriving over HTTP from a
    device that called it SEALED, it means the bytes are short."""
    return sealed_chunk(seq, close=False)


# ------------------------------------------------------------------ fake device


@dataclass
class Request:
    method: str
    path: str
    range: str | None = None


class _Handler(BaseHTTPRequestHandler):
    """The §5b route table. Deliberately literal — it is the contract."""

    protocol_version = "HTTP/1.1"
    server_version = "sdlog-fake/1"

    def log_message(self, fmt, *args):  # noqa: A002 - a test suite is not a web log
        pass

    @property
    def _dev(self) -> FakeDevice:
        return self.server.fake  # type: ignore[attr-defined]

    def _json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler's spelling
        dev = self._dev
        rng = self.headers.get("Range")
        dev.record(Request("GET", self.path, rng))
        if self.path == "/sdlog/index":
            self._serve_index()
        elif self.path == "/sdlog/status":
            self._json(200, dev.status_payload())
        elif self.path.startswith("/sdlog/f/"):
            self._serve_chunk(unquote(self.path[len("/sdlog/f/") :]), rng)
        else:
            self._json(404, {"error": "no such route"})

    def _serve_index(self) -> None:
        dev = self._dev
        # A 500 here is the most likely thing a heap-bound httpd says (§5a/§9.3),
        # and it is a stumble, not an answer: the collector must ask again.
        if dev.index_faults:
            self._json(dev.index_faults.pop(0), {"error": "busy"})
            return
        if dev.index_raw is not None:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(dev.index_raw)))
            self.end_headers()
            self.wfile.write(dev.index_raw)
            return
        if dev.index_override is not None:
            self._json(200, dev.index_override)
            return
        self._json(200, {"device": dev.device, "chunks": dev.index_entries()})

    def _serve_chunk(self, name: str, rng: str | None) -> None:
        dev = self._dev
        # The open file is never servable — it is the one file the writer holds.
        if name == dev.open_file:
            self._json(409, {"error": "open"})
            return
        data = dev.chunks.get(name)
        if data is None or name in dev.confirmed:
            self._json(404, {"error": "no such chunk"})
            return

        start = 0
        end = len(data) - 1
        honour = rng is not None and not dev.ignore_range
        if honour:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", rng.strip())
            if match is None:
                self._json(400, {"error": f"bad range {rng!r}"})
                return
            start = int(match.group(1))
            if match.group(2):
                end = min(end, int(match.group(2)))
            if start >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(data)}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

        body = data[start : end + 1]
        self.send_response(206 if honour else 200)
        self.send_header("Content-Type", "text/csv")
        if honour:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        # Always the honest length for what is being sent. A cut below writes
        # fewer bytes than this and hangs up, which is what a car driving out of
        # range looks like from the socket: a short body, no error, no status.
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()

        cut = dev.cut_after.get(name)
        if cut is not None and cut < len(body):
            self.wfile.write(body[:cut])
            self.wfile.flush()
            self.close_connection = True
            if name in dev.cut_once:
                dev.cut_once.discard(name)
                dev.cut_after.pop(name, None)
            return
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler's spelling
        dev = self._dev
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        dev.record(Request("POST", self.path, None))
        if not self.path.startswith("/sdlog/done/"):
            self._json(404, {"error": "no such route"})
            return
        name = unquote(self.path[len("/sdlog/done/") :])
        # Fired the instant the confirm lands, before the collector learns the
        # outcome: this is how a test sees what was on local disk at the moment
        # the device was told the chunk was deletable.
        if dev.on_confirm is not None:
            dev.on_confirm(name)
        if name == dev.open_file:
            self._json(409, {"error": "open"})
            return
        status = dev.confirm_status.get(name, 200)
        if status != 200:
            self._json(status, {"error": "refused"})
            return
        if name not in dev.chunks:
            self._json(404, {"error": "no such chunk"})
            return
        if dev.confirm_sticks and name not in dev.confirmed:
            dev.confirmed.append(name)
        self._json(200, {"confirmed": name})


class _Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address) -> None:
        # The collector reads an identity window and hangs up on the rest. That
        # is a broken pipe here and is expected, not a fixture fault.
        if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


class FakeDevice:
    """An ESP32 that serves §5b and can be told to fail in specific ways.

    Faults, each modelling something the bench has actually seen or the design
    explicitly allows:

    ``cut(name, after)``        drop the connection after N body bytes — WiFi
                                out of range mid-chunk.
    ``open_file`` / ``list_open``  the writer's file. Never listed normally; with
                                ``list_open`` the index is stale and names it,
                                which is the only way a collector meets the 409.
    ``confirm_status[name]``    the confirm fails after the bytes arrived.
    ``confirm_sticks = False``  the confirm is answered 200 but the rename does
                                not survive (power cut on the ECU), so the chunk
                                is served again next run.
    ``index_faults = [500]``    the index answers a status before it answers the
                                index — a busy httpd, per §5a's ~10 KB budget.
    ``index_override``          serve this payload verbatim: the only way to test
                                an entry the C++ spelled wrong.
    ``index_raw``               serve these bytes as the index, object or not.
    ``ignore_range = True``     answer 200 with the whole file however the client
                                asked. `Range` support is in §5b, but an early
                                httpd that ignores it must not corrupt anything.
    ``report_bytes = False``    the index omits the length, so nothing but the
                                bytes themselves can prove a chunk's identity.
    """

    def __init__(self, name: str = "mr-orange") -> None:
        self.device = name
        self.chunks: dict[str, bytes] = {}
        self.confirmed: list[str] = []
        self.open_file: str | None = None
        self.list_open = False
        self.cut_after: dict[str, int] = {}
        self.cut_once: set[str] = set()
        self.confirm_status: dict[str, int] = {}
        self.confirm_sticks = True
        self.on_confirm = None
        self.index_faults: list[int] = []
        self.index_override: dict | None = None
        self.index_raw: bytes | None = None
        self.ignore_range = False
        self.report_bytes = True
        self.requests: list[Request] = []
        self.url = ""
        self._lock = threading.Lock()
        self._server: _Server | None = None

    # -- content

    def add(self, seq: int, data: bytes | None = None) -> str:
        name = f"L{seq:07d}.LOG"
        self.chunks[name] = sealed_chunk(seq) if data is None else data
        return name

    def cut(self, name: str, after: int, *, once: bool = True) -> None:
        self.cut_after[name] = after
        if once:
            self.cut_once.add(name)

    def sealed(self) -> list[str]:
        """Insertion order, deliberately not sorted: the collector owns the
        oldest-first ordering, so the device must not do it for free."""
        names = [n for n in self.chunks if n not in self.confirmed]
        if self.open_file is not None and not self.list_open:
            names = [n for n in names if n != self.open_file]
        return names

    def index_entries(self) -> list[dict]:
        entries = []
        for name in self.sealed():
            data = self.chunks[name]
            seq = int(name[1:8])
            entry = {
                "name": name,
                "seq": seq,
                "first_t_us": seq * 1000,
                "last_t_us": seq * 1000 + 20,
            }
            if self.report_bytes:
                entry["bytes"] = len(data)
            entries.append(entry)
        return entries

    def status_payload(self) -> dict:
        return {
            "device": self.device,
            "sealed": len(self.sealed()),
            "confirmed": len(self.confirmed),
            "card_percent": 41,
            "discarded_chunks": 0,
            "discarded_bytes": 0,
        }

    # -- the request log

    def record(self, request: Request) -> None:
        with self._lock:
            self.requests.append(request)

    def gets(self, name: str | None = None) -> list[Request]:
        want = None if name is None else f"/sdlog/f/{name}"
        return [r for r in self.requests if r.method == "GET" and (want is None or r.path == want)]

    def bodies(self, name: str) -> list[Request]:
        """GETs that asked for the chunk itself rather than an identity window.
        The window is ~256 B; a chunk is megabytes, and it is the chunk that
        must not cross the WiFi twice."""
        window = f"bytes=0-{HEAD - 1}"
        return [r for r in self.gets(name) if r.range != window]

    def confirms(self, name: str | None = None) -> list[Request]:
        want = None if name is None else f"/sdlog/done/{name}"
        return [r for r in self.requests if r.method == "POST" and (want is None or r.path == want)]

    # -- lifecycle

    def start(self) -> FakeDevice:
        server = _Server(("127.0.0.1", 0), _Handler)
        server.fake = self  # type: ignore[attr-defined]
        self._server = server
        self.url = f"http://127.0.0.1:{server.server_address[1]}"
        # A short poll interval only because `shutdown()` waits for one, and a
        # device is torn down per case: at the 0.5 s default this file alone
        # would cost half a minute, and even 10 ms is most of its runtime.
        loop = threading.Thread(
            target=server.serve_forever, kwargs={"poll_interval": 0.002}, daemon=True
        )
        loop.start()
        return self

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._server = None


@pytest.fixture
def devices():
    """Factory: more than one device is needed to prove (device, seq) dedup keys
    on the device half too."""
    made: list[FakeDevice] = []

    def factory(name: str = "mr-orange") -> FakeDevice:
        device = FakeDevice(name).start()
        made.append(device)
        return device

    yield factory
    for device in made:
        device.stop()


@pytest.fixture
def device(devices) -> FakeDevice:
    return devices()


@pytest.fixture
def dest(tmp_path: Path) -> Path:
    return tmp_path / "archive"


def collector(fake: FakeDevice, dest: Path, **kwargs):
    """Backoff defaults to zero so the suite stays inside its one-second budget;
    the cases that care about backoff pass a recording sleeper instead. The first
    parameter is not named ``device`` because ``Collector`` has a ``device``
    keyword of its own."""
    kwargs.setdefault("retries", 3)
    kwargs.setdefault("backoff", 0)
    return sdlog_collect.Collector(fake.url, dest, **kwargs)


def stored(dest: Path, device: str) -> list[str]:
    directory = dest / device
    if not directory.is_dir():
        return []
    return sorted(p.name for p in directory.iterdir())


def http_get(url: str, headers: dict | None = None):
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=5)


# ------------------------------------------------- the fixture proves itself first


def test_the_fake_device_serves_the_index_contract(device: FakeDevice) -> None:
    """§5b: name, seq, bytes, first/last t_us — and CONFIRMED chunks drop out of
    the listing, which is what makes a re-served chunk distinguishable from a
    chunk the device forgot about."""
    name = device.add(431)
    device.add(432)
    payload = json.loads(http_get(f"{device.url}/sdlog/index").read())
    assert payload["device"] == "mr-orange"
    assert [c["name"] for c in payload["chunks"]] == [name, "L0000432.LOG"]
    assert payload["chunks"][0]["seq"] == 431
    assert payload["chunks"][0]["bytes"] == len(device.chunks[name])

    device.confirmed.append(name)
    payload = json.loads(http_get(f"{device.url}/sdlog/index").read())
    assert [c["name"] for c in payload["chunks"]] == ["L0000432.LOG"]


def test_the_fake_device_can_stumble_on_its_own_index(device: FakeDevice) -> None:
    """§5a puts the httpd on ~10 KB of heap on a chip §9.3 calls heap-bound, so
    "500, ask again" is a thing this device really does say."""
    device.add(431)
    device.index_faults = [500]
    with pytest.raises(urllib.error.HTTPError) as caught:
        http_get(f"{device.url}/sdlog/index")
    assert caught.value.code == 500
    assert json.loads(http_get(f"{device.url}/sdlog/index").read())["chunks"]  # recovered


def test_the_fake_device_serves_a_range_as_206_and_cuts_on_demand(device: FakeDevice) -> None:
    """The three behaviours every resume case below leans on: an open-ended
    range, a bounded one (the identity window), and a cut."""
    name = device.add(431)
    data = device.chunks[name]

    tail = http_get(f"{device.url}/sdlog/f/{name}", {"Range": f"bytes={len(data) - 10}-"})
    assert tail.status == 206
    assert tail.headers["Content-Range"] == f"bytes {len(data) - 10}-{len(data) - 1}/{len(data)}"
    assert tail.read() == data[-10:]

    head = http_get(f"{device.url}/sdlog/f/{name}", {"Range": "bytes=0-255"})
    assert head.status == 206
    assert int(head.headers["Content-Length"]) == 256
    assert head.read() == data[:256]

    device.cut(name, 400)
    response = http_get(f"{device.url}/sdlog/f/{name}")
    assert int(response.headers["Content-Length"]) == len(data)  # the promise
    # What a car driving out of range looks like from the socket: the body stops
    # short of the promised length and the connection ends. There is no status
    # code for it, which is why the collector has to count bytes itself.
    with pytest.raises(http.client.IncompleteRead) as caught:
        response.read()
    assert caught.value.partial == data[:400]
    assert http_get(f"{device.url}/sdlog/f/{name}").read() == data  # heals after one


def test_the_fake_device_can_ignore_a_range_entirely(device: FakeDevice) -> None:
    """An httpd that does not implement §5b's Range yet. It must cost efficiency,
    never correctness — so the collector has to notice the missing 206."""
    name = device.add(431)
    device.ignore_range = True
    response = http_get(f"{device.url}/sdlog/f/{name}", {"Range": "bytes=400-"})
    assert response.status == 200
    assert response.read() == device.chunks[name]


def test_the_fake_device_never_lists_the_open_file_and_409s_it(device: FakeDevice) -> None:
    """OPEN is not servable and not listed (§3). ``list_open`` is the stale-index
    race, which is the only way a well-behaved collector ever sees the 409."""
    device.add(431)
    open_name = device.add(432)
    device.open_file = open_name

    payload = json.loads(http_get(f"{device.url}/sdlog/index").read())
    assert [c["name"] for c in payload["chunks"]] == ["L0000431.LOG"]

    with pytest.raises(urllib.error.HTTPError) as caught:
        http_get(f"{device.url}/sdlog/f/{open_name}")
    assert caught.value.code == 409

    device.list_open = True
    payload = json.loads(http_get(f"{device.url}/sdlog/index").read())
    assert [c["name"] for c in payload["chunks"]] == ["L0000431.LOG", open_name]


def test_the_fake_device_confirms_idempotently_and_can_refuse(device: FakeDevice) -> None:
    """The confirm route and its two failure knobs. Without this, a broken
    ``do_POST`` would surface as a puzzling collector failure five cases below."""
    name = device.add(431)
    seen: list[str] = []
    device.on_confirm = seen.append

    def confirm(target: str):
        url = f"{device.url}/sdlog/done/{target}"
        request = urllib.request.Request(url, data=b"", method="POST")
        return urllib.request.urlopen(request, timeout=5)

    assert confirm(name).status == 200
    assert seen == [name]
    assert device.confirmed == [name]
    assert device.index_entries() == []  # CONFIRMED chunks stop being listed
    with pytest.raises(urllib.error.HTTPError) as caught:
        http_get(f"{device.url}/sdlog/f/{name}")
    assert caught.value.code == 404
    assert confirm(name).status == 200  # idempotent

    # A confirm that fails after the bytes arrived.
    refused = device.add(432)
    device.confirm_status[refused] = 500
    with pytest.raises(urllib.error.HTTPError) as caught:
        confirm(refused)
    assert caught.value.code == 500
    assert refused in [c["name"] for c in device.index_entries()]

    # A confirm the ECU answered and then lost to a power cut: 200, no rename.
    device.confirm_status.clear()
    device.confirm_sticks = False
    assert confirm(refused).status == 200
    assert refused in [c["name"] for c in device.index_entries()]


# ----------------------------------------------------------------- the happy path


def test_every_sealed_chunk_is_stored_verified_and_confirmed(
    device: FakeDevice, dest: Path
) -> None:
    names = [device.add(seq) for seq in (431, 432, 433)]

    outcome = collector(device, dest).collect()

    assert outcome.device == "mr-orange"
    assert outcome.confirmed == names
    assert outcome.failed == []
    assert stored(dest, "mr-orange") == sorted(names)
    for name in names:
        assert (dest / "mr-orange" / name).read_bytes() == device.chunks[name]
    assert outcome.bytes_fetched == sum(len(device.chunks[n]) for n in names)
    # Nothing but the chunks themselves: a clean run pays for no identity window.
    assert outcome.bytes_wire == outcome.bytes_fetched
    # The device saw the confirms, so the chunks are now CONFIRMED and gone from
    # the index — a second run has nothing to do.
    assert device.confirmed == names
    assert device.index_entries() == []


def test_no_partial_file_survives_a_successful_run(device: FakeDevice, dest: Path) -> None:
    """A partial never wears the final name, so anything without ``.part`` is
    complete — and nothing keeps ``.part`` once the chunk landed."""
    device.add(431)
    collector(device, dest).collect()
    assert stored(dest, "mr-orange") == ["L0000431.LOG"]


def test_an_empty_index_is_a_quiet_no_op(device: FakeDevice, dest: Path) -> None:
    """Most runs against a parked car find nothing. That is success."""
    outcome = collector(device, dest).collect()
    assert outcome.confirmed == []
    assert outcome.failed == []
    assert device.confirms() == []


def test_chunks_are_collected_oldest_first(device: FakeDevice, dest: Path) -> None:
    """Retention drops the oldest un-collected chunk when the card fills (§7), so
    the oldest is the one racing deletion. The device lists in whatever order it
    walked the directory; the ordering is the collector's job."""
    for seq in (433, 431, 432):
        device.add(seq)

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == ["L0000431.LOG", "L0000432.LOG", "L0000433.LOG"]


# ------------------------------------------------------------------- the ordering


def test_a_confirm_never_precedes_the_verified_bytes_on_disk(
    device: FakeDevice, dest: Path
) -> None:
    """THE rule (§3). A confirm is a licence to delete, so at the instant it
    lands the complete chunk must already be on local disk under its final name.
    Observed from inside the device's POST handler, which is the only vantage
    point that can see 'before'."""
    names = [device.add(seq) for seq in (431, 432)]
    at_confirm: dict[str, bytes | None] = {}

    def snapshot(name: str) -> None:
        path = dest / "mr-orange" / name
        at_confirm[name] = path.read_bytes() if path.is_file() else None

    device.on_confirm = snapshot

    collector(device, dest).collect()

    assert at_confirm == {name: device.chunks[name] for name in names}


def test_the_wire_order_is_serve_then_confirm(device: FakeDevice, dest: Path) -> None:
    name = device.add(431)
    collector(device, dest).collect()
    kinds = [(r.method, r.path) for r in device.requests]
    assert kinds.index(("GET", f"/sdlog/f/{name}")) < kinds.index(("POST", f"/sdlog/done/{name}"))


# ------------------------------------------------------------- drops and resumes


def test_a_cut_transfer_resumes_at_the_byte_offset(device: FakeDevice, dest: Path) -> None:
    """A dropped connection resumes with ``Range``, it does not restart. The
    device promised a Content-Length the body did not reach — nothing else says
    the transfer failed, since a short body is not an HTTP error.

    The resume is three requests, not two, and both extra reads are identity: the
    head window proves the partial belongs to the chunk being offered, and the
    ``SEAM`` bytes before the offset prove the join is a join and not a splice.
    Those windows are the only bytes paid for twice."""
    name = device.add(431)
    data = device.chunks[name]
    device.cut(name, 400)

    outcome = collector(device, dest).collect()

    assert [r.range for r in device.gets(name)] == [
        None,
        f"bytes=0-{HEAD - 1}",
        f"bytes={400 - SEAM}-",
    ]
    assert (dest / "mr-orange" / name).read_bytes() == data
    assert outcome.resumed == [name]
    assert outcome.confirmed == [name]
    # Every byte of the chunk was written exactly once...
    assert outcome.bytes_fetched == len(data)
    # ...and what it cost is the cut attempt plus the two identity windows.
    assert outcome.bytes_wire == 400 + HEAD + (len(data) - (400 - SEAM))


def test_a_partial_waits_on_disk_and_resumes_on_the_next_run(
    device: FakeDevice, dest: Path
) -> None:
    """The car drove off mid-chunk and the run ended. What survives is a
    ``.part`` file, and the next run continues it instead of paying for those
    bytes again — with ``retries=1`` there is no in-run retry to hide behind."""
    name = device.add(431)
    data = device.chunks[name]
    device.cut(name, 400)

    first = collector(device, dest, retries=1).collect()

    assert first.confirmed == []
    assert [n for n, _ in first.failed] == [name]
    assert stored(dest, "mr-orange") == [name + ".part"]
    assert (dest / "mr-orange" / (name + ".part")).read_bytes() == data[:400]
    assert device.confirms(name) == []

    second = collector(device, dest, retries=1).collect()

    assert [r.range for r in device.gets(name)] == [
        None,
        f"bytes=0-{HEAD - 1}",
        f"bytes={400 - SEAM}-",
    ]
    assert second.resumed == [name]
    assert second.confirmed == [name]
    assert (dest / "mr-orange" / name).read_bytes() == data
    assert stored(dest, "mr-orange") == [name]


def test_retries_are_bounded_and_back_off(device: FakeDevice, dest: Path) -> None:
    """A device that never delivers a byte must not be hammered and must not spin
    forever: ``retries`` is attempts per chunk per run, so there are that many
    GETs and one fewer sleeps, each longer than the last."""
    name = device.add(431)
    device.cut(name, 0, once=False)
    slept: list[float] = []

    outcome = collector(device, dest, retries=3, backoff=0.25, sleeper=slept.append).collect()

    assert len(device.gets(name)) == 3
    assert len(slept) == 2
    assert slept[0] == pytest.approx(0.25)
    assert slept[1] > slept[0]
    assert [n for n, _ in outcome.failed] == [name]
    assert outcome.confirmed == []
    assert device.confirms(name) == []


def test_a_device_that_vanished_is_reported_not_raised(device: FakeDevice, dest: Path) -> None:
    """The normal case in a car, per the design: the collector reports and exits,
    it does not traceback."""
    pull = collector(device, dest)
    device.stop()

    outcome = pull.collect()

    assert outcome.failed != []
    assert outcome.confirmed == []


def test_a_device_that_ignores_range_restarts_instead_of_splicing(
    device: FakeDevice, dest: Path
) -> None:
    """§5b says Range is supported; an httpd that has not got there yet answers
    200 with the whole file. Appending that to a partial would corrupt it
    silently, so the missing 206 means restart — and the restart must not
    truncate a partial before a byte has actually arrived, or a device that is
    both range-blind and flaky would erase what the last run collected."""
    name = device.add(431)
    data = device.chunks[name]
    device.cut(name, 400)
    collector(device, dest, retries=1).collect()
    part = dest / "mr-orange" / (name + ".part")
    assert part.read_bytes() == data[:400]

    device.ignore_range = True
    device.cut(name, 0, once=False)
    failed = collector(device, dest, retries=1).collect()

    assert [n for n, _ in failed.failed] == [name]
    assert part.read_bytes() == data[:400]  # not truncated, not emptied

    device.cut_after.clear()
    outcome = collector(device, dest, retries=1).collect()

    assert (dest / "mr-orange" / name).read_bytes() == data
    assert outcome.resumed == []  # a restart is not a resume
    assert outcome.confirmed == [name]
    assert stored(dest, "mr-orange") == [name]


# ---------------------------------------------------------------- the open file


def test_a_409_on_the_open_file_does_not_stop_the_run(device: FakeDevice, dest: Path) -> None:
    """A stale index can name the file the writer holds. That chunk is not
    collectable — but it is also not a reason to abandon the ones that are, and
    it must never be confirmed, because confirming it makes the *live* file
    deletable."""
    device.add(431)
    open_name = device.add(432)
    device.add(433)
    device.open_file = open_name
    device.list_open = True

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == ["L0000431.LOG", "L0000433.LOG"]
    assert [n for n, _ in outcome.failed] == [open_name]
    assert device.confirms(open_name) == []
    assert stored(dest, "mr-orange") == ["L0000431.LOG", "L0000433.LOG"]


# ------------------------------------------------------------------------- dedup


def test_a_re_served_chunk_is_neither_stored_nor_fetched_twice(
    device: FakeDevice, dest: Path
) -> None:
    """The ECU answered the confirm and then lost power before the rename stuck,
    so the chunk comes back in the index. Dedup on (device, seq) means the bytes
    do not cross the WiFi a second time — that saving is the point of the key —
    and the archive keeps exactly one copy.

    The second run does spend one ~256 B window re-proving that the file it
    already has is the file being offered, because what follows the skip is a
    confirm; see the two cases below for what that window catches."""
    name = device.add(431)
    device.confirm_sticks = False

    first = collector(device, dest).collect()
    second = collector(device, dest).collect()

    assert first.confirmed == [name]
    assert second.skipped == [name]
    assert second.fetched == []
    assert [r.range for r in device.gets(name)] == [None, f"bytes=0-{HEAD - 1}"]
    assert device.bodies(name) == device.gets(name)[:1]
    assert second.bytes_fetched == 0
    assert second.bytes_wire == HEAD
    assert stored(dest, "mr-orange") == [name]
    assert (dest / "mr-orange" / name).read_bytes() == device.chunks[name]
    # Idempotent: the device is told again, because as far as it knows the chunk
    # is still un-collected.
    assert len(device.confirms(name)) == 2


def test_two_devices_may_use_the_same_seq(devices, dest: Path) -> None:
    """``seq`` is monotonic per *card*, so it only identifies a chunk together
    with the device. Two cards both at 431 are two different chunks."""
    orange = devices("mr-orange")
    green = devices("mr-green")
    orange.add(431)
    green.add(431, sealed_chunk(431, records=7))

    collector(orange, dest).collect()
    collector(green, dest).collect()

    assert stored(dest, "mr-orange") == ["L0000431.LOG"]
    assert stored(dest, "mr-green") == ["L0000431.LOG"]
    assert (dest / "mr-orange" / "L0000431.LOG").read_bytes() != (
        dest / "mr-green" / "L0000431.LOG"
    ).read_bytes()


def test_the_archive_is_device_then_chunk_name(device: FakeDevice, dest: Path) -> None:
    """The device name comes off the index unless it was given explicitly, and it
    is half the dedup key — so it has to be stable, not derived from whichever
    IP the car got today."""
    name = device.add(431)
    pull = collector(device, dest)
    pull.collect()

    assert pull.archive_dir() == dest / "mr-orange"
    assert pull.archive_path(name) == dest / "mr-orange" / name
    assert pull.partial_path(name) == dest / "mr-orange" / (name + ".part")


def test_an_explicit_device_name_overrides_the_index(device: FakeDevice, dest: Path) -> None:
    device.add(431)
    outcome = collector(device, dest, device="orange-spare").collect()
    assert outcome.device == "orange-spare"
    assert stored(dest, "orange-spare") == ["L0000431.LOG"]


# ------------------------------------------------ a filename is not an identity

# `seq` restarts at 0 on a fresh card (`scan_next_seq_()`) and sd_logger.cpp
# rescans it on every remount, commented "the card may be a different one". So
# the same name can name different bytes, and every path that ends in a confirm
# has to answer for the bytes rather than for the name.


def test_an_archived_chunk_from_another_card_is_never_confirmed(
    device: FakeDevice, dest: Path
) -> None:
    """The reported failure: the archive holds a short L0000001.LOG from an old
    card, the device offers a different, longer L0000001.LOG that was never
    collected. Confirming on the strength of the filename would hand the device
    permission to delete 7 KB nobody has a copy of."""
    name = device.add(1, sealed_chunk(1, records=60, stamp=99_000_000))
    old = sealed_chunk(1, records=4)
    (dest / "mr-orange").mkdir(parents=True)
    (dest / "mr-orange" / name).write_bytes(old)

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == []
    assert outcome.skipped == []
    assert device.confirms(name) == []
    assert [n for n, _ in outcome.failed] == [name]
    # The report names both lengths, which is what tells an operator it is a card
    # swap rather than a transfer that went wrong.
    assert f"{len(old)} B" in outcome.failed[0][1]
    assert f"{len(device.chunks[name])} B" in outcome.failed[0][1]
    # Neither copy was destroyed to make the run tidy.
    assert (dest / "mr-orange" / name).read_bytes() == old
    assert device.chunks[name] != old


def test_an_archived_chunk_of_the_same_length_is_still_checked(
    device: FakeDevice, dest: Path
) -> None:
    """Rotation is size-bound, so two chunks off two cards are *likely* to share
    a length — a length check alone is not identity. The head window is read off
    the device and compared with the copy on disk."""
    name = device.add(431, sealed_chunk(431, mark="AA"))
    other = sealed_chunk(431, vary_from=0, mark="BB")
    assert len(other) == len(device.chunks[name])  # the case only bites if equal
    assert other[:HEAD] != device.chunks[name][:HEAD]  # ...and the head differs
    (dest / "mr-orange").mkdir(parents=True)
    (dest / "mr-orange" / name).write_bytes(other)

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == []
    assert device.confirms(name) == []
    assert [n for n, _ in outcome.failed] == [name]
    assert (dest / "mr-orange" / name).read_bytes() == other


def test_a_stale_file_under_the_final_name_is_never_confirmed(
    device: FakeDevice, dest: Path
) -> None:
    """A zero-byte file, or a truncated one, dropped under the final name by a
    crash or a half-finished copy. The name says collected; the bytes say
    nothing was. Only the bytes may be believed, with or without a length in the
    index."""
    name = device.add(431)
    (dest / "mr-orange").mkdir(parents=True)
    (dest / "mr-orange" / name).write_bytes(b"")
    device.report_bytes = False  # not even a length to check against

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == []
    assert device.confirms(name) == []
    assert [n for n, _ in outcome.failed] == [name]


def test_a_resume_never_splices_a_partial_onto_another_file(
    device: FakeDevice, dest: Path
) -> None:
    """Run 1 leaves 400 bytes of one file; by run 2 the card was swapped and the
    same name holds different bytes. A byte-offset resume would produce a hybrid
    that lands on exactly the promised length, parses clean, verifies clean and
    is neither file — the one corruption no later check can catch. The partial is
    dropped instead and the chunk fetched whole."""
    name = device.add(431)
    first_card = device.chunks[name]
    device.cut(name, 400)
    collector(device, dest, retries=1).collect()
    assert (dest / "mr-orange" / (name + ".part")).read_bytes() == first_card[:400]

    second_card = sealed_chunk(431, records=45, stamp=7_000_000)
    device.chunks[name] = second_card

    outcome = collector(device, dest).collect()

    got = (dest / "mr-orange" / name).read_bytes()
    assert got == second_card
    assert got != first_card
    assert got != first_card[:400] + second_card[400:]  # the hybrid, spelled out
    assert outcome.restarted == [name]
    assert outcome.resumed == []
    assert outcome.confirmed == [name]
    assert stored(dest, "mr-orange") == [name]


def test_a_resume_checks_the_bytes_under_the_seam_not_just_the_head(
    device: FakeDevice, dest: Path
) -> None:
    """The harder version: two files that agree on their header and first records
    and diverge later — one card reformatted and restarted at the same seq within
    the same second. The head window says yes and the join is still a lie, so the
    bytes immediately before the resume offset are compared too."""
    name = device.add(431, sealed_chunk(431, records=40))
    first_card = device.chunks[name]
    device.cut(name, 400)
    collector(device, dest, retries=1).collect()
    assert (dest / "mr-orange" / (name + ".part")).read_bytes() == first_card[:400]

    # Same header, same first three records, different from there on: the head
    # window cannot tell these apart, and the seam window must.
    second_card = sealed_chunk(431, records=40, vary_from=3, mark="EE")
    assert second_card[:HEAD] == first_card[:HEAD]
    assert second_card[400 - SEAM : 400] != first_card[400 - SEAM : 400]
    device.chunks[name] = second_card

    outcome = collector(device, dest).collect()

    got = (dest / "mr-orange" / name).read_bytes()
    assert got == second_card
    assert got != first_card[:400] + second_card[400:]  # the hybrid, spelled out
    assert outcome.restarted == [name]
    assert outcome.confirmed == [name]


def test_a_cut_during_the_identity_window_keeps_the_partial(
    device: FakeDevice, dest: Path
) -> None:
    """The identity check is a network request like any other, and the car can
    drive away during it. A window that never arrived says nothing about the
    bytes on disk — treating it as a verdict would let one WiFi drop delete a
    good partial, which is the failure the check exists to prevent."""
    name = device.add(431)
    data = device.chunks[name]
    device.cut(name, 400)
    collector(device, dest, retries=1).collect()
    part = dest / "mr-orange" / (name + ".part")
    assert part.read_bytes() == data[:400]

    device.cut(name, 0, once=False)  # every request now dies before a byte
    outcome = collector(device, dest, retries=2).collect()

    assert [n for n, _ in outcome.failed] == [name]
    assert part.read_bytes() == data[:400]  # still there, still the same bytes
    assert device.confirms(name) == []

    device.cut_after.clear()
    healed = collector(device, dest, retries=1).collect()

    assert healed.resumed == [name]
    assert (dest / "mr-orange" / name).read_bytes() == data


def test_a_partial_longer_than_the_chunk_offered_is_dropped(
    device: FakeDevice, dest: Path
) -> None:
    """The index says nothing about length, the head matches, and the partial is
    still not a prefix of what is on the card — it is longer than the whole
    chunk. The device answers 416 to the resume, which is the same finding by
    another route, and the partial goes."""
    # Enough records that the 256 B head window lands entirely inside the body the
    # two chunks share — v2 lines are ~27 B, so three of them no longer reach it.
    name = device.add(431, sealed_chunk(431, records=8))
    small = device.chunks[name]
    big = sealed_chunk(431, records=40)
    assert big[:HEAD] == small[:HEAD]  # the head window cannot tell them apart
    assert len(big) > len(small) + SEAM  # ...so the resume asks past the end
    device.report_bytes = False
    (dest / "mr-orange").mkdir(parents=True)
    (dest / "mr-orange" / (name + ".part")).write_bytes(big)

    outcome = collector(device, dest).collect()

    assert (dest / "mr-orange" / name).read_bytes() == small
    assert outcome.restarted == [name]
    assert outcome.confirmed == [name]
    assert stored(dest, "mr-orange") == [name]


def test_a_complete_partial_is_renamed_without_refetching_the_chunk(
    device: FakeDevice, dest: Path
) -> None:
    """The crash window at the other end: every byte arrived and the process died
    before the rename. The bytes are already here — but they are still proved
    against the device before the confirm, and they are not paid for twice."""
    name = device.add(431)
    data = device.chunks[name]
    (dest / "mr-orange").mkdir(parents=True)
    (dest / "mr-orange" / (name + ".part")).write_bytes(data)

    outcome = collector(device, dest).collect()

    assert (dest / "mr-orange" / name).read_bytes() == data
    assert outcome.confirmed == [name]
    assert outcome.bytes_fetched == 0  # nothing was written
    assert outcome.bytes_wire == HEAD  # only the identity window crossed the air
    assert device.bodies(name) == []


# ------------------------------------------------------- verification before confirm


def test_a_corrupt_chunk_is_never_confirmed(device: FakeDevice, dest: Path) -> None:
    """A confirm on bytes that do not parse hands the device permission to delete
    the only good copy. The reader's verdict is the gate — and the bad bytes are
    deleted, not left as a ``.part``: a partial that survives is resumed *past*
    on the next run, so keeping it is how a corrupt chunk never heals."""
    name = device.add(431, corrupt_chunk(431))

    outcome = collector(device, dest).collect()

    assert device.confirms(name) == []
    assert outcome.confirmed == []
    assert [n for n, _ in outcome.failed] == [name]
    assert stored(dest, "mr-orange") == []


def test_a_delta_coded_chunk_verifies_on_its_own(tmp_path: Path) -> None:
    """The collector fetches chunks one at a time, so a chunk that needed its
    predecessor to decode could never be verified at all. Every chunk anchoring its
    own first stream line is what makes that work."""
    path = tmp_path / "L0000431.LOG"
    path.write_bytes(sealed_chunk(431))
    assert sdlog_collect.verify_chunk(path) is None


def test_a_chunk_in_a_format_this_collector_cannot_read_is_never_confirmed(
    device: FakeDevice, dest: Path
) -> None:
    """The bytes may be perfect; this side simply cannot say so. Refusing keeps the
    chunk on the device for a collector that can read it, which is the only outcome
    that does not lose it — a confirm would let retention delete a file nothing has
    ever read."""
    name = device.add(431, sealed_chunk(431, version=99))

    outcome = collector(device, dest).collect()

    assert device.confirms(name) == []
    assert [n for n, _ in outcome.failed] == [name]
    assert "v99" in outcome.failed[0][1]
    assert stored(dest, "mr-orange") == []


def test_a_chunk_without_a_close_line_is_treated_as_short(device: FakeDevice, dest: Path) -> None:
    """A SEALED chunk always carries `#close` (§3). Arriving without one, it is a
    transfer that ended early — not a power cut, which is only a card's excuse."""
    name = device.add(431, unclosed_chunk(431))

    outcome = collector(device, dest).collect()

    assert device.confirms(name) == []
    assert [n for n, _ in outcome.failed] == [name]
    assert stored(dest, "mr-orange") == []


def test_a_failed_confirm_leaves_the_chunk_for_the_next_run(
    device: FakeDevice, dest: Path
) -> None:
    """The bytes are safe, the acknowledgement is not. The chunk must come back —
    the inverse mistake, treating a confirm we could not send as sent, is how a
    chunk gets deleted on the device having never been acknowledged."""
    name = device.add(431)
    device.confirm_status[name] = 500

    first = collector(device, dest).collect()

    assert first.confirmed == []
    assert [n for n, _ in first.failed] == [name]
    assert (dest / "mr-orange" / name).read_bytes() == device.chunks[name]
    assert name in [c["name"] for c in device.index_entries()]

    device.confirm_status.clear()
    second = collector(device, dest).collect()

    assert second.confirmed == [name]
    assert device.confirmed == [name]
    assert stored(dest, "mr-orange") == [name]


def test_a_confirm_that_stumbles_is_retried_and_a_refusal_is_not(
    device: FakeDevice, dest: Path
) -> None:
    """A 5xx is the heap-bound httpd losing a race and is worth asking again —
    the confirm is idempotent, so a duplicate costs nothing. A 4xx is an answer:
    asking again just delays the report."""
    busy = device.add(431)
    device.confirm_status[busy] = 500
    slept: list[float] = []

    outcome = collector(device, dest, retries=3, backoff=0.1, sleeper=slept.append).collect()

    assert len(device.confirms(busy)) == 3
    assert [n for n, _ in outcome.failed] == [busy]

    device.confirm_status[busy] = 403
    again = collector(device, dest, retries=3).collect()

    assert len(device.confirms(busy)) == 4  # one attempt, not three
    assert "403" in again.failed[0][1]


@pytest.mark.parametrize(
    ("data", "defective"),
    [
        (sealed_chunk(431), False),
        (corrupt_chunk(431), True),
        (unclosed_chunk(431), True),
        (sealed_chunk(431)[:300], True),
        (sealed_chunk(431, version=99), True),
        (v1_chunk(431), True),
        (b"", True),
    ],
    ids=["intact", "malformed-line", "no-close", "truncated", "unknown-version", "v1", "empty"],
)
def test_verify_chunk_reuses_the_reader_s_verdict(
    tmp_path: Path, data: bytes, defective: bool
) -> None:
    """Verification is ``sdlog.py``'s parser, not a second parser that might
    accept what the real reader rejects. ``None`` means intact; anything else is
    a reason a human can read."""
    path = tmp_path / "L0000431.LOG"
    path.write_bytes(data)
    reason = sdlog_collect.verify_chunk(path)
    assert (reason is not None) == defective
    if defective:
        assert isinstance(reason, str) and reason


# --------------------------------------------------------------- what the index says


def test_a_busy_index_is_retried_not_abandoned(device: FakeDevice, dest: Path) -> None:
    """§5a gives the httpd ~10 KB on a chip §9.3 calls heap-bound, so a transient
    500 is the likeliest answer of all. Abandoning the run on it would report
    'the car is not on the WiFi' about a car that is."""
    name = device.add(431)
    device.index_faults = [500, 503]

    pull = collector(device, dest, retries=3)
    outcome = pull.collect()

    assert pull.unreachable is False
    assert outcome.confirmed == [name]
    assert len([r for r in device.requests if r.path == "/sdlog/index"]) == 3


def test_an_index_that_keeps_stumbling_is_reported_as_unreachable(
    device: FakeDevice, dest: Path
) -> None:
    device.add(431)
    device.index_faults = [500] * 10

    pull = collector(device, dest, retries=3)
    outcome = pull.collect()

    assert pull.unreachable is True
    assert [n for n, _ in outcome.failed] == ["<index>"]
    assert "500" in outcome.failed[0][1]
    assert len([r for r in device.requests if r.path == "/sdlog/index"]) == 3
    assert device.confirms() == []


def test_an_index_that_answers_4xx_is_not_retried(device: FakeDevice, dest: Path) -> None:
    """A 404 means this device has no such route — a different firmware, a wrong
    port. Asking three times only makes the operator wait for the same answer."""
    device.add(431)
    device.index_faults = [404] * 10

    pull = collector(device, dest, retries=3)
    outcome = pull.collect()

    assert pull.unreachable is True
    assert len([r for r in device.requests if r.path == "/sdlog/index"]) == 1
    assert "404" in outcome.failed[0][1]


def test_a_broken_index_entry_costs_one_chunk_not_the_run(
    device: FakeDevice, dest: Path
) -> None:
    """The wire keys are the other half of a contract with the device's httpd. If
    the C++ spells one differently or emits a null, the operator must get a
    report and the other chunks, not a traceback."""
    good = device.add(431)
    nulls = device.add(432)
    device.index_override = {
        "device": "mr-orange",
        "chunks": [
            {"seq": 1, "bytes": 10},  # no name at all
            {"name": nulls, "seq": None, "bytes": None},  # nulls, not absent
            "L0000433.LOG",  # not even an object
            {"name": good, "seq": 431, "bytes": len(device.chunks[good])},
        ],
    }

    outcome = collector(device, dest).collect()

    # The null-valued entry is still usable — seq falls back to the digits in the
    # name and an unknown length is simply unknown — so it is collected, and only
    # the nameless entry and the non-object are reported.
    assert outcome.confirmed == [good, nulls]
    assert stored(dest, "mr-orange") == sorted([good, nulls])
    assert sorted(n for n, _ in outcome.failed) == ["<index entry>", "<index entry>"]


def test_a_null_seq_falls_back_to_the_name(device: FakeDevice, dest: Path) -> None:
    """Tolerant where tolerance is safe: the ordering that retention makes matter
    (§7) can be recovered from the filename."""
    device.add(432)
    device.add(431)
    device.index_override = {
        "device": "mr-orange",
        "chunks": [{**e, "seq": None} for e in device.index_entries()],
    }

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == ["L0000431.LOG", "L0000432.LOG"]
    assert outcome.failed == []


def test_a_chunk_name_may_never_become_a_path(device: FakeDevice, dest: Path) -> None:
    """The device supplies the name and the name becomes a filesystem path. A
    traversal is skipped and reported; it never opens a file, and it is never
    confirmed."""
    good = device.add(431)
    device.index_override = {
        "device": "mr-orange",
        "chunks": [
            {"name": "../../escaped.LOG", "seq": 1, "bytes": 10},
            {"name": "/tmp/absolute.LOG", "seq": 2, "bytes": 10},
            {"name": ".hidden", "seq": 3, "bytes": 10},
            *device.index_entries(),
        ],
    }

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == [good]
    assert sorted(n for n, _ in outcome.failed) == [
        "../../escaped.LOG",
        ".hidden",
        "/tmp/absolute.LOG",
    ]
    assert not (dest.parent / "escaped.LOG").exists()
    assert not Path("/tmp/absolute.LOG").exists()
    assert device.confirms() == [Request("POST", f"/sdlog/done/{good}")]


@pytest.mark.parametrize(
    "name",
    ["../x.LOG", "/tmp/x.LOG", "a/b.LOG", "", ".", "..", ".hidden", "x\x00.LOG"],
)
def test_archive_paths_reject_an_unusable_name(device: FakeDevice, dest: Path, name: str) -> None:
    pull = collector(device, dest)
    with pytest.raises(ValueError):
        pull.archive_path(name)
    with pytest.raises(ValueError):
        pull.partial_path(name)


def test_an_index_that_is_not_an_object_says_so(device: FakeDevice, dest: Path) -> None:
    """It answered, with JSON, and it is still not an index. The operator has to
    be told which of the two it is — 'not on the WiFi' would send them out to the
    car for a firmware mismatch."""
    device.add(431)
    device.index_raw = json.dumps([{"name": "L0000431.LOG"}]).encode()

    pull = collector(device, dest, retries=3)
    outcome = pull.collect()

    assert pull.unreachable is True
    assert "not a JSON object" in outcome.failed[0][1]
    # The shape will not change on the next try, so it is not retried.
    assert len([r for r in device.requests if r.path == "/sdlog/index"]) == 1


def test_a_chunk_listed_twice_is_collected_once(device: FakeDevice, dest: Path) -> None:
    """A name listed twice is one chunk listed twice. Collecting it twice would
    confirm it and then trip over the 404 its own confirm just created."""
    name = device.add(431)
    device.index_override = {
        "device": "mr-orange",
        "chunks": [*device.index_entries(), *device.index_entries()],
    }

    outcome = collector(device, dest).collect()

    assert outcome.confirmed == [name]
    assert outcome.failed == []
    assert len(device.bodies(name)) == 1
    assert stored(dest, "mr-orange") == [name]


def test_a_local_filesystem_that_says_no_is_reported_not_raised(
    device: FakeDevice, dest: Path
) -> None:
    """``collect`` promises never to raise: a bench operator gets a report and an
    exit status, never a traceback. A directory sitting where the chunk wants to
    land is the cheapest way to make the archive refuse."""
    name = device.add(431)
    other = device.add(432)
    (dest / "mr-orange" / name).mkdir(parents=True)

    outcome = collector(device, dest, retries=1).collect()

    assert [n for n, _ in outcome.failed] == [name]
    assert outcome.confirmed == [other]
    assert device.confirms(name) == []


# --------------------------------------------------------------- the pinned surface


def test_fetch_stores_and_verifies_without_confirming(device: FakeDevice, dest: Path) -> None:
    """``fetch`` is the half of the pass that may be used on its own — it puts
    verified bytes under the final name and tells the device nothing."""
    name = device.add(431)
    pull = collector(device, dest)
    chunk = pull.index()[0]

    path = pull.fetch(chunk)

    assert path == dest / "mr-orange" / name
    assert path.read_bytes() == device.chunks[name]
    assert device.confirms(name) == []


def test_fetch_raises_nothing_and_stores_nothing_for_a_bad_chunk(
    device: FakeDevice, dest: Path
) -> None:
    name = device.add(431, corrupt_chunk(431))
    pull = collector(device, dest)
    chunk = pull.index()[0]

    with pytest.raises(Exception):  # noqa: B017 - the class is private to the tool
        pull.fetch(chunk)

    assert stored(dest, "mr-orange") == []
    assert device.confirms(name) == []


def test_confirm_reports_true_only_when_the_device_said_so(
    device: FakeDevice, dest: Path
) -> None:
    name = device.add(431)
    pull = collector(device, dest, retries=1)

    assert pull.confirm(name) is True
    assert device.confirmed == [name]

    refused = device.add(432)
    device.confirm_status[refused] = 409
    assert pull.confirm(refused) is False
    assert device.confirmed == [name]


def test_print_outcome_names_the_device_and_the_failures(capsys) -> None:
    outcome = sdlog_collect.Outcome(device="mr-orange")
    outcome.fetched.append("L0000431.LOG")
    outcome.resumed.append("L0000431.LOG")
    outcome.restarted.append("L0000431.LOG")
    outcome.skipped.append("L0000432.LOG")
    outcome.confirmed.append("L0000431.LOG")
    outcome.failed.append(("L0000433.LOG", "confirm: HTTP 500"))
    outcome.bytes_fetched = 2048
    outcome.bytes_wire = 4096

    sdlog_collect.print_outcome(outcome)

    out, err = capsys.readouterr()
    assert "mr-orange" in out
    assert "1 fetched (2.0 KB)" in out
    assert "1 resumed" in out and "1 restarted" in out and "1 already had" in out
    assert "1 confirmed" in out
    assert "4.0 KB off the wire" in out
    assert "L0000433.LOG" in err and "500" in err


# ------------------------------------------------------------------- the CLI shell


def test_pull_exits_zero_when_everything_was_collected(device: FakeDevice, dest: Path) -> None:
    device.add(431)
    assert sdlog_collect.main(["pull", device.url, "--into", str(dest)]) == 0
    assert stored(dest, "mr-orange") == ["L0000431.LOG"]


def test_pull_exits_one_when_a_chunk_was_left_behind(device: FakeDevice, dest: Path) -> None:
    name = device.add(431)
    device.confirm_status[name] = 500
    argv = ["pull", device.url, "--into", str(dest), "--retries", "1", "--backoff", "0"]
    assert sdlog_collect.main(argv) == 1


def test_pull_exits_two_when_the_device_is_unreachable(device: FakeDevice, dest: Path) -> None:
    """Distinguished from 1 on purpose: 'the car is not on the WiFi' is a
    different message to a bench operator than 'a chunk failed to verify'."""
    url = device.url
    device.stop()
    argv = ["pull", url, "--into", str(dest), "--retries", "1", "--backoff", "0"]
    assert sdlog_collect.main(argv) == 2


def test_index_lists_and_confirms_nothing(device: FakeDevice, dest: Path, capsys) -> None:
    """The read-only subcommand: the one that is safe to point at a car that is
    still driving."""
    device.add(431)
    device.add(432)

    assert sdlog_collect.main(["index", device.url]) == 0

    out, _ = capsys.readouterr()
    assert "mr-orange: 2 sealed chunk(s)" in out
    assert "L0000431.LOG  seq=431" in out
    assert "span=0.0s" in out
    assert device.confirms() == []
    assert stored(dest, "mr-orange") == []


def test_index_reports_an_entry_it_could_not_read(device: FakeDevice, capsys) -> None:
    device.add(431)
    device.index_override = {
        "device": "mr-orange",
        "chunks": [{"name": "../oops.LOG", "seq": 1, "bytes": 3}, *device.index_entries()],
    }

    assert sdlog_collect.main(["index", device.url]) == 1

    out, err = capsys.readouterr()
    assert "L0000431.LOG" in out
    assert "../oops.LOG" in err


def test_index_exits_two_when_the_device_is_unreachable(device: FakeDevice, capsys) -> None:
    url = device.url
    device.stop()
    assert sdlog_collect.main(["index", url, "--retries", "1"]) == 2
    assert "index:" in capsys.readouterr().err
