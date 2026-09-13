"""script/sdlog.py — the reader half of write formats v1 and v2.

A format has two sides, and a spec only holds if both agree. The lines this suite
feeds the reader are the **same golden lines** that
``tests/host/test_log_format.cpp`` asserts the C++ formatter produces, so the two
halves are pinned to one another: change the writer and the host test fails,
change the reader and this one does.

The verdicts under test are the ones a bench operator actually asks for:

* a missing ``#close`` is the power-cut signature (spec §6 F1d) — it is what turns
  the manual "parses as valid CSV up to the last line" criterion in
  ``tests/hil/HIL.md`` into something a script decides, and what makes the §10.7
  power-loss procedure mechanical;
* a torn trailing line is *expected* after a cut and must not be called corrupt;
* a malformed line anywhere else is a real defect and must fail the exit status;
* the three drop counters are reported apart, never summed;
* a ``#gap`` is retention's in-band statement that whole chunks were deleted off the
  card (design §7), and its ``first_seq``/``last_seq`` pair names the **endpoints of the
  window retention emptied**, never a promise that every seq between them is missing —
  over-reporting loss is the one thing that marker must never do;
* a v2 stamp column decodes back to the absolute µs the writer put in (spec §6 F1h),
  and a write format this reader does not know is refused **by name** rather than
  walked line by line — a wall of "malformed line" about a newer format reads as a
  corrupt card and sends someone after a hardware fault that is not there.

Both v1 and v2 files are fed in below, because both are on cards right now: the
fixtures named ``V1_*`` are there to pin that the older files stay readable, not as
a description of what the writer emits today.
"""

from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

import pytest

# script/ is a tool directory, not a package, so the reader is loaded by path.
# It has to land in sys.modules before exec_module: @dataclass resolves the
# defining module out of there and fails on a module that is not registered.
_SDLOG = Path(__file__).parent.parent.parent / "script" / "sdlog.py"
_spec = importlib.util.spec_from_file_location("sdlog", _SDLOG)
sdlog = importlib.util.module_from_spec(_spec)
sys.modules["sdlog"] = sdlog
_spec.loader.exec_module(sdlog)


# The header block a file opens with, and the record/text lines below it. Every one
# of these is a byte-for-byte golden line from tests/host/test_log_format.cpp.
HEADER = [
    "#sdlog,2,7,@64960,2026.7.0",
    # `C1`/`C2` are what the default label now produces — the interface number. `lin`
    # is an explicit one, kept so the label-resolving path is exercised by something
    # the token could not have implied.
    "#src,C,C1,1,can_gateway,500000",
    "#src,C,C2,2,can_gateway,500000",
    "#src,L,lin,5,linbus,19200",
    "#types,C=can,L=lin,U=user,I=isotp,X=esphome-log,#=meta",
    "#flags,x=extended,r=rtr,t=tx,s=shed,~=truncated,none=absent",
    "#layout,rec=<K><tag><flags>:t:id:data,log=X<level>:t:tag:msg"
    ",t=@abs|step us HEX,tag/id/data HEX,dlc=len(data)/2",
]
# Capture times 412345, 412388, 412502, 412610 as v2 writes them: the first stream
# line of the chunk anchors (`@64AB9`), the three after it step (`2B`, `72`, `6C`),
# and every number is hex. The text line sits in the middle of the chain on purpose,
# because it is the one line type a reader could plausibly keep a separate chain for.
RECORDS = [
    "C1x,@64AB9,1A2,0011223344556677",
    "C2xs,2B,1A2,0011223344556677",
    "XI,72,sd_logger,records=4120 dropped=0 bytes=198112",
    "L5,6C,3C,55AA0000000000FF",
]

# A v1 file, which every card written before 2026-07-29 is full of. It is here to be
# *refused*, not read: v1's stamp column was decimal and absolute and it had a `dlc`
# field, so a v1 line is well-formed v2 CSV that means something else entirely.
V1_HEADER = ["#sdlog,1,7,412000,2026.7.0", *HEADER[1:]]
V1_RECORDS = [
    "C,412345,seg1,1A2,x,8,0011223344556677",
    "C,412388,seg2,1A2,xs,8,0011223344556677",
    "X,412502,I,sd_logger,records=4120 dropped=0 bytes=198112",
    "L,412610,lin,3C,-,8,55AA0000000000FF",
]


def write_log(tmp_path: Path, lines: list[str], *, terminated: bool = True) -> Path:
    """Write a card file. `terminated=False` cuts the last line mid-way, which is
    exactly what pulling the power does."""
    path = tmp_path / "L0000007.LOG"
    body = "\n".join(lines)
    if terminated:
        body += "\n"
    path.write_bytes(body.encode("latin-1"))
    return path


def parse(tmp_path: Path, lines: list[str], **kwargs):
    return sdlog.parse_file(write_log(tmp_path, lines, **kwargs))


# ------------------------------------------------------------------- the header


def test_the_header_makes_the_file_self_describing(tmp_path: Path) -> None:
    """A card found on a bench has to explain itself without the YAML that
    produced it — that is what the `#src` table replaced the bare numeric source
    column for."""
    report = parse(tmp_path, HEADER + RECORDS + ["#close,@F3ED0,clean"])
    assert report.format_version == 2
    assert report.seq == 7
    assert report.esphome_version == "2026.7.0"
    assert [(src.kind, src.label, src.tag) for src in report.sources] == [
        ("C", "C1", 1),
        ("C", "C2", 2),
        ("L", "lin", 5),
    ]
    assert report.clean


def test_records_are_counted_by_type_and_by_label(tmp_path: Path) -> None:
    report = parse(tmp_path, HEADER + RECORDS + ["#close,@F3ED0,clean"])
    assert report.counts == {"C": 2, "L": 1, "X": 1}
    # Resolved through `#src` — the line carries the tag, the header carries the name.
    assert report.labels == {"C1": 1, "C2": 1, "lin": 1}
    assert report.first_t_us == 412345
    assert report.last_t_us == 412610


def test_the_pad_line_is_ignored_not_parsed(tmp_path: Path) -> None:
    """It is spaces to the next sector boundary and carries no information."""
    pad = "#pad," + " " * 200
    report = parse(tmp_path, HEADER + [pad] + RECORDS + ["#close,@1,clean"])
    assert report.clean
    assert report.counts == {"C": 2, "L": 1, "X": 1}


# ------------------------------------------------------- the power-cut verdicts


def test_a_missing_close_is_reported(tmp_path: Path) -> None:
    """THE finding this format gained a line for. Every orderly end writes
    `#close`, so its absence means the file was cut."""
    report = parse(tmp_path, HEADER + RECORDS)
    assert report.close_reason is None
    # Not a corruption: the file itself is sound up to its last line.
    assert report.clean


def test_a_torn_trailing_line_is_expected_not_corrupt(tmp_path: Path) -> None:
    """The formatter never emits a bare newline except the terminator, so exactly
    one partial line can exist and everything before it is sound. Reported as
    torn, and *not* as a malformed line."""
    report = parse(
        tmp_path,
        HEADER + RECORDS + ["C1x,5A,1A2,0011223"],
        terminated=False,
    )
    assert report.torn_tail
    assert report.bad_lines == []
    assert report.clean
    assert report.close_reason is None
    # The complete records before the tear are all still counted.
    assert report.counts["C"] == 2


def test_each_close_reason_is_read_back(tmp_path: Path) -> None:
    for reason in ("clean", "rotate", "emergency"):
        report = parse(tmp_path, HEADER + RECORDS + [f"#close,@F3ED0,{reason}"])
        assert report.close_reason == reason


def test_a_rotation_names_its_successor(tmp_path: Path) -> None:
    report = parse(
        tmp_path,
        HEADER + RECORDS + ["#rotate,@7A120,L0000008.LOG", "#close,@7A121,rotate"],
    )
    assert report.rotate_to == "L0000008.LOG"
    assert report.close_reason == "rotate"


# -------------------------------------------------------------- drop accounting


def test_drop_markers_are_reported_apart_never_summed(tmp_path: Path) -> None:
    """Three counters, three bottlenecks, three different fixes: the logger's own
    ring, a gateway tap ring, and the text ring."""
    report = parse(
        tmp_path,
        HEADER
        + RECORDS
        + [
            "#drop,@65132,ring,17,17",
            "#drop,@65194,tap:seg1,3,3",
            "#drop,@651F8,text,1,1",
            "#close,@F3ED0,clean",
        ],
    )
    assert [(d.what, d.delta, d.total) for d in report.drops] == [
        ("ring", 17, 17),
        ("tap:seg1", 3, 3),
        ("text", 1, 1),
    ]
    assert report.clean  # a drop is a finding about the run, not a broken file


def test_a_healthy_run_carries_no_markers(tmp_path: Path) -> None:
    """Markers are emitted on counter *change* only, so a clean run costs nothing
    — which also means their absence is the thing to assert."""
    report = parse(tmp_path, HEADER + RECORDS + ["#close,@F3ED0,clean"])
    assert report.drops == []
    assert report.dropped_total == 0


# ------------------------------------------------------- retention gaps (M6 §7)
#
# `#gap,<t_us>,<chunks>,<bytes>,<first_seq>,<last_seq>` is what retention writes when it
# deletes chunks that were never collected — whole files gone off the card, not records
# lost out of a ring, which is why it is a second marker and not a fourth `#drop`
# counter. The lines below are the golden lines from
# tests/host/test_log_format.cpp's `format_gap()` cases, so the two halves stay pinned.
#
# THE SEMANTICS, settled (design §7a is the arbiter and
# components/sd_logger/collection_policy.h `take_pending_discards()` is the producer;
# log_format.h's docstring used to claim an "inclusive range that went away" and was
# corrected to match on 2026-07-28):
#
#   first_seq/last_seq are the ENDPOINTS OF THE WINDOW retention emptied, in the order
#   it emptied them. `chunks` is how many were actually discarded. When
#   `chunks != span(first_seq, last_seq)` the window is PUNCTURED: some seqs inside it
#   were collected safely and are in the archive. A reader that reports the whole span
#   as lost is claiming data loss that did not happen, which defeats the only reason
#   this marker exists.

GAP_GOLDEN = "#gap,@65132,3,50331648,412,414"  # three chunks, 48 MB, seqs 412..414


def states_chunks(text: str, count: int) -> bool:
    """Does `text` state a chunk count of `count`? Both spellings the report already uses
    are accepted — prose (`3 chunks`) and the `DROPS` line's `what=count` form
    (`chunks=3`) — so these assertions pin the *number*, not the punctuation."""
    return re.search(rf"(?<![\d=]){count} chunks?\b|\bchunks?={count}(?!\d)", text) is not None


def states_window(text: str, first: int, last: int) -> bool:
    """Does `text` show the window's two endpoints as a range? `412..414`, `412-414` and
    `412 to 414` all count."""
    return re.search(rf"(?<!\d){first}\s*(?:\.\.|-|to )\s*{last}(?!\d)", text) is not None


def check_lines(tmp_path: Path, lines: list[str], capsys, *flags: str) -> list[str]:
    """`check` a file and return its stdout one line at a time, whitespace collapsed so
    the assertions pin wording and never column alignment."""
    path = write_log(tmp_path, lines)
    sdlog.main(["check", *flags, str(path)])
    return [" ".join(line.split()) for line in capsys.readouterr().out.splitlines()]


def gap_findings(lines: list[str]) -> list[str]:
    """The report's gap findings. Upper case like `DROPS`, and for the same reason: the
    section shouts only when there is something to say."""
    return [line for line in lines if line.startswith("GAPS")]


def test_a_gap_line_parses_and_the_file_is_not_malformed(tmp_path: Path) -> None:
    """The writer half already emits this line and is host-tested; the moment retention
    is wired up, the first discard puts one in a real file. A reader that calls it
    malformed turns every retention event into a CI and bench failure."""
    report = parse(tmp_path, HEADER + RECORDS + [GAP_GOLDEN, "#close,@F3ED0,clean"])
    assert report.bad_lines == []
    assert report.clean  # a gap is a finding about the run, not a broken file
    assert len(report.gaps) == 1
    gap = report.gaps[0]
    assert (gap.t_us, gap.chunks, gap.bytes, gap.first_seq, gap.last_seq) == (
        414002,
        3,
        50331648,
        412,
        414,
    )
    # Three chunks across a three-wide window: nothing survived inside it.
    assert gap.span == 3
    assert not gap.punctured


def test_gaps_are_surfaced_apart_from_drops(tmp_path: Path) -> None:
    """Different loss, different unit, different fix: a `#drop` counts records the ring
    could not hold, a `#gap` counts whole files retention deleted. Summing them would be
    the same mistake the three drop counters are kept apart to avoid."""
    report = parse(
        tmp_path,
        HEADER + RECORDS + ["#drop,@65132,ring,17,17", GAP_GOLDEN, "#close,@F3ED0,clean"],
    )
    assert [(d.what, d.delta, d.total) for d in report.drops] == [("ring", 17, 17)]
    assert [(g.chunks, g.first_seq, g.last_seq) for g in report.gaps] == [(3, 412, 414)]
    assert report.dropped_total == 17  # records
    assert report.discarded_chunks_total == 3  # files
    assert report.discarded_bytes_total == 50331648


def test_a_punctured_window_never_claims_the_seqs_inside_it(tmp_path: Path) -> None:
    """THE case. Retention sealed 10, 11 and 12, the collector took 11, then retention
    discarded 10 and 12 — so the window is 10..12 but only two chunks went away and seq
    11 is safe in the archive. `chunks` is the loss; the endpoints are only the window."""
    report = parse(tmp_path, HEADER + RECORDS + ["#gap,@65132,2,8388608,10,12", "#close,@1,clean"])
    assert report.clean
    gap = report.gaps[0]
    assert gap.chunks == 2  # what was lost
    assert gap.span == 3  # how wide the window is
    assert gap.punctured  # ... and therefore that it has a hole in it
    # The count that must never be inflated to the span.
    assert report.discarded_chunks_total == 2


def test_a_punctured_window_prints_as_punctured(tmp_path: Path, capsys) -> None:
    """The parse being right is not enough — the operator reads the report, and a report
    that says "3 chunks missing" sends someone hunting for a chunk that is sitting in the
    archive. State the loss, state the window, and say the window is not solid."""
    lines = check_lines(
        tmp_path, HEADER + RECORDS + ["#gap,@65132,2,8388608,10,12", "#close,@1,clean"], capsys
    )
    findings = gap_findings(lines)
    assert len(findings) == 1, lines
    finding = findings[0]
    assert states_chunks(finding, 2), finding
    assert states_window(finding, 10, 12), finding
    assert "punctur" in finding.lower(), finding
    # The one thing this marker must never do, asserted against the whole report: the
    # number 3 may not appear anywhere as a *chunk* count. The span is three seqs wide,
    # never three chunks — three chunks is the loss that did not happen.
    assert not states_chunks("\n".join(lines), 3), lines


def test_a_contiguous_window_is_reported_as_a_plain_range(tmp_path: Path, capsys) -> None:
    """When `chunks` equals the span, every seq in the window really is gone — say so
    plainly, and do not decorate a solid range with a puncture the file never claimed."""
    lines = check_lines(tmp_path, HEADER + RECORDS + [GAP_GOLDEN, "#close,@1,clean"], capsys)
    findings = gap_findings(lines)
    assert len(findings) == 1, lines
    assert states_chunks(findings[0], 3), findings[0]
    assert states_window(findings[0], 412, 414), findings[0]
    assert "punctur" not in "\n".join(lines).lower(), lines


def test_a_single_discarded_chunk_repeats_its_seq(tmp_path: Path) -> None:
    """The field count never varies — one chunk writes its seq into both endpoints
    (log_format.h) — so a one-wide window must not read as an empty or malformed one."""
    report = parse(tmp_path, HEADER + ["#gap,@7A120,1,4194304,7,7", "#close,@1,clean"])
    assert report.clean
    gap = report.gaps[0]
    assert gap.first_seq == gap.last_seq == 7
    assert gap.span == 1
    assert not gap.punctured


def test_multiple_gap_markers_accumulate(tmp_path: Path) -> None:
    """Markers are emitted whenever the discard counters move, and a long unattended run
    moves them repeatedly. Each window stands on its own — they are not merged into one
    span, which would invent loss between them — and the totals add up."""
    report = parse(
        tmp_path,
        HEADER
        + RECORDS
        + [
            GAP_GOLDEN,
            "#gap,@7A120,1,4194304,415,415",
            # 64-bit byte total: a day of retention at 148 KB/s overflows 32 bits.
            "#gap,@141DD76000,3200,12787200000,1,3200",
            "#close,@F3ED0,clean",
        ],
    )
    assert report.clean
    assert [(g.t_us, g.chunks, g.first_seq, g.last_seq) for g in report.gaps] == [
        (414002, 3, 412, 414),
        (500000, 1, 415, 415),
        (86400000000, 3200, 1, 3200),
    ]
    assert report.discarded_chunks_total == 3 + 1 + 3200
    assert report.discarded_bytes_total == 50331648 + 4194304 + 12787200000


def test_a_window_that_straddles_the_seq_wrap_is_not_the_whole_card(tmp_path: Path) -> None:
    """`seq` wraps at 10 000 000 and the endpoints are emitted in the order retention
    emptied the window, never sorted — so `first > last` is a legal wrapped window of
    three chunks (collection_policy.h `take_pending_discards()`). A plain
    `last - first + 1` reads it as a negative span, and a sorted one reads it as the
    entire card."""
    report = parse(tmp_path, HEADER + ["#gap,@1,3,3145728,9999999,1", "#close,@2,clean"])
    assert report.clean
    gap = report.gaps[0]
    assert gap.span == 3  # 9999999, 0, 1
    assert not gap.punctured


def test_verbose_lists_each_gap_window(tmp_path: Path, capsys) -> None:
    """Same treatment `-v` already gives each `#drop`: one line per marker, led by its
    timestamp, so a window can be placed against the rest of the run."""
    lines = check_lines(
        tmp_path,
        HEADER + [GAP_GOLDEN, "#gap,@7A120,1,4194304,7,7", "#close,@1,clean"],
        capsys,
        "-v",
    )
    assert sum(1 for line in lines if line.startswith("t=414002")) == 1, lines
    assert sum(1 for line in lines if line.startswith("t=500000")) == 1, lines


def test_a_healthy_run_carries_no_gap_markers(tmp_path: Path, capsys) -> None:
    """Retention that never fired writes nothing, so the absence is the assertion — and
    the report says so out loud rather than leaving the reader to notice a missing
    section, exactly as `drops none` does."""
    body = HEADER + RECORDS + ["#close,@F3ED0,clean"]
    report = parse(tmp_path, body)
    assert report.gaps == []
    assert report.discarded_chunks_total == 0
    assert report.discarded_bytes_total == 0
    lines = check_lines(tmp_path, body, capsys)
    assert "gaps none" in lines, lines
    assert gap_findings(lines) == []


@pytest.mark.parametrize(
    ("line", "why"),
    [
        ("#gap,@65132,3,50331648,412", "a field short"),
        ("#gap,@65132", "only a timestamp"),
        ("#gap", "no fields at all"),
        ("#gap,not_a_time,3,50331648,412,414", "a timestamp that is not hex"),
        ("#gap,65132,3,50331648,412,414", "an absolute timestamp with no anchor mark"),
        ("#gap,@65132,three,50331648,412,414", "non-decimal chunk count"),
        ("#gap,@65132,3,,412,414", "empty byte count"),
        ("#gap,@65132,-3,50331648,412,414", "negative chunk count"),
        ("#gap,@65132,3,50331648,412,-414", "negative seq"),
    ],
)
def test_a_malformed_gap_line_is_a_defect(tmp_path: Path, line: str, why: str) -> None:
    """A `#gap` the formatter cannot have produced means the marker pass is broken or the
    card returned garbage — and this marker in particular is the only record that the
    data it accounts for ever existed, so a bad one is never quietly ignored."""
    report = parse(tmp_path, HEADER + [line] + RECORDS + ["#close,@1,clean"])
    assert not report.clean, why
    assert len(report.bad_lines) == 1
    # ... and rejected for the *right* reason: `#gap` is a known meta line whose fields
    # failed validation, not an unrecognised head. Without this the whole case passes
    # trivially against a reader that has never heard of `#gap` at all.
    _, complaint = report.bad_lines[0]
    assert "unknown meta line" not in complaint, why
    assert report.gaps == []  # nothing half-parsed gets counted


# ------------------------------------------------------------- malformed lines


@pytest.mark.parametrize(
    ("line", "why"),
    [
        ("C1x,2B,1A2,0011223", "an odd hex count — half a byte of payload"),
        ("C1x,2B,1A2,001122334455667788", "nine payload bytes, above the eight a record holds"),
        ("C1q,2B,1A2,", "unknown flag letter in the token"),
        ("C1x,2B,1A2", "a field short"),
        ("C1x,2B,1A2,0011,extra", "a field too many"),
        ("C1x,not_a_time,1A2,", "a timestamp that is not hex"),
        ("C1x,2b,1A2,", "a lowercase hex timestamp the formatter cannot emit"),
        ("Cx,2B,1A2,", "a token with no tag at all"),
        ("C1x,2B,ZZZ,", "non-hex id"),
        ("C1x,2B,1A2,00GG", "non-hex payload"),
        ("Q1,2B,1A2,", "unknown type letter"),
        ("#nope,1,2", "unknown meta line"),
        ("#sdlog,1", "truncated header"),
    ],
)
def test_a_malformed_line_is_a_defect(tmp_path: Path, line: str, why: str) -> None:
    """Anywhere other than the very end this is a formatter bug or a card that
    returned garbage, and it is what the exit status exists for.

    The bad line goes *after* the good records rather than before them so that each
    case fails for the reason it names: ahead of the chunk's anchor every one of
    them would also be a step with nothing to step from, and the parametrisation
    would be checking one rule seven times over."""
    report = parse(tmp_path, HEADER + RECORDS + [line, "#close,@1,clean"])
    assert not report.clean, why
    assert len(report.bad_lines) == 1


def test_a_message_with_commas_is_not_a_malformed_line(tmp_path: Path) -> None:
    """The message is the last field precisely so its commas need no escaping."""
    report = parse(
        tmp_path, HEADER + ["XW,@1,gw,route[0]: shed, again, and again", "#close,@2,clean"]
    )
    assert report.clean
    assert report.counts == {"X": 1}


def test_utf8_in_a_message_survives_the_round_trip(tmp_path: Path) -> None:
    """Bytes >= 0x80 pass through the formatter so a German message still reads
    as German; the reader must not be the place that mangles it."""
    report = parse(tmp_path, HEADER + ["XI,@1,x,Kabelbrüche", "#close,@2,clean"])
    assert report.clean


# ------------------------------------------- the v2 stamp column (spec §6 F1h)
#
# `@<abs>` anchors, a bare number steps. The chain runs over every record and text
# line in file order and the `#` meta lines stay out of it, so a reader that skips a
# `#drop` still decodes the records after it.
#
# What makes these cases worth writing rather than reasoning about: a chain decoded
# wrongly produces no error anywhere. Every line still parses, every column is still
# present, and the times are simply wrong — which is the one failure a bench run
# cannot see and a card cannot be re-read for.


def test_a_step_is_read_as_microseconds_from_the_line_before_it(tmp_path: Path) -> None:
    report = parse(tmp_path, HEADER + RECORDS + ["#close,@F3ED0,clean"])
    assert report.clean
    # The same four absolute times the v1 fixture spells out line by line.
    assert report.first_t_us == 412345
    assert report.last_t_us == 412610


def test_the_chain_runs_through_the_text_line_not_around_it(tmp_path: Path) -> None:
    """One chain over every stream line, so a reader never has to tell a frame from
    a log message. Dropping the `X` line's 157 µs would put the `L` line — and every
    record after it in a real file — 157 µs early, with nothing to say so."""
    report = parse(tmp_path, HEADER + RECORDS + ["#close,@F3ED0,clean"])
    assert report.last_t_us == 412610  # 412345 + 43 + 114 + 108


def test_meta_lines_do_not_move_the_chain(tmp_path: Path) -> None:
    """`#drop` and `#gap` carry absolute times and stay outside the chain. If they
    advanced it, every reader that skips the markers it does not care about would
    decode every record after one of them wrongly."""
    lines = HEADER + RECORDS[:2] + ["#drop,@65132,ring,17,17", GAP_GOLDEN] + RECORDS[2:]
    report = parse(tmp_path, lines + ["#close,@F3ED0,clean"])
    assert report.clean
    assert report.last_t_us == 412610
    assert report.drops[0].t_us == 414002


def test_an_anchor_restarts_the_chain_wherever_it_appears(tmp_path: Path) -> None:
    """A card stall, a backward stamp and the periodic re-anchor all land mid-file,
    so an anchor is not just a file-open thing — it must reset the running value
    rather than adding to it."""
    report = parse(
        tmp_path,
        HEADER
        + [
            "C1x,@64AB9,1A2,0011223344556677",
            "C1x,@DBBA0,1A2,0011223344556677",
            "C1x,32,1A2,0011223344556677",
            "#close,@F3ED0,clean",
        ],
    )
    assert report.clean
    assert report.first_t_us == 412345
    assert report.last_t_us == 900050


def test_a_step_before_any_anchor_is_a_defect(tmp_path: Path) -> None:
    """Every chunk anchors its own first stream line, so a step with nothing to step
    from means the anchor is gone — from a truncated head, or a splice. The times
    behind it are unrecoverable, and printing them as if they had been read would put
    confident wrong numbers in front of someone."""
    report = parse(tmp_path, HEADER + ["C1x,2B,1A2,0011223344556677", "#close,@1,clean"])
    assert not report.clean
    assert len(report.bad_lines) == 1
    assert "before any anchor" in report.bad_lines[0][1]


def test_a_v1_file_is_refused_by_name_and_not_read(tmp_path: Path) -> None:
    """THE reason a version field is worth having, and why this reader supports
    exactly one format.

    A v1 line is well-formed v2 CSV that means something else: `C,412345,seg1,...`
    has fields where v2 has fields, and its stamp is decimal and absolute where v2
    reads hex and relative. Nothing about the line itself gives that away — only the
    header does. So a v1 file is named and stopped at the header, and none of its
    lines are reported as anything, because none of them were read."""
    report = parse(tmp_path, V1_HEADER + V1_RECORDS + ["#close,999120,clean"])
    assert report.format_version == 1
    assert report.unsupported_version
    assert report.bad_lines == []
    assert report.counts == {}
    assert report.first_t_us is None


def test_a_v1_file_exits_two_rather_than_calling_the_card_broken(tmp_path: Path) -> None:
    """Without the version check this file would come back as four malformed lines,
    which reads as a card that returned garbage — and sends someone to the bench
    after a fault that is not there."""
    path = write_log(tmp_path, V1_HEADER + V1_RECORDS + ["#close,999120,clean"])
    assert sdlog.main(["check", str(path)]) == 2


# -------------------------------------------------------- an unknown write format


def test_an_unknown_format_version_is_named_not_parsed_line_by_line(tmp_path: Path) -> None:
    """THE gap this bump closed. Nothing rejected an unknown version before, so a
    newer file was handed to a parser that does not fit it and came back as a wall of
    malformed lines — which reads as a card that returned garbage and sends someone
    after a hardware fault that is not there."""
    report = parse(tmp_path, ["#sdlog,99,7,412000,2026.7.0", *HEADER[1:], *RECORDS, "#close,@1,clean"])
    assert report.format_version == 99
    assert report.unsupported_version
    # Nothing below the header was read, and the report says nothing it did not read.
    assert report.bad_lines == []
    assert report.counts == {}


def test_an_unknown_format_version_exits_two_not_one(tmp_path: Path) -> None:
    """2 is "could not be read", which is what happened. 1 is "this file has a
    defect" and would blame the card for the reader being behind."""
    path = write_log(tmp_path, ["#sdlog,99,7,412000,2026.7.0", *HEADER[1:], *RECORDS, "#close,@1,clean"])
    assert sdlog.main(["check", str(path)]) == 2


def test_an_unknown_format_version_says_so_in_the_report(tmp_path: Path, capsys) -> None:
    path = write_log(tmp_path, ["#sdlog,99,7,412000,2026.7.0", *HEADER[1:], *RECORDS, "#close,@1,clean"])
    sdlog.main(["check", str(path)])
    out = capsys.readouterr().out
    assert "v99" in out
    # And not the counts of a file it never parsed.
    assert "no records" not in out


def test_extract_refuses_an_unknown_format_version(tmp_path: Path, capsys) -> None:
    """A column of numbers from a format this reader does not know is worse than no
    output: it looks exactly like a successful extract."""
    path = write_log(tmp_path, ["#sdlog,99,7,412000,2026.7.0", *HEADER[1:], *RECORDS, "#close,@1,clean"])
    assert sdlog.main(["extract", str(path), "--type", "C"]) == 2
    assert capsys.readouterr().out.strip() == ""


# ------------------------------------------------------------------ M1-era file


def test_an_m1_era_file_is_named_not_rejected(tmp_path: Path) -> None:
    """A card can hold both eras — which is why the boot scan matches both
    extensions — so the reader says which era it is looking at."""
    path = tmp_path / "L0000003.CSV"
    path.write_text("# t_us,source,id_hex,flags,dlc,data_hex\n412345,1,1A2,1,8,0011223344556677\n")
    report = sdlog.parse_file(path)
    assert report.legacy
    assert report.format_version is None


# ------------------------------------------------------------------ exit status


def test_check_exit_status_is_clean_for_a_clean_file(tmp_path: Path) -> None:
    path = write_log(tmp_path, HEADER + RECORDS + ["#close,@1,clean"])
    assert sdlog.main(["check", str(path)]) == 0


def test_check_exit_status_fails_on_a_malformed_line(tmp_path: Path) -> None:
    path = write_log(tmp_path, HEADER + ["C,1,seg1,1A2,x,8,00"] + ["#close,@2,clean"])
    assert sdlog.main(["check", str(path)]) == 1


def test_check_tolerates_a_power_cut_by_default(tmp_path: Path) -> None:
    """Pulling the power is a *test* (spec §10.7), and its expected result must
    not read as a failure — the file is intact, it just has no `#close`."""
    path = write_log(tmp_path, HEADER + RECORDS, terminated=False)
    assert sdlog.main(["check", str(path)]) == 0


def test_strict_makes_a_missing_close_and_any_drop_fail(tmp_path: Path) -> None:
    """The bench-gate spelling: on a run that was *not* meant to be interrupted,
    both are failures."""
    cut = write_log(tmp_path, HEADER + RECORDS, terminated=False)
    assert sdlog.main(["check", "--strict", str(cut)]) == 1

    dropped = tmp_path / "L0000009.LOG"
    dropped.write_text("\n".join(HEADER + RECORDS + ["#drop,@1,ring,5,5", "#close,@2,clean"]) + "\n")
    assert sdlog.main(["check", "--strict", str(dropped)]) == 1


def test_check_tolerates_a_gap_by_default(tmp_path: Path) -> None:
    """A card that ran long enough for retention to fire is a *working* card doing the
    thing the design asks of it — bounded storage — and the file it left behind is
    intact. Reading one on the bench must not exit 1 by default, or the status stops
    distinguishing "this file is broken" from "this run shed old history"."""
    path = write_log(tmp_path, HEADER + RECORDS + [GAP_GOLDEN, "#close,@F3ED0,clean"])
    assert sdlog.main(["check", str(path)]) == 0


def test_strict_fails_on_a_gap_the_way_it_fails_on_a_drop(tmp_path: Path) -> None:
    """PINNED: `--strict` fails on a `#gap`.

    `--strict` is the CI and bench-gate spelling of "this run was supposed to lose
    nothing", which is why it already fails on a `#drop` and on a missing `#close`. A
    `#gap` is a strictly larger loss than a drop — whole sealed chunks deleted off the
    card and never collected, unrecoverable rather than merely absent from one ring — so
    a gate that fails on the smaller loss and passes the larger one is backwards.

    The gate's own reference run collects everything it writes, so a gap there means
    collection did not keep up and the archive has a hole: exactly the condition worth
    stopping for."""
    path = write_log(tmp_path, HEADER + RECORDS + [GAP_GOLDEN, "#close,@F3ED0,clean"])
    # Asserted together on purpose: the same file exits 0 without the flag, so the 1
    # below is `--strict` reacting to the gap and cannot be a malformed line in disguise.
    assert sdlog.main(["check", str(path)]) == 0
    assert sdlog.main(["check", "--strict", str(path)]) == 1


def test_check_reports_an_unreadable_file_separately(tmp_path: Path) -> None:
    assert sdlog.main(["check", str(tmp_path / "nope.LOG")]) == 2


# --------------------------------------------------------------------- extract


def test_extract_yields_a_single_schema_csv(tmp_path: Path, capsys) -> None:
    path = write_log(tmp_path, HEADER + RECORDS + ["#close,@1,clean"])
    assert sdlog.main(["extract", str(path), "--type", "C"]) == 0
    lines = capsys.readouterr().out.strip().split("\n")
    assert lines[0] == "t_us,label,id_hex,flags,dlc,data_hex"
    assert lines[1:] == [
        "412345,C1,1A2,x,8,0011223344556677",
        "412388,C2,1A2,xs,8,0011223344556677",
    ]


def test_extract_can_select_one_segment(tmp_path: Path, capsys) -> None:
    path = write_log(tmp_path, HEADER + RECORDS + ["#close,@1,clean"])
    sdlog.main(["extract", str(path), "--type", "C", "--label", "C1", "--no-header"])
    assert capsys.readouterr().out.strip() == "412345,C1,1A2,x,8,0011223344556677"


def test_extract_can_select_shed_frames(tmp_path: Path, capsys) -> None:
    """The Issue #1 hunt: a frame that was on the wire and was not forwarded."""
    path = write_log(tmp_path, HEADER + RECORDS + ["#close,@1,clean"])
    sdlog.main(["extract", str(path), "--type", "C", "--shed", "--no-header"])
    assert capsys.readouterr().out.strip() == "412388,C2,1A2,xs,8,0011223344556677"


def test_extract_skips_a_torn_trailing_record(tmp_path: Path, capsys) -> None:
    path = write_log(tmp_path, HEADER + RECORDS + ["C1x,5A,1A2,0011"], terminated=False)
    sdlog.main(["extract", str(path), "--type", "C", "--no-header"])
    assert "412700" not in capsys.readouterr().out


def test_extract_text_lines(tmp_path: Path, capsys) -> None:
    path = write_log(tmp_path, HEADER + RECORDS + ["#close,@1,clean"])
    sdlog.main(["extract", str(path), "--type", "X", "--no-header"])
    assert capsys.readouterr().out.strip() == "412502,I,sd_logger,records=4120 dropped=0 bytes=198112"


def test_extract_resolves_the_chain_across_the_types_it_is_not_asked_for(tmp_path: Path, capsys) -> None:
    """`--type L` still has to read the `C` and `X` lines above it, because their
    steps are part of the number it prints. Filtering by prefix first — which is what
    a v1 extract could afford to do — would print 108 µs instead of 412610."""
    path = write_log(tmp_path, HEADER + RECORDS + ["#close,@1,clean"])
    sdlog.main(["extract", str(path), "--type", "L", "--no-header"])
    assert capsys.readouterr().out.strip() == "412610,lin,3C,-,8,55AA0000000000FF"


def test_extract_puts_back_every_column_the_card_stopped_paying_for(
    tmp_path: Path, capsys
) -> None:
    """The saving is in the file, not in what a caller gets back out of it.

    Steps become absolute µs in **decimal**, the label comes back out of `#src`, the
    flags come back out of the token, and `dlc` comes back out of the data width.
    Each costs ~3600 lines a second on the card and nothing at all in a CSV."""
    path = write_log(tmp_path, HEADER + RECORDS + ["#close,@1,clean"])
    sdlog.main(["extract", str(path), "--type", "C", "--no-header"])
    assert capsys.readouterr().out.strip().split("\n") == [
        "412345,C1,1A2,x,8,0011223344556677",
        "412388,C2,1A2,xs,8,0011223344556677",
    ]


def test_extract_refuses_a_v1_file(tmp_path: Path, capsys) -> None:
    """Reading a v1 stamp as a v2 step would turn 412388 into a 412 388 µs jump from
    the line before it and print a column of confident nonsense — which looks exactly
    like a successful extract."""
    path = write_log(tmp_path, V1_HEADER + V1_RECORDS + ["#close,999120,clean"])
    assert sdlog.main(["extract", str(path), "--type", "C"]) == 2
    assert capsys.readouterr().out.strip() == ""


def test_head_prints_only_the_header_block(tmp_path: Path, capsys) -> None:
    path = write_log(tmp_path, HEADER + ["#pad,   "] + RECORDS + ["#close,@1,clean"])
    assert sdlog.main(["head", str(path)]) == 0
    assert capsys.readouterr().out.strip().split("\n") == HEADER
