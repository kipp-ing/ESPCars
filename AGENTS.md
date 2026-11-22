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
  `[bench-pending]` in its title, and add a row for it to the P9 ledger in
  the plan: what could differ, and how P9 checks it. It is merged after
  review; the bench check happens once, in P9.
- **Gates you can run:** `script/check.sh` (pytest, host C++ tests, clang-format,
  `esphome config` on all build YAMLs) and `script/test_build_components.py compile` on the affected
  `tests/components/` fixtures. CI runs the full compile matrix. `script/check.sh`
  must pass before every commit.
- Ruff, Ruff format, pyupgrade, yamllint, EOF and trailing-whitespace findings now block both `script/check.sh` and CI.
- **`gateway_core.h` and other ISR or lock-free code:** change it only together
  with new host test cases in `tests/host/`. A concurrency change needs a case
  with a real thread, not an argument.

## The PR description is your report

A reviewer (Claude) reads the PR, not your session, so the description must
stand on its own:

1. **Phase**: which plan phase this is, e.g. `P1a`, and whether it is
   *mechanical* or *bench-pending*.
2. **What changed**, and what was deliberately left out, with the reason.
3. **Gates run**: the exact commands and their results (pass/fail, counts).
   Name a gate you could not run, and say why.
4. **Finding counts** before and after, for every lint or tidy tool the
   phase touches.
5. **Behavior**: anything that could differ at runtime, even if it is
   near-identical (see the plan's "Behavior neutrality").
6. **Questions**: decisions you need from someone. Then stop. Don't guess.

If the plan is wrong or no longer matches upstream, fix the plan in the same
PR and say so under 2.

## Working rules

- Branch off `main`, open a PR, merge with a merge commit. Never push to `main`.
- Never mix reformatting with logic changes in one PR.
- Don't silence a linter with `NOLINT` or `noqa` unless a comment says why.
- If a step needs a decision the plan doesn't cover, stop and ask. Don't guess.
