# HIL rig v2 — specification

Status: **spec, not built.** Decisions marked *settled* were confirmed by Jan on
2026-07-26 (topology sizing, Purple's second CAN via stock `canbus`, literal green on
pass, phased sequencer). Implementation follows this spec later; the current bench
(`tests/hil/HIL.md`) stays the release gate until milestone M5 below cuts over.

Related: `docs/HANDOVER.md` (open todos and the coverage-gap analysis this spec
answers), `docs/commissioning-mr-purple.md` (Purple pin map and open items),
`docs/bench-hmi-wiring.md` (the future HMI/SD node this rig deliberately does not
consume a board for).

## 1. Purpose and principles

The rig exists to prove component function on real silicon with no human in the
loop: **flash the YAML with the newest driver → the nodes coordinate themselves →
they run a phased self-check → when everything passes, every LED goes green.**

Principles, each bought by a lesson already recorded in this repo:

- **Self-starting.** No runtime switch exists on the bench (no WiFi, no API, no OTA —
  `tests/hil/mr-purple-can.yaml` states it, and it stays that way: serial is for
  flashing and observation only). Coordination therefore rides on the buses under
  test. That is a feature: the coordination channel is itself continuous coverage.
- **Level-based, never edge-triggered.** All coordination state is carried in cyclic
  frames that are restaged, not one-shot messages. A lost frame delays nothing and
  wedges nothing; only a genuinely dead component stalls the sequence. This is the
  same property that makes the token ring robust ("der tote Ring lebt").
- **Every claimed check names its observer.** A test that transmits into the void
  proves nothing. Each phase criterion below states which node observes it and
  through which entity. Where absence must be proven ("the filtered frame did NOT
  cross"), the observer is value-independent (MCP2515 `on_frame` counting or a
  rolling-payload decode), never "a decode sensor went quiet" — decode entities
  publish on change only, the trap that already cost one bench run
  (`tests/hil/mr-orange-todo1.yaml` header documents it).
- **Host = recorder, not judge.** `script/hil/verify.py` keeps its role as CI gate
  and log archiver, but it asserts structured verdict lines the nodes print — it no
  longer computes the verdict itself.
- **Scope honesty.** The rig cannot reach ISR-level structures (slot pool, rings,
  seqlocks, rule-engine unit behavior). Those remain owned by `tests/host/`
  (`CLAUDE.md`). The rig's job is end-to-end function on hardware: wire-level LIN
  and CAN, forwarding under load, rule evaluation on real frames, isotp flow
  control, recovery behavior, and the negative control nothing covers today.

Coverage this rig adds over the current bench (from `docs/HANDOVER.md`): the rule
engine evaluated on hardware (today both routes are accept-all — "the code deciding
what gets transmitted onto a vehicle bus is validated by nothing"), isotp on
hardware for the first time, `linbus.run_self_test` in the gate, a negative control
proving the ring depends on gateway forwarding, and the Todo-1 "observation survives
shed" contract as a permanent regression check.

## 2. Rig topology

*Settled:* sized by what is needed to prove function, not by what is on the shelf.
Three boards; the fourth (Mr. Blue) leaves the ring.

| Board | Rig role | Why this board |
|---|---|---|
| **Mr. Orange** (C6, MAC `20:6e:f1:0a:d1:e0`) | **Gateway-under-test (DUT).** 2 TWAI ports, routes carrying real filter+patch rules, forwarding enable switch, LIN listener as extra observer. | The only board with a proven dual-port history; its CAN2 jumpers are already closed and validated by the current ring. |
| **Mr. Green** (C6, MAC `f0:f5:bd:0e:dd:50`) | **Coordinator / checker.** Dual-homed: CAN1 on Seg1, CAN2 on Seg2, **no routes** — so it hears both segments directly even when the DUT stops forwarding. LIN master. Load generator, probe injector, isotp endpoint A, sequencer state machine, bench verdict. | Carries the LIN master pull-up. The coordinator must own the LIN schedule anyway, because `linbus.run_self_test` is master-only — putting master and sequencer on one board avoids cross-node schedule negotiation. |
| **Mr. Purple** (classic ESP32, CP2102 `0x10c4:0xea60` serial `0001`) | **Dual-homed auditor.** MCP2515 via stock ESPHome `canbus` on Seg1, native TWAI via `can_gateway` on Seg2, LIN0 slave (ring relay), LIN1 listener, 12 V rail ADC. | *Settled:* the MCP2515 stays on stock `canbus` (proven in `tests/hil/mr-purple-selfloop.yaml`); a `can_gateway` SPI backend (W5) is future work. Stock `canbus` has the per-ID `on_frame` trigger `can_gateway` deliberately removed — the frame-presence/absence primitive only Purple can provide. Dual LIN on one classic ESP32 is proven (`tests/hil/mr-purple-lin.yaml`). |
| **Mr. Blue** (C6, MAC `20:6e:f1:0a:d0:f0`) | **Not in the rig.** Reserved as: spare (MAC-table swap is cheap), future sd-logger/HMI node (`sd_logger` is C6-only; `docs/bench-hmi-wiring.md`), and the sacrificial stub for the *manual* bus-off procedure (§4, last paragraph) — deliberately not a rig member, so bus-off experiments never risk the gate. | Frees a C6 without losing any needed observer. |

### Wiring

Both CAN segments 125 kbps. Termination rule: **exactly two 120 Ω terminations per
segment, at the two physical ends** — verify each board's termination jumper against
the PCB repo netlist during M1, the README pin table is known-stale.

| Bus | Nodes and pins |
|---|---|
| **Seg1** | Green CAN1 (GPIO2/3) — Orange port `seg1` (GPIO2/3) — Purple MCP2515 (SPI CLK GPIO18 / MISO 19 / MOSI 23 / CS 5, `/INT` GPIO34 unused by the polling driver) |
| **Seg2** | Green CAN2 (GPIO10/11 — **close JP20/JP22 on Green**, see risk §9) — Orange port `seg2` (GPIO10/11, jumpers closed as today) — Purple native TWAI (TX GPIO26 / RX GPIO25) |
| **LIN** (one bus, 19200, 12 V, J9.3 daisy-chain) | Green master (TX GPIO19 / RX 18 / CS 7, UART1, master pull-up on Green) — Purple LIN0 slave (TX 12 / RX 13 / CS 27, UART1) — Purple LIN1 listener (TX 33 / RX 32 / CS 21, UART2) — Orange listener (19/18/7, UART1) |
| **12 V** | J8 on both C6 boards (mandatory — USB-only power makes every transceiver look like broken wiring), Purple J4 fed from the C6 LIN-connector daisy-chain (H14). Purple's MCP2025 LIN transceivers are dead below ~12 V; the rail is sensed on GPIO36 (×11 divider) and gates P0. |

Boards are identified by MAC / USB-bridge descriptor via `script/hil/ports.py`,
never by port path — hub paths have drifted twice in one day already
(`docs/HANDOVER.md`).

**Cabled as of 2026-07-27 — a strict subset of the table above, at 500 kbit/s
for the M2 stress run** (`tests/hil/HIL.md`):

| Segment | Nodes cabled today | Still owed by M1 |
|---|---|---|
| **Seg1** | Green CAN1 — Orange `seg1` | Purple MCP2515 |
| **Seg2** | Orange `seg2` — Purple native TWAI | Green CAN2 (needs JP20/JP22 closed on Green) |

The load-bearing property is already true: **Green and Purple are on different
DUT ports**, so the DUT is the only path between them and forwarding is on the
critical path. Adding the two missing nodes extends each segment; it does not
move anything listed here.

## 3. Coordination protocol

### CAN ID map

| ID(s) | Meaning |
|---|---|
| `0x150/0x151`, `0x160/0x161` (+`0x152/0x162` if the load band needs them) | Cyclic background floods from Green, per port, highest priority — as today. |
| `0x260` / `0x261` | Patch-probe / drop-probe through DUT route Seg2→Seg1. |
| `0x270` / `0x271` | Patch-probe / drop-probe through DUT route Seg1→Seg2. |
| `0x3A0` / `0x3B0` | Ring ack / token — unchanged from the current bench. |
| `0x6A0` / `0x6A1` | isotp Green→Purple / Purple→Green, crossing the DUT. |
| **`0x700–0x71F`** | **Coordination block.** Lowest priority. **Dropped by the DUT's routes in both directions** (`can_id: 0x700, can_id_mask: 0x7E0, action: drop`). |
| `0x700` | PHASE command — Green, cyclic 100 ms on *both* ports, restaged via `can_gateway.set_cyclic_data`. |
| `0x710` | Green STATUS (both ports — observability, future HMI). |
| `0x711` | Orange STATUS (cyclic on both its ports). |
| `0x712` | Purple STATUS via native TWAI (Seg2), cyclic. |
| `0x713` | Purple STATUS via MCP2515 (`canbus.send` from a 200 ms interval, Seg1). |

The drop rule on the coordination block is load-bearing three ways: it makes
coordination **segment-local** (no duplicated IDs on one segment from two
transmitters; commands and status still flow when forwarding is off, because Green
is dual-homed and every node's status reaches Green on the segment where it
originates); it keeps the **rule engine exercised in every phase**, not just P2; and
any coordination frame observed crossing the gateway is itself a rule-engine
failure, for free.

### PHASE frame `0x700` (payload changes on every transition → decode `on_value` fires)

| Byte | Field |
|---|---|
| 0 | Protocol version = `0x01` |
| 1 | Phase: 0 rollcall, 1 ring, 2 rules, 3 isotp, 4 lin, 5 negctl, 6 **GREEN**, 7 **FAIL** |
| 2 | Flags: bit0 `gateway_enable_expected`, bit1 `ring_watch` (staleness watchdogs armed), bit2 `fail_latched` |
| 3 | Sequence number (increments on every restage) |
| 4–5 | Coordinator's node-pass bitmap (bit0 Green, bit1 Orange, bit2 Purple) |
| 6 | Failing phase (`0xFF` = none) |
| 7 | Reserved = 0 |

### STATUS frames `0x710–0x713` (cyclic 200 ms)

| Byte | Field |
|---|---|
| 0 | Protocol version |
| 1 | Acked phase |
| 2 | Node state: 0 boot, 1 ready, 2 running, 3 phase_pass, 4 phase_fail, 5 green, 6 fault |
| 3 | Local phase-pass bitmap (bit *n* = phase *n* passed) |
| 4 | Error code (0 none; per-node enum, documented in the node YAML) |
| 5–6 | Phase-specific detail (e.g. probe counts) |
| 7 | **Alive counter** — increments on every send. Because the byte always changes, a decode sensor's `on_value` fires per frame, giving liveness detection despite publish-on-change semantics. |

### Receiver mechanics

- C6 nodes receive PHASE/STATUS through `can_gateway` decode sensors (one decode for
  bytes 1–3, one for byte 7). Purple receives PHASE on Seg1 through MCP2515
  `on_frame` and on Seg2 through native decode sensors.
- Subscription budget: ~10 IDs per port, far under the 128-ID observe limit.
- Purple's MCP2515 hardware acceptance filters admit **only** the low-rate audited
  IDs (probes @10 Hz, PHASE, ring ack). The polling driver with its 2-deep RX FIFO
  must never be used for total-traffic accounting (§9).

### Sequencer rules (Green, one 500 ms `interval` lambda — the idiom proven in `tests/hil/mr-orange-todo1.yaml`)

1. Advance to phase *n*+1 when every node's STATUS reports phase-pass bit *n*.
2. Each phase has a timeout (table §4). Expiry → phase 7 (FAIL), failing phase and
   node bitmap in the PHASE frame, state latched.
3. A node whose alive counter is stale > 3 s is lost → FAIL.
4. A node whose reported pass bitmap *loses* bits (it rebooted) → restart the
   sequence from P0. Maximum 3 restarts, then FAIL.
5. Diagnostic counters are read as **deltas per phase window**, with `std::isnan()`
  guards (diagnostic sensors are NaN until their first poll — standard idiom here).

## 4. Phase catalogue

| Phase | What runs | On-device pass criteria (observer in parentheses) | Timeout |
|---|---|---|---|
| **P0 roll-call** | All nodes boot, run local checks, broadcast STATUS `ready`. Coordinator waits for the full roster. | Alive counters advancing on `0x711` (both segments), `0x712`, `0x713` (Green); protocol version == 1; neither DUT port `bus_off` (Orange binary sensors); 12 V rail > 10.0 V (Purple ADC) | 120 s from coordinator boot |
| **P1 ring soak** | Green starts cyclic floods on both ports. Token ring runs: Green LIN `0x30` carries lap N → Purple LIN0 relays to `0x31` and emits CAN `0x3B0` on Seg2 → DUT forwards → Green closes the lap **on CAN1** (the forwarded copy — CAN2 hearing it pre-gateway is a cross-check, not the criterion) → ack `0x3A0` crosses back → Purple sees the ack. | Lap delta ≥ 300 in 30 s and no staleness trip (Green); both route `forwarded` deltas ≥ lap delta (Orange); `bus_err` delta == 0 on all four TWAI ports; LIN checksum-error deltas == 0 (all LIN instances); relay and ack counters climbing (Purple); bus load within 10–60 % band on both segments | 60 s |
| **P2 rule engine** | Floods stay on. Green injects probes at 10 Hz: `0x260`/`0x261` on CAN2, `0x270`/`0x271` on CAN1. DUT rules: patch byte 7 → `0xA5` on `0x260`/`0x270` (rules carry `id:` so `can_gateway.set_patch` stays exercisable later), drop `0x261`/`0x271`, drop the `0x700` block, default accept. Probe payloads carry rolling counters. | ≥ 20 frames of `0x260` with byte7 == `0xA5` and rolling counter advancing, 0 mismatches, **and** `0x261` count delta == 0 in the same window (Purple MCP — paired presence-plus-absence proof); same for `0x270`/`0x271` (Purple native decode; `0x271` rolling payload means any crossing would fire the decode); `filtered` delta ≥ 20 per route (Orange) | 30 s |
| **P3 isotp** | First hardware run of isotp, deliberately across the DUT under load. Green CAN1 instance (tx `0x6A0` / rx `0x6A1`) ↔ Purple native instance (tx `0x6A1` / rx `0x6A0`). Green sends a 300-byte pattern (segmented, flow-controlled); Purple verifies content in `on_message` and replies with the reversed pattern; Green verifies. | Both sides: `messages_sent` +1, `messages_received` +1, `transfers_failed` == 0; content checks pass (both endpoints) | 30 s |
| **P4 LIN** | Coordinator clears `ring_watch` (the schedule is about to get busy; staleness watchdogs must not false-fail). Green runs `linbus.run_self_test` (installs 9 synthetic schedule IDs `0x00,0x01,0x15,0x1F,0x3B–0x3F` at 120–330 ms — no collision with ring `0x30`/`0x31`; 2+9 = 11 ≤ 16 schedule slots). After the verdict: teardown, re-arm `ring_watch`. | `self_test_pass` binary sensor true (needs its ≥ 5 s window) (Green); LIN1 `frames_received` delta ≥ 100 — the dual-LIN listener independently hears the synthetic traffic (Purple); checksum/PID error deltas == 0 on all LIN instances; ring laps advancing again ≤ 5 s after teardown | 40 s |
| **P5 negative control** | Coordinator clears `gateway_enable_expected` and `ring_watch`; Orange executes `switch.turn_off: gw_enable`. Probes and floods keep flowing into the dead gateway. After 10 s, re-enable. | Off-window: lap delta == 0 — the ring genuinely depends on the DUT (Green); `0x260` count delta == 0 (Purple MCP); route `disabled` delta > 100 (Orange); **port `observed` delta > 0 on both DUT ports** — a shed frame is still received and tapped, the Todo-1 fix contract as a permanent regression check (Orange). After re-enable: laps and probes resume ≤ 10 s; `bus_err` delta == 0 throughout; no bus-off anywhere | 45 s |
| **GREEN** | Coordinator broadcasts phase 6. All LEDs steady green (§5). Ring and floods keep running as a permanent soak with P1-grade monitoring. `HILSEQ bench verdict=PASS …` printed every 5 s. | Any regression during the soak → broadcast FAIL. | — |
| **FAIL** | Failing node reports `phase_fail` + error code; coordinator latches phase 7. **The bench holds state** — nothing resets, so the failure is inspectable over serial and STATUS frames. `verdict=FAIL` line every 5 s. | — | — |

**Manual-only procedure, never in the automated sequence:** bus-off entry and
recovery. By ISO 11898, a lone transmitter on a terminated stub freezes TEC at 128
(ACK-error exception) and cannot reach bus-off; the arming lever is the JP20-open
float trick (`docs/HANDOVER.md`). This is run on **Mr. Blue as an off-rig stub**
with the Issue-1 repro configs, so deliberate bus-off never touches the rig or its
gate.

## 5. LED specification

Rules (from `CLAUDE.md`, reconciled with the bench history in `tests/hil/HIL.md`):
brightness ≤ 40 % **always**; red for errors **only**; brightness changes preferred
over color changes in flight; literal green on bench-wide pass (*settled*).

| State | LED |
|---|---|
| Boot / waiting (P0) | Identity hue, steady 10 % |
| Phase running | Identity hue, activity flip 20 % ↔ 40 % per token/probe event (today's `led_token`, unchanged — brightness-only, in flight) |
| Node fault | 150 ms **red** blip at 35 % once per second, identity hue steady 10 % between blips — red-for-errors honored while identity survives (the "three identical red boards" lesson: a stopped bench must still tell you which board is which) |
| Bench GREEN | Steady green 35 %, no flicker, all nodes |
| Dark | Not running firmware (ROM download mode) |

Implementation note: `tests/hil/hil_common.yaml`'s current `led_fault` blips at
100 % and must be brought under the 40 % cap; new scripts `led_wait` and `led_pass`
join `led_health`/`led_fault`/`led_token`.

**Purple has no WS2812** in any config or commissioning doc. He reports via STATUS
frames and serial only; his green/fault state is visible on the C6 LEDs through the
coordinator's node-pass bitmap. Optional non-blocking hardware item for later: a
WS2812 on a free GPIO.

## 6. Prerequisite component changes

Small, and each carries the repo's definition of done (schema accept **and** reject
tests, build-yaml coverage, compile at least one affected target).

1. **`can_gateway`: expose `err_events` as a port sensor.** It is the primary
   bus-health signal (`docs/HANDOVER.md`: it moves while TEC/REC stay pinned at 0)
   and today exists only as a log line — the counter lives at
   `components/can_gateway/gateway_core.h:369`, printed in the 15 s stats line
   (`can_gateway.cpp:485`). Change: append `KIND_ERR_EVENTS` to `KindIndex`
   immediately before `KIND_COUNT` (`can_gateway.h:625-640`); append an
   `("err_events",)` tuple to the **end** of `ALL_KINDS`
   (`sensor.py:42-54` — append-only order lock, enforced by
   `tests/component_tests/can_gateway/test_entities.py`); publish it in the sensor
   hub in `can_gateway.cpp` (it is not contiguous with the existing
   injected…recoveries walk). `gateway_core.h` gains no logic → no new host tests.
   Rig use: recorded as per-phase deltas; asserted == 0 only on *receiving-only*
   ports — a transmitter under flood legitimately accumulates arbitration-loss
   events.
2. **`linbus`: self-test teardown.** Verified in code: `enableSelfTest(false)`
   (`components/linbus/LINCommunication.cpp:415-420`) only clears two flags; the 9
   synthetic schedule IDs and their responses installed by `selfTestInitIfNeeded()`
   (`:468`, `CASES[]` at `:474`) stay installed forever — permanently inflated LIN
   load, and a re-run double-adds entries. `master_remove_id_schedule()` (`:264`)
   exists and is unused by the teardown path. Change: on disable, remove each
   `CASES[]` id from the schedule and clear its response slot. Without this, P4 is
   not repeatable on a permanent rig. Acceptance: P4 run twice back-to-back without
   reflashing, identical lap rate before/after.
3. **`tests/build/isotp/test.esp32-idf.yaml` (new).** isotp today has only a C6
   compile target, but the rig runs it on Purple (classic ESP32, `esp32dev`).
   Compile-gate the variant before any hardware flash.

Explicitly **not** needed: a `can_gateway` SPI/MCP2515 backend (W5, future), any
change to `gateway_core.h`.

> **Superseded 2026-07-27.** This section used to list an `on_frame` trigger for
> `can_gateway` as explicitly not needed — rejected for a per-frame heap
> allocation, with the MCP2515 covering the auditor primitive. `can_gateway` has
> one since `bf80982`: the live topology catcher identifies boards by MAC, so
> the ids are MAC-derived and cannot be enumerated at codegen time, and no
> per-ID subscriber can express "whatever arrives". The heap objection was
> answered rather than accepted — the payload reaches the lambda as a borrowed
> `const uint8_t *` plus `dlc`, never a container, so the hot path still
> allocates nothing. What remains true is the cost that made it diagnostics-only:
> it is the sole subscriber naming no id, so it forces `observe_all` and keeps
> the hardware filter offload off (B25).

## 7. Host tooling

- **`tests/hil/bench-rig.yaml`** (rig manifest; becomes `bench.yaml` at M5). Boards
  gain `purple`, matched by USB bridge descriptor (`vid: 0x10c4, pid: 0xea60,
  serial: "0001"`) since CP2102s carry no MAC — `ports.py` `KNOWN_BRIDGES`
  (`script/hil/ports.py:41`) already resolves it. Each board gains a `criteria:`
  block (`need` / `forbid` substrings, optional coordinator-only `laps_min`). The
  hardcoded `CRITERIA` dict leaves `verify.py` (`verify.py:36-40`) entirely — that
  also kills the `KeyError` on any board not in the dict.
- **Verdict log grammar** (log tag `hilseq`; designed so no forbidden string ever
  appears in a healthy run):
  - Node phase verdict: `HILSEQ node=<name> phase=<n> verdict=PASS|FAIL detail=<k=v,...>`
  - Coordinator per phase: `HILSEQ bench phase=<n> verdict=PASS nodes=0x07`
  - Terminal, repeated every 5 s so any capture window after the fact still sees
    it: `HILSEQ bench verdict=PASS phases=0x3F laps=<n>` /
    `HILSEQ bench verdict=FAIL phase=<n> nodes=0x<m>`
- **`script/hil/verify.py`**: criteria come from the manifest (`--bench` flag,
  default `tests/hil/bench.yaml`). Primary assertion: terminal `verdict=PASS` in the
  coordinator capture, plus per-node phase-verdict lines, plus per-board forbids
  (`verdict=FAIL`; `bus-off` stays globally forbidden — safe, since P5 uses the
  enable switch and deliberate bus-off lives off-rig on Blue). Unchanged: parallel
  serial capture at 115200 (the CP2102 included), `--outdir` per-board logs, exit
  codes `0` green / `1` not green / `2` board not attached, and the attached-boards
  preflight.
- **`script/hil/flash.sh`**: default set becomes `green orange purple`; still
  resolves every port through `ports.py` at flash time and still uses
  `esphome run` (not `upload` — upload silently reflashes stale binaries).
- **`script/hil/ports.py`**: role-text update only (blue → "reserved: sd-logger/HMI,
  spare, manual bus-off stub").

## 8. Rollout milestones

The old ring configs and gate stay untouched and green until M5 replaces them.

| M | Content | Done means |
|---|---|---|
| **M1** | Rewire per §2 (checklist: close Green JP20/JP22, verify 2×120 Ω per segment against the netlist, LIN daisy-chain, 12 V to Purple J4). New `rig-green/orange/purple.yaml` running the **ported ring only** — Purple LIN0 replaces Blue as relay, DUT already carries the `0x700`-drop rules, no sequencer yet. `bench-rig.yaml` + criteria-driven `verify.py`. | `verify.py --bench tests/hil/bench-rig.yaml` exit 0 on ring criteria; new baseline (laps/s, bus loads) recorded in the manifest header; legacy configs untouched. |
| **M2** | STATUS/PHASE protocol, sequencer with **P0+P1 only**, LED overhaul (§5), `err_events` sensor (§6.1, own DoD). | Flash → rig self-starts → all-green LEDs with no host involvement; `verify.py` exit 0 on the structured verdict; fault drill: pull one CAN connector → FAIL + red blip + `verdict=FAIL`; reconnect + power-cycle → green again. |
| **M3** | P2 (rules) + P3 (isotp), incl. `tests/build/isotp/test.esp32-idf.yaml`. | P0–P3 green. Negative test documented and run once: flash an Orange variant missing the patch rule → bench FAILs at P2 (the rig can see its own DUT misconfigured). |
| **M4** | P4 (+ linbus teardown fix §6.2) + P5. | Full sequence green **twice consecutively without reflashing** (P4 repeatability); P5 off-window shows lap freeze + `disabled` climb + `observed` alive. |
| **M5** | Cutover: `bench-rig.yaml` → `bench.yaml`, `flash.sh` defaults, `tests/hil/HIL.md` rewrite (topology, protocol, phase table, LED table, new baseline), `docs/HANDOVER.md` + `CLAUDE.md` bench sections, legacy `mr-green/blue/orange.yaml` marked legacy (kept for Blue-as-stub work). | From cold boot: `script/hil/flash.sh && .venv/bin/python script/hil/verify.py --seconds 60` exit 0. Docs merged. |

Later, out of scope here: the sd-logger/HMI node on Mr. Blue
(`docs/bench-hmi-wiring.md`, blocked on SW-1 SPI-host sharing), the K1 gateway
bypass relay as a second physical negative control (SW-8), W5, and the P6 MCP2515
rate ceiling measurement.

## 9. Risks and open items

| Risk | Mitigation |
|---|---|
| **Issue #2 — cyclic intervals floored at ~16 ms** by the main loop: per-ID ceiling ~62 f/s, so two flood IDs per segment deliver ~11–12 %, not the configured rate. | State honest load numbers; add third/fourth flood IDs per port to reach the band; P1 asserts a 10–60 % band, never a configured-rate figure. |
| **MCP2515 polling ceiling unknown** (open item P6): stock driver polls a 2-deep FIFO and will drop under full segment traffic. | Hardware acceptance filters admit only the low-rate audited IDs; the MCP is never used for total-traffic accounting. P6 measurement stays future work. |
| **Green CAN2 unproven**: the commissioning log's partner-CAN2 failure was never resolved (suspects: partner jumpers open, cable). | M1 closes Green's JP20/JP22 and bench-verifies both Green ports before any sequencer work. If CAN2 misbehaves, Blue's proven board swaps in — the MAC table makes swaps cheap. |
| **Purple 12 V dependency**: MCP2025s dead below 12 V; a sagging rail mimics a LIN fault. | P0 gates on rail > 10.0 V; STATUS error codes distinguish power from bus faults. |
| **Coordination rides at lowest CAN priority** under floods. | Level-based 100/200 ms cyclics with a 3 s staleness threshold tolerate heavy arbitration loss; coordination is duplicated across both segments for the DUT. |
| **LIN schedule pressure in P4**: 11 of 16 slots used, ~60 % LIN load stretches token slots. | `ring_watch` flag suspends staleness fail during P4/P5; teardown fix (§6.2) restores baseline load afterward. |
| **Publish-on-change traps** (decode entities publish once for a constant value). | Every probe/STATUS payload carries a rolling counter; absence proofs use MCP counting or rolling-payload decodes, never "decode went quiet". |
| **Classic ESP32 under load**: H13 errata regime untested above ~1.4 % on this board, H12 SMP audit open (dual-core). | No new concurrency on Purple (stock canbus + one can_gateway port); anomalies surface as findings via STATUS error codes, not as silent flakiness. |
| **Observed-ID budget** (128/port). | Rig subscribes ~10 IDs/port; the budget is documented in each node YAML. |
