# sdlog_fleet — unattended collection from many devices

2026-10-10. The host-side companion to `script/sdlog_collect.py`. That tool is
one pass against one device, run by hand. This one is what runs on a home
server every few minutes, against every device in a config file, and says
something only when something is actually wrong.

Every requirement below carries an ID and the test that proves it. **A
requirement without a test is not done.** Tests live in
`tests/sd_logger/test_collect.py` (collector exit contract, `C*`) and
`tests/sd_logger/test_fleet.py` (fleet, `F*`), both against the in-process
`FakeDevice`. No board and no WiFi are needed.

## 1. Why the collector's exit status had to change first

The fleet calls the collector as a subprocess and judges each pass by its exit
status alone. Its whole alarm policy (*a car that is away is silent, a defect is
loud*) is only as good as that status. Before this change it was not good
enough:

| Before | Problem |
|---|---|
| `2` = device unreachable | `2` is also argparse's usage error and what `python missing.py` returns. A wrong path in the deploy would look like "car away" forever. |
| `1` = any chunk not collected | It mixed "WiFi dropped mid-chunk", which needs a silent retry next run (E3), with "the chunk fails verification", which needs a human. |
| `2` = index answered 404 / not JSON | The URL points at something that is not this device: a wrong port, or a DHCP lease handed to another host. That is a config error, and it looked like "car away". |
| disk full | A failed write was caught as a transport error, retried, and reported as one more failed chunk. |

## 2. Collector exit contract (`sdlog_collect.py pull` and `index`)

Codes 74 and 75 are the BSD `sysexits.h` values (`EX_IOERR`, `EX_TEMPFAIL`; see
[sysexits(3)](https://man.freebsd.org/cgi/man.cgi?query=sysexits)). Python exposes
them as `os.EX_IOERR` and `os.EX_TEMPFAIL`, and nothing in the toolchain emits
them by accident.

| Exit | Meaning | The fleet treats it as |
|---|---|---|
| `0` | Every listed chunk is archived, verified and confirmed. An empty index counts too. | success |
| `1` | **Defect.** At least one chunk needs a human: it failed verification, the archived copy under its name is a different file, the index entry is broken, or the device refused a request (4xx) for a reason other than a benign race. | alarm |
| `2` | Usage error (argparse). Not emitted by the collector itself. | alarm |
| `3` | **Wrong target.** Something answered at the URL, but not the expected sd_logger device: the index answered a non-retryable 4xx, or a body that is not a JSON object, or a device name other than `--expect`. | alarm |
| `74` | **Archive I/O.** The local archive could not be written (full disk, permissions). The pass stops at the first such error, because later chunks would hit the same disk. | alarm |
| `75` | **Only the network failed.** Either the device is not there (connection refused or timed out, or the index kept answering 5xx/408/429), or the pass was cut short by transport failures alone. Partials stay in place and the next run resumes. | silent |

When several apply, the most severe wins: `74 > 3 > 1 > 75 > 0`.

**Transient, not defects** (they lead to 75 when nothing worse happened): a body
cut short, a socket error or timeout, retryable statuses after the attempts run
out, during the fetch, the identity re-check or the confirm; a fetch answered
`404` (already confirmed) or `409` (open file named by a stale index); and a
partial dropped as a mismatch when the attempts ran out before the whole fetch.
All of these converge on the next run without help.

| ID | Requirement | Test |
|---|---|---|
| C1 | An unreachable device exits 75, for both `pull` and `index`. | `test_pull_exits_tempfail_when_the_device_is_unreachable`, `test_index_exits_tempfail_when_the_device_is_unreachable` |
| C2 | An index that keeps answering 5xx exits 75. | `test_an_index_that_keeps_stumbling_exits_tempfail` |
| C3 | An index answered with 404, with non-JSON or with a non-object exits 3, and is not retried. | `test_an_index_from_something_else_exits_wrong_target` (parametrized) |
| C4 | `--expect NAME`: a device naming itself differently, or not at all, exits 3 before any chunk is fetched or confirmed. | `test_expect_refuses_a_different_device`, `test_expect_refuses_a_device_that_does_not_say_who_it_is`, `test_expect_accepts_the_right_device` |
| C5 | A transfer cut mid-chunk, with every attempt cut too, exits 75 and keeps the partial. | `test_a_pass_cut_short_by_the_network_exits_tempfail` |
| C6 | A chunk that fails verification exits 1, even when another chunk was only cut short. | `test_a_defect_outranks_a_network_cut` |
| C7 | A confirm that never gets through exits 75, and a refused confirm exits 1. | `test_a_confirm_lost_to_the_network_exits_tempfail`, `test_a_refused_confirm_exits_defect` |
| C8 | An archive that cannot be written exits 74, stops the pass, and confirms nothing. | `test_an_unwritable_archive_exits_ioerr_and_confirms_nothing` |
| C9 | A disk that fails mid-write is reported as archive I/O and is not retried as a transport error. | `test_a_write_error_mid_chunk_is_archive_io_not_network` |
| C10 | A fetch answered 404/409 is transient, not a defect. | `test_a_fetch_race_is_transient` |

## 3. Fleet behaviour (`script/sdlog_fleet.py`)

```
sdlog_fleet.py run    --config fleet.toml             # one pass over every device
sdlog_fleet.py run    --config fleet.toml --every 300 # container loop
sdlog_fleet.py status --config fleet.toml             # heartbeat table; exit 1 if any alarm
```

### 3.1 Config

TOML, read with the stdlib `tomllib`, so the host needs Python ≥ 3.11 and
nothing else. The instance config, with real names, addresses and push tokens,
lives with the deployment and never in this repo. `deploy/sdlog-fleet/` holds a
fictional example.

```toml
archive = "/srv/sdlog/archive"     # chunks land in <archive>/<device>/, as the collector lays them out
state = "/var/lib/sdlog-fleet"     # <device>.json heartbeat files and <device>.lock
stale_after_days = 3               # E4: alarm when the last success is older than this
min_free_mb = 1024                 # pre-flight: alarm instead of pulling onto a nearly full disk
timeout_s = 3600                   # per device pass; a killed pass resumes next run

[[device]]
name = "mr-orange"                 # must equal the name the device reports (--expect)
url = "http://192.0.2.10:8080"
push_url = "http://kuma.example:3001/api/push/TOKEN"   # optional
```

### 3.2 Requirements

| ID | Requirement | Test |
|---|---|---|
| F1 | **Config.** It loads the archive, state, thresholds and a list of devices. It rejects an unknown key, a duplicate name, a name that cannot be a path component, a non-http(s) URL, a non-positive threshold, and an empty device list, with exit 2 and a message that names the problem. A typo such as `pushurl` must not disable an alarm without anyone noticing. | `test_config_loads_the_example`, `test_config_rejects` (parametrized), `test_a_bad_config_exits_two` |
| F2 | **One collector pass per device**: `sdlog_collect.py pull <url> --into <archive> --expect <name>` as a subprocess, using the interpreter the fleet runs under and the collector next to it. | `test_each_device_gets_one_pull_with_expect`, `test_end_to_end_two_devices_are_archived_and_confirmed` |
| F3 | **Exit-status mapping.** 0 → `ok`; 75 → `offline`; anything else (incl. 1, 2, 3, 74, a signal, a timeout, a collector that cannot be started) → `error`. | `test_exit_status_mapping` (parametrized), `test_a_hung_pass_is_killed_and_is_an_error`, `test_a_missing_collector_is_an_error_not_offline` |
| F4 | **Away is silent.** An `offline` pass changes neither `last_success` nor the verdict. It does not fail the fleet's exit status, and it does not turn the push red while the last success is fresh. | `test_an_offline_device_is_quiet`, `test_end_to_end_an_absent_device_is_quiet` |
| F5 | **E3 end-to-end.** WiFi lost mid-transfer → that run is `offline` and the partial is kept. The next run completes the chunk, confirms it once and stores it once. | `test_end_to_end_a_cut_transfer_resumes_on_the_next_run` |
| F6 | **Append-only.** The fleet never deletes, rewrites or truncates a file in the archive. An archived file is byte-identical and keeps its mtime after any run, including one that hit a same-name mismatch. | `test_end_to_end_the_archive_is_append_only` |
| F7 | **Disk pre-flight.** Free space under `min_free_mb` → every device's pass is an `error` ("archive low on space") and no collector is started. | `test_a_nearly_full_disk_is_an_error_and_pulls_nothing` |
| F8 | **Heartbeat file** `<state>/<name>.json`, written atomically after every non-busy pass. It holds `last_run`, `last_result`, `last_success`, `verdict`, `last_error {at, exit, reason}` and `created`. | `test_the_heartbeat_file_records_each_pass`, `test_the_heartbeat_file_is_replaced_atomically` |
| F9 | **Alarm rule (E4).** A device is in alarm iff `verdict == "error"` or the age of `last_success` exceeds `stale_after_days`. A device never collected is aged from `created`, so a fresh deploy does not alarm at once. An error stays in alarm through later `offline` passes, and only a successful pass clears it. | `test_staleness_raises_the_alarm`, `test_a_never_collected_device_ages_from_creation`, `test_an_error_survives_offline_passes_until_a_success` |
| F10 | **Push** (optional per device): a GET to `push_url` with `status=up\|down` and `msg=<summary>` after every `ok`, `offline` and `error` pass, `down` iff in alarm. A push that fails is logged and changes nothing else. Kuma's push route takes exactly these parameters ([api-router.js](https://github.com/louislam/uptime-kuma/blob/master/server/routers/api-router.js): `status` defaults to `up`, anything else is down; `msg`; 404 on an unknown token). | `test_push_reports_up_and_down`, `test_push_failure_does_not_fail_the_pass`, `test_push_message_is_bounded_and_encoded` |
| F11 | **Lock per device**: `flock` on `<state>/<name>.lock`, non-blocking. If it is held, the pass is `busy`: no collector, no state write, no push. The kernel drops the lock if the holder dies. | `test_a_held_lock_skips_the_device_quietly`, `test_the_lock_is_released_after_a_pass` |
| F12 | **Devices are independent.** They run concurrently, so one slow or hung device does not starve the others. | `test_a_slow_device_does_not_delay_another` |
| F13 | **Fleet exit status**: 0 if no device had an `error` in this run, 1 otherwise, 2 for a usage or config error. Staleness alone does not fail a run; that is `status`'s job (F14). | `test_run_exit_status` |
| F14 | **`status`** prints one line per device (result, age of the last success, alarm) and exits 1 if any device is in alarm. | `test_status_reports_alarm_and_exits_one` |
| F15 | **`--every N`** repeats `run` every N seconds until interrupted. | `test_every_repeats_the_pass` |
| F16 | The example config and units in `deploy/sdlog-fleet/` are fictional, and the private-data guard scans them (`.toml`, `.service`, `.timer`). | `test_config_loads_the_example`, `script/check_no_private_data.py` |

### 3.3 Why the push goes out on every pass, not only on success

The E4 threshold (3 days) lives in this config, not in Kuma's monitor interval.
The fleet knows when the last success was and pushes `down` itself once it is
too old. So the Kuma push monitor's own interval only has to cover **"is the
fleet host still running"** (e.g. 30 min with a 5 min timer), and nothing
depends on Kuma accepting an interval of several days. If the host dies, the
pushes stop, and Kuma raises the alarm on its own.

### 3.4 Deliberately out of scope

- An HTTP status endpoint, an archive index, MF4 export. Those are a later step
  (the wish list calls it "Status-Web"). The heartbeat JSON is the interface
  they would read.
- Backup. The archive is append-only so an external tool (Borg or similar) can
  snapshot it at any time.
- Retiring a chunk the collector keeps refusing: for example a new card reusing
  a seq whose archived copy is a different file (`sdlog-collection-design.md`
  §10.2, no boot id yet). The fleet raises the alarm, and a human decides.
- Wake-on-arrival. Polling every few minutes is cheap: a device that is away
  costs one refused TCP connect.

## 4. Deployment shape

`deploy/sdlog-fleet/` ships a fictional `fleet.example.toml`, plus
`sdlog-fleet.service` (oneshot) and `sdlog-fleet.timer` (every 5 min). systemd
does not start a second instance of a oneshot that is still running, and the
per-device lock (F11) covers manual runs and the container loop
(`run --every 300`). The instance (host, config, Kuma monitors) is deployed
from outside this repo.
