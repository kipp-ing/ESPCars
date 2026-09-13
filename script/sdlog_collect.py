#!/usr/bin/env python3
"""Pull sealed sd_logger chunks off a device (docs/sdlog-collection-design.md §5).

The sibling of ``script/sdlog.py``: that one reads a card you carried to your
desk, this one fetches the card's chunks over WiFi while the car stays where it
is. It runs on a Mac, never on the ECU — the design puts retries, credentials,
dedup and the archive on the machine with a filesystem and a debugger, and
leaves the ECU owning only "which chunks exist and what state are they in".

    ┌──────── device (ESP32-C6) ────────┐          ┌──── this script ────┐
    │ GET  /sdlog/index   sealed chunks │ ───────▶ │ index               │
    │ GET  /sdlog/f/<n>   bytes, Range  │ ───────▶ │ fetch, resume       │
    │                                   │          │ verify (sdlog.py)   │
    │ POST /sdlog/done/<n> SEALED→.UPL  │ ◀─────── │ confirm             │
    └───────────────────────────────────┘          └─────────────────────┘

THE ORDERING IS THE WHOLE DESIGN, AND ONLY ONE ORDER IS SAFE

    serve bytes  →  verify locally  →  confirm  →  (device renames to .UPL)

A confirm is a licence to delete. Sending one before the bytes are on local disk
*and* parse-checked means a chunk that never arrived becomes deletable, which is
the one failure mode that loses data permanently instead of merely costing a
retry. A crash anywhere in that window re-serves the chunk; that is correct and
cheap, because the puller dedups on ``(device, seq)``.

A FILENAME IS NOT AN IDENTITY, AND THAT IS THE SHARP EDGE OF THIS TOOL

``seq`` is monotonic per *card*, not per device: `sd_logger.cpp` rescans it on
every remount, commented "the card may be a different one", and the scan starts
at 0 on a fresh or reformatted card. So ``L0000431.LOG`` in the archive and
``L0000431.LOG`` in today's index can be two unrelated files, and the format
carries no boot id to tell them apart (design §10.2, deferred). Two paths would
otherwise trust the name alone, and both end in a confirm — which is to say in a
deletion:

    already have it?    The dedup skip. Before confirming a chunk it did not
                        fetch this run, the collector re-proves the archived copy
                        *is* the offered one: same length as the index says, still
                        parses, same seq inside, and its first bytes match the
                        device's own first bytes (one ~256 B request, not the
                        chunk). Any disagreement is reported and nothing is
                        confirmed — neither copy is worth destroying for tidiness.

    resume a partial?   A ``.part`` that outlived a run is bytes of unknown
                        provenance. Before continuing one, the collector checks
                        the head the same way and compares the ~256 B under the
                        seam against what the device sends back, so a resume can
                        never splice the front of one file onto the tail of
                        another — a hybrid that parses clean, verifies clean, and
                        is neither file. A partial that fails either check is
                        deleted and the chunk is fetched whole.

Both are windows, not digests: the format offers no per-chunk hash, so a device
that re-created a file with the same seq, the same length, the same first 256
bytes *and* the same bytes under the seam would still slip through. Closing that
completely needs §10.2's boot id (or a digest in the index), which is a format
decision, not a collector one.

WHAT IS DELIBERATE HERE

    resume, not restart     A dropped connection resumes at the byte offset with
                            an HTTP ``Range`` request. Car WiFi drops mid-chunk
                            as a matter of course; restarting a 4 MB chunk each
                            time is how a collection run fails to converge.

    dedup on (device, seq)  A re-served chunk is neither stored twice nor fetched
                            twice — the second cost is the one that matters over
                            WiFi. Only the identity windows above cross the wire
                            again.

    a vanished device       is the normal case, not an error. The car drove away.
                            Retry with backoff, report, exit non-zero, and leave
                            every partial in place for the next run.

    a 5xx is a stumble      §5a budgets ~10 KB of heap for the httpd on a chip
                            §9.3 calls heap-bound, so "500, ask again in a
                            second" is the most likely thing a busy ECU says —
                            on the index as much as on a chunk. A 4xx is an
                            answer and is taken as one.

    verification is reused  ``sdlog.py``'s parser decides whether a chunk is
                            intact. A second, subtly different parser here would
                            be a way to confirm a chunk the real reader rejects.

    a broken index entry    is skipped and reported, never raised. The wire keys
                            are the other half of a contract with the device's
                            httpd; a missing or null field must cost one chunk,
                            not the whole run and a traceback.

TWO PLACES THE OBVIOUS BEHAVIOUR IS THE WRONG ONE, AND WHY

    a short body is not an HTTP error. The device promises a ``Content-Length``
    and then the car drives out of range; the socket simply ends. There is no
    status code for that, so this counts bytes itself and treats "fewer than
    promised" as the failed attempt — see :meth:`Collector._download`.

    a *complete* transfer that fails verification is never retried. The device
    delivered every byte it promised, so asking again returns the same bytes.
    The partial is deleted (keeping it would make the next run resume *past* the
    bad bytes and never heal) and the chunk is left for a later run.

Usage:
    script/sdlog_collect.py pull http://mr-orange.local:8080 --into ~/logs
    script/sdlog_collect.py pull <url> --into <dir> --device mr-orange
    script/sdlog_collect.py pull <url> --into <dir> --retries 1 --backoff 0
    script/sdlog_collect.py index http://mr-orange.local:8080

Archive layout: ``<into>/<device>/L0000433.LOG``, with an in-flight transfer at
``<into>/<device>/L0000433.LOG.part`` — a partial is never given the final name,
so anything without ``.part`` is complete and verified.

Exit status: 0 everything listed was collected and confirmed, 1 at least one
chunk was not, 2 the device could not be reached at all.
"""

from __future__ import annotations

import argparse
import http.client
import importlib.util
import json
import re
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import quote, urlparse


# script/ is a tool directory, not a package, so the reader is loaded by path —
# the same dance tests/sd_logger/test_sdlog_reader.py does, and for the same
# reason (@dataclass resolves its defining module out of sys.modules).
def _load_reader():
    if "sdlog" in sys.modules:
        return sys.modules["sdlog"]
    spec = importlib.util.spec_from_file_location("sdlog", Path(__file__).parent / "sdlog.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["sdlog"] = module
    spec.loader.exec_module(module)
    return module


sdlog = _load_reader()

DEFAULT_TIMEOUT = 10.0
DEFAULT_RETRIES = 4  # attempts per chunk per run, not retries *after* the first
DEFAULT_BACKOFF = 0.5  # seconds before the second attempt; grows from there
PART_SUFFIX = ".part"

# One read from the socket. Big enough that a 4 MB chunk is not a syscall storm,
# small enough that a partial keeps most of what arrived before a cut.
BLOCK = 64 * 1024

# The two identity windows. Both are re-read off the device, so both are wire
# cost on a resume — 0.01 % of a 4 MB chunk, which is the right price for not
# splicing two cards' files together. The head window covers the `#sdlog` line
# (fmtver, seq, open time) and most of the `#src` block.
HEAD_WINDOW = 256
SEAM_WINDOW = 256

# A chunk name is used as a path component, so it is validated before it becomes
# one: the device supplies it, and `../../x` or `/tmp/x` would otherwise write
# wherever it liked. The writer's own names are `L0000433.LOG`; this is looser
# than that on purpose (a `.UPL` or a renamed card file is still collectable)
# but never a directory, never hidden, never a traversal.
_SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")

_RETRYABLE_STATUS = frozenset({408, 429})


def _retryable(status: int) -> bool:
    """A status worth asking again about. 5xx on this device usually means the
    httpd lost a race for heap, not that the request was wrong."""
    return status >= 500 or status in _RETRYABLE_STATUS


@dataclass
class Chunk:
    """One entry of ``GET /sdlog/index``. Wire keys: name, seq, bytes, first_t_us,
    last_t_us — ``bytes`` lands in ``size`` because the builtin name would read
    badly everywhere else."""

    name: str
    seq: int
    size: int
    first_t_us: int | None = None
    last_t_us: int | None = None


@dataclass
class Outcome:
    """What one pass over one device did. Reported rather than logged, so the
    caller (and the tests) can assert on it."""

    device: str = ""
    fetched: list[str] = field(default_factory=list)  # bytes came down this run
    resumed: list[str] = field(default_factory=list)  # continued from a partial
    restarted: list[str] = field(default_factory=list)  # a partial was discarded
    confirmed: list[str] = field(default_factory=list)  # the device acknowledged
    skipped: list[str] = field(default_factory=list)  # already archived (dedup)
    failed: list[tuple[str, str]] = field(default_factory=list)  # (name, reason)
    # Written into the archive this run — a restarted partial counts what it
    # wrote before it was discarded, because that is what the run cost.
    bytes_fetched: int = 0
    bytes_wire: int = 0  # everything read off the socket, incl. identity windows


class _Unreachable(Exception):
    """The device did not answer at all. Distinguished from a chunk-level failure
    because it is a different message to a bench operator, and a different exit
    status: 'the car is not on the WiFi', not 'a chunk failed to verify'."""


class _ChunkFailed(Exception):
    """This chunk was not collected. The run continues with the next one."""


class _Interrupted(Exception):
    """The body stopped short of the promised Content-Length — a cut connection,
    which has no HTTP status of its own."""


class _Mismatch(Exception):
    """The bytes on disk are not the bytes the device is offering under that
    name. The local copy is not a prefix of this chunk, so continuing it would
    splice two files; the partial is dropped and the chunk fetched whole."""


@dataclass
class _Transfer:
    """The bookkeeping :meth:`Collector.fetch` cannot return through a ``Path``.
    Passed in rather than returned so the byte counts survive a failure."""

    stored: int = 0  # bytes written into the .part
    wire: int = 0  # bytes read off the socket, including discarded attempts
    resumed: bool = False
    restarted: bool = False


def _size(path: Path) -> int:
    try:
        return path.stat().st_size
    except OSError:
        return 0


def _unlink(path: Path) -> None:
    """Dropping bytes that were already judged useless must not be able to end
    the run — this is called from failure paths only."""
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


def _read_head(path: Path, count: int) -> bytes:
    if count <= 0:
        return b""
    with path.open("rb") as handle:
        return handle.read(count)


def _read_at(path: Path, offset: int, count: int) -> bytes:
    if count <= 0:
        return b""
    with path.open("rb") as handle:
        handle.seek(offset)
        return handle.read(count)


def _why(err: BaseException) -> str:
    return f"{type(err).__name__}: {err}"


def _seq_of(name: str) -> int:
    """The 7 digits in ``L0000431.LOG``. Only a fallback — the index carries the
    seq — but a chunk with no usable seq would break the oldest-first ordering
    that retention makes matter (§7)."""
    digits = "".join(ch for ch in Path(name).stem if ch.isdigit())
    return int(digits) if digits else 0


def _int_or(value, fallback: int) -> int:
    """A wire field that is absent, null or not a number falls back. ``.get(k, d)``
    covers only the absent case, and JSON ``null`` is the shape a C++ httpd emits
    when it has nothing to say."""
    if value is None:
        return fallback
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def _opt_int(value) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def safe_name(name: str) -> str:
    """The device names the file; this decides whether that name may become a
    path. Traversal is the whole risk: ``../../x.LOG`` and ``/tmp/x.LOG`` are
    both perfectly good JSON strings."""
    if not isinstance(name, str) or Path(name).name != name or not _SAFE_NAME.fullmatch(name):
        raise ValueError(f"unusable chunk name {name!r}")
    return name


def _read_report(path: Path):
    """``(report, problem)`` — the parse and the verdict from one pass, so an
    identity check can look at the header without parsing the file twice."""
    try:
        report = sdlog.parse_file(path)
    except OSError as err:
        return None, f"cannot read: {err}"
    if report.bad_lines:
        number, detail = report.bad_lines[0]
        more = f" (+{len(report.bad_lines) - 1} more)" if len(report.bad_lines) > 1 else ""
        return report, f"malformed line {number}: {detail}{more}"
    if report.legacy:
        return report, "not an sd_logger chunk (M1-era .CSV legend, no #sdlog header)"
    if report.unsupported_version:
        # The bytes may be perfect; this side is simply too old to say. Refusing
        # keeps the chunk on the device for a newer collector, which is the only
        # outcome that does not lose it — confirming it would let retention delete
        # a file nothing has ever read.
        return report, (
            f"write format v{report.format_version}, which this collector cannot verify"
            f" (it reads {', '.join('v%d' % v for v in sdlog.SUPPORTED_FORMAT_VERSIONS)})"
        )
    if report.format_version is None:
        return report, "no #sdlog header — this is not an sd_logger chunk"
    if report.close_reason is None:
        return report, "short transfer: no #close line (a sealed chunk always carries one)"
    return report, None


def verify_chunk(path: Path) -> str | None:
    """Decide whether a locally stored chunk is a complete, intact sealed chunk.

    Returns ``None`` if it is, else a short reason. Built on ``sdlog.parse_file``:
    a malformed line is a defect, and a *missing* ``#close`` means the transfer is
    short — a SEALED chunk always carries one, so its absence here is the signal
    that the bytes are incomplete rather than that the device lost power.

    That last reading is the one place this tool disagrees with a bench operator
    holding the card: on a card a missing ``#close`` is the power-cut signature
    and an expected test result, but a device that put the chunk in the index
    called it SEALED, so over HTTP the only remaining explanation is short bytes.
    """
    return _read_report(path)[1]


class Collector:
    """One device, one archive directory, one pass per :meth:`collect` call."""

    def __init__(
        self,
        base_url: str,
        dest: Path | str,
        *,
        device: str | None = None,
        timeout: float = DEFAULT_TIMEOUT,
        retries: int = DEFAULT_RETRIES,
        backoff: float = DEFAULT_BACKOFF,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.dest = Path(dest)
        # An explicit name wins; otherwise the index says who it is; otherwise the
        # host out of the URL. The name is half the dedup key, so it has to be
        # stable across runs — which is why guessing it is the last resort.
        self.device = device or ""
        self._device_pinned = bool(device)
        self.timeout = timeout
        self.retries = retries
        self.backoff = backoff
        self.sleeper = sleeper
        # Set by collect(): the device never answered, as opposed to answering
        # and refusing. Only main() reads it, and only to pick exit 2 over 1.
        self.unreachable = False
        # Entries the index offered that could not be understood. Kept rather
        # than raised: one broken entry costs one chunk, not the run.
        self.index_problems: list[tuple[str, str]] = []

    # ------------------------------------------------------------------ plumbing

    def _attempts(self) -> Iterator[int]:
        """Attempt numbers, sleeping between them and never after the last.

        ``retries`` is attempts per chunk per run — not retries *after* the first
        — so ``retries=1`` means one try and no sleep, which is what makes a
        cross-run resume observable instead of hidden by an in-run retry.
        """
        total = max(1, int(self.retries))
        for number in range(total):
            if number:
                self.sleeper(self.backoff * (2 ** (number - 1)))
            yield number

    def _open(self, path: str, *, method: str = "GET", headers: dict | None = None, data=None):
        request = urllib.request.Request(
            self.base_url + path, data=data, headers=headers or {}, method=method
        )
        return urllib.request.urlopen(request, timeout=self.timeout)

    def _name(self) -> str:
        return self.device or urlparse(self.base_url).hostname or "device"

    # -------------------------------------------------------------------- routes

    def index(self) -> list[Chunk]:
        """``GET /sdlog/index``, oldest seq first.

        The device lists in whatever order it walked the directory. Ordering is
        the collector's job because retention drops the *oldest* un-collected
        chunk when the card fills (§7), so the oldest is the one racing deletion.

        Raises :class:`_Unreachable` only when the device gave no usable answer at
        all. A single unusable *entry* never raises — see ``index_problems``.
        """
        self.index_problems = []
        payload = None
        reason = "no attempt was made"
        for _ in self._attempts():
            try:
                with self._open("/sdlog/index") as response:
                    payload = json.loads(response.read().decode("utf-8"))
            except urllib.error.HTTPError as err:
                reason = f"HTTP {err.code} {err.reason}"
                if _retryable(err.code):
                    # A busy httpd, not a wrong request (§5a: ~10 KB of heap).
                    payload = None
                    continue
                # Any other status is an answer; asking again will not change it.
                raise _Unreachable(f"index: {reason}") from err
            except (OSError, http.client.HTTPException, ValueError) as err:
                reason = _why(err)
                continue
            if isinstance(payload, dict):
                break
            # It answered, and with JSON, but not with an index. Asking again
            # will not change the shape, so say what arrived instead of leaving
            # the operator with the last transport error (or with none at all).
            reason = f"the index is not a JSON object but a {type(payload).__name__}"
            payload = None
            break
        if not isinstance(payload, dict):
            raise _Unreachable(f"index: {reason}")

        named = payload.get("device")
        if named and not self._device_pinned:
            self.device = str(named)

        entries = payload.get("chunks")
        if not isinstance(entries, list):
            entries = []
        chunks = []
        seen = set()
        for entry in entries:
            try:
                chunk = _chunk_of(entry)
            except ValueError as err:
                self.index_problems.append((_entry_label(entry), str(err)))
                continue
            # A name listed twice is one chunk listed twice. Collecting it twice
            # would confirm it, then meet the 404 its own confirm just created.
            if chunk.name in seen:
                continue
            seen.add(chunk.name)
            chunks.append(chunk)
        chunks.sort(key=lambda chunk: (chunk.seq, chunk.name))
        return chunks

    def archive_dir(self) -> Path:
        return self.dest / self._name()

    def archive_path(self, name: str) -> Path:
        return self.archive_dir() / safe_name(name)

    def partial_path(self, name: str) -> Path:
        return self.archive_dir() / (safe_name(name) + PART_SUFFIX)

    def fetch(self, chunk: Chunk) -> Path:
        """Bytes to disk, resuming a partial with ``Range``. No confirm here."""
        return self._fetch(chunk, _Transfer())

    # ------------------------------------------------------------------ identity

    def _probe_head(self, chunk: Chunk, count: int, transfer: _Transfer) -> bytes:
        """The chunk's first ``count`` bytes, off the device, now.

        Only the window is read and then the response is dropped: a device that
        ignores the end of the range and starts sending 4 MB costs one closed
        socket, not the chunk.

        A window that arrives short is a cut connection, not a verdict —
        :class:`_Interrupted`, so the caller retries instead of concluding
        anything about bytes it never saw. Getting that backwards would let a
        WiFi drop delete a good partial.
        """
        headers = {"Range": f"bytes=0-{count - 1}"}
        data = b""
        with self._open(f"/sdlog/f/{quote(chunk.name)}", headers=headers) as response:
            promised = response.headers.get("Content-Length")
            promised = int(promised) if promised is not None else None
            while len(data) < count:
                block = response.read(count - len(data))
                if not block:
                    break
                data += block
        transfer.wire += len(data)
        # A device that ignored the end of the range promises far more than the
        # window; only what was asked for counts as owed.
        owed = count if promised is None else min(promised, count)
        if len(data) < owed:
            raise _Interrupted(f"identity window ended after {len(data)} of {owed} bytes")
        return data

    def _prove_local(self, chunk: Chunk, path: Path, transfer: _Transfer) -> None:
        """Are the bytes at ``path`` the first bytes of the chunk being offered?

        ``seq`` restarts at 0 on a fresh card and `sd_logger.cpp` rescans it on
        every remount ("the card may be a different one"), so a name is not an
        identity and neither is a length. Raises :class:`_Mismatch` if not.
        """
        local = _size(path)
        if local <= 0:
            raise _Mismatch("the local copy is empty")
        if chunk.size and local > chunk.size:
            raise _Mismatch(f"local copy holds {local} B, the device offers {chunk.size} B")
        want = min(local, HEAD_WINDOW)
        head = self._probe_head(chunk, want, transfer)
        if len(head) < want:
            # The device's whole file is shorter than the head of what is held
            # locally, so the local bytes cannot be a prefix of it.
            raise _Mismatch(f"the device holds only {len(head)} B under this name")
        if head != _read_head(path, len(head)):
            raise _Mismatch("the local copy's first bytes are not this chunk's first bytes")

    def _already_have(self, chunk: Chunk, transfer: _Transfer) -> str | None:
        """``None`` when the archived file under this name really is this chunk.

        This is the gate in front of the dedup skip, and the skip ends in a
        confirm — which is a licence to delete. Confirming on the strength of a
        filename is how a chunk that was never collected becomes deletable, so
        the archived copy has to answer for itself: right length, still parses,
        right seq inside, and the same head the device is serving right now.
        """
        path = self.archive_path(chunk.name)
        local = _size(path)
        if chunk.size and local != chunk.size:
            return f"archived copy is {local} B, the device offers {chunk.size} B"
        report, problem = _read_report(path)
        if problem is not None:
            return f"archived copy does not verify ({problem})"
        if chunk.seq and report.seq is not None and report.seq != chunk.seq:
            return f"archived copy carries seq {report.seq}, the index says {chunk.seq}"
        reason = "no attempt was made"
        for _ in self._attempts():
            try:
                self._prove_local(chunk, path, transfer)
            except _Mismatch as err:
                return str(err)
            except _Interrupted as err:
                reason = str(err)
                continue
            except urllib.error.HTTPError as err:
                reason = f"HTTP {err.code} {err.reason}"
                if _retryable(err.code):
                    continue
                return f"could not re-check against the device: {reason}"
            except (OSError, http.client.HTTPException) as err:
                reason = _why(err)
                continue
            else:
                return None
        return f"could not re-check against the device: {reason}"

    # --------------------------------------------------------------------- fetch

    def _fetch(self, chunk: Chunk, transfer: _Transfer) -> Path:
        directory = self.archive_dir()
        try:
            directory.mkdir(parents=True, exist_ok=True)
        except OSError as err:
            # A full disk or an archive root that is not a directory. One chunk's
            # problem as far as this pass is concerned; the report says which.
            raise _ChunkFailed(f"cannot open the archive directory: {err}") from err
        partial = self.partial_path(chunk.name)
        final = self.archive_path(chunk.name)

        complete = False
        reason = "no attempt was made"
        for _ in self._attempts():
            offset = _size(partial)
            try:
                if offset:
                    # Bytes we did not write in this call are bytes of unknown
                    # provenance, and so are bytes written before a device could
                    # have had its card swapped. Prove them or drop them.
                    self._prove_local(chunk, partial, transfer)
                    if chunk.size and offset == chunk.size:
                        # A previous run got every byte and died before the
                        # rename. They are already here; do not pay again.
                        complete = True
                        break
                self._download(chunk, partial, offset, transfer)
            except _Mismatch as err:
                _unlink(partial)
                transfer.restarted = True
                reason = f"discarded the partial: {err}"
                continue
            except urllib.error.HTTPError as err:
                if err.code == 416:
                    # We asked from beyond the end of what the device holds, so
                    # what we hold is not a prefix of it. (A partial that is
                    # merely complete never gets here — that is the length check
                    # above.)
                    _unlink(partial)
                    transfer.restarted = True
                    reason = "discarded the partial: it is longer than the chunk offered"
                    continue
                if 400 <= err.code < 500:
                    # 409 is the open file out of a stale index and 404 is a chunk
                    # already confirmed. Both are answers, not stumbles: this run
                    # is done with the chunk, and the next ones still get theirs.
                    raise _ChunkFailed(f"HTTP {err.code} {err.reason}") from err
                reason = f"HTTP {err.code} {err.reason}"
            except _Interrupted as err:
                reason = str(err)
            except (OSError, http.client.HTTPException) as err:
                reason = _why(err)
            else:
                complete = True
                break

        if not complete:
            raise _ChunkFailed(reason)

        problem = verify_chunk(partial)
        if problem is not None:
            # Every promised byte arrived and the result still does not parse, so
            # a retry would fetch the same bytes. Drop them: a bad partial left in
            # place would be resumed *past* on the next run and never heal.
            _unlink(partial)
            raise _ChunkFailed(problem)
        # Only now does the chunk get its final name. Anything without `.part` is
        # complete and verified — that is the whole on-disk state machine.
        try:
            partial.replace(final)
        except OSError as err:
            raise _ChunkFailed(f"cannot store as {final.name}: {err}") from err
        return final

    def _download(self, chunk: Chunk, partial: Path, offset: int, transfer: _Transfer) -> None:
        """One attempt. Appends to ``partial`` and raises if the body ran short.

        A resume asks from ``offset - SEAM_WINDOW`` and compares that window with
        what is already on disk before appending a byte. Without it, a resume is
        a byte offset into a file nobody checked was the same file: the result
        splices two chunks, lands on exactly the promised length, and parses
        clean, so no later check can catch it.
        """
        overlap = min(offset, SEAM_WINDOW)
        start = offset - overlap
        headers = {}
        if offset:
            headers["Range"] = f"bytes={start}-"
        with self._open(f"/sdlog/f/{quote(chunk.name)}", headers=headers) as response:
            promised = response.headers.get("Content-Length")
            promised = int(promised) if promised is not None else None
            mode = "ab"
            if offset and response.status != 206:
                # The device ignored the Range and is resending the whole chunk.
                # Splicing that onto the partial would corrupt it silently, so the
                # partial is abandoned instead — by the truncating 'wb' open,
                # which still only happens once a first byte has arrived.
                mode = "wb"
                overlap = 0
                start = offset = 0
            elif offset:
                transfer.resumed = True
            expect = _read_at(partial, start, overlap)
            # Opened lazily so a response that carries no body — a refusal, a cut
            # before the first byte — never leaves an empty `.part` in the archive
            # and never truncates one that is already there.
            handle = None
            got = 0
            seen = b""
            try:
                while True:
                    block = response.read(BLOCK)
                    if not block:
                        break
                    transfer.wire += len(block)
                    got += len(block)
                    if len(seen) < overlap:
                        want = overlap - len(seen)
                        seen += block[:want]
                        block = block[want:]
                        if len(seen) == overlap and seen != expect:
                            raise _Mismatch(
                                "the bytes under the resume point are not the ones held locally"
                            )
                    if not block:
                        continue
                    if handle is None:
                        handle = partial.open(mode)
                    handle.write(block)
                    transfer.stored += len(block)
            finally:
                if handle is not None:
                    handle.close()
        if overlap and len(seen) < overlap:
            # Cut inside the overlap window: nothing was appended and nothing was
            # proved, which is a failed attempt like any other short body.
            raise _Interrupted(f"body ended after {got} bytes, inside the resume window")
        if promised is not None and got < promised:
            # No status code says this happened. The car drove out of range.
            raise _Interrupted(f"body ended after {start + got} of {start + promised} bytes")

    def confirm(self, name: str) -> bool:
        """``POST /sdlog/done/<name>``. Idempotent; only ever called on a chunk
        already archived, verified and re-proved against the device."""
        return self._confirm(name) is None

    def _confirm(self, name: str) -> str | None:
        reason = "no attempt was made"
        for _ in self._attempts():
            try:
                with self._open(f"/sdlog/done/{quote(name)}", method="POST", data=b"") as response:
                    if response.status == 200:
                        return None
                    if _retryable(response.status):
                        reason = f"HTTP {response.status}"
                        continue
                    return f"confirm: HTTP {response.status}"
            except urllib.error.HTTPError as err:
                reason = f"HTTP {err.code} {err.reason}"
                if _retryable(err.code):
                    continue
                # The device answered and refused. Treating that as sent is the
                # mistake that deletes a chunk nobody ever acknowledged, so the
                # chunk simply comes back next run.
                return f"confirm: {reason}"
            except (OSError, http.client.HTTPException) as err:
                reason = _why(err)
                continue
        return f"confirm: {reason}"

    # ----------------------------------------------------------------- the pass

    def collect(self) -> Outcome:
        """One pass: index, then fetch/verify/confirm each chunk oldest first.

        Never raises — not on a network fault, not on an index entry that makes
        no sense. A car that drove away mid-run is the expected case, and every
        partial is left exactly where the next run wants to find it.
        """
        outcome = Outcome()
        self.unreachable = False
        try:
            chunks = self.index()
        except _Unreachable as err:
            self.unreachable = True
            outcome.device = self._name()
            outcome.failed.append(("<index>", str(err)))
            return outcome
        outcome.device = self._name()
        outcome.failed.extend(self.index_problems)

        for chunk in chunks:
            transfer = _Transfer()
            try:
                if self.archive_path(chunk.name).is_file():
                    # Dedup on (device, seq): the archive already holds a file of
                    # this name, so the device is probably re-serving one whose
                    # confirm did not stick. Skip the bytes — the point of the key
                    # is not paying for them twice — but only once the copy on
                    # disk has answered for being this chunk, because what follows
                    # is a confirm.
                    problem = self._already_have(chunk, transfer)
                    if problem is not None:
                        raise _ChunkFailed(f"{problem} — nothing was confirmed")
                    outcome.skipped.append(chunk.name)
                else:
                    self._fetch(chunk, transfer)
                    outcome.fetched.append(chunk.name)
                    if transfer.resumed:
                        outcome.resumed.append(chunk.name)
                    if transfer.restarted:
                        outcome.restarted.append(chunk.name)
            except _ChunkFailed as err:
                outcome.failed.append((chunk.name, str(err)))
                continue
            finally:
                outcome.bytes_fetched += transfer.stored
                outcome.bytes_wire += transfer.wire

            problem = self._confirm(chunk.name)
            if problem is None:
                outcome.confirmed.append(chunk.name)
            else:
                outcome.failed.append((chunk.name, problem))
        return outcome


def _entry_label(entry) -> str:
    """What to call an index entry that could not be parsed. Its own name if it
    has a printable one, so the operator can find it in the device's listing."""
    if isinstance(entry, dict):
        name = entry.get("name")
        if isinstance(name, str) and name.strip():
            return name[:40]
    return "<index entry>"


def _chunk_of(entry) -> Chunk:
    """One wire entry to a :class:`Chunk`, or ``ValueError`` with the reason.

    Tolerant where tolerance is safe (a null or absent ``seq`` falls back to the
    digits in the name), strict where it is not (the name becomes a path).
    """
    if not isinstance(entry, dict):
        raise ValueError(f"index entry is not an object: {type(entry).__name__}")
    name = entry.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError("index entry has no usable 'name'")
    safe_name(name)
    return Chunk(
        name=name,
        seq=_int_or(entry.get("seq"), _seq_of(name)),
        size=_int_or(entry.get("bytes"), 0),
        first_t_us=_opt_int(entry.get("first_t_us")),
        last_t_us=_opt_int(entry.get("last_t_us")),
    )


def _human(count: int) -> str:
    if count < 1024:
        return f"{count} B"
    if count < 1024 * 1024:
        return f"{count / 1024:.1f} KB"
    return f"{count / (1024 * 1024):.1f} MB"


def print_outcome(outcome: Outcome) -> None:
    parts = [f"{len(outcome.fetched)} fetched ({_human(outcome.bytes_fetched)})"]
    if outcome.resumed:
        parts.append(f"{len(outcome.resumed)} resumed")
    if outcome.restarted:
        parts.append(f"{len(outcome.restarted)} restarted")
    if outcome.skipped:
        parts.append(f"{len(outcome.skipped)} already had")
    parts.append(f"{len(outcome.confirmed)} confirmed")
    if outcome.bytes_wire != outcome.bytes_fetched:
        # Identity windows, and the attempts that were thrown away. Over car WiFi
        # the difference between what was paid for and what was kept is the
        # number that explains a slow run.
        parts.append(f"{_human(outcome.bytes_wire)} off the wire")
    print(f"{outcome.device}: " + ", ".join(parts))
    for name, reason in outcome.failed:
        print(f"  LEFT   {name}: {reason}", file=sys.stderr)


def cmd_pull(args) -> int:
    pull = Collector(
        args.url,
        args.into,
        device=args.device,
        timeout=args.timeout,
        retries=args.retries,
        backoff=args.backoff,
    )
    outcome = pull.collect()
    if pull.unreachable:
        # Not a chunk report: nothing was listed, so counting zeroes would read
        # like a clean run against a parked car with nothing to give.
        why = outcome.failed[0][1] if outcome.failed else "no answer"
        print(f"{args.url}: unreachable — {why}", file=sys.stderr)
        return 2
    print_outcome(outcome)
    return 1 if outcome.failed else 0


def cmd_index(args) -> int:
    """What the device says it is holding. Read-only: nothing is confirmed, so
    this is the safe thing to point at a car mid-drive."""
    pull = Collector(args.url, ".", timeout=args.timeout, retries=args.retries, backoff=0.5)
    try:
        chunks = pull.index()
    except _Unreachable as err:
        print(f"{args.url}: {err}", file=sys.stderr)
        return 2
    total = sum(chunk.size for chunk in chunks)
    print(f"{pull._name()}: {len(chunks)} sealed chunk(s), {_human(total)}")
    for chunk in chunks:
        span = "-"
        if chunk.first_t_us is not None and chunk.last_t_us is not None:
            span = f"{(chunk.last_t_us - chunk.first_t_us) / 1e6:.1f}s"
        print(f"  {chunk.name}  seq={chunk.seq}  {_human(chunk.size):>9}  span={span}")
    for name, why in pull.index_problems:
        print(f"  SKIPPED {name}: {why}", file=sys.stderr)
    return 1 if pull.index_problems else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pull sealed sd_logger chunks off a device.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    pull = sub.add_parser("pull", help="fetch, verify and confirm every sealed chunk")
    pull.add_argument("url", help="device base url, e.g. http://mr-orange.local:8080")
    pull.add_argument("--into", required=True, help="archive root; chunks land in <into>/<device>/")
    pull.add_argument("--device", help="archive under this name instead of the one the index gives")
    pull.add_argument(
        "--retries",
        type=int,
        default=DEFAULT_RETRIES,
        help=f"attempts per chunk per run, not retries after the first (default {DEFAULT_RETRIES})",
    )
    pull.add_argument("--backoff", type=float, default=DEFAULT_BACKOFF, help="seconds, doubling")
    pull.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    pull.set_defaults(func=cmd_pull)

    index = sub.add_parser("index", help="list what the device is holding, confirm nothing")
    index.add_argument("url")
    index.add_argument("--retries", type=int, default=DEFAULT_RETRIES)
    index.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    index.set_defaults(func=cmd_index)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
