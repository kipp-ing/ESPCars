# P9 bench sequence

Keep binaries, credentials, logs, JSON, scope traces, SD archives and comparison
Markdown **outside this repository**, e.g. `/tmp/p9`. Never attach real vehicle
traffic. These images are for the isolated bench only. Use the same ESPHome
installation, frozen YAML hash, boards and capture durations for both refs.
No command below builds firmware on the bench.

Prepare before the session (14 images per ref):

```bash
.venv/bin/python script/hil/p9_build.py cb0db60 --out /tmp/p9/ref --secrets /tmp/bench-secrets.yaml
.venv/bin/python script/hil/p9_build.py HEAD --out /tmp/p9/now --yaml-source /tmp/p9/ref/yaml --secrets /tmp/bench-secrets.yaml
```

Both manifests must have the same frozen YAML hash. The snapshot includes
uncommitted harness files; `master`/`slave` work on both schemas. Local component
paths resolve inside each selected worktree. Every build validates all 14 roles
against reference/HEAD/target; commands/results/SHAs are in `config_checks`.
`--config-only` needs no secrets and creates no images. Real collection builds
need the external WiFi secrets. Private build directories prevent node-name
collisions. Flash the hashed factory image at 0x0, never a cached node upload.

For **each** set, reference first, set `M` and `D` accordingly:

```bash
M=/tmp/p9/ref/manifest.json
D=/tmp/p9/ref
# Second pass: M=/tmp/p9/now/manifest.json; D=/tmp/p9/now
```

1. Measure **12 V on J8**, check GPIO7 / JP14 with the selftest rail/xcvr gates,
   and identify all three C6 boards with `script/hil/ports.py`. Check that the
   bench carries only fictional traffic. Stop any previous stress run before
   the echo gate: put *all* boards on quiet selftest firmware before judging it.

   ```bash
   .venv/bin/python script/hil/p9_measure.py --manifest "$M" --role selftest-green --role selftest-blue --role selftest-orange --seconds 1 --flash --flash-only --out "$D/selftest-flash.json"
   .venv/bin/python -c 'import sys; sys.path.insert(0,"script/hil"); import selftest; selftest.reset_all(["green","blue","orange"])'
   .venv/bin/python script/hil/selftest.py --no-flash --seconds 35 --report "$D/selftest.md"
   ```

   Require every reach/rail/xcvr/bus/LIN/SD gate, Green's `lin.selftest`, and
   topology to pass. `--no-flash` names reach as n/a; the preceding successful
   manifest uploads supply reach evidence. Green drives LIN, Blue responds,
   Orange listens. Resolve identity through MAC every time; never reuse a port.

2. Ring: Green → `ring-green`, Blue → `ring-blue`, Orange → `ring-orange`.

   ```bash
   .venv/bin/python script/hil/p9_measure.py --manifest "$M" --role ring --seconds 45 --flash --out "$D/ring.json"
   .venv/bin/python script/hil/verify.py --seconds 45 --min-laps 75 --outdir "$D/verify" > "$D/verify.txt"
   ```

   Require verify exit 0, zero error counters, laps/s ≥ reference **and ≥19.4**.
   Attach a scope/CAN analyzer during this capture: LIN on GPIO19/18 / J9.3;
   both CAN segments, Green on Orange GPIO2/3, Blue on Orange GPIO10/11.
   Confirm wiring from this run's `[topo]` lines; do not open another capture
   tool concurrently. Trigger on the existing cyclic IDs 0x15x/0x16x; export
   interval distribution and maximum absolute deviation from 10/25 ms.
   Record LIN break (half-baud 0x00), sync/PID order, first-response-byte latency,
   response/inter-byte windows, and schedule cadence for all three modes.
   Repeat with the existing blocking-warning stalls visible, without adding a
   new stall/TX workload. Check listener has zero TX and that IDs, payloads,
   checksums, timeout/length-hint finalization and break/framing recovery match.

3. Separate active-loop profile (adds runtime_stats overhead; never use its lap
   rate for the baseline gate). Same board allocation as ring:

   ```bash
   .venv/bin/python script/hil/p9_measure.py --manifest "$M" --role profile --seconds 90 --flash --out "$D/profile.json"
   ```

   `[loop]` gives max period and approximate mean (10 s/count; inspect startup
   samples separately); runtime_stats gives active iteration max. Blocking
   warnings time individual operations. CPU/task stats attribute load.

4. Loaded gateway: Green → `load-green`, Blue → `load-blue`, Orange →
   `gateway-orange`, **all together at 500 kbps**; Green/Blue burst both 2.

   ```bash
   .venv/bin/python script/hil/p9_measure.py --manifest "$M" --role load-green --role load-blue --role gateway-orange --seconds 120 --flash --out "$D/load.json"
   ```

   Reconcile drops/taps/errors and heap under load. Measure RX-wire→forwarded-wire
   latency with the CAN analyzer; this includes arbitration and is **not** true
   ISR-entry→forward-call latency. Neither ref prints that internal number or
   the complete slot tracker diagnostics. Preserve printed `Slots in use` and
   tracker counters if present; leave missing internal evidence open in the
   ledger. Closing it needs a separately reviewed component instrumentation
   change; this harness does not modify ISR/component code.

5. SD rotation / collection: return Green and Blue to `ring-*` (125 kbps) so
   Green supplies the LIN schedule; Orange has no CAN ports in these two images.
   Use the bench test card. `rotate-orange` parks retention at 98%; collection
   retains the existing 90% setting. Allow multiple 4 MB rotations; do not reset
   during writes. No format/power-cycle/retention change is part of this run.

   ```bash
   .venv/bin/python script/hil/p9_measure.py --manifest "$M" --role ring-green --role ring-blue --seconds 1 --flash --flash-only --out "$D/sd-partners.json"
   .venv/bin/python script/hil/p9_measure.py --manifest "$M" --role rotate-orange --seconds 120 --flash --out "$D/rotate.json"
   # In a second terminal, while the next capture runs, pull sealed chunks:
   .venv/bin/python script/hil/p9_measure.py --manifest "$M" --role collect-orange --seconds 120 --flash --out "$D/collect.json"
   .venv/bin/python script/sdlog_collect.py index http://mr-orange.local:8080
   .venv/bin/python script/sdlog_collect.py pull http://mr-orange.local:8080 --into "$D/archive"
   .venv/bin/python script/sdlog.py check "$D"/archive/mr-orange/L*.LOG
   .venv/bin/python script/sdlog_decode.py run --config "$D/decode.toml"
   .venv/bin/python script/sdlog_decode.py status --config "$D/decode.toml"
   ```

   Use external decode configs based on `deploy/sdlog-decode/decode.example.toml`,
   with the same fictional decode rules and separate reference/current output
   databases. Require zero defects/drops, file sequence advancing on rotation,
   successful serve/confirm, no rail sag. Compare decoded kind/label/ID/flags/
   DLC/data and signal values by the fictional producer sequence, excluding boot
   time, file numbers and log-text records; different wall-clock runs are not
   byte-identical archives. Keep collection/decode evidence with both run records.

Compare each pair (`ring`, `profile`, `load`, `rotate`, `collect`):

```bash
.venv/bin/python script/hil/p9_compare.py /tmp/p9/ref/ring.json /tmp/p9/now/ring.json --out /tmp/p9/ring.md
```

Exit 0 = printed metrics pass; 1 = hard failure; 2 = REVIEW (missing evidence,
different inputs/boards, or >5% delta where P9 has no hard limit). Scope-only
metrics remain REVIEW, never fabricated zeroes. A no-flash capture assumes the
operator has staged the manifest's images; it cannot attest the running binary.
Real evidence stays external; link it from the review, never paste it into git.

| Ledger row | Evidence required |
|---|---|
| P3b-sd_logger | Setup/load heap free/largest/min; load, rotate, collect JSON; decoded payload/values and clean file/collection sequence |
| P4 commander | Green LIN scope traces and schedule/selftest; ring/profile loop and errors, including observed stalls |
| P4 responder | Blue response latency/window, payload/checksum and recovery traces; ring/profile errors |
| P4 listener | Orange zero-TX/decoded-frame/recovery traces; ring/load/collect errors |
| P4 logging | DEBUG capture has no new low-level RX/TX/event-task diagnostics; initialization-error reporting remains an explicit separate exercise; VERBOSE diagnosis separately labelled |
| P4 YAML aliases | Both-schema config manifest entries; same selftest counters/TX roles and ring outcomes |
| P6 schema guards | Both-schema manifests, selftest/verify; unsupported-platform rejections remain pytest evidence |
| P9 harness | Matching YAML/tool/board hashes; separate profiling overhead; missing ISR/slot evidence explicitly open |

Only mark a ledger row complete when **all** its evidence exists. The capture
tools prepare evidence; they do not claim that P9 or its recovery exercises ran.
