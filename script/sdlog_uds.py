#!/usr/bin/env python3
"""Diagnostic answers in sd_logger chunks → signals and events (docs/sdlog-decode-spec.md §7).

A device that polls an ECU logs the answers as plain CAN frames. This reassembles them as
ISO-TP on the ECU's response id and decodes them with the same ``.dcat`` catalog the device
was flashed with, read by ``components/uds/catalog.py``: one artifact, two readers, so no bit
position, scale or sentinel is ever restated here.

Used by ``sdlog_decode.py``; one ``UdsEcu`` per ``[[device.uds]]`` entry (loaded once per run),
one ``UdsChunk`` per chunk (every chunk starts fresh, spec §7.1). Standard library only (U16).
"""

from __future__ import annotations

import difflib
import importlib.util
import math
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - the host is too old, say so plainly
    sys.exit("sdlog_uds.py needs Python >= 3.11 (tomllib)")

# The catalog reader is shared with the ESPHome codegen and script/uds_catalog.py, and loaded by
# path under the name uds_catalog.py uses, so both tools in one process share one module.
_CATALOG_PATH = Path(__file__).absolute().parent.parent / "components" / "uds" / "catalog.py"
if "uds_catalog_lib" in sys.modules:
    lib = sys.modules["uds_catalog_lib"]
else:
    _spec = importlib.util.spec_from_file_location("uds_catalog_lib", _CATALOG_PATH)
    lib = importlib.util.module_from_spec(_spec)
    sys.modules["uds_catalog_lib"] = lib  # dataclasses needs the module registered
    _spec.loader.exec_module(lib)

# ISO 14229-1 negative response codes.
NRC_NAMES = {
    0x10: "generalReject",
    0x11: "serviceNotSupported",
    0x12: "subFunctionNotSupported",
    0x13: "incorrectMessageLengthOrInvalidFormat",
    0x14: "responseTooLong",
    0x21: "busyRepeatRequest",
    0x22: "conditionsNotCorrect",
    0x24: "requestSequenceError",
    0x25: "noResponseFromSubnetComponent",
    0x26: "failurePreventsExecutionOfRequestedAction",
    0x31: "requestOutOfRange",
    0x33: "securityAccessDenied",
    0x35: "invalidKey",
    0x36: "exceededNumberOfAttempts",
    0x37: "requiredTimeDelayNotExpired",
    0x70: "uploadDownloadNotAccepted",
    0x71: "transferDataSuspended",
    0x72: "generalProgrammingFailure",
    0x73: "wrongBlockSequenceCounter",
    0x78: "requestCorrectlyReceivedResponsePending",
    0x7E: "subFunctionNotSupportedInActiveSession",
    0x7F: "serviceNotSupportedInActiveSession",
}

KEYS = {"label", "dcat", "ecu", "names", "dtc"}
_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")
_DTC_CODE = re.compile(r"[PCBU][0-9A-F]{6}\Z")
_DTC_LETTERS = "PCBU"


def is_state(field) -> bool:
    """A pure enumeration: every valid scale row names its state (at least one). A measurement
    with a text row beside its linear range ("value not defined") is not a state (spec §7.4)."""
    valid = [s for s in field.scales if not s.invalid]
    return bool(valid) and all(s.text for s in valid)


class UdsConfigError(Exception):
    """A `[[device.uds]]` entry cannot be trusted to mean what it says (exit 2)."""


@dataclass(frozen=True)
class UdsSpec:
    """One `[[device.uds]]` entry as configured: files checked to exist, nothing parsed yet."""

    label: str
    dcat: Path
    ecu: str | None = None
    names: Path | None = None
    dtc: Path | None = None

    def hash_parts(self) -> list[bytes]:
        """What the catalog hash covers for this entry (U13)."""
        parts = [self.label.encode(), self.dcat.read_bytes(), (self.ecu or "").encode()]
        for path in (self.names, self.dtc):
            parts.append(path.read_bytes() if path is not None else b"")
        return parts


def parse_spec(entry: object, where: str) -> UdsSpec:
    if not isinstance(entry, dict):
        raise UdsConfigError(f"{where}: uds must be [[device.uds]] tables")
    unknown = set(entry) - KEYS
    if unknown:
        raise UdsConfigError(f"{where}: uds: unknown key(s): {', '.join(sorted(unknown))}")
    label = entry.get("label")
    if not isinstance(label, str) or not label:
        raise UdsConfigError(f"{where}: uds: label is required")
    if not isinstance(entry.get("dcat"), str):
        raise UdsConfigError(f"{where}: uds {label!r}: dcat is required")
    paths: dict[str, Path | None] = {}
    for key in ("dcat", "names", "dtc"):
        value = entry.get(key)
        if value is None:
            paths[key] = None
            continue
        if not isinstance(value, str) or not Path(value).is_file():
            raise UdsConfigError(f"{where}: uds {label!r}: {key} file not found: {value}")
        paths[key] = Path(value)
    ecu = entry.get("ecu")
    if ecu is not None and (not isinstance(ecu, str) or not ecu):
        raise UdsConfigError(f"{where}: uds {label!r}: ecu must be a name")
    return UdsSpec(label=label, dcat=paths["dcat"], ecu=ecu, names=paths["names"], dtc=paths["dtc"])


# ------------------------------------------------------------------------ one ECU, loaded


def dtc_code(record: bytes) -> str:
    """SAE notation: letter and four digits from the first two bytes, failure type as two hex digits."""
    hi, mid, low = record[0], record[1], record[2]
    return f"{_DTC_LETTERS[hi >> 6]}{(hi >> 4) & 0x3}{hi & 0xF:X}{mid:02X}{low:02X}"


def load_dtc_texts(path: Path) -> dict[str, str]:
    texts: dict[str, str] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        code, sep, text = line.partition("\t")
        code = code.strip().upper()
        if not sep or not _DTC_CODE.match(code):
            raise UdsConfigError(f"{path}:{number}: expected CODE<TAB>text with an SAE code like P056200")
        texts[code] = text.strip()
    return texts


class UdsEcu:
    """One `[[device.uds]]` entry, loaded: the ECU, its answer lookup, the signal names."""

    def __init__(self, spec: UdsSpec, where: str) -> None:
        self.spec = spec
        self.label = spec.label
        try:
            catalog = lib.read_catalog(spec.dcat.read_bytes())
        except lib.CatalogError as err:
            raise UdsConfigError(f"{where}: uds {spec.label!r}: {spec.dcat} is not a catalog: {err}") from None
        if spec.ecu is None:
            if len(catalog.ecus) != 1:
                names = ", ".join(e.name for e in catalog.ecus)
                raise UdsConfigError(f"{where}: uds {spec.label!r}: the catalog holds several ECUs ({names}), name one with ecu")
            self.ecu = catalog.ecus[0]
        else:
            found = [e for e in catalog.ecus if e.name == spec.ecu]
            if not found:
                names = ", ".join(e.name for e in catalog.ecus)
                raise UdsConfigError(f"{where}: uds {spec.label!r}: ecu {spec.ecu!r} is not in {spec.dcat.name} ({names})")
            self.ecu = found[0]
        self.catalog = catalog
        self.source = self.ecu.name
        # positive answer prefix -> group index: `request[0] + 0x40`, then the echoed request bytes
        self.answers: dict[bytes, int] = {}
        for gi, group in enumerate(self.ecu.groups):
            self.answers.setdefault(bytes([(group.sid + 0x40) & 0xFF]) + group.request[1:], gi)
        self.prefix_lengths = sorted({len(k) for k in self.answers}, reverse=True)
        self.names = self._names(where)
        self.dtc_texts = load_dtc_texts(spec.dtc) if spec.dtc is not None else {}
        self.state_fields = [[is_state(f) for f in g.fields] for g in self.ecu.groups]

    def _group_index(self, group) -> int:
        return next(i for i, g in enumerate(self.ecu.groups) if g is group)

    def _resolve_field(self, key: str, where: str) -> tuple[int, int]:
        service, sep, field_name = key.rpartition("/")
        try:
            if sep:
                if re.fullmatch(r"0[xX][0-9A-Fa-f]{1,4}", service):
                    did = int(service, 16)
                    groups = [i for i, g in enumerate(self.ecu.groups) if g.did == did]
                    if not groups:
                        raise UdsConfigError(f"{where}: names: {key!r}: no group with DID {service} in ECU {self.source}")
                    gi = groups[0]
                else:
                    resolution = self.catalog.resolve(service, "group")
                    if resolution.ecu is not self.ecu:
                        raise UdsConfigError(f"{where}: names: {key!r}: group {service!r} is not in ECU {self.source}")
                    gi = self._group_index(resolution.obj)
                fields = [i for i, f in enumerate(self.ecu.groups[gi].fields) if f.name == field_name]
                if not fields:
                    raise UdsConfigError(f"{where}: names: {key!r}: group {self.ecu.groups[gi].name!r} has no field {field_name!r}")
                return gi, fields[0]
            hits = [
                (gi, fi)
                for gi, g in enumerate(self.ecu.groups)
                for fi, f in enumerate(g.fields)
                if f.name == field_name
            ]
            if not hits:
                close = difflib.get_close_matches(field_name, [f.name for g in self.ecu.groups for f in g.fields], n=3)
                hint = f" (close: {', '.join(close)})" if close else ""
                raise UdsConfigError(f"{where}: names: {key!r}: no such field in ECU {self.source}{hint}")
            if len(hits) > 1:
                groups = ", ".join(self.ecu.groups[gi].name for gi, _ in hits)
                raise UdsConfigError(f"{where}: names: {key!r} is ambiguous, write <service>/<field> (groups: {groups})")
            return hits[0]
        except lib.CatalogError as err:
            raise UdsConfigError(f"{where}: names: {key!r}: {err}") from None

    def _names(self, where: str) -> dict[tuple[int, int], str]:
        """(group index, field index) -> signal name without element suffix (spec §7.2)."""
        aliases: dict[tuple[int, int], str] = {}
        if self.spec.names is not None:
            try:
                raw = tomllib.loads(self.spec.names.read_text(encoding="utf-8"))
            except (OSError, tomllib.TOMLDecodeError) as err:
                raise UdsConfigError(f"{where}: names: {self.spec.names}: {err}") from None
            unknown = set(raw) - {"fields"}
            if unknown:
                raise UdsConfigError(f"{where}: names: unknown key(s): {', '.join(sorted(unknown))}")
            fields = raw.get("fields", {})
            if not isinstance(fields, dict):
                raise UdsConfigError(f"{where}: names: [fields] must be a table of field = name")
            taken: dict[str, str] = {}
            for key, alias in fields.items():
                if not isinstance(alias, str) or not _NAME.match(alias):
                    raise UdsConfigError(f"{where}: names: {key!r}: {alias!r} is not a name ([A-Za-z][A-Za-z0-9_]*)")
                if alias in taken:
                    raise UdsConfigError(f"{where}: names: {alias!r} is given to both {taken[alias]!r} and {key!r}")
                taken[alias] = key
                target = self._resolve_field(key, where)
                if target in aliases:
                    raise UdsConfigError(f"{where}: names: {key!r} names a field that already has a name")
                aliases[target] = alias
        names: dict[tuple[int, int], str] = {}
        seen: dict[str, tuple[int, int]] = {}
        for gi, group in enumerate(self.ecu.groups):
            for fi, field in enumerate(group.fields):
                base = f"{self.source}.{aliases[(gi, fi)]}" if (gi, fi) in aliases else f"{self.source}.{group.name}.{field.name}"
                width = len(str(field.repeat_count))
                full = [base] if field.repeat_count == 1 else [f"{base}.{k + 1:0{width}d}" for k in range(field.repeat_count)]
                for name in full:
                    if name in seen:
                        raise UdsConfigError(f"{where}: two fields end up as signal {name!r}; give one a name")
                    seen[name] = (gi, fi)
                names[(gi, fi)] = base
        return names


# ------------------------------------------------------------------------ one chunk


@dataclass
class _Rx:
    """ISO-TP receive state on one ECU's response id, plus what the events follow."""

    started: bool = False  # a single or first frame has been seen in this chunk
    buf: bytearray | None = None
    need: int = 0
    sn: int = 0
    utc: int | None = None
    quiet: bool = False  # an isotp event was raised for the frames that follow; do not repeat it
    request: bytes | None = None  # the last logged single-frame request (log_tap_tx)


Emit = Callable[..., None]


def _fmt(value: float, text: str) -> str:
    if text:
        return text
    if value is None or math.isnan(value):
        return "unavailable"
    return f"{value:g}"


class UdsChunk:
    """Feeds one chunk's CAN frames through every configured ECU (spec §7)."""

    def __init__(self, ecus: list[UdsEcu], signal: Emit, event: Emit) -> None:
        self.routes: dict[tuple[str, int, bool], tuple[UdsEcu, str]] = {}
        for e in ecus:
            self.routes[(e.label, e.ecu.response_id, e.ecu.extended_id)] = (e, "rx")
            self.routes[(e.label, e.ecu.request_id, e.ecu.extended_id)] = (e, "tx")
        self.rx = {id(e): _Rx() for e in ecus}
        self.states: dict[int, dict[str, tuple]] = {id(e): {} for e in ecus}
        self.dtcs: dict[int, dict[bytes | None, dict[str, int]]] = {id(e): {} for e in ecus}
        self.signal = signal  # (name, utc, value, text)
        self.event = event  # (utc, source, kind, name, value, text, detail)
        self.counts: Counter[str] = Counter()
        self.events = 0

    @staticmethod
    def routes_of(ecus: list[UdsEcu]) -> dict[tuple[str, int, bool], str]:
        """Every routed (label, id, extended) and its ECU, to refuse two entries on one id (U1)."""
        out: dict[tuple[str, int, bool], str] = {}
        for e in ecus:
            for can_id in (e.ecu.request_id, e.ecu.response_id):
                key = (e.label, can_id, e.ecu.extended_id)
                if key in out:
                    raise UdsConfigError(f"uds: {e.source} and {out[key]} both use id 0x{can_id:X} on {e.label!r}")
                out[key] = e.source
        return out

    def _event(self, e: UdsEcu, utc, kind: str, name: str, value: float, text: str = "", detail: str = "") -> None:
        self.events += 1
        self.event(utc, e.source, kind, name, value, text, detail)

    def feed(self, label: str, can_id: int, extended: bool, data_hex: str, utc: int | None) -> bool:
        """One CAN frame. True when it is an ECU's request or answer (counts as matched, U12)."""
        route = self.routes.get((label, can_id, extended))
        if route is None:
            return False
        e, role = route
        data = bytes.fromhex(data_hex)
        rx = self.rx[id(e)]
        if not data:
            self.counts["bad frames"] += 1
            return True
        pci = data[0] >> 4
        if role == "tx":
            if pci == 0 and 1 <= (data[0] & 0xF) <= len(data) - 1:
                rx.request = bytes(data[1:1 + (data[0] & 0xF)])
            elif pci == 1:
                rx.request = None  # a multi-frame request: not followed
            return True
        if pci == 0:
            length = data[0] & 0xF
            if length == 0 or length > len(data) - 1:
                self.counts["bad frames"] += 1
                return True
            self._abandon(e, rx, utc)
            rx.started, rx.quiet = True, False
            self._message(e, rx, bytes(data[1:1 + length]), utc)
        elif pci == 1:
            length = ((data[0] & 0xF) << 8) | data[1] if len(data) >= 2 else 0
            if length < 8 or len(data) < 8:
                self.counts["bad frames"] += 1
                return True
            self._abandon(e, rx, utc)
            rx.started, rx.quiet = True, False
            rx.buf, rx.need, rx.sn = bytearray(data[2:]), length, 1
        elif pci == 2:
            if rx.buf is None:
                if not rx.started:
                    self.counts["frames cut at the start"] += 1
                elif not rx.quiet:
                    rx.quiet = True
                    self._event(e, utc, "isotp", "orphan", float(len(data) - 1), "", "consecutive frame without a first frame")
                return True
            sn = data[0] & 0xF
            if sn != rx.sn:
                got = len(rx.buf)
                rx.buf, rx.quiet = None, True
                self._event(e, utc, "isotp", "sequence", float(got), "", f"expected SN {rx.sn:X}, got {sn:X}")
                return True
            rx.buf += data[1:]
            rx.sn = (rx.sn + 1) & 0xF
            if len(rx.buf) >= rx.need:
                message = bytes(rx.buf[:rx.need])
                rx.buf = None
                self._message(e, rx, message, utc)
        elif pci == 3:
            pass  # flow control from the ECU: our own multi-frame request, nothing to decode
        else:
            self.counts["bad frames"] += 1
        return True

    def _abandon(self, e: UdsEcu, rx: _Rx, utc) -> None:
        if rx.buf is not None:
            got = len(rx.buf)
            rx.buf = None
            self._event(e, utc, "isotp", "incomplete", float(got), "", f"{got} of {rx.need} bytes, then a new message")

    def finish(self) -> None:
        for rx in self.rx.values():
            if rx.buf is not None:
                rx.buf = None
                self.counts["frames cut at the end"] += 1

    # -------------------------------------------------------------------- one message

    def _message(self, e: UdsEcu, rx: _Rx, msg: bytes, utc) -> None:
        self.counts["answers"] += 1
        if msg[0] == 0x7F:
            if len(msg) < 3:
                self.counts["unknown answers"] += 1
                return
            sid, nrc = msg[1], msg[2]
            if nrc == 0x78:
                self.counts["pending"] += 1
                return
            request = rx.request if rx.request and rx.request[0] == sid else None
            name = request.hex(" ").upper() if request else f"SID {sid:02X}"
            self._event(e, utc, "nrc", name, float(nrc), NRC_NAMES.get(nrc, ""))
            rx.request = None
            return
        if len(msg) >= 3 and msg[0] == 0x59 and msg[1] == 0x02:
            request = rx.request if rx.request and rx.request[:2] == b"\x19\x02" else None
            self._dtcs(e, msg, utc, request)
            rx.request = None
            return
        gi = None
        for length in e.prefix_lengths:
            gi = e.answers.get(msg[:length])
            if gi is not None:
                break
        if gi is None:
            self.counts["unknown answers"] += 1
            return
        rx.request = None
        self._group(e, gi, msg, utc)

    def _group(self, e: UdsEcu, gi: int, msg: bytes, utc) -> None:
        group = e.ecu.groups[gi]
        bits = len(msg) * 8
        numeric = available = 0
        partial = False
        is_state_field = e.state_fields[gi]
        for fi, f in enumerate(group.fields):
            base = e.names[(gi, fi)]
            width = len(str(f.repeat_count))
            for k in range(f.repeat_count):
                start = f.bit_pos + k * f.repeat_stride
                if start + f.bit_size > bits:
                    partial = True
                    self.counts["values past the answer's end"] += 1
                    continue
                name = base if f.repeat_count == 1 else f"{base}.{k + 1:0{width}d}"
                if f.ascii or f.hexdump:
                    if start % 8 or f.bit_size % 8:
                        self.counts["misaligned text fields"] += 1  # the device refuses these too
                        continue
                    raw = msg[start // 8:(start + f.bit_size) // 8]
                    text = raw.split(b"\0", 1)[0].decode("latin-1") if f.ascii else raw.hex().upper()
                    self.signal(name, utc, math.nan, text)
                    continue
                value, text = lib.evaluate(f, lib.extract(f, msg, k))
                numeric += 1
                if value is None:
                    self.counts["unavailable values"] += 1
                    if is_state_field[fi]:
                        self._state(e, name, math.nan, text or "", utc)
                    continue
                available += 1
                self.signal(name, utc, value, text or "")
                if is_state_field[fi]:
                    self._state(e, name, value, text or "", utc)
        if partial:
            self.counts["partial answers"] += 1
        if numeric and not available:
            self._event(e, utc, "unavailable", group.name, float(numeric))

    def _state(self, e: UdsEcu, name: str, value: float, text: str, utc) -> None:
        key = (None if math.isnan(value) else value, text)
        states = self.states[id(e)]
        old = states.get(name)
        if old == key:
            return
        states[name] = key
        detail = "initial" if old is None else f"from {_fmt(math.nan if old[0] is None else old[0], old[1])}"
        self._event(e, utc, "state", name, value, text, detail)

    def _dtcs(self, e: UdsEcu, msg: bytes, utc, request: bytes | None) -> None:
        records = msg[3:]
        if len(records) % 4:
            self.counts["torn fault records"] += 1
        current: dict[str, int] = {}
        for i in range(0, len(records) - len(records) % 4, 4):
            current[dtc_code(records[i:i + 3])] = records[i + 3]
        known = self.dtcs[id(e)]
        previous = known.get(request)
        for code, status in current.items():
            if previous is None:
                detail = "initial"
            elif code not in previous:
                detail = "new"
            elif previous[code] != status:
                detail = f"status 0x{previous[code]:02X}->0x{status:02X}"
            else:
                continue
            self._event(e, utc, "dtc", code, float(status), e.dtc_texts.get(code, ""), detail)
        for code in previous or ():
            if code not in current:
                self._event(e, utc, "dtc", code, 0.0, e.dtc_texts.get(code, ""), "cleared")
        known[request] = current

    def summary(self) -> str:
        """The counts that are not events, for `chunks.detail` (spec §7.4)."""
        if not self.counts and not self.events:
            return ""
        order = [
            "answers", "partial answers", "unknown answers", "pending", "unavailable values",
            "values past the answer's end", "frames cut at the start", "frames cut at the end",
            "bad frames", "misaligned text fields", "torn fault records",
        ]
        parts = [f"{self.counts[k]} {k}" for k in order if self.counts[k]]
        parts.append(f"{self.events} events")
        return "uds: " + ", ".join(parts)
