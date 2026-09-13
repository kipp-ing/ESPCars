# Conventions — layout, done, and the rules that don't bend

Split out of `CLAUDE.md` on 2026-07-29 to keep that file a router. Nothing here
changed in the move.

## Layout

- `components/<name>/` — the ESPHome components (Python codegen + C++ runtime)
- `tests/<name>/` — pytest schema/codegen suites (`tests/conftest.py` maps
  `esphome.components.<name>` onto `components/`, so suites written in-tree run
  unmodified)
- `tests/build/<name>/` — complete YAML configs that must `esphome config` and
  `esphome compile`; `common*.yaml` holds the feature-exercising config,
  `test.<target>.yaml` wraps it with board/framework/external_components
- `tests/host/` — C++ unit tests for `can_gateway/gateway_core.h`, which is
  ESPHome- and IDF-free precisely so the code that runs in the RX ISR also runs
  on the host. Plain `make`, no CMake, no test framework (`harness.h` is it),
  ASan+UBSan. This is where rule engine, slot pool/tracker, rings and backoff
  are actually tested — the bench cannot reach them.
- `tests/hil/` — the hardware bench; see [`HIL.md`](../tests/hil/HIL.md).

## Commands

```bash
script/check.sh                                      # pytest + clang-format + config
.venv/bin/python -m pytest tests/ -q                 # fast (<1s), run always
make -C tests/host                                   # host C++ tests (~10 s)
.venv/bin/esphome config tests/build/<c>/<t>.yaml    # fast, run always
.venv/bin/esphome compile tests/build/<c>/<t>.yaml   # minutes, run when C++ changed
```

`make -C tests/host FILTER=<substring>` runs a subset; `SAN=` drops the
sanitizers. If `.venv` is missing:
`python3 -m venv .venv && .venv/bin/pip install -r requirements_test.txt`.

## Definition of done — every change

1. Behavior changes ship with schema tests in `tests/<component>/` (validation
   acceptance AND rejection paths).
2. New config keys/actions must also appear in a `tests/build/` yaml so CI
   compiles them.
3. `pytest` green and `esphome config` green on all build yamls before commit.
4. C++ touched → `esphome compile` at least one affected target locally.
5. `gateway_core.h` touched → host cases in `tests/host/` and `make -C
   tests/host` green. Concurrency contracts (seqlocks, the slot tracker's
   driver-FIFO mirror) get a case with a real thread, not a reasoned argument:
   that is how the `SnapshotRing` reader fence was found.
6. C++ follows `.clang-format` (ESPHome style); CI enforces it.
7. Component behavior changed → the bench gate, `verify.py` exit 0. See
   [`HIL.md`](../tests/hil/HIL.md).

## Hard rules

- Components must stay **external-component-safe**: never require edits to
  esphome core. `USE_*`/sizing defines are emitted via `cg.add_define(...)` in
  the component's Python — never assume `esphome/core/defines.h` entries.
- Don't break released YAML schemas silently. Deprecate with a warning for one
  release cycle (see `can_gateway.inject` → `can_gateway.send`).
- TX on a vehicle bus is safety-relevant: keep examples/docs observe-first, and
  never widen a default that causes transmission.
- Version against pinned esphome in `requirements_test.txt`; bump deliberately
  and run the full matrix when you do.
- **Real, bench-specific diagnostic data never goes in this repo.** A DID, a
  tester address, a field name, a catalog format — all fine as generic or
  fictional examples (the `mini` fixture in `tests/uds/fixtures/` is the
  canonical one; extend it rather than inventing a second). What is never
  committed: a real vehicle's model/manufacturer name, a compiled factory
  catalog derived from a real vehicle's diagnostic database, real
  reverse-engineered decode tables, real measured telemetry, or an absolute
  path into a sibling private repo. That data lives under a gitignored
  `private/` directory local to each bench (`private/catalogs/`,
  `private/fixtures/`, `private/hil/`, `private/notes/`) — tests and HIL
  configs that need it look there and skip cleanly (pytest
  `skipif`/`pytestmark`, or C++ `SKIP_IF` in `tests/host/harness.h`) when it
  is absent, so a fresh clone and CI both pass without it. A test whose
  *source*, not just its data, hardcodes real field names/DIDs (not
  something reusable against `mini`) does not get gated in place — it moves
  to `private/tests/` or `private/tests_host/` entirely (untracked); copy it
  back into `tests/uds/` or `tests/host/` locally to run it, per that file's
  own header comment.
  `script/check_no_private_data.py` (run from `script/check.sh`) greps
  tracked files for the known markers of this — extend its pattern list
  rather than silencing a hit.

## Known sharp edges

- Issue #2 is the open bug: `cyclic_sends` intervals are floored by the
  main-loop period (~16 ms) — high aggregate load needs many cyclic IDs.
  Issue #1 (a healthy TWAI port delivering no RX while its sibling cycles
  bus-off recovery) was fixed on 2026-07-25 but is **not yet re-run on the
  bench**; its repro config is the regression test. Both have full handovers in
  [`HANDOVER.md`](HANDOVER.md), which also tracks what the same session left
  half-closed.
- The ESP32-C5 is rejected at config time (V27) and by an `#error`: it builds
  the TWAI-FD HAL, where a frame halted by bus-off still completes — the
  opposite of the contract the forwarding slot tracker is written against.
  Don't "fix" this by deleting the guard.
- A shed forward still gets received and tapped, so observation, snapshot and
  bus load survive a shed — except in a build with no diagnostics at all, where
  `CAN_GATEWAY_HAS_RX_TAPS` compiles the whole tap path away. Keep
  `tests/build/can_gateway/common_bridge.yaml` diagnostics-free; it is the only
  config compiling that branch.

## Provenance

Extracted from the in-tree development fork `swifty99/esphome`:
`can_gateway` from branch `can-gateway-v06` (eed11f7b46),
`linbus` from branch `linbus` (e9bfc1bdb6),
`isotp` from branch `can-gateway-v06` (805122b5c, layered on can_gateway v0.6).
Upstreaming to esphome/esphome remains a goal — keep component code in
upstream-mergeable shape (in-tree layout, CODEOWNERS-style docs, esphome test
conventions).

The ESP32-C6 reference PCB (CAN + Modbus + LIN) lives in the
`PCB-ESP32C6-Adapter-CAN-Modbus-LIN` repo (git.kipp.ing); its docs hold the
can_gateway spec history.
