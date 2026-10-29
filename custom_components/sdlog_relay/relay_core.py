"""sdlog_relay core: forward the sd_logger collection endpoints, hand out firmware.

No Home Assistant import in this module (docs/sdlog-relay-spec.md R13): aiohttp
and the stdlib only, so everything that touches the wire is tested without a Home
Assistant install. The integration around it only looks up config entries and
checks who is asking.
"""

from __future__ import annotations

import asyncio
from collections.abc import Mapping
from dataclasses import dataclass
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import tempfile

import aiohttp
from aiohttp import web

BLOCK = 64 * 1024
CONNECT_TIMEOUT = 5.0
READ_TIMEOUT = 30.0
# A request for a logger that is busy waits this long before 503 (R6). The
# collector sends its next request the instant it has the last byte, often
# before the previous handler has finished; refusing at once loses that race.
BUSY_WAIT = 5.0

LOGGER_ID_RE = re.compile(r"[a-z0-9][a-z0-9_-]{0,31}")
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,31}")

# Everything else is dropped in both directions (R2): no cookies, no
# Authorization towards the logger, nothing from the logger but the framing.
REQUEST_HEADERS = ("Range",)
RESPONSE_HEADERS = ("Content-Length", "Content-Range", "Content-Type", "Accept-Ranges")

MANIFEST = "manifest.json"
BINARY_RE = re.compile(r"firmware-[0-9a-f]{32}\.ota\.bin")
VERSION_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,31}")
# ESP Web Tools spelling. ESP32 family only: the update path has no signature,
# and the ESP8266 cannot even verify TLS.
CHIP_FAMILIES = frozenset(
    {"ESP32", "ESP32-C2", "ESP32-C3", "ESP32-C5", "ESP32-C6", "ESP32-H2", "ESP32-P4", "ESP32-S2", "ESP32-S3"}
)
MAX_FIRMWARE = 8 * 1024 * 1024
# The binary the previous manifest names stays servable, so a logger that read
# the old manifest a moment before an upload still gets a matching binary (R11).
KEEP_BINARIES = 2


@dataclass(frozen=True)
class Target:
    host: str
    port: int = 8080


def json_error(status: int, reason: str) -> web.Response:
    return web.json_response({"reason": reason}, status=status)


def upstream_path(tail: str, method: str) -> str | web.Response:
    """The logger path for the part after ``/sdlog/``, or the error to answer.

    Only the four collection endpoints exist (R1, R4). A name that could leave
    the endpoint is refused here, before anything is sent upstream.
    """
    if tail in ("index", "status"):
        allowed, path = "GET", f"/sdlog/{tail}"
    else:
        kind, sep, name = tail.partition("/")
        if not sep or kind not in ("f", "done"):
            return json_error(404, "unknown_endpoint")
        if NAME_RE.fullmatch(name) is None or ".." in name:
            return json_error(400, "bad_name")
        allowed, path = ("GET" if kind == "f" else "POST"), f"/sdlog/{kind}/{name}"
    if method != allowed:
        return json_error(405, "method_not_allowed")
    return path


class Relay:
    """Pass-through to the loggers. Never stores a byte of a chunk."""

    def __init__(self, session: aiohttp.ClientSession, *, busy_wait: float = BUSY_WAIT) -> None:
        self._session = session
        self._busy_wait = busy_wait
        self._locks: dict[str, asyncio.Lock] = {}

    async def forward(
        self, request: web.Request, targets: Mapping[str, Target], logger_id: str, tail: str
    ) -> web.StreamResponse:
        target = targets.get(logger_id)
        if target is None:
            return json_error(404, "unknown_logger")
        path = upstream_path(tail, request.method)
        if isinstance(path, web.Response):
            return path
        # One upstream request per logger (R6): the ESP's httpd serves few
        # sockets. Queue briefly, refuse only if the logger stays busy.
        lock = self._locks.setdefault(logger_id, asyncio.Lock())
        try:
            async with asyncio.timeout(self._busy_wait):
                await lock.acquire()
        except TimeoutError:
            return json_error(503, "busy")
        try:
            return await self._forward(request, target, path)
        finally:
            lock.release()

    async def _forward(self, request: web.Request, target: Target, path: str) -> web.StreamResponse:
        headers = {"Accept-Encoding": "identity"}
        for name in REQUEST_HEADERS:
            if name in request.headers:
                headers[name] = request.headers[name]
        timeout = aiohttp.ClientTimeout(total=None, connect=CONNECT_TIMEOUT, sock_read=READ_TIMEOUT)
        try:
            upstream = await self._session.request(
                request.method,
                f"http://{target.host}:{target.port}{path}",
                headers=headers,
                timeout=timeout,
                allow_redirects=False,
                auto_decompress=False,
            )
        except (aiohttp.ClientError, TimeoutError):
            return json_error(502, "logger_unreachable")

        async with upstream:
            # A 401 from here would count as a failed login against whoever HA
            # thinks is asking, which behind a tunnel is the tunnel itself (R8).
            if upstream.status == 401:
                return json_error(502, "logger_answered_401")
            response = web.StreamResponse(status=upstream.status)
            for name in RESPONSE_HEADERS:
                if name in upstream.headers:
                    response.headers[name] = upstream.headers[name]
            await response.prepare(request)
            try:
                async for block in upstream.content.iter_chunked(BLOCK):
                    await response.write(block)
            except (aiohttp.ClientError, TimeoutError):
                # The logger went away mid-body. Hang up without completing:
                # the collector must see exactly what it would see on the LAN,
                # a body shorter than its Content-Length (R5).
                response.force_close()
                if request.transport is not None:
                    request.transport.close()
                return response
            await response.write_eof()
            return response


def same_address(remote: str | None, host: str) -> bool:
    """``request.remote`` against the configured logger address, tolerant of the
    IPv4-mapped form a dual-stack socket reports."""
    if not remote:
        return False
    try:
        a = ipaddress.ip_address(remote)
        b = ipaddress.ip_address(host)
    except ValueError:
        return False
    if isinstance(a, ipaddress.IPv6Address) and a.ipv4_mapped is not None:
        a = a.ipv4_mapped
    return a == b


class FirmwareError(ValueError):
    def __init__(self, status: int, reason: str) -> None:
        super().__init__(reason)
        self.status = status
        self.reason = reason


def _atomic_write(path: Path, data: bytes) -> None:
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


class FirmwareStore:
    """``<root>/<logger_id>/``: one manifest, the binaries it may name.

    Blocking file I/O: call it from an executor, never from the event loop.
    """

    def __init__(self, root: Path) -> None:
        self.root = root

    def put(self, logger_id: str, data: bytes, version: str, chip_family: str) -> dict:
        if LOGGER_ID_RE.fullmatch(logger_id) is None:
            raise FirmwareError(400, "bad_logger_id")
        if VERSION_RE.fullmatch(version or "") is None:
            raise FirmwareError(400, "bad_version")
        if chip_family not in CHIP_FAMILIES:
            raise FirmwareError(400, "bad_chip_family")
        if not data:
            raise FirmwareError(400, "empty_firmware")
        if len(data) > MAX_FIRMWARE:
            raise FirmwareError(413, "firmware_too_large")
        md5 = hashlib.md5(data).hexdigest()  # noqa: S324 - the manifest format is MD5
        binary = f"firmware-{md5}.ota.bin"
        directory = self.root / logger_id
        directory.mkdir(parents=True, exist_ok=True)
        # Binary first, manifest second: the manifest never names a file that
        # is not complete on disk.
        _atomic_write(directory / binary, data)
        manifest = {
            "name": logger_id,
            "version": version,
            "builds": [{"chipFamily": chip_family, "ota": {"md5": md5, "path": binary}}],
        }
        _atomic_write(directory / MANIFEST, json.dumps(manifest, indent=2).encode())
        self._prune(directory, binary)
        return manifest

    @staticmethod
    def _prune(directory: Path, current: str) -> None:
        binaries = sorted(
            (p for p in directory.iterdir() if BINARY_RE.fullmatch(p.name) and p.name != current),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        for old in binaries[KEEP_BINARIES - 1 :]:
            old.unlink(missing_ok=True)

    def resolve(self, logger_id: str, name: str) -> Path | None:
        if LOGGER_ID_RE.fullmatch(logger_id) is None:
            return None
        if name != MANIFEST and BINARY_RE.fullmatch(name) is None:
            return None
        path = self.root / logger_id / name
        return path if path.is_file() else None


def _not_found() -> web.Response:
    # 404, not 401 or 403: HA counts a 401 as a failed login (R9).
    return json_error(404, "not_found")


async def serve_firmware(
    request: web.Request, store: FirmwareStore, targets: Mapping[str, Target], logger_id: str, name: str
) -> web.StreamResponse:
    """Unauthenticated on purpose: only the logger itself may fetch (R9, R12)."""
    target = targets.get(logger_id)
    if target is None or not same_address(request.remote, target.host):
        return _not_found()
    path = await asyncio.get_running_loop().run_in_executor(None, store.resolve, logger_id, name)
    if path is None:
        return _not_found()
    data = await asyncio.get_running_loop().run_in_executor(None, path.read_bytes)
    content_type = "application/json" if name == MANIFEST else "application/octet-stream"
    return web.Response(body=data, content_type=content_type, headers={"Cache-Control": "no-store"})


async def upload_firmware(
    request: web.Request, store: FirmwareStore, targets: Mapping[str, Target], logger_id: str, *, is_admin: bool
) -> web.Response:
    """Admin only (R10). 403, never 401, for a user who is not."""
    if not is_admin:
        return json_error(403, "admin_required")
    if logger_id not in targets:
        return json_error(404, "unknown_logger")
    data = bytearray()
    async for block in request.content.iter_chunked(BLOCK):
        data += block
        if len(data) > MAX_FIRMWARE:
            return json_error(413, "firmware_too_large")
    try:
        manifest = await asyncio.get_running_loop().run_in_executor(
            None,
            store.put,
            logger_id,
            bytes(data),
            request.query.get("version", ""),
            request.query.get("chip_family", ""),
        )
    except FirmwareError as err:
        return json_error(err.status, err.reason)
    return web.json_response(manifest)
