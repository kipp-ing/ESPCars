# sdlog_decode: archived chunks into a queryable database

2026-10-11. O1–O3 decided (cantools, HTTP via urllib, lightweight DELETE per chunk). This is the host-side step after `script/sdlog_fleet.py`. The fleet
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
                                                      └──▶ DBC decode ──▶ sink: signals
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
```

## 2. Tables

The tool creates these (`init`). They are written only by this tool.

| Table | Columns | Engine / order |
|---|---|---|
| `frames` | device, chunk, kind (C/L/U/I), label, utc DateTime64(3) NULL, id UInt32, flags, dlc, data UInt64 (the data hex as one big-endian number) | MergeTree, PARTITION BY device, ORDER BY (device, label, id, utc) |
| `signals` | vehicle, device, chunk, signal (`<Message>.<Signal>`), utc DateTime64(3) NULL, value Float64 | MergeTree, PARTITION BY vehicle, ORDER BY (vehicle, signal, utc) |
| `chunks` | device, chunk, file, seq, sha256, bytes, frames, can_frames, matched, signals, utc_min/max DateTime64(3) NULL, gaps, catalog, result, detail, decoded_at | ReplacingMergeTree(decoded_at), ORDER BY (device, file) |

- **`chunk` = `<seq>-<sha256[:12]>`.** A card that restarts its seq after an erase
  produces new chunk keys, not collisions (`sdlog-collection-design.md` §10 item 2: no boot id yet).
- **`catalog` = sha256 over the device's DBC files**, sorted by label. It is a content
  hash, so no git is needed and any edit to a DBC counts as a change.
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
| D5 | **Commit marker last.** For one chunk: delete its rows → insert frames → insert signals → write the `chunks` row. A chunk without a `chunks` row with `result=ok` and the current `catalog` is pending. | `test_a_crash_before_the_marker_redoes_the_chunk` |
| D6 | **Idempotent.** Running `run` twice changes nothing: the row counts and the `chunks` rows stay the same. A chunk is pending iff it has no `chunks` row or one under another catalog; `.part` files are never loaded. | `test_second_run_is_a_noop`, `test_partials_are_not_loaded`, `test_a_reused_seq_is_a_new_chunk_key` |
| D7 | **Catalog change.** If a device's DBC changes, all of its chunks become pending. A rerun replaces their signals with no duplicates, and the `frames` count is unchanged. | `test_dbc_edit_redecodes_without_duplicates` |
| D8 | **Defect.** A chunk with a malformed line, an unknown write format, no `#close` (archived chunks are sealed; a torn file gets `#close,…,recovered` from the logger's boot scan, `sd_logger-spec.md`) or an `#origin` naming another device: `chunks.result=defect` with the reason, no frames, no signals. A defect is not retried until the catalog changes (the file cannot change). A torn last line is not a defect. | `test_a_bad_chunk_is_a_defect_and_writes_nothing` (parametrized), `test_a_torn_last_line_is_not_a_defect` |
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
- Retention, downsampling, dashboards, alerts. Those are queries over these tables.
- UDS / `.dcat` decoding of ISO-TP records (`I`). Those load as frames only for now.

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
