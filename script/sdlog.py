#!/usr/bin/env python3
"""Read back an sd_logger card file (write format v2, docs/sd_logger-spec.md §6).

The card is the only artifact that survives a run, and a bench operator's two
questions about one are "is it intact?" and "did anything get dropped?". Both are
answerable from the file itself and neither is answerable by eye once a file runs
to 32 MB, which is what this exists for.

WHAT IT DECIDES, AND WHY EACH ONE IS A DECISION AND NOT A STATISTIC

    a missing `#close`      THE POWER-CUT SIGNATURE. Every orderly end to a file
                            — rotation, shutdown, the VCC emergency close —
                            writes `#close` as its last line. So a file without
                            one was cut, and the manual bench criterion in
                            tests/hil/HIL.md ("parses as valid CSV up to the last
                            line") becomes something a script can answer. This is
                            what makes the §10.7 power-loss procedure mechanical.

    a torn trailing line    Expected after a power cut, and harmless: the
                            formatter never emits a bare newline except the line
                            terminator, so exactly one partial line can exist and
                            everything before it is sound. Reported as `torn`,
                            not as a parse error.

    `#drop` markers         A gap in the traffic that the logger knows about. The
                            three counters stay separate on purpose: `ring` is a
                            producer that found the logger's ring full, `tap:<x>`
                            is the gateway's RX ISR that found a tap ring full,
                            `text` is a log line that found the text ring full.
                            They name different bottlenecks with different fixes,
                            so this never sums them.

    `#gap` markers          Retention's in-band statement that whole SEALED
                            chunks were deleted off the card before anything
                            collected them (design §7). A different loss from a
                            `#drop` in both unit and cause — files, not records —
                            so it is reported apart and never summed with one.

                            `first_seq`/`last_seq` are the ENDPOINTS OF THE
                            WINDOW retention emptied, in the order it emptied it
                            (`collection_policy.h take_pending_discards()`), and
                            never a promise that every seq between them is
                            missing. `chunks` is how many actually went away.
                            When `chunks != span` the window is PUNCTURED: the
                            seqs inside it that are not among those `chunks` were
                            collected safely and are sitting in the archive. This
                            reader therefore states the loss and the window as two
                            separate numbers. Over-reporting loss is the one thing
                            the marker exists to prevent, so the span is never
                            printed as a chunk count.

                            The endpoints are unsorted on purpose, so `first >
                            last` is a legal window that wrapped `seq` at
                            10 000 000 — three seqs wide, not the whole card.

    a malformed line        Anywhere other than the very end, this is a real
                            defect — a formatter bug, or a card that returned
                            garbage. It is the finding the exit status is for.

    a format it does not    Refused BY NAME, not parsed — and **v1 is one of
    know                    them**. v2 changed the stamp column's base and
                            dropped `dlc`, so a v1 line and a v2 line are both
                            well-formed CSV and mean different things; there is no
                            version-blind reading that is not a guess. A reader
                            that only printed the version would hand the wrong
                            parser a file and report every record "malformed",
                            which reads exactly like a corrupt card and sends
                            someone hunting a hardware fault that is not there.
                            Exits 2, the same as a file that cannot be read,
                            because that is what it is.

THE LINE (spec §6 F1a/F1h)

    <K><tag><flags>,t,id,data         C1x,1068,1A2,0011223344556677
    X<level>,t,tag,message            XI,72,sd_logger,records=4120

    Every number on a stream line is **uppercase hex**. `t` is the stamp column:
    `@<hex>` is an absolute µs anchor, a bare `<hex>` is the step from the
    previous stream line of the same file. The chain runs over every record and
    text line in file order; the `#` meta lines carry `@<hex>` absolutes and stay
    outside it, so a reader that skips `#drop` still decodes the records after it.
    Every chunk anchors its own first stream line, so a chunk pulled off the
    collection server decodes without the chunks around it.

    There is no `dlc` column: it is `len(data)/2`, which is where it always came
    from. Meta lines keep decimal counts and seqs, because those match the decimal
    in a filename and in a stats line.

    Everything this reader *reports* — spans, `#gap` and `#drop` times, extracted
    CSV — is in absolute decimal µs. The encoding is a byte budget on the card, not
    something a caller should have to know about.

Usage:
    script/sdlog.py check   L0000007.LOG [...]      # verdict per file, exit 1 on a defect
    script/sdlog.py check   /Volumes/SD/*.LOG       # a whole card
    script/sdlog.py extract L0000007.LOG --type C   # one type as a plain CSV
    script/sdlog.py extract L0000007.LOG --type C --label seg1 --shed
    script/sdlog.py head    L0000007.LOG            # just the #src/#types header

Exit status: 0 clean, 1 a defect was found (malformed line, or a `#drop`/`#gap`
with --strict), 2 a file could not be read. A missing `#close` on its own is
reported but does not fail — pulling the power is a *test*, and the expected
result of it. A `#gap` does not fail by default either, and for the same kind of
reason: a card that ran long enough for retention to fire is a working card doing
what the design asks of it, and the file it left behind is intact.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Type letters and their field counts, from spec §6 F1a/F1c. A record line is one
# layout for every bus, which is the whole point of the type letter: one parser
# covers CAN, LIN, user and (later) isotp.
RECORD_KINDS = {"C", "L", "U", "I"}
RECORD_FIELDS = 4  # <K><tag><flags>,t,id,data
TEXT_FIELDS = 4  # X<level>,t,tag,message

# Lowercase and `~`, never a hex digit — which is what makes the leading token
# self-delimiting (spec §6 F1a). v1's `-` for "no flags" is gone with the column it
# padded: no flags now means no characters.
FLAG_LETTERS = set("xrts~")

# The write formats this reader understands (`components/sd_logger/log_format.h`
# SD_LOG_FORMAT_VERSION). Exactly one, and that is deliberate: v1 wrote a decimal
# absolute stamp and a `dlc` column, so a v1 line parses perfectly as a v2 line and
# means something else. There is no version-blind reading of the two that is not a
# guess, so anything not in here is refused by name.
SUPPORTED_FORMAT_VERSIONS = (2,)

# Prefixes an absolute stamp; a bare number in that column is a step.
ANCHOR_MARK = "@"

# Every number on a stream line is uppercase hex. Lowercase is not accepted: the
# formatter cannot emit it, so taking it would hide a real defect the same way
# `int()` accepting '+1' would.
HEX_DIGITS = set("0123456789ABCDEF")

# `data` holds at most 8 bytes, so at most 16 hex characters (F1a).
MAX_DATA_HEX = 16

# `seq` wraps here (components/sd_logger/collection_policy.h SD_LOG_SEQ_MODULUS),
# which is why a `#gap` window's width is modular arithmetic and not subtraction.
SEQ_MODULUS = 10_000_000


@dataclass
class Source:
    """One `#src` line: what the file says a label means."""

    kind: str
    label: str
    tag: int
    origin: str
    detail: str


@dataclass
class Drop:
    t_us: int
    what: str
    delta: int
    total: int


@dataclass
class Gap:
    """One `#gap` line: the window retention emptied, and what it cost.

    `chunks` is the loss. `first_seq`/`last_seq` are only the two ends of the
    window, in the order retention emptied it — never a claim that everything
    between them went away. Keeping those two facts in separate fields is the
    whole point of the record; see the module docstring.
    """

    t_us: int
    chunks: int
    bytes: int
    first_seq: int
    last_seq: int

    @property
    def span(self) -> int:
        """How many seqs wide the window is. Wrap-aware: the endpoints are
        emitted unsorted, so `first > last` is a short window that crossed the
        `seq` wrap and not — as `last - first + 1` would have it — a negative
        one, nor, if they were sorted first, the entire card."""
        if self.last_seq >= self.first_seq:
            return self.last_seq - self.first_seq + 1
        return SEQ_MODULUS - self.first_seq + self.last_seq + 1

    @property
    def punctured(self) -> bool:
        """Fewer chunks discarded than the window is wide: some seqs inside it
        were collected before retention got to them and are in the archive. A
        punctured window must never be reported as a solid one."""
        return self.chunks != self.span


@dataclass
class Report:
    path: Path
    format_version: int | None = None
    seq: int | None = None
    esphome_version: str = ""
    sources: list[Source] = field(default_factory=list)
    counts: dict[str, int] = field(default_factory=dict)
    labels: dict[str, int] = field(default_factory=dict)
    drops: list[Drop] = field(default_factory=list)
    gaps: list[Gap] = field(default_factory=list)
    rotate_to: str = ""
    close_reason: str | None = None
    torn_tail: bool = False
    bad_lines: list[tuple[int, str]] = field(default_factory=list)
    first_t_us: int | None = None
    last_t_us: int | None = None
    total_lines: int = 0
    legacy: bool = False

    @property
    def dropped_total(self) -> int:
        return sum(drop.delta for drop in self.drops)

    @property
    def discarded_chunks_total(self) -> int:
        """Files retention deleted. Deliberately not summed with `dropped_total`:
        that one counts records a ring could not hold."""
        return sum(gap.chunks for gap in self.gaps)

    @property
    def discarded_bytes_total(self) -> int:
        """Bytes those files held. A day of retention at 148 KB/s overflows 32
        bits, so this stays a Python int and is never narrowed."""
        return sum(gap.bytes for gap in self.gaps)

    def label_for(self, tag: int, token: str) -> str:
        """What `#src` says this tag is called, else the token as written.

        The label moved off the record line into the header — stated once per file
        instead of ~3600 times a second — so resolving it is the reader's job now.
        An undeclared tag keeps its token rather than being dropped or renamed: that
        is the same D1 rule the writer follows, and it means a lambda calling
        `log_frame(9, …)` still shows up in the counts as `U9`."""
        for src in self.sources:
            if src.tag == tag:
                return src.label
        return token

    @property
    def unsupported_version(self) -> bool:
        """A `#sdlog` header naming a write format this reader does not have. The
        file is not broken and this reader is not the one to say anything else
        about it — nothing below the header was parsed."""
        return self.format_version is not None and self.format_version not in SUPPORTED_FORMAT_VERSIONS

    @property
    def clean(self) -> bool:
        """Malformed lines are a defect. A missing `#close` is a finding about the
        run, not about the file's integrity, so it is not counted here — and a
        `#drop` or a `#gap` is a finding about the run for the same reason."""
        return not self.bad_lines


def _parse_u(text: str) -> int:
    """Strict unsigned decimal. `int()` would accept '+1', ' 1' and '1_0'; the
    formatter emits none of those, so accepting them would hide a real defect."""
    if not text or not text.isdigit():
        raise ValueError(f"not a decimal number: {text!r}")
    return int(text)


def _parse_x(text: str) -> int:
    """Strict uppercase hex, the base of every number on a stream line. As strict as
    `_parse_u` and for the same reason: `int(text, 16)` takes '0x1A', '+1A' and
    lowercase, none of which the formatter can produce."""
    if not text or not set(text) <= HEX_DIGITS:
        raise ValueError(f"not an uppercase hex number: {text!r}")
    return int(text, 16)


def _parse_abs_stamp(text: str) -> int:
    """A meta line's timestamp: always absolute, so always anchor-marked."""
    if not text.startswith(ANCHOR_MARK):
        raise ValueError(f"meta timestamp {text!r} is not anchor-marked")
    return _parse_x(text[len(ANCHOR_MARK) :])


class Chain:
    """The stamp column, decoded to absolute µs (spec §6 F1h).

    `@<hex>` is an anchor and a bare `<hex>` is a step, so this carries one running
    value over every `C`/`L`/`U`/`I`/`X` line **in file order**, whatever type is
    being asked for.

    Meta lines are deliberately not fed through here. They carry an anchor-marked
    absolute and stay outside the chain, which is what lets a reader skip a `#drop`
    it does not care about and still decode the records after it — if they moved the
    chain, a reader that filters markers and one that does not would disagree about
    every timestamp after one.
    """

    def __init__(self) -> None:
        self.prev = 0
        self.started = False

    def read(self, text: str) -> int:
        """One stamp column to absolute µs, advancing the chain. Raises
        `ValueError` for anything the formatter cannot have written."""
        if text.startswith(ANCHOR_MARK):
            self.prev = _parse_x(text[len(ANCHOR_MARK) :])
            self.started = True
            return self.prev
        value = _parse_x(text)
        if not self.started:
            # Every chunk anchors its own first stream line, so a step with
            # nothing to step from means the anchor is gone. The times behind it
            # are unrecoverable, and reporting them as if they had been read would
            # put confident wrong numbers in front of someone.
            raise ValueError(f"delta timestamp {text!r} before any anchor")
        self.prev += value
        return self.prev


def split_token(token: str) -> tuple[str, int, str]:
    """The leading token `<K><tag><flags>` -> `(kind, tag, flags)`.

    `C1x` is CAN, source tag 1, extended. It is one token and not three columns
    because a comma only earns its place between fields whose width is not
    self-evident, and this one delimits itself: the tag is uppercase hex, the flag
    letters are lowercase or `~`, and the two alphabets do not overlap. So a reader
    takes hex characters until one is not, and everything left is flags — which is
    why `flags()` on the writing side is explicit that its alphabet is load-bearing.
    """
    kind = token[0]
    end = 1
    while end < len(token) and token[end] in HEX_DIGITS:
        end += 1
    tag = _parse_x(token[1:end])
    flags = token[end:]
    if not set(flags) <= FLAG_LETTERS:
        raise ValueError(f"unknown flag letters {flags!r} in token {token!r}")
    return kind, tag, flags


def _check_record(fields: list[str]) -> None:
    # The stamp column is checked by `Chain.read()`, which has to see every stream
    # line in order anyway — validating it twice would mean two places to keep the
    # anchor rule in. The token is checked by `split_token()`, for the same reason.
    if len(fields) != RECORD_FIELDS:
        raise ValueError(f"expected {RECORD_FIELDS} fields, got {len(fields)}")
    _parse_x(fields[2])  # id
    # The `dlc` column is gone (F1a): it was always `len(data)/2`, so the width is
    # the length and the only check left. An odd count is half a byte, which no
    # formatter can have produced.
    data = fields[3]
    if len(data) % 2:
        raise ValueError(f"data is {len(data)} hex chars, which is half a byte short")
    if len(data) > MAX_DATA_HEX:
        raise ValueError(f"data is {len(data) // 2} bytes, above the 8 a record holds")
    if data:
        _parse_x(data)


def _check_text(fields: list[str]) -> None:
    # The message is the last field and may contain commas, so it is never split.
    if len(fields) < TEXT_FIELDS:
        raise ValueError(f"expected at least {TEXT_FIELDS} fields, got {len(fields)}")
    # The level letter rides in the token, `X<level>`, so the token is two characters.
    if len(fields[0]) != 2:
        raise ValueError(f"a log line's token is X plus one level letter, got {fields[0]!r}")


def parse_file(path: Path) -> Report:
    report = Report(path=path)
    # Absolute until the `#sdlog` header says otherwise, which it does on the very
    # first line: a file with no header at all is pre-v1 and was never delta-coded.
    chain = Chain()
    raw = path.read_bytes()
    # Latin-1 never fails, and the format only ever escapes below 0x20 — bytes
    # >= 0x80 pass through so UTF-8 survives, and re-encoding it here would be
    # the one place a reader could corrupt what the card holds.
    text = raw.decode("latin-1")
    # A file cut by a power loss ends mid-line. Exactly one such line can exist,
    # because the formatter never writes a bare newline inside a line.
    ends_clean = text.endswith("\n")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    elif lines:
        report.torn_tail = True

    for number, line in enumerate(lines, start=1):
        is_last = number == len(lines)
        if not line:
            continue
        report.total_lines += 1
        # The `#pad` line is spaces to the next sector boundary; it carries no
        # information and its length is deliberately large.
        if line.startswith("#pad,"):
            continue
        # An M1-era file: a bare `# t_us,source,…` legend and no `#sdlog` header.
        # That legend is the only version marker such a file carries, which is
        # why the `.CSV` -> `.LOG` rename was worth its small cost. Named rather
        # than parsed — this reader is not a v0 parser, and walking a whole
        # pre-v1 file to call every line malformed would be noise, not a finding.
        if number == 1 and line.startswith("# "):
            report.legacy = True
            return report
        try:
            if line.startswith("#"):
                _parse_meta(report, line)
                if report.unsupported_version:
                    # Named, never parsed. Walking another format with this parser
                    # calls every record line malformed, and a wall of malformed
                    # lines reads as a card that returned garbage.
                    return report
                continue
            fields = line.split(",")
            kind = fields[0][0] if fields[0] else ""
            # The stamp comes first, and off *every* stream line: the chain is only
            # in step if it saw them all, so a line whose other fields are broken
            # still contributes its step. That is exactly what the writer did — it
            # advanced the chain when it wrote the line, not when the line turned
            # out to be well-formed.
            t_us = 0
            if kind in RECORD_KINDS or kind == "X":
                if len(fields) < 2:
                    raise ValueError(f"expected at least 2 fields, got {len(fields)}")
                t_us = chain.read(fields[1])
            if kind in RECORD_KINDS:
                _, tag, _flags = split_token(fields[0])
                _check_record(fields)
                _note_time(report, t_us)
                report.counts[kind] = report.counts.get(kind, 0) + 1
                # Counted under the name `#src` gives the tag, which is where the
                # label lives now: it is stated once per file instead of on every
                # line. A tag the header never declared is counted under the token
                # itself rather than dropped — the same D1 rule the writer follows.
                label = report.label_for(tag, fields[0])
                report.labels[label] = report.labels.get(label, 0) + 1
            elif kind == "X":
                _check_text(fields)
                _note_time(report, t_us)
                report.counts["X"] = report.counts.get("X", 0) + 1
            else:
                raise ValueError(f"unknown type letter {kind!r}")
        except ValueError as err:
            if is_last and report.torn_tail:
                continue  # the expected torn line, already reported as such
            report.bad_lines.append((number, f"{err}: {line[:80]!r}"))

    if not ends_clean and not lines:
        report.torn_tail = True
    return report


def _note_time(report: Report, t_us: int) -> None:
    if report.first_t_us is None:
        report.first_t_us = t_us
    report.last_t_us = t_us


def _parse_meta(report: Report, line: str) -> None:
    fields = line.split(",")
    head = fields[0]
    if head == "#sdlog":
        if len(fields) < 5:
            raise ValueError("truncated #sdlog header")
        report.format_version = _parse_u(fields[1])
        report.seq = _parse_u(fields[2])
        report.esphome_version = fields[4]
        # fields[3] is the open time, anchor-marked like every other meta stamp.
        # Checked only once the version is known to be one we read: a v1 header
        # carries a bare decimal there, and calling *that* a malformed line would be
        # the exact misdiagnosis the version check exists to prevent.
        if report.format_version in SUPPORTED_FORMAT_VERSIONS:
            _parse_abs_stamp(fields[3])
    elif head == "#src":
        if len(fields) < 6:
            raise ValueError("truncated #src line")
        report.sources.append(
            Source(
                kind=fields[1],
                label=fields[2],
                tag=_parse_x(fields[3]),
                origin=fields[4],
                detail=fields[5],
            )
        )
    elif head == "#drop":
        if len(fields) < 5:
            raise ValueError("truncated #drop line")
        report.drops.append(
            Drop(
                t_us=_parse_abs_stamp(fields[1]),
                what=fields[2],
                delta=_parse_u(fields[3]),
                total=_parse_u(fields[4]),
            )
        )
    elif head == "#gap":
        # Six fields, every one of them numeric and strict-unsigned: this marker is
        # the only record that the data it accounts for ever existed, so a line the
        # formatter cannot have produced is never half-read. Nothing is appended
        # until all five numbers have parsed.
        if len(fields) < 6:
            raise ValueError("truncated #gap line")
        report.gaps.append(
            Gap(
                t_us=_parse_abs_stamp(fields[1]),
                chunks=_parse_u(fields[2]),
                bytes=_parse_u(fields[3]),
                first_seq=_parse_u(fields[4]),
                last_seq=_parse_u(fields[5]),
            )
        )
    elif head == "#rotate":
        if len(fields) < 3:
            raise ValueError("truncated #rotate line")
        report.rotate_to = fields[2]
    elif head == "#close":
        if len(fields) < 3:
            raise ValueError("truncated #close line")
        report.close_reason = fields[2]
    elif head in ("#types", "#flags", "#layout"):
        pass  # the legend; self-describing by construction
    else:
        raise ValueError(f"unknown meta line {head!r}")


def _plural(count: int, noun: str) -> str:
    return noun if count == 1 else noun + "s"


def _bytes_human(count: int) -> str:
    """A byte total an operator can read at a glance. The exact number always stays
    printed next to it — a rounded one is a convenience, not evidence."""
    size = float(count)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024.0:
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024.0
    return f"{size:.1f} TiB"


def _gap_window(gap: Gap) -> str:
    """One window as the report states it: the two endpoints, plus — only when the
    window has a hole in it — how wide it actually is.

    The width is never spelled as a chunk count. A window that is three seqs wide
    after two chunks were discarded means the third was collected and is in the
    archive, so calling it "3 chunks" would report a loss that did not happen, and
    send someone hunting for a file that is not missing."""
    window = f"{gap.first_seq}..{gap.last_seq}"
    if not gap.punctured:
        return window
    return f"{window} (punctured: the window is {gap.span} seqs wide)"


def _duration(report: Report) -> str:
    if report.first_t_us is None or report.last_t_us is None:
        return "-"
    return f"{(report.last_t_us - report.first_t_us) / 1e6:.1f}s"


def print_report(report: Report, verbose: bool) -> None:
    name = report.path.name
    if report.legacy:
        print(f"{name}: format M1-era (.CSV legend, no #sdlog header) — not a v2 chunk")
    if report.unsupported_version:
        # One line and nothing else. Everything this function prints below is a
        # count of lines that were not read, and printing "no records, no drops"
        # about a file nobody parsed is worse than printing nothing.
        supported = ", ".join(f"v{v}" for v in SUPPORTED_FORMAT_VERSIONS)
        print(
            f"{name}: write format v{report.format_version}, which this reader does not know"
            f" (it reads {supported}) — nothing below the #sdlog header was parsed."
            " Update script/sdlog.py rather than reading the numbers off this file."
        )
        return
    kinds = " ".join(f"{k}={v}" for k, v in sorted(report.counts.items())) or "no records"
    print(f"{name}: v{report.format_version} seq={report.seq} {kinds} span={_duration(report)}")

    if verbose and report.sources:
        for src in report.sources:
            print(f"  src  {src.kind},{src.label} tag={src.tag} {src.origin} {src.detail}")
    if verbose and report.labels:
        print("  by label: " + " ".join(f"{k}={v}" for k, v in sorted(report.labels.items())))

    # Never summed: three counters, three bottlenecks, three different fixes.
    if report.drops:
        by_what: dict[str, int] = {}
        for drop in report.drops:
            by_what[drop.what] = by_what.get(drop.what, 0) + drop.delta
        detail = " ".join(f"{what}={count}" for what, count in sorted(by_what.items()))
        print(f"  DROPS  {len(report.drops)} marker(s): {detail}")
        if verbose:
            for drop in report.drops:
                print(f"    t={drop.t_us} {drop.what} +{drop.delta} (total {drop.total})")
    else:
        print("  drops  none")

    # Never folded into DROPS: a drop is records a ring could not hold, a gap is
    # whole sealed files deleted off the card before anything collected them.
    # Different unit, different fix. The chunk count is the loss; the seq range is
    # only where the loss happened, and each window is listed on its own because
    # merging two of them would invent the span between them.
    if report.gaps:
        shown = [_gap_window(gap) for gap in report.gaps[:6]]
        if len(report.gaps) > 6:
            shown.append(f"... and {len(report.gaps) - 6} more")
        total = report.discarded_chunks_total
        print(
            f"  GAPS   {len(report.gaps)} {_plural(len(report.gaps), 'window')}:"
            f" {total} {_plural(total, 'chunk')} discarded,"
            f" {report.discarded_bytes_total} bytes ({_bytes_human(report.discarded_bytes_total)}),"
            f" seqs {', '.join(shown)}"
        )
        if verbose:
            for gap in report.gaps:
                print(
                    f"    t={gap.t_us} {gap.chunks} {_plural(gap.chunks, 'chunk')} discarded,"
                    f" {gap.bytes} bytes, seqs {_gap_window(gap)}"
                )
    else:
        print("  gaps   none")

    if report.close_reason is not None:
        extra = f" -> {report.rotate_to}" if report.rotate_to else ""
        print(f"  close  {report.close_reason}{extra}")
    else:
        # The one finding this whole file format gained a line for.
        print("  close  MISSING — the file was cut, i.e. power loss (or still open)")
    if report.torn_tail:
        print("  tail   torn (one partial trailing line, discarded — expected after a cut)")
    if report.bad_lines:
        print(f"  BAD    {len(report.bad_lines)} malformed line(s):")
        for number, why in report.bad_lines[:10]:
            print(f"    line {number}: {why}")
        if len(report.bad_lines) > 10:
            print(f"    ... and {len(report.bad_lines) - 10} more")


def cmd_check(args) -> int:
    status = 0
    for name in args.files:
        path = Path(name)
        try:
            report = parse_file(path)
        except OSError as err:
            print(f"{name}: cannot read ({err})", file=sys.stderr)
            status = max(status, 2)
            continue
        print_report(report, args.verbose)
        if report.unsupported_version:
            # 2, not 1: the file was not read, so there is no verdict to give about
            # it. Calling a newer format a defect would blame the card for the
            # reader being behind.
            status = max(status, 2)
            continue
        if not report.clean:
            status = max(status, 1)
        if args.strict and report.drops:
            status = max(status, 1)
        # A gap is a strictly larger loss than a drop — whole sealed chunks deleted
        # off the card and never collected, unrecoverable rather than merely absent
        # from one ring — so a gate that fails on the smaller loss and waves the
        # larger one through is backwards. The default still tolerates it: a card
        # that ran long enough for retention to fire is a working card, and the file
        # it left is intact.
        if args.strict and report.gaps:
            status = max(status, 1)
        if args.strict and report.close_reason is None:
            status = max(status, 1)
    return status


def cmd_extract(args) -> int:
    """One type as a plain single-schema CSV — what a spreadsheet or pandas wants,
    without the typed-line dispatch getting in the way.

    The `t_us` column is **absolute decimal µs**, and `dlc` is put back: the saving
    is in the file, not in what a caller gets out of it. Resolving the chain is the
    whole reason this reads every stream line rather than only the wanted one —
    skipping the other types would drop their steps on the floor and put every later
    timestamp out by however much they came to.
    """
    path = Path(args.file)
    wanted = args.type.upper()
    record_header = "t_us,label,id_hex,flags,dlc,data_hex"
    header = {
        "C": record_header,
        "L": record_header,
        "U": record_header,
        "I": record_header,
        "X": "t_us,level,tag,message",
    }.get(wanted)
    if header is None:
        print(f"unknown type {args.type!r} (use C, L, U, I or X)", file=sys.stderr)
        return 2
    chain = Chain()
    printed_header = False
    # tag -> label, filled in from `#src` as the header streams past. The record
    # lines carry the tag; the name lives in the header now.
    labels: dict[int, str] = {}
    with path.open("r", encoding="latin-1") as handle:
        for line in handle:
            # A line with no terminator is the one line a power cut can tear, and
            # it can only be the last. Defined by the missing newline rather than
            # by its content: a torn line is often still field-count-correct and
            # merely short a few payload bytes, which no field check would catch.
            if not line.endswith("\n"):
                break
            line = line.rstrip("\n")
            if line.startswith("#"):
                if line.startswith("#sdlog,"):
                    fields = line.split(",")
                    version = int(fields[1]) if len(fields) > 1 and fields[1].isdigit() else None
                    if version not in SUPPORTED_FORMAT_VERSIONS:
                        print(
                            f"{path.name}: write format v{version}, which this reader does not know"
                            f" — refusing to extract rather than print wrong columns",
                            file=sys.stderr,
                        )
                        return 2
                elif line.startswith("#src,"):
                    src = line.split(",")
                    if len(src) >= 4 and set(src[3]) <= HEX_DIGITS and src[3]:
                        labels[int(src[3], 16)] = src[2]
                continue
            fields = line.split(",")
            kind = fields[0][0] if fields[0] else ""
            if kind not in RECORD_KINDS and kind != "X":
                continue
            try:
                t_us = chain.read(fields[1]) if len(fields) > 1 else None
            except ValueError:
                # A stamp the formatter cannot have written. `check` is the command
                # that reports that; here the honest move is to stop, because the
                # chain could not take its step and every timestamp after this line
                # is adrift by an unknown amount. Printing those is the one thing
                # worse than printing nothing.
                print(f"{path.name}: unreadable stamp column, extract stops here", file=sys.stderr)
                return 2
            if t_us is None or kind != wanted:
                continue
            label = None
            if wanted in RECORD_KINDS:
                if len(fields) != RECORD_FIELDS:
                    continue
                try:
                    _, tag, flags = split_token(fields[0])
                except ValueError:
                    continue
                label = labels.get(tag, fields[0])
                if args.label and label != args.label:
                    continue
                if args.shed and "s" not in flags:
                    continue
            elif len(fields) < TEXT_FIELDS:
                continue
            if not printed_header:
                # Held back until a line is known to be extractable, so a refused
                # file does not leave a lone header on stdout.
                printed_header = True
                if not args.no_header:
                    print(header)
            if wanted in RECORD_KINDS:
                # The columns the card no longer spends bytes on are put back here,
                # each from the one place it comes from: `label` out of `#src`, `dlc`
                # out of the data width, `flags` out of the token. They cost ~3600
                # lines a second on the card and nothing at all in a CSV, and a
                # spreadsheet wants them as columns.
                data = fields[3]
                print(f"{t_us},{label},{fields[2]},{flags or '-'},{len(data) // 2},{data}")
                continue
            # An `X` line goes out as the level letter off the token, then everything
            # after the stamp verbatim: the message is the last field and may hold
            # commas, so it is never re-split.
            _, _, tail = line.partition(",")[2].partition(",")
            print(f"{t_us},{fields[0][1:]},{tail}")
    if not printed_header and not args.no_header:
        print(header)
    return 0


def cmd_head(args) -> int:
    """The header block only: what a card says about itself."""
    path = Path(args.file)
    with path.open("r", encoding="latin-1") as handle:
        for line in handle:
            if line.startswith("#pad,"):
                break
            if not line.startswith("#"):
                break
            sys.stdout.write(line)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read back an sd_logger card file (write format v2).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    check = sub.add_parser("check", help="parse-check, report drops and gaps, flag a missing #close")
    check.add_argument("files", nargs="+")
    check.add_argument("-v", "--verbose", action="store_true", help="per-source and per-marker detail")
    check.add_argument(
        "--strict",
        action="store_true",
        help="also fail on any #drop or #gap marker, or a missing #close (for a CI/bench gate)",
    )
    check.set_defaults(func=cmd_check)

    extract = sub.add_parser("extract", help="one type as a plain CSV")
    extract.add_argument("file")
    extract.add_argument("--type", default="C", help="type letter: C, L, U, I or X (default C)")
    extract.add_argument("--label", help="only this source label")
    extract.add_argument("--shed", action="store_true", help="only frames carrying the shed flag")
    extract.add_argument("--no-header", action="store_true")
    extract.set_defaults(func=cmd_extract)

    head = sub.add_parser("head", help="print the file's own #src/#types header")
    head.add_argument("file")
    head.set_defaults(func=cmd_head)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
