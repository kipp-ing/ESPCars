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
   Acquire the separate passive blocks described below with these same images.
   Scope captures are sequenced after serial collection: pico-bench's grounding
   contract requires DUT USB disconnected. Preserve the serial stall/recovery
   exercises separately; a wire trace alone cannot attest firmware finalization
   or that a particular blocking warning occurred.

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

   Reconcile drops/taps/errors and heap under load. The two-channel scope block
   below measures input EOF → output SOF, including arbitration/queueing.
   **RX-ISR entry → forward-call remains REVIEW**: current esp_timing cannot
   safely wrap these IRAM callbacks (feasibility audit below). Preserve printed
   `Slots in use` and tracker counters if present; missing counters stay open.

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
different inputs/boards, or >5% delta where P9 has no hard limit). Scope results can be attached with `--scope-ref NAME=JSON --scope-now NAME=JSON`
(repeat per pair); absent metrics remain REVIEW, never fabricated zeroes. A no-flash capture assumes the
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
| P9 harness | Matching YAML/tool/board hashes; hashed PicoScope raw blocks and stdlib wire analyses; separate profiling overhead; esp_timing IRAM limitation and missing slot evidence explicitly open |

Only mark a ledger row complete when **all** its evidence exists. The capture
tools prepare evidence; they do not claim that P9 or its recovery exercises ran.

## Passive PicoScope blocks

The bench scope is a **3203D / ps3000a**, with **64,000,000 samples shared by
all enabled channels**. `capture` checks `ceil(rate * seconds) * channels`
before opening/arming; only A/B are admitted. It disables the unused channel,
uses a real edge trigger (no auto-trigger), and captures one block. It never
calls the AWG. Close PicoScope and any other scope process first. No new bus
workload, fault injection or transmission is introduced by this procedure.

Set the sibling location explicitly; nothing discovers a machine-specific path:

```bash
export PICOBENCH_DIR="$(cd ../../../../pico-bench && pwd)" # from this worktree; adjust for a normal checkout
# Alternatively: capture --picobench-dir "$PICOBENCH_DIR" ...
```

For the sibling's Rosetta SDK and x86 venv setup, follow **pico-bench README.md
and docs/macos-sdk-setup.md**. The wrapper always runs
`arch -x86_64 "$PICOBENCH_DIR/.venv-x86/bin/python"`; its helper imports
`$PICOBENCH_DIR/src/picobench/driver`. ESPCars' arm64 interpreter never imports
picobench or numpy. Acquisition uses `Scope.capture_block`; the public API's
volts are rounded back onto its actual `_max_adc` integer scale, preserving
ADC counts. No ctypes/SDK acquisition implementation is duplicated here.
`picobench.measure` supplies analog numpy measurements, not LIN/CAN decoding;
the protocol analysis here needs only the Python stdlib.

**Grounding:** follow pico-bench HANDOVER.md: floating bench PSU only, **all DUT
USB physically disconnected**, one scope-to-DUT GND tie. Flash/identify boards
by MAC and finish serial collection first. Power the isolated rig from 12 V on
J8; use the same manifest images. Do not run serial logging/esp_timing capture
while the scope is attached under this grounding arrangement. Reconnect USB
only after removing the probes/ground tie. Record the manifest SHA, board MACs,
role allocation, supply/probe arrangement and serial-run association in an
external operator note under `$D/scope/`. This note binds the scope evidence to
the firmware; a scope cannot identify the running image.

All commands below run from ESPCars. For each side set `D=/tmp/p9/ref` or
`D=/tmp/p9/now` as above, and `SIDE=ref` or `SIDE=now`. Paths are deliberately
`$D/scope/<ref|now>-<what>/`; `capture` requires a **new** directory. Use a new
suffix for repeats/stalls; never overwrite a raw block. `--range-v` and trigger
volts refer to the **scope BNC**; `--probe` multiplies the saved voltage scale
for analysis, whose `--threshold` is at the **probe tip**. Set both physical
probes to the same attenuation. The actual driver-selected `dt_s` is recorded;
analysis refuses <20 samples/bit, even if the requested rate looked sufficient.

| Capture | Existing images / traffic | Probe A | Probe B | Requested rate / seconds / shared samples | Trigger (BNC) |
|---|---|---|---|---|---|
| commander LIN | ring-green + ring-blue + ring-orange; 19,200 baud | J9.3 LIN vs GND, **10:1** | disabled | 500,000 / 2 / 1 MS | A falling 0.6 V (6 V bus) |
| responder LIN | same, observe Blue response to ID 0x31 | J9.3 LIN vs GND, **10:1** | disabled | 500,000 / 2 / 1 MS | A falling 0.6 V |
| listener LIN | same, Orange listener | J9.3 LIN, **10:1** | Orange **GPIO19 TXD**, **10:1** | 500,000 / 2 / 2 MS | A falling 0.6 V |
| ring CAN | ring-*; **125 kbps** | Orange **GPIO3 RX**, Green-side segment | Orange **GPIO11 RX**, Blue-side segment | 5,000,000 / 2 / 20 MS | A falling 1.65 V, 1:1 probes |
| loaded forward | load-green + load-blue + gateway-orange; **500 kbps** | Orange **GPIO3 RX**, Green-side input | Orange **GPIO11 RX**, Blue-side output | 20,000,000 / 1.5 / 60 MS | A falling 1.65 V, 1:1 probes |

Pin evidence: `mr-orange.yaml` maps `green_side` to TX GPIO2 / **RX GPIO3**,
`blue_side` to TX GPIO10 / **RX GPIO11**; `stress-orange-gateway.yaml` has those
same RX pins at 500 kbps. **GPIO2/10 are TX, not probe RX points.** HIL.md's
historical CAN1/CAN2 names are corrected by its topology note: verify the
current `[topo]` results. Green/Blue ring CAN are 125 kbps and LIN is 19,200 in
`mr-green.yaml`, `mr-blue.yaml`, `mr-orange.yaml`; loaded CAN is 500 kbps in
`stress-green-load.yaml`, `stress-blue-load.yaml`, `stress-orange-gateway.yaml`.
Confirm these against the frozen YAML before each run. Do not probe CANH/CANL
with the logic thresholds here.

There are only two channels: capture LIN, re-probe for CAN, then change to the
existing loaded images for forward latency. Never re-probe a powered pin
blindly. At 500 kbps, 20 MS/s gives 40 requested samples/bit and timebase
rounding margin above the 20-sample minimum. 5 MS/s gives 40 at 125 kbps;
500 kS/s gives 26 at 19,200 baud. Actual block duration can be slightly longer
than requested because pico-bench selects the next slower supported timebase;
the sample count and shared-buffer bound stay fixed.

```bash
mkdir -p "$D/scope"
# Repeat for WHAT=ring-green-lin and WHAT=ring-blue-lin with the same ring allocation.
WHAT=ring-green-lin
.venv/bin/python script/hil/p9_scope.py capture --what "$WHAT" --channels A --rate 500000 --seconds 2 --range-v 2 --probe 10 --trigger A:falling:0.6 --out "$D/scope/$SIDE-$WHAT"
.venv/bin/python script/hil/p9_scope.py lin "$D/scope/$SIDE-$WHAT" --baud 19200 --threshold 6 --length 0x30=4 --length 0x31=4 --checksum enhanced --out "$D/scope/$SIDE-$WHAT/result.json"

# Listener: both probes 10:1, B is GPIO19 TXD (3.3 V logic at the tip).
WHAT=ring-orange-listener
.venv/bin/python script/hil/p9_scope.py capture --what "$WHAT" --channels A,B --rate 500000 --seconds 2 --range-v 2 --probe 10 --trigger A:falling:0.6 --out "$D/scope/$SIDE-$WHAT"
.venv/bin/python script/hil/p9_scope.py lin "$D/scope/$SIDE-$WHAT" --baud 19200 --threshold 6 --length 0x30=4 --length 0x31=4 --checksum enhanced --tx-channel B --out "$D/scope/$SIDE-$WHAT/result.json"

# Ring CAN: re-probe both channels, 1:1, range +/-5 V at BNC.
WHAT=ring-orange-can
.venv/bin/python script/hil/p9_scope.py capture --what "$WHAT" --channels A,B --rate 5000000 --seconds 2 --range-v 5 --trigger A:falling:1.65 --out "$D/scope/$SIDE-$WHAT"
.venv/bin/python script/hil/p9_scope.py can "$D/scope/$SIDE-$WHAT" --channel A --baud 125000 --threshold 1.65 --period 0x15x=10ms --period 0x16x=25ms --out "$D/scope/$SIDE-$WHAT/A-result.json"
.venv/bin/python script/hil/p9_scope.py can "$D/scope/$SIDE-$WHAT" --channel B --baud 125000 --threshold 1.65 --period 0x15x=10ms --period 0x16x=25ms --out "$D/scope/$SIDE-$WHAT/B-result.json"

# Flash the existing load-* / gateway-orange allocation first, then disconnect USB.
WHAT=gateway-orange-forward
.venv/bin/python script/hil/p9_scope.py capture --what "$WHAT" --channels A,B --rate 20000000 --seconds 1.5 --range-v 5 --trigger A:falling:1.65 --out "$D/scope/$SIDE-$WHAT"
.venv/bin/python script/hil/p9_scope.py forward "$D/scope/$SIDE-$WHAT" --baud 500000 --threshold 1.65 --out "$D/scope/$SIDE-$WHAT/A-to-B.json"
.venv/bin/python script/hil/p9_scope.py forward "$D/scope/$SIDE-$WHAT" --baud 500000 --threshold 1.65 --input-channel B --output-channel A --out "$D/scope/$SIDE-$WHAT/B-to-A.json"
```

The two LIN producer roles share one bus; their independent captures label
which timing exercise is being reviewed, not different electrical segments.
Repeat the passive captures while the existing workload produces stalls;
retain separately associated serial warnings. No extra stall/TX workload is
added. Probe Orange TXD to establish zero listener transmission: the shared LIN
bus alone cannot identify the sender. Any dominant TXD sample fails that gate.
Do not select A as both LIN bus and TXD.

`capture.json` binds `A.bin`/`B.bin` (signed little-endian int16, per-file SHA256)
to `dt_s`, BNC range, ADC max, probe ratio, trigger, overflow, scope identity and
requested settings. `capture_sha256` hashes canonical `json.dumps(meta,
sort_keys=True)` **before** adding that field, including channel hashes.
Analysis verifies all hashes before decoding. Each result records capture hash,
actual dt, channel, threshold, scope serial and tool version. Overflow is a
hard analysis error. Preserve raw blocks and results together outside git.

LIN reports break/delimiter in bit times, UART 0x55, PID parity, header duration,
PID stop-end → first response start, spaces from stop-end → next start, payload,
both checksum checks, frame duration and PID start-to-start cadence. Prefer
explicit lengths from the role config; without hints it infers bytes until the
next break/capture edge and cannot prove the expected payload length. Checksum
mode defaults to enhanced; IDs 0x3c/0x3d always require classic. `auto` accepts
either and is exploratory, not a matched enhanced-checksum gate. Nominal frame
length is `(34 + 10 * (data_length + 1)) / baud`; the measured break-start →
checksum-stop-end must be <=1.4 times that. No-response headers are explicit
and stay REVIEW for response/finalization evidence. A capture starting within
a break is ignored until the next complete header. Retain complete trailing
frames (a cut-off frame reports an error; recapture with a different window).

CAN supports **Classical CAN** base/extended and remote frames, SOF after at
least 11 recessive bits, bit-centre sampling with falling-edge boundary resync
(SJW 0.2 nominal bit), stuffing through CRC sequence, CRC-15, delimiter/ACK/EOF.
It reports ACK presence but does not infer a delivery acknowledgement from an
unmatched frame. Error/overload/FD frames are not decoded as data; observed
stuff/form/CRC failures are errors, never valid samples. This offline decoder
is synthetic-tested, **not yet scope-validated**. Inspect real traces if the
waveform contains ringing, glitches, large clock mismatch or undecoded activity;
there is no analog deglitch filter that might hide an error. Per-ID cadence
uses complete frames only, with nearest-rank p95 and maximum absolute deviation
from each matching period mask. Missing masks/too few intervals stay REVIEW.

Forward matching consumes the next unused output with identical ID, extended/
RTR flags, DLC and data **after input EOF**. Latency ends at output SOF. The
label is always **wire latency (includes arbitration/queueing)**. It is not
ISR-entry latency. Unrouted/local/repeated-identical traffic and capture edges
can cause unmatched/ambiguous pairs: inspect IDs/data and directional matches;
unmatched counts remain REVIEW. These scope results alone do not establish
lossless delivery or unique causality for identical repeated payloads.

Compare individual scope results directly, or attach them to existing records:

```bash
.venv/bin/python script/hil/p9_compare.py /tmp/p9/ref/scope/ref-ring-green-lin/result.json /tmp/p9/now/scope/now-ring-green-lin/result.json --out /tmp/p9/lin-wire.md
.venv/bin/python script/hil/p9_compare.py /tmp/p9/ref/ring.json /tmp/p9/now/ring.json --scope-ref ring-green-lin=/tmp/p9/ref/scope/ref-ring-green-lin/result.json --scope-now ring-green-lin=/tmp/p9/now/scope/now-ring-green-lin/result.json --scope-ref ring-orange-listener=/tmp/p9/ref/scope/ref-ring-orange-listener/result.json --scope-now ring-orange-listener=/tmp/p9/now/scope/now-ring-orange-listener/result.json --out /tmp/p9/ring-with-scope.md
.venv/bin/python script/hil/p9_compare.py /tmp/p9/ref/load.json /tmp/p9/now/load.json --scope-ref gateway-orange-forward=/tmp/p9/ref/scope/ref-gateway-orange-forward/A-to-B.json --scope-now gateway-orange-forward=/tmp/p9/now/scope/now-gateway-orange-forward/A-to-B.json --out /tmp/p9/load-with-scope.md
```

Repeat `--scope-ref/--scope-now` for responder, both CAN channel results and both
forward directions (give each pair a unique name). Names starting with
`<role>-` replace that role's unprinted LIN/cyclic timing placeholders; the
new detailed scope rows carry their own verdicts. Other missing evidence,
especially ISR timing, remains open. The positional records retain YAML/tool/
board checks; scope-only comparisons rely on the operator association note.

**Proposed P9 wire tolerance:** for each per-PID timing/space/cadence statistic,
per-CAN-ID interval/deviation statistic and wire-latency statistic, PASS if
`abs(now-ref) <= max(2 * max(dt_ref, dt_now), 0.05 * abs(ref))`; otherwise FAIL.
Use consistent units (the implementation converts dt to microseconds for LIN).
Two sample intervals cover two independently quantized edge endpoints; 5% is
a provisional regression allowance, not a LIN/CAN electrical specification.
Review repeat spread before adopting a tighter limit. Use matching baud,
threshold, channel, duration, scope serial, tool and decode settings; mismatches
stay REVIEW. Capture hashes are expected to differ. Counts are descriptive.

Independent hard FAILs in either side: LIN break <13 bits, delimiter <1 bit,
frame >1.4x nominal, bad parity/checksum, any reported decode/CRC/stuff/overflow
error, or any listener TXD dominant sample. Missing responses/samples/evidence,
settings mismatches and unmatched frames stay REVIEW. Timing PASS does **not**
close all of P4: payload semantics, 20 ms TX timeout, length-hint/timeout
finalization, collision/framing/break and reboot-on-busy-bus recovery, and
initialization-error/VERBOSE reporting still need the existing exercises and
review. No measurements are claimed by this harness change.

## esp_timing feasibility audit (cb0db60 and HEAD)

Set the sibling location when inspecting/updating this audit (adjust for a
normal checkout):

```bash
export ESP_TIMING_DIR="$(cd ../../../../esp_timing && pwd)"
```

No absolute sibling path, copied component, new firmware behavior or traced
build has been added to ESPCars. Read that repo's README.md,
`docs/runtime-trace.md`, `docs/driver-measurement.md`,
`docs/d101-port-handoff.md`, `firmware/c6/README-d101-driver.md` and
`scripts/ap71_scope.py` (the JSON-line x86 helper pattern reused here).

**Verdict: the requested Orange RX-ISR → forward-call measurement is not
feasible with current esp_timing and YAML-only selection.** Source evidence in
both `git show cb0db60:components/can_gateway/can_gateway.cpp` and HEAD:
`on_rx_done_cb` → `esphome::can_gateway::GatewayPort::handle_rx_isr` are
`IRAM_ATTR`; forwarding calls `twai_node_transmit` inside `handle_rx_isr`.
These gateway names did not change. `docs/runtime-trace.md` “Recorded context”
admits function hooks only for level-1 GPTimer callbacks, explicitly excluding
other IRAM/cache-off paths. `docs/driver-measurement.md` “First-run rates, ISR
identity and callback limits” says **no current CLI angle splits TWAI RX-forward
and TX-done callbacks**. Aggregate CAN IRQ context totals therefore cannot
supply this latency, and their stable source-set identity does not identify
an individual frame's forward call.

What is missing: an explicitly IRAM-safe, bounded callback-specific endpoint
hook with RAM/ROM closure verification, target calibration, and a definition
of entry→forward-call timing. That requires a separately reviewed driver/
component instrumentation change or a newer esp_timing implementation that
safely supplies those endpoints. Removing exclusions or using the D-101
vendored port is not a substitute for comparing cb0db60 to HEAD. The D-101
handoff is **target-unverified**, compares other source revisions/variants,
and its IRQ aggregates and product wire summaries do not provide this metric.
No trace wrapper YAML / `p9_build --esp-timing-dir` option / compare-result
import is exposed while the requested measurement cannot be produced safely.
Unprinted slot tracker diagnostics also remain REVIEW.

LIN functions are ordinary UART-event-task functions (not UART ISR endpoints),
so duration tracing is possible in principle but would not solve RX-ISR timing.
Selections would need revision-specific **source paths and names**:

| Endpoint | cb0db60 | HEAD |
|---|---|---|
| Header TX | `components/linbus/LinProtocolHandler.cpp`, `LinProtocolHandler::master_send` | `components/linbus/lin_protocol_handler.cpp`, `esphome::linbus::LinProtocolHandler::commander_send` |
| RX decode / responder dispatch | same reference source, `LinProtocolHandler::receiveDecode` | same HEAD source, `esphome::linbus::LinProtocolHandler::receive_decode` |
| UART writes / break | `components/linbus/LinSerial.cpp`, `LinSerial::writeBytes` / `sendBreak` | `components/linbus/lin_serial.cpp`, `esphome::linbus::LinSerial::write_bytes` / `send_break` |

The table uses repo-relative source names; esp_timing selection uses the build's
`esphome/components/...` names and fully qualified function names. Reference
helper classes are in the global namespace; HEAD moved them into
`esphome::linbus::`. It needs `origins: [external_component]`, `contexts: [task]`,
session start/reader/stop/drain control and **esp_timing's own patched
build wrapper** (`scripts/esphome.sh`, not plain ESPHome). Renames make native
`compare` function-set identity differ; `--intersect` would omit exactly these
changed endpoints. There is no established cross-name endpoint mapping or
per-frame correlation in this harness. Separate traced images, sidecar
retention and cross-revision comparison support would make this more than a
cheap YAML-only addition, so optional LIN internal tracing is deferred.

When safe endpoint support becomes available, preserve each build's
`trace_generated` sidecars and firmware identity outside git; use
`"$ESP_TIMING_DIR/.venv/bin/python" "$ESP_TIMING_DIR/scripts/esp_timing.py"
capture DIR --generated GEN --port MAC_RESOLVED_PORT --seconds N` on **at least
3 independent boots per side**, separate from scope acquisition. Follow
esp_timing `docs/howto-ci-gate.md`: compare baseline runs pairwise, then
`esp_timing.py compare BEFORE AFTER --out FILE` for **every** reference/current
pair. Any refusal blocks a verdict, any failure fails. Default compare exits
0 pass, 4 regression, 5 refused; missing/insufficient endpoints cannot pass.
Use the matching target policy and review observer cost/spread, rather than
claiming the wire tolerance above calibrates instrumentation. These are future
requirements, not currently executable P9 endpoint instructions. Traced images
**never** supply the laps/s gate: tracing changes heap, interrupt load and
scheduling. Keep both sides' untraced ring images for that gate.
