#!/usr/bin/env bash
# Fast quality gate: run before every commit (CI runs the same plus compiles).
set -euo pipefail
cd "$(dirname "$0")/.."

PY=.venv/bin/python
ESPHOME=.venv/bin/esphome
if [ ! -x "$PY" ]; then
  echo "No .venv — run: python3 -m venv .venv && .venv/bin/pip install -r requirements_test.txt -r requirements_lint.txt" >&2
  exit 1
fi

echo "== no private data =="
"$PY" script/check_no_private_data.py

echo "== pytest =="
"$PY" -m pytest -q

# gateway_core.h runs in the RX ISR and is unit-tested on the host; CI has its
# own job for this, so keep it here or the two gates drift apart.
echo "== host tests =="
make -C tests/host

echo "== clang-format =="
# Share pre-commit's full C/C++ scope and byte-exact exclusions.
"$PY" script/lint_report.py --only clang-format --summary

# HA glue for custom_components/sdlog_relay needs its own venv (a pinned HA);
# CI always runs it, locally only when .venv-ha exists.
if [ -x .venv-ha/bin/python ]; then
  echo "== sdlog_relay HA glue =="
  .venv-ha/bin/python -m pytest -c tests_ha/pytest.ini tests_ha -q
else
  echo "== sdlog_relay HA glue: skipped (no .venv-ha; see tests_ha/requirements.txt) =="
fi

echo "== esphome config =="
for y in tests/build/*/test*.yaml; do
  "$ESPHOME" config "$y" > /dev/null
  echo "VALID $y"
done

# P2's clean Python/YAML/whitespace hooks block locally and in CI.
echo "== Python, YAML and whitespace lint =="
"$PY" script/lint_report.py --only ruff ruff-format pyupgrade yamllint end-of-file-fixer trailing-whitespace --summary

# The remaining hooks stay advisory until their cleanup phases clear the baseline.
# clang-tidy analyzes available local builds and skips missing/stale targets.
# The dedicated CI clang-tidy job requires all builds with --strict.
# CI uses this runner with requirements_lint.txt. Fixers use temporary copies.
echo "== upstream lint (report-only; pins: requirements_lint.txt) =="
"$PY" script/lint_report.py --only hook-config no-commit-to-branch ci-custom clang-tidy --report-only --summary
echo "For details, run: .venv/bin/python script/lint_report.py"

echo "All checks passed."
