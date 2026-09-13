# Roadmap

Drafted 2026-07-22, two days after bootstrap. Status refreshed **2026-07-31**,
after the `uds` milestone landed and the doc sweep.

## Where the repo stands

Five components (`can_gateway`, `linbus`, `isotp`, `sd_logger`, `uds`), CI with
schema tests + host C++ tests + config validation + compile matrix +
clang-format, and the HIL bench with a documented baseline (19.4 laps/s, zero
errors) as the release gate. `can_gateway` is no longer C6-only, and the ISR-side
core (`gateway_core.h`) has real unit tests — `tests/host/`, 427 cases now, which
on their first run turned up a memory-ordering bug in `SnapshotRing` that no
bench run could have shown.

The bench is no longer the gap it was: `verify.py` has run green at the original
baseline, `isotp` and `uds` are both proven on hardware against a real ECU
(multi-frame reassembly, the `7F xx 78` response-pending path, a multi-block
capacity read), and `sd_logger` has an hour of rotations and a proven in-band
card recovery behind it.

Missing: **no release/tag yet**, and per-component config reference docs exist
only for `uds` ([`uds-user-guide.md`](uds-user-guide.md)) — for the rest you
still have to read `tests/build/`.

The bug picture is in [`HANDOVER.md`](HANDOVER.md): §1 is what is open, §3 the
`can_gateway` ledger. Issue #1's fix is confirmed on the bench but its bus-off
entry path needs one jumper; issue #2 is untouched; the 2026-07-25 review's
findings are closed except for one decision and two small gaps.

## Phase 1 — Stabilize (before everything else)

- [x] **Fix issue #1** — healthy port delivers no RX while the sibling port
  cycles bus-off recovery. Root cause was the shed-before-receive order in
  `handle_rx_isr`; fixed 2026-07-25 by tapping every shed frame instead of
  dropping it ([`HANDOVER.md`](HANDOVER.md) §3.1).
- [~] **Confirm issue #1 on the bench** — the fix *mechanism* is confirmed
  (2026-07-26, via the enable gate) and the ring holds its baseline. What is
  still owed is the destination-bus-off **entry path**, which cannot arm on the
  current wiring: it needs JP20 opened on Green ([`HANDOVER.md`](HANDOVER.md)
  §1.5). Blocks any honest v0.1.
- [ ] **Re-run the release gate with the battery attached** — the beacon move to
  `0x0C0` unblocked it, and nothing has re-run it since
  ([`HANDOVER.md`](HANDOVER.md) §1.1).
- [ ] **Defuse issue #2 short-term** — validation warning for `cyclic_sends`
  intervals below the main-loop resolution, plus docs for the ~16 ms floor
  and the many-IDs aggregate pattern. The GPTimer/ISR path for true short
  intervals is a separate, later item (with ISR-flash-audit discipline from
  the PCB repo).
- [x] **Put isotp on the bench** — done 2026-07-30, and further than this entry
  asked: `isotp` carries the `uds` client against the real BMS, with multi-frame
  reassembly and the response-pending path proven on hardware. It is exercised by
  the diagnostic rigs rather than the token ring, so it is not yet under
  `verify.py` itself.

## Phase 2 — First release

- [ ] **Tag v0.1** with a fixed ritual: pytest + config + compile matrix +
  `verify.py` green → tag. Switch README to recommend
  `github://kipp-ing/ESPCars@v0.1` pinning so users don't silently track main.
- [~] **Config reference per component** (`docs/<name>.md`): all keys, actions,
  triggers — today you have to read `tests/build/` for that. Done for `uds`
  ([`uds-user-guide.md`](uds-user-guide.md)), `sd_logger`
  ([`sd_logger-user-guide.md`](sd_logger-user-guide.md)), `can_gateway`
  ([`can_gateway-user-guide.md`](can_gateway-user-guide.md)) and `linbus`
  ([`linbus-user-guide.md`](linbus-user-guide.md)); `isotp` still owes its.
  Writing it in esphome-docs style pays directly into upstreaming.

## Phase 3 — Reach & features

- [x] **Open can_gateway to single-TWAI chips** — done at 870923c: every
  TWAI-capable variant is accepted, forwarding still requires two controllers.
  The C5 is the one exclusion (unverified TWAI-FD HAL, `docs/HANDOVER.md`
  Todo 4); un-rejecting it means teaching `OutstandingTracker` the v2 event
  model and proving it on silicon.
- [x] **UDS on top of isotp** — landed 2026-07-31, and it took a different shape
  than this entry assumed. Rather than
  hardcoding PIDs and DIDs in the component, the decode rules are compiled from
  the factory diagnostic database into a `.dcat` catalog and flashed into a
  memory-mapped partition, so YAML names a *field* and adding a DID is a catalog
  reflash rather than a firmware change. Design in
  [`DESIGN-uds.md`](DESIGN-uds.md), binary format in
  [`uds-catalog-format.md`](uds-catalog-format.md), how-to in
  [`uds-user-guide.md`](uds-user-guide.md). First target was a real bench BMS,
  which it now reads on hardware.
  Mode 01 OBD-II PIDs are not covered by this and remain open — same transport,
  different catalog source.
- [x] **DBC tooling** — `script/dbc2yaml.py` (11d4214) generates decode-sensor
  YAML from a DBC, including the per-signal SNA handling. `.dcat` is the same
  idea taken further: compiled to binary and read from flash instead of expanded
  into YAML.
- [ ] **Issue #2 long-term** — move cyclic TX to a GPTimer/ISR path for true
  configured rates.

## Phase 4 — Upstreaming

- [ ] **Refactor linbus upstream-fit** — file names like `LINCommunication.cpp`
  / `LinProtocolHandler.cpp` are not esphome convention (snake_case); any
  upstream review flags this immediately. linbus is likely the best first
  candidate: no overlap with anything existing.
- [ ] **Settle can_gateway positioning** — esphome already has `canbus`;
  before an upstream PR, decide whether can_gateway stays standalone or
  slots in as a canbus platform.

## Housekeeping

- [ ] `requirements_test.txt` says `esphome>=2026.7.0` — a floor, not a pin,
  although CLAUDE.md claims "pinned". Either pin exactly or fix the docs;
  otherwise CI silently drifts onto new esphome releases.

## Open decision

Recommended order: Phase 1 → Phase 2 (v0.1). After that it depends on what
the repo is primarily for — own vehicle projects (then Phase 3 first,
especially UDS) or community/upstream (then docs and Phase 4 move up).
