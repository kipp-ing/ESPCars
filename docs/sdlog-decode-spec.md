# sdlog_decode: archived chunks into a queryable database

2026-10-11. O1–O3 decided (cantools, HTTP via urllib, lightweight DELETE per chunk).
2026-10-16: diagnostic answers (UDS over ISO-TP) decoded with the device's own `.dcat`
catalog into signals and events (§7, U1–U16). This is the host-side step after `script/sdlog_fleet.py`. The fleet
leaves sealed, verified chunks in `<archive>/<device>/`. This tool loads each
chunk into a column store, once as raw frames and once as decoded signals. When the
decode rules change, it replaces exactly that chunk's rows.

The archive is the source of truth, and the database is derived from it: dropping
the database and re-running the tool rebuilds it bit for bit. Every requirement
carries an ID and the test that proves it. **A requirement without a test is not
done.** Tests live in `tests/sd_logger/test_decode.py` and run against the in-process
`FakeSink` with the fictional `tests/sd_logger/fixtures/mini.dbc`. No database and no
board are needed. A ClickHouse round trip runs only when `SDLOG_CLICKHOUSE_URL` is set
(skip otherwise).

## 1. Shape

```
<archive>/<device>/L*.LOG ──▶ reader (sdlog.py, shared) ──▶ frames ──▶ sink: frames
                                                      ├──▶ DBC decode ──▶ sink: signals
                                                      └──▶ ISO-TP reassembly ──▶ .dcat decode ──▶ sink: signals, events (§7)
                                                                  └──▶ sink: chunks (commit marker, last)
```

```
sdlog_decode.py init   --config decode.toml   # create the tables (idempotent)
sdlog_decode.py run    --config decode.toml   # every chunk that is new or stale
sdlog_decode.py status --config decode.toml   # per device: chunks ok / defect / pending
```

### 1.1 Config

TOML, read with `tomllib`. The real instance config (device names, DBC paths into the
consuming project) lives with the deployment and never in this repo.
`deploy/sdlog-decode/decode.example.toml` is fictional.

```toml
archive = "/srv/sdlog/archive"
sink = "clickhouse"                       # or "fake" (tests)
clickhouse_url = "http://localhost:8123"
database = "sdlog"

[[device]]
name = "mr-orange"                        # = <archive>/<name>/ and the chunk's #origin
vehicle = "demo-car"                      # what signals are filed under (may change per device over time, §4)
[device.dbc]
EV = "/path/to/consuming-project/vehicles/demo/dbc/ev.dbc"   # bus label (#src) → DBC

[[device.uds]]                            # optional, one per diagnosed ECU (§7)
label = "EV"                              # bus label the ECU answers on
dcat = "/path/to/consuming-project/vehicles/demo/uds/mini.dcat"
ecu = "MINI"                              # optional when the catalog holds one ECU
names = "/path/to/…/mini.names.toml"      # optional: readable signal names (§7.2)
dtc = "/path/to/…/mini-dtc.txt"           # optional: fault code texts (§7.3)
```

## 2. Tables

The tool creates these (`init`). They are written only by this tool.

| Table | Columns | Engine / order |
|---|---|---|
| `frames` | device, chunk, kind (C/L/U/I), label, utc DateTime64(3) NULL, id UInt32, flags, dlc, data UInt64 (the data hex as one big-endian number) | MergeTree, PARTITION BY device, ORDER BY (device, label, id, utc) |
| `signals` | vehicle, device, chunk, signal (`<Message>.<Signal>`, or `<ECU>.<name>` from a diagnostic answer, §7.2), utc DateTime64(3) NULL, value Float64, text String (§7.2; empty for DBC signals) | MergeTree, PARTITION BY vehicle, ORDER BY (vehicle, signal, utc) |
| `events` | vehicle, device, chunk, utc DateTime64(3) NULL, source (ECU), kind (`state`/`dtc`/`nrc`/`isotp`/`unavailable`), name, value Float64, text, detail (§7.4) | MergeTree, PARTITION BY vehicle, ORDER BY (vehicle, kind, source, name, utc) |
| `chunks` | device, chunk, file, seq, sha256, bytes, frames, can_frames, matched, signals, utc_min/max DateTime64(3) NULL, gaps, catalog, result, detail, decoded_at | ReplacingMergeTree(decoded_at), ORDER BY (device, file) |

- **`chunk` = `<seq>-<sha256[:12]>`.** A card that restarts its seq after an erase
  produces new chunk keys, not collisions (`sdlog-collection-design.md` §10 item 2: no boot id yet).
- **`catalog` = sha256 over the device's DBC files**, sorted by label, and over every
  `[[device.uds]]` entry with the content of its `.dcat`, names and dtc files. It is a content
  hash, so no git is needed and any edit to a decode rule counts as a change.
- **Schema upgrades are additive.** `init` (and every `run`, which calls it first) creates what is
  missing — `events`, the `text` column of `signals` — with `IF NOT EXISTS`, so a database written by
  an older version keeps its rows and the catalog change (D7) re-decodes them.
- **One time column, milliseconds.** `utc` is UTC truncated to ms. Boot-relative µs (`t`) stay in the
  archive only: µs buy nothing a query asks for, and a second time column was 73 % of `frames` and 97 % of
  `signals` on disk (measured 2026-10-11). Nullable key columns need `allow_nullable_key = 1`.
- **Resolving `utc`.** A chunk never spans a boot, and `t` is monotonic within one, so a UTC anchor (`#utc`, or a
  `#clock` with basis `utc`) holds for the whole chunk: records **before** the chunk's first anchor are resolved
  backwards from it, records after an anchor from the newest one before them. `est`/`drift` readings never fill
  `utc`. NULL remains only for a chunk with no UTC anchor at all. Resolving across chunks of one boot needs a
  boot id (`sdlog-collection-design.md` §10) and is not done.
- Signals are stored **in full**, not only on change. Repeated values cost almost nothing
  after delta and ZSTD, and queries need no last-value logic.

## 3. Requirements

| ID | Requirement | Test |
|---|---|---|
| D1 | **Config.** Rejects an unknown key, a duplicate device, a missing DBC file, an unknown sink, with exit 2 naming the problem. | `test_config_rejects` (parametrized) |
| D2 | **Shared reader.** Frames come from the same parser as `sdlog.py extract` (one generator, used by both). `extract`'s output is byte-identical before and after the refactor. | `test_extract_unchanged_after_refactor`, `test_reader_yields_frames_with_utc` |
| D3 | **Frames are lossless up to the ms.** Every C/L/U/I record in a clean chunk becomes one `frames` row with identical label/id/flags/data; `utc` is the record's UTC truncated to ms, resolved as in §2 (backwards from the chunk's first UTC anchor, forward from each later one), NULL only in a chunk without one — a `#clock` estimate (`est`/`drift`) never fills it, and `#ident`/`#clock` lines change no row. | `test_frames_match_extract`, `test_utc_before_the_first_anchor_is_resolved_backwards`, `test_a_chunk_without_anchor_has_null_utc`, `test_utc_is_truncated_to_ms`, `test_ident_and_clock_lines_change_no_frame` |
| D4 | **Decode.** With a DBC for the frame's label: every signal of the matched message becomes one `signals` row (physical value: factor/offset, signed, Intel and Motorola, multiplexed). A message matches only if standard/extended agrees with the `x` flag. Frames without a DBC message stay frames only; LIN is not DBC-decoded. | `test_decode_against_reference` (mini.dbc, hand-computed values), `test_unknown_id_is_frames_only`, `test_an_extended_flag_mismatch_does_not_match` |
| D4a | **Real DBCs carry mistakes.** A DBC loads non-strict: a signal that does not fit its message fails only its own frames (counted in `chunks.detail`), the rest of the file decodes. A DBC that does not load at all exits 2. | `test_a_signal_that_does_not_fit_spares_the_rest_of_the_dbc`, `test_an_unloadable_dbc_exits_two` |
| D5 | **Commit marker last.** For one chunk: delete its rows → insert frames → insert signals → insert events (§7) → write the `chunks` row. A chunk without a `chunks` row with `result=ok` and the current `catalog` is pending. | `test_a_crash_before_the_marker_redoes_the_chunk` |
| D6 | **Idempotent.** Running `run` twice changes nothing: the row counts and the `chunks` rows stay the same. A chunk is pending iff it has no `chunks` row or one under another catalog; `.part` files are never loaded. | `test_second_run_is_a_noop`, `test_partials_are_not_loaded`, `test_a_reused_seq_is_a_new_chunk_key` |
| D7 | **Catalog change.** If a device's DBC changes, all of its chunks become pending. A rerun replaces their signals with no duplicates, and the `frames` count is unchanged. | `test_dbc_edit_redecodes_without_duplicates` |
| D8 | **Defect.** A chunk with a malformed line, an unknown write format, no `#close` (archived chunks are sealed; a torn file gets `#close,…,recovered` from the logger's boot scan, `sd_logger-spec.md` F2c — or the collector accepted it as torn, `sdlog-fleet-spec.md` C12: with a `<file>.torn` sidecar it loads up to its last complete line, `result=ok`, `detail` starts with `torn`) or an `#origin` naming another device: `chunks.result=defect` with the reason, no frames, no signals. A defect is not retried until the catalog changes (the file cannot change). A torn last line is not a defect. | `test_a_bad_chunk_is_a_defect_and_writes_nothing` (parametrized), `test_a_torn_last_line_is_not_a_defect`, `test_a_collector_accepted_torn_chunk_loads` |
| D9 | **Gaps.** `#gap` lines are counted into `chunks.gaps`; the frames around them load normally. | `test_gaps_are_recorded` |
| D10 | **Archive is read-only** to this tool. | `test_archive_is_untouched` |
| D11 | **Exit status** of `run`: 0 all ok; 1 a defect found in this run; 2 usage/config; 75 sink unreachable (nothing marked, retry next run). `status` prints ok/defect/pending per device and exits 1 if any defect is on record. | `test_exit_status`, `test_status_counts` |
| D12 | **One writer.** `flock` on a lock file next to the config, non-blocking; held → exit 0 with "busy", nothing written. | `test_a_held_lock_skips` |
| D13 | **No new runtime dependency for the rest of the tooling.** `sdlog.py`, the collector and the fleet stay stdlib-only; only `sdlog_decode.py` imports the DBC library (§5). | `test_reader_imports_stdlib_only` |
| D14 | Example config and units in `deploy/sdlog-decode/` fictional, scanned by the private-data guard. | `test_the_example_config_loads`, `script/check_no_private_data.py` |
| D15 | **ClickHouse round trip** (only with `SDLOG_CLICKHOUSE_URL`): init, load, idempotent rerun, values, `utc` in ms, re-decode after a DBC edit. Uses a throwaway database and drops it. | `test_clickhouse_round_trip` |
| D16 | **Coverage.** `chunks.matched` = the chunk's CAN frames whose label has a DBC and whose id matched a DBC message (D4). `can_frames` = its CAN (`C`) frames. `status` prints per device `coverage = Σ matched / Σ can_frames` over its ok chunks under the current catalog, so a wrong or thin DBC shows as a number, not as empty signals. | `test_matched_counts_frames_with_a_message`, `test_status_prints_coverage` |

## 4. Out of scope

- **Which vehicle a device served when.** The config states it per device. The consuming
  project derives the config from its own history. A device that moves to another car
  gets a config change, and the old rows keep their old `vehicle`.
- Retention, downsampling, dashboards, alerts. Those are queries over these tables, and so are
  threshold events ("cell spread above x mV"): a threshold that moves must not force a re-decode.
- `I` records (ISO-TP messages reassembled on the device, `sd_logger-spec.md` S5) are reserved and not
  written yet. §7 reassembles from the raw `C` frames instead; when `I` lines arrive they load as frames.
- KWP2000 answers and UDS services other than those in §7.3/§7.4 load as frames only.

## 5. Decisions

| ID | Decision | Why |
|---|---|---|
| O1 | [`cantools`](https://pypi.org/project/cantools/) at runtime, `sdlog_decode.py` only (D13) | A wrong decode is the expensive failure here; `dbc2yaml.py` refuses signals the component cannot decode for the same reason |
| O2 | ClickHouse over its HTTP interface with `urllib`, TSV inserts | No second dependency; the interface is the official one |
| O3 | Lightweight `DELETE … WHERE device=… AND chunk=…` per chunk | Partition replace is only for a full rebuild (drop the database and run) |

## 6. Measured (real chunks, 2026-10-11)

Two archived chunks, 229,300 CAN frames at about 580 frames/s, decoded with a real 400-message DBC
on a laptop against ClickHouse 26.8 on a 2 GB container, over an SSH tunnel:

- 2.2 s CPU, 8.3 s wall, 200 MB RSS for the decoder; ClickHouse peak 50 MB per query
- 824,794 signal rows (172 signals); on disk before merges: frames 664 KiB, signals 1.8 MiB (about 2.2 B per signal row)
- aggregate over all signals 58 ms, frames grouped by id 18 ms

## 7. Diagnostic answers (UDS)

A device that polls an ECU logs the ECU's answers as plain CAN frames on the bus (the logger's own
requests only with `log_tap_tx`). This section turns them into signals and events with the **same
`.dcat` catalog the device was flashed with** ([`uds-catalog-format.md`](uds-catalog-format.md)), read by
`components/uds/catalog.py` (`read_catalog`, `extract`, `evaluate`): one artifact, two readers, so the
host never carries a second copy of a bit position, scale or sentinel. Worked example: the fictional
`MINI` catalog in `tests/uds/fixtures/` (request `0x7E7`, response `0x7EF`).

### 7.1 Reassembly

Per `[[device.uds]]` entry, the CAN frames on its `label` with the catalog ECU's response id (and the
ECU's extended flag) are reassembled as ISO-TP (ISO 15765-2, classic CAN, 8-byte frames): single frame,
first frame + consecutive frames with the sequence number checked, flow control ignored. A message's
`utc` is that of its last frame. Frames on the ECU's request id are the tester's requests (only there
with `log_tap_tx`): a single-frame request is remembered and names the next answer's negative response
(§7.4). Both ids count as `matched` (D16): the catalog covers them.

Every chunk starts fresh (the chunk is the unit of idempotence, D6): consecutive frames before the
chunk's first single or first frame, and a message still open at the chunk's end, are cut by the chunk
boundary and only counted.

### 7.2 Signals

A positive answer belongs to the catalog group whose request it echoes: `answer[0] == request[0] + 0x40`
and the next `len(request) - 1` bytes equal the rest of the request (design §4). Every field of that
group which the answer covers becomes one `signals` row; a field past the answer's end (a `PARTIAL`
answer) is skipped and counted, never guessed.

| field | `value` | `text` |
|---|---|---|
| linear row | `raw * factor + offset` | empty |
| enumerated row (`TEXT`) | `raw * factor + offset` | the row's text |
| `INVALID` row, or no row matches | **no row** (counted as unavailable) | |
| `ASCII` | NaN | the bytes up to the first NUL, Latin-1 |
| `HEXDUMP` | NaN | uppercase hex pairs |

**Names.** `signal = <ECU>.<name>` for a field the `names` file names, `<ECU>.<group>.<field>` (catalog
names: the group's canonical name and the field's name, like a DBC's `<Message>.<Signal>`) for every other
field. `<ECU>` is the catalog's ECU qualifier. The fallback is the same for every unnamed field, because a
field name alone is not unique across groups (two identity DIDs can name their field alike) and a rule that
qualified only the colliding ones would rename a signal when the catalog grows. An array field (`repeat_count > 1`)
is one row per covered element: `<ECU>.<name>.<n>` with `n` = element + 1 (element 0 is the factory's
element 1, format §5.1b), zero-padded to the width of `repeat_count`, so names sort in element order.

The `names` file is TOML and belongs to the consuming project, next to its `.dcat`:

```toml
[fields]
DT_Mini_Voltage_Maximum_Cell = "cell_voltage_max"     # a field, by its catalog name
"0x0208/PRES_Mini_Cell_2Byte" = "cell_voltage"        # <service>/<field> where the name alone is ambiguous
```

`<service>` is a group name, an original service qualifier or a DID (`0x0208`), exactly as `service:` in
YAML (design §3.0). A name is `[A-Za-z][A-Za-z0-9_]*`.

### 7.3 Fault codes

A positive answer to ReadDTCInformation / reportDTCByStatusMask (`59 02 <availability mask>` followed by
4-byte records `<DTC high> <DTC middle> <DTC low> <status>`) is not a catalog group: its layout is the
standard's. Each record's code is written the SAE way, letter and four digits from the first two bytes,
the failure type byte as two hex digits (`P056200`). The `dtc` file gives it a text: one `CODE<TAB>text`
per line, `#` comments and blank lines ignored. A code it does not know is still an event, with empty text.

### 7.4 Events

One `events` row per occurrence, `source` = the ECU qualifier. Each state is followed **within the chunk**;
the first observation in a chunk is an event with `detail = initial`, so a query over chunk boundaries
can tell a real change (`lagInFrame` over `utc`) from a chunk start.

| kind | when | name | value / text | detail |
|---|---|---|---|---|
| `state` | an enumerated field (every valid scale row has a `TEXT`, at least one; a measurement with a text row beside its linear range is not one) shows another value or text than in its previous answer — unavailable counts as a state (NaN, the sentinel's text) | the signal name (§7.2) | new value / new text | `initial`, or `from <old text>` |
| `dtc` | a code appears, changes status, or is missing from a later fault answer of the same ECU (and the same request, when it is logged) | the code | status byte / the `dtc` text | `initial`, `new`, `status 0x2F->0x2E`, `cleared` |
| `nrc` | a negative answer `7F <sid> <nrc>`, except `0x78` (response pending, counted only) | the logged request in hex, else `SID 22` | the NRC / its ISO 14229 name | — |
| `isotp` | reassembly broke inside the chunk: a wrong sequence number, a new message before the open one was complete, a consecutive frame without a first frame | `sequence`, `incomplete`, `orphan` | bytes received / — | expected vs got |
| `unavailable` | a positive answer whose covered numeric fields are all not available (§7.2), at least one | the group's canonical name | number of fields / — | — |

Counts that are not events — answers, partial and unknown answers, unavailable values, pending
answers, frames cut by the chunk boundary — go into `chunks.detail` (`uds: …`).

### 7.5 Requirements

Tests live in `tests/sd_logger/test_decode_uds.py`, against the `MINI` catalog and chunks written in the
test. Expected values are computed by hand in the test, not by `catalog.py`.

| ID | Requirement | Test |
|---|---|---|
| U1 | **Config.** `[[device.uds]]` rejects an unknown key, a missing `label`, a missing or unreadable `.dcat`, an `ecu` the catalog does not hold (or none given for a catalog with several), a missing `names`/`dtc` file, a `names` entry that does not resolve or is ambiguous, an invalid or duplicate name, two fields ending up with one signal name, two entries routing the same CAN id on one label, and an ECU qualifier that is also a DBC message name on the device, with exit 2 naming the problem. | `test_uds_config_rejects` (parametrized), `test_an_ecu_name_that_is_a_dbc_message_exits_two` |
| U2 | **Reassembly.** Single and multi-frame answers are reassembled to their declared length (padding dropped); a message's `utc` is that of its last frame; frames of another id, label or extended flag are ignored. | `test_single_and_multi_frame_answers_decode`, `test_other_ids_and_labels_are_ignored` |
| U3 | **Group by echo.** An answer decodes against the group whose request it echoes; an answer to no group (e.g. `7E 00`) is counted as unknown and writes nothing. | `test_an_answer_decodes_against_the_echoed_group`, `test_an_unknown_answer_writes_nothing` |
| U4 | **Values** follow format §5.1/§6 exactly: signed, scaled, enum value + text, ASCII and HEXDUMP as text with NaN value, `INVALID`/no row → no row. | `test_values_against_hand_computed_reference` |
| U5 | **Partial answers.** Fields and array elements past the answer's end are skipped and counted; the covered ones decode. | `test_a_short_answer_decodes_what_it_covers` |
| U6 | **Names.** Readable names from the `names` file, `<group>.<field>` otherwise; arrays `<name>.<n>`, 1-based, zero-padded to `repeat_count`'s width. | `test_readable_names_and_array_elements` |
| U7 | **State events**: first observation `initial`, a change `from <old>`, no event for a repeat; unavailable is a state; a measurement with a text row is not a state. | `test_state_events`, `test_a_measurement_with_a_text_row_is_not_a_state` |
| U8 | **Fault codes**: SAE code, text from the `dtc` file (empty when unknown), `initial`/`new`/`status`/`cleared`. | `test_dtc_events` |
| U9 | **Negative answers**: `nrc` event with the NRC's name, named by the logged request when there is one; `0x78` is counted, not an event. | `test_nrc_events` |
| U10 | **Broken reassembly** inside a chunk is an `isotp` event; the chunk's edges are counted, not events. | `test_isotp_events_inside_but_not_at_the_edges` |
| U11 | **All unavailable**: an answer whose numeric fields are all not available is one `unavailable` event. | `test_an_all_unavailable_answer_is_an_event` |
| U12 | **Coverage** counts the ECU's request and response frames as matched (D16). | `test_diag_frames_count_as_matched` |
| U13 | **Catalog** hash covers the `.dcat`, `names` and `dtc` files: an edit re-decodes signals and events without duplicates, `frames` unchanged (D7). | `test_a_names_edit_redecodes_without_duplicates` |
| U14 | **Defect** and re-decode delete a chunk's `events` with its other rows (D5, D8). | `test_a_defect_writes_no_events`, `test_a_crash_before_the_marker_redoes_the_chunk` |
| U15 | **Schema**: `init` adds `events` and `signals.text` to an older database without touching its rows; `run` calls it first. DBC signals carry an empty `text`. | `test_run_initializes_the_sink`, `test_clickhouse_round_trip` |
| U16 | **Stdlib only.** The UDS path imports nothing outside the standard library and `components/uds/catalog.py` (D13). | `test_uds_decode_imports_stdlib_only` |

### 7.6 Checked against real chunks (2026-10-16)

Four archived chunks of a real polling device (drive and bench, 534k CAN frames), decoded with the
device's real catalog, in 3.7 s:

- 547 diagnostic answers, 125 distinct signals, 92 events (fault codes with texts, states, two
  `requestOutOfRange`, one ISO-TP message broken off after 27 of 403 bytes); no defect.
- **Cell voltages cross-checked against an independent reader** (the consuming project's own
  log-to-CSV tool, which decodes from the catalog's manifest JSON rather than the `.dcat`): 237
  answers, 22,041 values, 0 differences. The sum of the cells equals the pack voltage the ECU reports.
- An ECU that declares 200 cell slots but has fewer wired answers `SNA` for the rest: those land as
  "unavailable values" in `chunks.detail`, not as rows.
- `test_clickhouse_round_trip` and `test_clickhouse_round_trip_with_uds` green against ClickHouse 26.8
  (the latter upgrading a database created without `events` and `signals.text`).
