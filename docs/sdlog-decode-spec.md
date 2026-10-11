# sdlog_decode: archived chunks into a queryable database

2026-10-11, **draft**. This is the host-side step after `script/sdlog_fleet.py`. The fleet
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
| `frames` | device, chunk, t_us UInt64, utc DateTime64(6) NULL, label, id UInt32, flags, dlc, data UInt64 | MergeTree, ORDER BY (device, label, id, t_us), PARTITION BY (device, toYYYYMM(utc)) |
| `signals` | vehicle, device, chunk, t_us, utc NULL, signal (`<Message>.<Signal>`), value Float64 | MergeTree, ORDER BY (vehicle, signal, t_us) |
| `chunks` | device, chunk, file, seq, sha256, bytes, frames, signals, utc_min/max, gaps, catalog, result, detail, decoded_at | ReplacingMergeTree(decoded_at), ORDER BY (device, chunk) |

- **`chunk` = `<seq>-<sha256[:12]>`.** A card that restarts its seq after an erase
  produces new chunk keys, not collisions (`sdlog-collection-design.md` §10 item 2: no boot id yet).
- **`catalog` = sha256 over the device's DBC files**, sorted by label. It is a content
  hash, so no git is needed and any edit to a DBC counts as a change.
- Signals are stored **in full**, not only on change. Repeated values cost almost nothing
  after delta and ZSTD, and queries need no last-value logic.

## 3. Requirements

| ID | Requirement | Test |
|---|---|---|
| D1 | **Config.** Rejects an unknown key, a duplicate device, a missing DBC file, an unknown sink, with exit 2 naming the problem. | `test_config_rejects` (parametrized) |
| D2 | **Shared reader.** Frames come from the same parser as `sdlog.py extract` (one generator, used by both). `extract`'s output is byte-identical before and after the refactor. | `test_extract_unchanged_after_refactor`, `test_reader_yields_frames_with_utc` |
| D3 | **Frames are lossless.** Every C/L record in a clean chunk becomes one `frames` row with identical t_us/label/id/flags/data; utc is filled where a `#utc` anchor resolves it, NULL before it. | `test_frames_match_extract` |
| D4 | **Decode.** With a DBC for the frame's label: every signal of the matched message becomes one `signals` row (physical value: factor/offset, signed, Intel and Motorola, multiplexed). Frames without a DBC message stay frames only. | `test_decode_against_reference` (mini.dbc, hand-computed values), `test_unknown_id_is_frames_only` |
| D5 | **Commit marker last.** For one chunk: delete its rows → insert frames → insert signals → write the `chunks` row. A chunk without a `chunks` row with `result=ok` and the current `catalog` is pending. | `test_a_crash_before_the_marker_redoes_the_chunk` |
| D6 | **Idempotent.** Running `run` twice changes nothing: the row counts and the `chunks` rows stay the same. | `test_second_run_is_a_noop` |
| D7 | **Catalog change.** If a device's DBC changes, all of its chunks become pending. A rerun replaces their signals with no duplicates, and the `frames` count is unchanged. | `test_dbc_edit_redecodes_without_duplicates` |
| D8 | **Defect.** A chunk that `sdlog.py check` would reject (unknown version, unreadable stamp, missing `#close`): `chunks.result=defect` with the reason, no frames, no signals. Exit 1. A truncated last line is not a defect. | `test_a_bad_chunk_is_a_defect_and_writes_nothing` |
| D9 | **Gaps.** `#gap` lines are counted into `chunks.gaps`; the frames around them load normally. | `test_gaps_are_recorded` |
| D10 | **Archive is read-only** to this tool. | `test_archive_is_untouched` |
| D11 | **Exit status**: 0 all ok; 1 at least one defect; 2 usage/config; 75 sink unreachable (nothing marked, retry next run). | `test_exit_status` |
| D12 | **One writer.** `flock` on a lock file next to the config, non-blocking; held → exit 0 with "busy", nothing written. | `test_a_held_lock_skips` |
| D13 | **No new runtime dependency for the rest of the tooling.** `sdlog.py`, the collector and the fleet stay stdlib-only; only `sdlog_decode.py` imports the DBC library (§5). | `test_reader_imports_stdlib_only` |
| D14 | Example config fictional, scanned by the private-data guard. | `script/check_no_private_data.py` |

## 4. Out of scope

- **Which vehicle a device served when.** The config states it per device. The consuming
  project derives the config from its own history. A device that moves to another car
  gets a config change, and the old rows keep their old `vehicle`.
- Retention, downsampling, dashboards, alerts. Those are queries over these tables.
- UDS / `.dcat` decoding of ISO-TP records (`I`). Those load as frames only for now.

## 5. Open decisions

| ID | Question | Proposal |
|---|---|---|
| O1 | DBC engine: [`cantools`](https://pypi.org/project/cantools/) (mature, multiplexing, both byte orders, a runtime dependency) or extend `dbc2yaml.py`'s stdlib parser (no dependency, but every edge case is ours to get right) | `cantools` at runtime for `sdlog_decode.py` only (D13). A wrong decode is the expensive failure here, and `dbc2yaml.py` already refuses signals the component cannot decode for exactly that reason |
| O2 | Sink transport: ClickHouse HTTP via `urllib` (TSV inserts) or a driver | HTTP via `urllib`: no second dependency, and the interface is the official one |
| O3 | Delete before re-insert: lightweight `DELETE` or partition replace | lightweight `DELETE … WHERE device=… AND chunk=…` per chunk; partition replace is for a full rebuild only |
