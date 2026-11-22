# ESPCars — guide for Codex and other agents

Read [`CLAUDE.md`](CLAUDE.md) first. It is the agent guide for this repo and
applies to every agent, not only Claude. Its hard rules hold here too, above
all: TX on a vehicle bus is safety-relevant, stay external-component-safe, and
never commit real vehicle data (`script/check_no_private_data.py` guards it).

Currently running work: [`docs/PLAN-esphome-quality.md`](docs/PLAN-esphome-quality.md),
which brings the repo up to ESPHome's upstream quality standard. Take one phase
per branch and per PR, in the order listed there.

## What differs for an agent without the bench

- **You cannot run `verify.py`.** The HIL bench is not reachable from your
  sandbox, and it may be offline. So a change must be either
  *mechanical* (behavior-neutral) or *bench-pending*, as defined in the plan
  (section "Behavior neutrality"). Mark a bench-pending PR with
  `[bench-pending]` in its title. It is not merged until someone runs
  `verify.py` with exit 0 on it.
- **Gates you can run:** `script/check.sh` (pytest, host C++ tests, clang-format,
  `esphome config` on all build YAMLs) and `esphome compile` on the affected
  `tests/build/` targets. CI runs the full compile matrix. `script/check.sh`
  must pass before every commit.
- **`gateway_core.h` and other ISR or lock-free code:** change it only together
  with new host test cases in `tests/host/`. A concurrency change needs a case
  with a real thread, not an argument.

## Working rules

- Branch off `main`, open a PR, merge with a merge commit. Never push to `main`.
- Never mix reformatting with logic changes in one PR.
- Don't silence a linter with `NOLINT` or `noqa` unless a comment says why.
- If a step needs a decision the plan doesn't cover, stop and ask. Don't guess.
