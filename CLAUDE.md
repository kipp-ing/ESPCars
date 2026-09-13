# ESPCars — agent guide

ESPHome external components for automotive buses (CAN, LIN, ISO-TP), pulled via
`external_components:` — so the repo must work untouched against stock esphome.
Detail lives in three docs; read the one you need rather than guessing:

- [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md) — layout, commands, definition of done, hard rules, sharp edges
- [`tests/hil/HIL.md`](tests/hil/HIL.md) — the bench: the boards, the gates, topology, hygiene
- [`docs/HANDOVER.md`](docs/HANDOVER.md) — open findings and per-milestone handovers

```bash
script/check.sh                                    # pytest + clang-format + config — before every commit
make -C tests/host                                 # host C++ (~10 s) — when gateway_core.h changes
.venv/bin/esphome compile tests/build/<c>/<t>.yaml # minutes — when C++ changed
.venv/bin/python script/hil/selftest.py            # START HERE on the bench; writes tests/hil/BENCH-STATE.md
.venv/bin/python script/hil/verify.py --seconds 45 --min-laps 75   # the release gate
```

Five things that do not bend — each has already cost a session or ships a hazard:

- **`verify.py` exit 0 gates every component-behavior change.** Baseline 19.4 laps/s, zero errors. The bench needs 12 V on J8; on USB alone every transceiver looks like broken wiring.
- **TX on a vehicle bus is safety-relevant.** Keep examples and docs observe-first; never widen a default that causes transmission.
- **Stay external-component-safe** — never require an edit to esphome core.
- **Never infer a wiring fault from symptoms.** An unpowered transceiver reproduces every one of them, so run `selftest.py` and read its report — it refuses to give a wiring verdict for a port whose transceiver gate failed, and a board it cannot reach at all fails the `reach` gate rather than silently skewing the rest. Identify boards by MAC (`script/hil/ports.py`), never by port path.
- **Onboard LEDs show state and identity**: hue is identity, brightness is state, under 40% at all times, red for errors only.
- **Real, bench-specific diagnostic data never goes in this repo.** No real vehicle name, no compiled catalog or decode table derived from a real vehicle's factory database, no real measured telemetry, no absolute path into a sibling private repo. Generic and fictional examples are fine — extend the `mini` fixture in `tests/uds/fixtures/` rather than reaching for real data. Real data lives under the gitignored `private/` directory; `script/check_no_private_data.py` (part of `script/check.sh`) guards against it leaking back in. See `docs/CONVENTIONS.md`.


## Way of working
Do not assume. Recheck your knowledge online if in doubt. Keep the links. This helps to not go into guessing trying loops.
Do not say done, if things left to do. Do another round until task done, or the user will request this anyway

## Working principles

**Plan before acting.** Plan what you want to do. Without a proper plan this will 
end in iterative fixing, which very costly

**No speculative fixes.** Reproduce the bug first, then fix it, or things can get worse.

**Done = green tests.** A feature without tests is unfinished. A milestone
without passing tests on the relevant tier is not done. If you discover open items
which lie in the task, do a loop, fix it. This is what a succesful task produces.

**Branch and PR** Do the work on a branch and open a PR. Commiting is always a good 
thing and allowed.

**Do not assume HW fails and stop** Although they can and did happen, most times it was misconfigured SW. If in doubt reflash a working verfy copy check HW and resume. This did cost us a lot of time to circle around assumed HW issues.

