# Handover — what is open, and the `can_gateway` findings ledger

Two things live here: **§1, the work queue** — everything genuinely open, in the
order a fresh session should pick it up — and **§3, the ledger** of the eight
`can_gateway` findings from the 2026-07-20 bench session and the 2026-07-25 code
review, compressed to what shipped, what is still owed, and the repro that is
the regression test.

Per-milestone detail lives in the documents §2 points at. Sessions whose
milestone closed are not kept as files; their durable findings were folded into
the specs, `tests/hil/HIL.md` and this ledger when they closed. `git log` has
the originals if a claim ever needs its provenance.

## 1. Open, in the order to pick it up

**1.1 The ring gate has not run since the beacon move.** `verify.py --seconds 45
--min-laps 75`, baseline 875 laps / 45 s = 19.4 laps/s, zero errors. A real
device attached to the bench for testing transmits in the same range at
50 Hz, which under the old `0x0F0 + node*2 + bus` scheme was exactly Blue's
bus0 beacon slot, so Blue could not join the ring while it was cabled. The
block moved to base `0x0C0` (`dbcfa30`), which clears the collision — but
**"unblocked" is not "passing"**, and nothing has re-run it. This is the gate
every component-behavior change is measured against.

**1.2 Green's CAN front end is mute, and it blocks 1.1.** Untouched — Green was
deliberately offline through the `uds` sessions. Note the diagnosis rule before
starting: never infer a wiring fault from symptoms, because an unpowered
transceiver reproduces every one of them. Run `script/hil/selftest.py` and read
its report, which refuses a wiring verdict for a port whose transceiver gate
failed.

**1.3 The soak card outgrew the collection index.** The card carries 700+ sealed
chunks from months of bench history against `collection.max_chunks: 256`, so
every chunk a new run seals is refused from the index and is neither servable
nor reclaimable: `chunk index full (256 of 256 entries): L0000768 is untracked,
so retention cannot reclaim it (raise collection.max_chunks; 513 refused so
far)` — the component names its own fix. Clear the card or raise `max_chunks`
before the next soak. Open design question worth deciding at the same time:
whether refused-at-seal chunks should be adopted into freed slots. Today they
are orphans forever. This also blocks the `check --strict` half of the recorder
soak (the counters half is done: 238 records/s, all drop counters 0 across
170 s).

**1.4 Todo 2 — `cyclic_sends` interval floored by the main loop.** The only
bench-reproduced bug still fully open, and the one Phase 1 item in
[`ROADMAP.md`](ROADMAP.md) nothing has moved. Detail in §3.2.

**1.5 One jumper unblocks two ledger items: open JP20 on Green**
(`GPIO11→CAN2_RX`). Green's CAN2 is a lone node on a terminated stub with JP20
closed. Alone on a bus it gets no ACK, TEC climbs to 128, and ISO 11898's
ACK-error exception then freezes TEC — so it parks at error-passive and
**bus-off is unreachable by the standard**, which is why the repro cannot arm.
Opening JP20 floats GPIO11 (held recessive by `mr-green-issue1-pullup.yaml`), so
the controller reads recessive while transmitting dominant → *bit* error, which
has no exception → TEC to 256 → bus-off → recovery → flap ~1/s. Unblocks Todo
1's entry path (§3.1) and Todo 3's race (§3.3).

**1.6 M6 collection, three questions the load runs left open.**

- `/sdlog/status` times out during a transfer (curl exit 28 at 20 s), reproduced
  with CAN load underneath: the httpd is single-threaded, so a fleet poller
  cannot get status while a pull runs.
- Radio cost and server cost are still confounded — the `ring-wifi` / `soak-wifi`
  rigs never ran, and the load run took its baseline from the same boot instead,
  which answers "does it drop records" but not the attribution question.
- `#gap` reconciliation is not closed: `discarded_chunks=0` all run, retention
  never fired at `retention_percent: 90` on a 3 %-full card.

**1.7 Smaller, unowned.**

- The route-less `bus_load` silent-zero shape: build the repro, or reject the
  shape at config time.
- `uds.raw` logs neither its request bytes nor a tie from response back to
  request, so two queued probes are attributable only by order; and a raw
  request drawing NRC 0x31 goes through the suspend path and logs `group '?'` —
  truthful, but "raw request" would read better.
- `uds.execute` is the one action never fired on hardware: the curated catalog
  contains no executable group, so firing it needs a catalog with one. A
  deliberate gap, not an oversight.
- Ledger leftovers: Todo 7b (§3.7), Todo 4's counter and `reclaim_head` check
  (§3.4), Todo 8's two never-compiled config shapes (§3.8).

## 2. Where the detail lives

| document | what it carries |
|---|---|
| [`../tests/hil/HIL.md`](../tests/hil/HIL.md) | the bench: cast, gates, topology, wiring, and the hygiene traps that have each cost a session |
| [`../private/notes/HANDOVER-uds.md`](../private/notes/HANDOVER-uds.md) (gitignored, bench-specific) | the `uds` milestone — what is proven at which tier, what the factory database settled against an independently reverse-engineered reference, and the hazards that would have wedged the client |
| [`HANDOVER-m6-headroom.md`](HANDOVER-m6-headroom.md) | M6 headroom: the tap-loss mechanism and its fix, where the heap goes, the ranked catalog of unpulled levers, and §7a's heap floor — read before planning the in-car config |
| [`../private/notes/HANDOVER-guest-bms.md`](../private/notes/HANDOVER-guest-bms.md) (gitignored, bench-specific) | the bench pack: the DBC that actually matches (12/12 ids), the SNA convention, the `ALL_ERR` trap, and the Motorola decode gap. Diagnostic TX to it is authorised |
| [`DESIGN-uds.md`](DESIGN-uds.md), [`uds-catalog-format.md`](uds-catalog-format.md), [`uds-user-guide.md`](uds-user-guide.md) | `uds` design rationale, the normative `.dcat` format, and the how-to |
| [`can_gateway-user-guide.md`](can_gateway-user-guide.md), [`linbus-user-guide.md`](linbus-user-guide.md) | the how-to for `can_gateway` (bridging, filters, cyclic sends, bus-off recovery) and `linbus` (master/slave/listener, checksum handling, self-test) — no separate design docs for either, the component sources carry the "why" |
| [`sd_logger-spec.md`](sd_logger-spec.md), [`sdlog-collection-design.md`](sdlog-collection-design.md), [`sdlog-phase-b-wire-contract.md`](sdlog-phase-b-wire-contract.md), [`sd_logger-user-guide.md`](sd_logger-user-guide.md) | the recorder: component spec, collection design, the normative wire contract, and the how-to |
| [`commissioning-mr-purple.md`](commissioning-mr-purple.md), [`hil-rig-spec.md`](hil-rig-spec.md), [`bench-hmi-wiring.md`](bench-hmi-wiring.md), [`TODO-bench-hmi.md`](TODO-bench-hmi.md) | the second platform, the rig, and the board-level SW/HW items |

## 3. The `can_gateway` findings ledger

Todos 1–2 came off the bench on 2026-07-20 — real, reproduced on silicon. Todos
3–8 came out of a static code review on 2026-07-25 and were verified against the
source, not against reality; where the bench has since spoken, it says so.

All were addressed on 2026-07-25. Verification after: 273 pytest cases, 120 host
cases, `esphome config` on all ten build yamls, compile on the full-feature C6 /
no-diagnostics bridge / classic-ESP32 monitor, clang-format clean. The hardware
run of 2026-07-26 gave the verdicts in each entry below.

### 3.1 Todo 1 — a healthy port delivered no RX while its sibling cycled bus-off

**Fixed 2026-07-25; mechanism confirmed on the bench 2026-07-26. The bus-off
entry path is still unproven — it needs §1.5.**

*Symptom, reproduced twice on silicon:* a board with both TWAI ports up and
forward-all routes, where port 1 sits on a dead bus (bus-off → recovery ~1/s
forever) and receives a periodic `can_gateway.send`. Port 0 on a live bus then
transmits fine and **hardware-ACKs incoming frames** — the peer's TEC stays 0,
impossible otherwise — but software RX is dead from boot: `rx 0.0%`, zero
decode-sensor updates.

*Root cause:* `handle_rx_isr` shed **before** it retrieved the frame, and every
consumer of a received frame sat after the shed points (gateway disabled,
destination bus-off, no TX slot). So a port that both forwards and observes lost
all of its non-forwarding function whenever forwarding was shed. No IDF bug was
involved — `twai_hal_read_rx_fifo()` pops the frame before `on_rx_done`, so
declining to receive simply discards it; nothing wedges.

*What shipped:* every non-forwarding consumer — bus-load bits, per-ID timings,
snapshot ring, observation ring — moved into one inlined `run_rx_taps_()`
reached from all three receive paths. `tap_shed_frame_()` taps from
`rx_staging_frame_` after each shed, but only when some tap on that port has a
consumer, so the extra copy is the price of a shed and never of a forward. With
no diagnostics compiled in, `CAN_GATEWAY_HAS_RX_TAPS` collapses both helpers to
empty stubs and a shed stays one counter increment and a return — the v0.5 data
plane byte for byte. `tests/build/can_gateway/common_bridge.yaml` exists to keep
that branch compiled in CI; it broke once during the rework.

The second symptom closed with it: a disabled gateway still receives and taps,
so `bus_load`, per-ID timings, `last_frame` and the decode entities stay live.
`set_enabled`'s header contract was rewritten to say that instead of merely
claiming it.

*Bench verdict:* run on the enable gate rather than the bus-off gate, because
all three shed points call the same `tap_shed_frame_()` and only the enable gate
is reachable without re-wiring. Pre-fix, gating forwarding froze the observe ring
dead (`observed` +0/s) and made `bus_load` read 0.0 % on a 24 %-loaded segment;
post-fix both hold full values (`observed` +20/s, rx 11.9 %) while
`disabled_shed` climbs at 142/s, `observe_overflow` 0. The rework costs nothing
measurable: 875 laps / 45 s, the pre-fix baseline exactly.

*Still owed:* the destination-bus-off entry path. `shed bus_off` stayed at 0 for
a full run because the repro cannot arm — see §1.5. An earlier revision of this
file claimed Green's CAN2 "sits dominant"; that was inferred from TEC 0 and never
measured, and a quiet probe disproved it (REC 0, zero errors, idle recessive).

*The repro is the regression test.* Add to any `tests/hil/mr-green.yaml`:

```yaml
can_gateway:
  ports:
    - id: can1        # live segment (bridged to Blue by Orange)
      tx_pin: GPIO2
      rx_pin: GPIO3
      bit_rate: 125kbps
      observe_queue_depth: 32
    - id: dead        # CAN2, jumpers open on Green -> floats, goes bus-off
      tx_pin: GPIO10
      rx_pin: GPIO11
      bit_rate: 125kbps
  routes:
    - from: can1
      to: dead
    - from: dead
      to: can1
interval:
  - interval: 250ms
    then:
      - can_gateway.send: {port: dead, can_id: 0x123, data: [0x01]}
```

With it applied, Green's `Token return` sensor goes silent and the ring stalls
(`TOKEN LOST`); revert → ring green. Definition of done: ring stays green with
the repro configured, the `bus_off` route counter climbs at bus rate *while*
decode sensors keep updating, and a build yaml covers the
two-port-with-dead-sibling shape.

### 3.2 Todo 2 — `cyclic_sends` interval floored by the main-loop period — OPEN

**Untouched in code.** Intervals of 4 ms and 10 ms both transmit at ~62 f/s
(≈16 ms effective): cyclic transmission is paced by the ESPHome main loop, and
config validation accepts any interval, which misleads. Two streams configured
for ~35 % combined load produced 11.6 % per segment.

The 2026-07-26 attempt to quantify it **failed for instrument reasons**: a 10 ms
rotating-ID send measured +242.2/s, but `injected` is a port counter that
aggregates every software TX on that port, so it does not isolate one interval.
Two decompositions disagree (~13.5 ms vs ~10.7 ms effective), and frame-bit
estimation cannot settle it. *The measurement that would work:* send one **fixed**
`can_id` from one interval, subscribe the peer to that ID, and read the peer's
`observed` counter, which increments once per ring push.

*Where to look:* the cyclic engine in `can_gateway.cpp` (search `cyclic`, driven
from `loop_`) and the `cyclic_sends` schema in `components/can_gateway/__init__.py`.

*Options, short-term first:* (1) validate/warn when the interval is below
scheduler resolution, and document the floor plus the many-IDs aggregate pattern
(HW-22 sustained ~2000 f/s with ~100 cyclic IDs); (2) longer term, move cyclic TX
to a GPTimer/ISR path — which must respect the ISR-flash-audit discipline from
the PCB repo (`test/tools/isr_flash_audit.py`).

*Definition of done (short-term):* a schema test asserting the warning, a docs
note in the component, and a bench measurement showing either honest warnings or
true configured rates.

### 3.3 Todo 3 — `inject()`'s bus-off check was a TOCTOU

**Fixed 2026-07-25. Soak clean 2026-07-26, but the race was never exercised — it
needs §1.5.**

*Defect:* `inject()` read `is_bus_off()`, then ~100–200 cycles of slot setup ran
before `portENTER_CRITICAL`. A state-change ISR in that window flips the port to
bus-off, and `twai_node_transmit()` then hits a guard that expands to
`esp_rom_printf`: a busy-wait UART write of ~48 chars ≈ **4 ms with all
interrupts masked**. Both TWAI RX FIFOs overrun (a 500 kbit/s bus delivers ~20
frames in that window) and frames are silently lost on both ports.

*What shipped:* the `is_bus_off()` read moved inside `portENTER_CRITICAL`, next
to the occupancy gate, so check and hand-off are one atomic step and both feed
the same `accepted` decision. The outer check survives as a cheap early-out and
its comment now says it is not the binding one. Both driver-cost comments were
corrected while in there: the bus-off guard costs the lockless busy-wait UART
write, whereas the queue-full path is the one that can abort in
`lock_acquire_generic` — the old comment blamed the wrong mechanism for both.
The same check in `handle_rx_isr` was never affected: `dest`'s state-change ISR
runs at the same priority and cannot preempt.

*Bench verdict:* a 10 min soak (`tests/hil/mr-green-todo3.yaml`) ran ALL GREEN at
20.0 laps/s with 90 699 accepted injects, `tx_fail`/`bus_err`/`observe_overflow`
all 0, no loop-stall warning, and `fail_reason` correctly reporting "tx queue
full" rather than "bus off". **But the TOCTOU window never opened** — it needs
the destination to *transition* to bus-off between the outer read and the
critical section. No regression; the flapping-port soak is still owed.

### 3.4 Todo 4 — the `OutstandingTracker` bus-off contract does not hold on ESP32-C5

**Closed 2026-07-25 by rejecting the C5, in two layers plus a guard.**

*Why it matters:* the contract in `gateway_core.h` — a frame halted by bus-off
never gets `on_tx_done`, so `reclaim_head()` may free its slot — holds for the
**v1** TWAI HAL (the C6 has no `SOC_TWAI_SUPPORT_FD` and builds `twai_hal_v1.c`,
which clears the TX-occupied flag on bus-off without raising
`TWAI_HAL_EVENT_TX_BUFF_FREE`). The C5 is the only 5.5.4 target with
`SOC_TWAI_SUPPORT_FD`; `twai_hal_v2.c` derives `TX_BUFF_FREE` from the
TXT-buffer hardware-command interrupt, which **also** fires on the abort
transitions a bus-off causes, so the halted frame plausibly does complete.

*The failure scenario that justifies the rejection:* bus-off with
`outstanding_ = [F0 mounted, F1 queued]`. `on_tx_done(F0)` pops and releases F0;
`loop_` then sees bus-off with `bus_off_head_reclaimed_ == false` and
`reclaim_head()` pops **F1** — which the driver still holds a pointer to. After
recovery the driver transmits from a slot since re-acquired and overwritten:
**wrong ID/payload on a vehicle bus.** When F1's `on_tx_done` finally arrives,
`complete()` reports "not tracked", mutates nothing, and the slot is released
anyway → double release → cascading pool corruption.

*What shipped:* an `#error` on `defined(SOC_TWAI_SUPPORT_FD) &&
SOC_TWAI_SUPPORT_FD` so no build reaches the v2 HAL, with both HAL contracts
documented side by side above it; `_validate_hal_variant` (V27,
`UNVERIFIED_HAL_VARIANTS`) rejecting the same variant at config time with a
readable message rather than a preprocessor error in a build log, covered by
three tests including that the C6 stays accepted; and `handle_tx_done_isr`
releasing a slot only when `complete()` actually changed occupancy — leaking one
slot is recoverable, handing the same slot to two owners is not.

*Left open:* the untracked-slot path has no counter; `reclaim_head()` still does
not verify it is popping the frame the hardware halted; and C5 support itself —
teaching the tracker the v2 event model and confirming it on silicon — is future
work, not a lost cause.

### 3.5 Todo 5 — an unbounded `filters` list truncated to `uint8_t`, failing open

**Fixed 2026-07-25, fully.** `filters` was validated with `cv.Length(min=1)` and
no maximum, while `len(rules)` was passed into a `uint8_t rule_count`. 256
filters → 0 → every rule dropped → the route degrades to bare `default_action`.
With `default_action: accept`, a route written as an allow-list **forwards
everything onto a vehicle bus**, and neither config nor compile says a word —
narrowing in a call argument is not diagnosed. High impact, low likelihood.

`MAX_ROUTE_FILTERS = 255` applied as `cv.Length(min=1, max=…)`, with the
narrowing hazard spelled out at both the constant and the schema key; tests pin
255 accepted, 256 rejected, and the pre-existing empty-list rejection.

### 3.6 Todo 6 — `gateway_core.h` had no host test target, which is its whole purpose

**Fixed 2026-07-25, and it found a real bug on its first run.**

`tests/host/` is a plain `make` target — `c++ -std=c++17` over the header, no
CMake and no framework (`harness.h` is ~40 lines), ASan + UBSan with
`-fno-sanitize-recover=undefined` so a finding cannot exit 0. `RulePatch` lives
in `can_gateway.h`, which cannot be included on the host, so the Makefile
extracts the class text verbatim and fails loudly if the extraction stops
matching — the tests exercise the real code, never a copy that could drift.

*The find:* `SnapshotRing::read_latest()` validated its seqlock with a bare
acquire load. An acquire orders only what *follows* it, so the non-atomic payload
copy could sink below the `seq_after` load and a torn record passed validation —
reproduced reliably on arm64, ~666 torn reads in 125k. Fixed with an explicit
`std::atomic_thread_fence(std::memory_order_acquire)` between copy and
validation; the concurrent case in `tests/host/test_snapshot.cpp` is the
regression test. This is exactly the class of defect the bench cannot see:
`last_frame` would have shown a plausible frame that never existed on the wire.

Not run under TSAN, deliberately — a seqlock is a data race by construction and
TSAN reports it as one. ASan + UBSan plus the torn-record assertion is the
substitute.

**The W5 premise re-audit still stands.** When SPI/MCP2515 ports add a
task-context release path to `SlotPool`, the "nothing preempts an ISR"
assumption under *every* structure in `gateway_core.h` — not just the pool — has
to be re-checked. Those cases should land **before** W5, not after; same for the
H12 SMP audit, since the classic ESP32 is dual-core.

### 3.7 Todo 7 — two unbounded-work paths in `loop()`

**7a fixed 2026-07-25 and confirmed on the bench 2026-07-26. 7b is a decision,
not a patch, and is open.**

*7a:* with `id_timings: true` and `id_timings_max: 128`, `log_statistics_()`
emitted one `ESP_LOGI` per tracked ID per port in a single `loop()` pass — up to
~260 lines, well over half a second of blocking UART against a ~16 ms budget.
Guaranteed on a bus with 100+ IDs, not incidental. Knock-on: the "took a long
time" warning, `CyclicSend` schedule resync, and observe-ring overflow.

Now `log_statistics_()` only *arms* the dump (cursor + captured table size) and
`log_id_timings_step_()` emits at most `ID_LOG_LINES_PER_PASS` lines per port
per pass. That constant is 1, sized against the UART rather than picked round:
ESPHome's ESP32 logger drains ~184 bytes per ~16 ms pass at 115200 baud and an
ID line is ~70 characters, so one line on each of two ports stays under the drain
rate and `uart_write_bytes` returns without waiting. Nothing is silently
dropped — the walk is append-only and resumes where it left off, IDs first seen
mid-dump print next interval, and a dump still running when the next interval
arrives warns with how many entries it got through.

*Bench verdict:* pre-fix, 3 bursts of 128 consecutive lines, tripping the
component watchdog (`took a long time … 80 ms, max is 50 ms`). Post-fix, the
**same 393 lines** in 133 bursts, longest 5, interleaved with ordinary log
traffic, watchdog never fired. Identical line count confirms the paced walk drops
nothing. Ring ALL GREEN at 19.6 laps/s, segments at 30.3 %.

*7b, open:* the observation drain is bounded in records, not in work.
`max_frames_per_loop` defaults to the full `observe_queue_depth` (32, up to 128).
Each record fans out through `dispatch_record` to every matching subscriber, and
each decode entity's `publish_state` reaches the API/MQTT/logger stack — so a
signal changing every frame with the default `throttle: 0` yields up to 128 × N
publishes in one pass. Shrinking the default is a behavior change for existing
configs, so it needs an owner: a smaller default, or a documented publish budget.

### 3.8 Todo 8 — safety-net gaps

**Four of five closed 2026-07-25.**

- `try_hw_filter_offload_` and the no-RX-taps branch are compiled by
  `tests/build/can_gateway/test-bridge.esp32-c6-idf.yaml` — a v0.5-shaped bridge
  with no diagnostic surface, the only config that leaves
  `CAN_GATEWAY_HAS_RX_TAPS` false. It also carries the RTR filter flags no other
  config emits. Its header says why it must stay that way, so nobody "improves"
  it by adding statistics.
- `test-monitor.esp32-idf.yaml` (classic ESP32, single controller) and the bridge
  yaml joined the CI compile matrix.
- `can_gateway.inject` now warns (V26): a `cv.All` shim in front of the shared
  send schema, deduped through `CORE.data` so one `esphome config|compile|run`
  emits one warning however many call sites exist. Five tests pin that it warns
  once, names `can_gateway.send`, never rejects, still yields byte-identical
  config, and still reaches codegen. CLAUDE.md's "deprecate with a warning for
  one release cycle" precedent is real code now.
- `KindIndex` ↔ `ALL_KINDS` drift is fully locked:
  `test_sensor_kind_order_matches_cpp_enum` parses the enum out of
  `can_gateway.h` and compares it to the Python tuple, naming the first drifting
  index on failure. Previously only the Python tuple was frozen, so a one-line
  insertion would have silently published every later counter into the wrong
  sensor — wrong diagnostics, no failure anywhere.

**Left open:** `open_drain_tx` and a templated `set_patch` `can_id` are compiled
by no build yaml. One added config would cover both.

## 4. Bench crib

Identify boards by MAC (`script/hil/ports.py`), never by port path — paths churn.
`script/hil/flash.sh [green|blue|orange]`, then
`.venv/bin/python script/hil/verify.py --seconds 45 --min-laps 75`.
Boards need **12 V on J8** — USB-only looks exactly like broken wiring.
Baseline to beat: 19.4 laps/s, zero errors on every counter.
Start every bench session with `script/hil/selftest.py`; full detail in
[`../tests/hil/HIL.md`](../tests/hil/HIL.md).
