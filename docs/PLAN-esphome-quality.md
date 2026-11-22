# Plan — ESPHome upstream quality

**Goal:** every component in this repo meets the standard that esphome/esphome
enforces on code it merges. Upstreaming itself is *not* scheduled. The goal is
that it could start any time without a cleanup first.

Decided 2026-11-22 (Jan):

1. Target is **upstream-mergeable**, not just "ESPHome-style external".
2. While the bench is offline, changes stay **behavior-neutral where it
   matters** (definition below). A result that is almost identical is fine.
3. The esphome pin is **bumped regularly**: every esphome release, in its own PR,
   with the full compile matrix. Phase 0 bumps it to 2026.11.0.

Executed mostly by Codex, one phase per branch and per PR. Agent rules are in
[`AGENTS.md`](../AGENTS.md).

## Sources

The standard, as read from esphome `dev` on 2026-11-22:

- https://developers.esphome.io/contributing/code/ — naming, memory, logging, `final`
- https://developers.esphome.io/architecture/components/ — `DOMAIN`, `CODEOWNERS`, `MULTI_CONF`, setup/loop rules
- https://developers.esphome.io/architecture/ci/component_tests/ — `tests/components/<name>/` layout
- https://developers.esphome.io/contributing/submitting-your-work/ — PR size and scope rules, AI policy
- https://github.com/esphome/esphome/blob/dev/script/ci-custom.py — the custom lint checks
- https://github.com/esphome/esphome/blob/dev/.clang-tidy — `Checks: '*'`, `WarningsAsErrors: '*'`
- https://github.com/esphome/esphome/blob/dev/pyproject.toml — ruff rule set
- https://github.com/esphome/esphome/blob/dev/.pre-commit-config.yaml — tool versions
- https://github.com/esphome/esphome/blob/dev/requirements_test.txt — tool pins

Pinned upstream versions on that date: ruff 0.17.5, pyupgrade 3.22.0 (`--py312-plus`),
yamllint 1.38.0, clang-format 23.1.3, pylint 4.1.4, prek 0.5.11. Re-read the
sources at the start of each phase, because upstream moves.

## Baseline (2026-11-22)

| Area | Upstream | ESPCars |
|---|---|---|
| ruff, upstream rule set | clean | 42 findings in `components/`, 100 in `script/`, 91 in `tests/`, 4 in `custom_components/`; 27 files need `ruff format` |
| `DOMAIN = "<name>"` | required | missing in 6 of 7 components (linbus has it) |
| `ci-custom.py` checks | clean | 6 banned calls (`std::to_string`, `sprintf`), 8 integer `#define`s, 1 header without `#pragma once`, 7 linbus files without a namespace, 2 linbus TAGs in the wrong form, no `ESPHOME_LOG_TAG` |
| clang-tidy | enforced, warnings are errors | not run at all |
| clang-format | 23.1.3 | 13.0.1 |
| `final` on leaf classes | required | 0 |
| Heap after `setup()`, `std::vector` for fixed sizes | avoid | 76 `std::vector`, 32 `std::function`, 28 `std::string`; not yet sorted into "fine" and "fix" |
| Component tests | `tests/components/<name>/common.yaml` + `test.<target>.yaml` | equivalent under `tests/build/`, plus pytest schema suites and host C++ tests |

## Behavior neutrality

The bench (`verify.py`) is the gate for component behavior, and it may be
offline. Every PR is therefore one of two kinds:

- **mechanical**: the gates `script/check.sh` and the CI compile matrix are
  enough to merge.
- **bench-pending**: `[bench-pending]` in the title, not merged before
  `verify.py` exits 0.

These must stay **exactly** the same, and any change to them is bench-pending:

- whatever goes on the wire: CAN/LIN frames, IDs, payloads, TX timing and TX
  conditions
- the RX ISR path, the slot tracker, rings, seqlocks, and everything else in
  `gateway_core.h`
- on-card and on-wire formats: the sd_logger record format and file naming, the
  collection protocol, the portal JSON and the UDS catalog format. These are
  contracts with code outside the device.
- the YAML schema: what validates, defaults, error behavior. Deprecate rather
  than break (`docs/CONVENTIONS.md`).

These may differ when the result is near-identical, so they count as mechanical:

- log text, log level, `dump_config()` output
- internal container types and buffer sizes, as long as capacity limits stay
  the same or grow
- the order of initialization inside one component's `setup()`, if nothing
  observable depends on it
- RAM and flash use, as long as nothing regresses by more than a few percent

When in doubt, call it bench-pending.

## Phases

Each phase lists its scope, its kind, and when it is done. Every phase ends
with `script/check.sh` green and CI green.

### P0 — Agent setup and pin bump · mechanical

Add `AGENTS.md` and this plan. Bump `esphome` to 2026.11.0, and fix whatever
the bump surfaces.

**Done:** the full CI compile matrix is green on 2026.11.0.

### P1 — Upstream tooling · infrastructure, no component code

Two PRs: **P1a** is items 1–3 and 5 (pre-commit, ruff, `ci_custom.py`, CI
wiring). **P1b** is item 4 (clang-tidy) plus its own CI wiring.

1. `.pre-commit-config.yaml` with the upstream hooks and versions: ruff,
   ruff-format, pyupgrade `--py312-plus`, yamllint and clang-format, plus the
   pre-commit-hooks for whitespace and newlines.
2. Copy the ruff `[tool.ruff]` config from upstream `pyproject.toml`, except
   `known-first-party`, which becomes our component layout.
3. `script/ci_custom.py`: port the upstream checks that apply to external
   components. Leave out the `esphome/const.py`, core-only, board-generation
   and alias-registry checks. Each check that is left out gets a one-line reason
   in the file.
4. clang-tidy: adopt upstream `.clang-tidy`. Get `compile_commands.json` from
   an `esphome compile` of each `tests/build/` target, then run
   `clang-tidy` (the upstream-pinned version) on `components/` only.
   This is the hardest step, so it goes in its own PR.
5. Every new check goes into both `script/check.sh` and `ci.yml`
   (`docs/CONVENTIONS.md`, "CI and main"). New CI jobs start **report-only**.
   A job moves into `needs:` of `Required checks` only once it reports zero
   findings, and never goes into the ruleset directly.

**Done:** each tool runs locally and in CI and reports a finding count that
matches the baseline.

#### P1a reproducible baseline

Reference: esphome/esphome dev at
`03eebae7b6e4cc81058f36a00a7f8c0c98eba341`. Use
`.venv/bin/python script/lint_report.py` with `requirements_lint.txt` installed.
The runner is read-only, includes tracked and non-ignored new files, and shares
the pre-commit byte-exact exclusions. `ci_custom.py` scans tracked `components/`
only. The existing blocking clang-format gate still scans only `components/`.

Measured against the pre-P1a tree (`ab95f7d`) with the new configuration, and
again after P1a; no existing source findings were fixed:

| Tool | Before | After | Count unit |
|---|---:|---:|---|
| ruff | 242 | 242 | diagnostics |
| ruff-format | 72 | 72 | files |
| pyupgrade | 4 | 4 | files |
| yamllint | 7 | 7 | diagnostics (all warnings) |
| clang-format 13.0.1, all hook-selected C/C++ | 86 | 86 | replacements in 11 test files |
| clang-format 13.0.1, existing components gate | 0 | 0 | replacements |
| end-of-file-fixer | 1 | 1 | files |
| trailing-whitespace | 1 | 1 | files |
| no-commit-to-branch | 0 | 0 | protected-branch findings |
| ci-custom | 120 | 120 | diagnostics |
| prek hook-config validation | 0 | 0 | errors |

These measurements supersede the preliminary counts above for P1a tooling.
Ruff's component-layout configuration reports 37 in `components/`, 100 in
`script/`, 100 in `tests/`, 4 in `custom_components/`, and 1 in `tests_ha/`.
The preliminary 42/100/91/4 and 27 formatter files were not reproduced against
this reference/configuration; the source tree was unchanged during measurement.
The wider pre-commit C/C++ scope accounts for the 86 formatting replacements
outside the existing clean component gate. Both whitespace hooks flag only
`CLAUDE.md`.

The full applicable upstream custom-check set finds 6 missing domains, 8 integer
defines, 1 missing pragma, 7 namespace findings, 11 legacy tag declarations,
30 unbraced log bodies, 44 bare log ternary literals, 5 `std::nothrow` uses,
7 inclusive-language findings, and 1 `sprintf` call. No `std::to_string` call was
found; the preliminary combined count of 6 banned calls was not reproduced.
All other ported checks report zero. Each of the 36 checks has bad/good snippet
coverage. The port documents its two upstream scanner corrections (offset-zero
matches and the extension check's empty-suffix wildcard) and every omitted check.
Clang-tidy remains P1b; clang-format's upgrade remains the P3 reformat-only PR.

### P2 — Python cleanup · mechanical

One PR each for `components/`, `script/`, `tests/` and `custom_components/`.
Each PR first runs `ruff --fix` and `ruff format`, then fixes the remaining
findings by hand. Add `DOMAIN` to every component.

**Done:** ruff and pyupgrade are blocking in CI.

### P3 — C++ format and custom lint · mechanical, with exceptions

1. Bump clang-format to the upstream version. The PR contains only the
   reformat.
2. Fix what `ci_custom.py` finds, one PR per component:
   - integer `#define` → `static constexpr`
   - `sprintf` → `snprintf`
   - replace `std::to_string`
   - add `#pragma once`
   - `ESPHOME_LOG_TAG`
   - `this->`
   - `final` on leaf classes

   A change in `gateway_core.h`, or in the text written to a file by
   sd_logger, is bench-pending (the second case is a format contract).

**Done:** clang-format and `ci_custom.py` are blocking in CI.

### P4 — linbus restructure · bench-pending

Put everything in namespace `esphome::linbus`. Rename files to snake_case
(`LinSerial.cpp` → `lin_serial.cpp` etc.), use the TAG form upstream requires,
and remove the Arduino-isms. LIN timing must not change.

The inclusive-language check flags linbus's public YAML `mode: master|slave`
and the corresponding C++ names. Rename them to the ISO 17987 terms
`commander`/`responder`. Keep `master`/`slave` accepted for one release cycle
with a deprecation warning, following [CONVENTIONS](CONVENTIONS.md)'s
"Don't break released YAML schemas silently" rule. Update the linbus user guide,
schema tests, and `tests/build/` YAMLs to use the new terms.

**Done:** compiles on all three linbus targets; `verify.py` exit 0 once the
bench is back.

### P5 — clang-tidy down to zero · one PR per component

Order by risk: ccp → isotp → uds → portal → linbus → sd_logger → can_gateway.

- Naming and readability fixes are mechanical.
- Memory work is bench-pending wherever it touches the paths listed under
  "Behavior neutrality". That means no heap after `setup()`; `std::vector` →
  `std::array`/`StaticVector` wherever the size is fixed; and `std::function`
  only where a callback really has to be stored.
- Before editing, sort the 76 `std::vector` / 32 `std::function` uses into
  "setup-time only, fine" and "runtime, fix", and put that list in the PR
  description.

**Done:** clang-tidy is blocking in CI with zero findings.

### P6 — Test layout like upstream · mechanical

Move `tests/build/<name>/` → `tests/components/<name>/`, with `common*.yaml` as
named packages and `test.<target>-<framework>.yaml` named the upstream way.
Add `validate.*.yaml` for configs that are only validated. Spell out each
component's platform limits in its schema (ESP32 + ESP-IDF only; the C5 is
rejected, V27), and document them. Update `ci.yml`, `check.sh`,
`docs/CONVENTIONS.md` and every path that refers to the old location.

**Done:** CI runs the same set of configs as before under the new paths, and
nothing refers to `tests/build/` any more.

### P7 — Docs in esphome.io format · mechanical

One page per component in the esphome.io structure: configuration variables,
actions, triggers, conditions, and observe-first examples. Generate a
`CODEOWNERS` file from the components' `CODEOWNERS` lists, with a CI check that
it is up to date.

**Done:** every config key in every component's schema appears in its page.

### P8 — Upstream split · only when upstreaming is scheduled

Upstream rejects PRs over ~1000 lines outside `tests/`, and takes a hub plus one
platform first. Plan the split for can_gateway and isotp. sd_logger (~12k
lines) and portal may stay external. Decide that when P8 is scheduled.

## Recurring

- **esphome pin:** one PR per esphome release. Bump `requirements_test.txt`,
  run the full matrix, then re-read the upstream sources above and update the
  tool pins in P1 to match.
