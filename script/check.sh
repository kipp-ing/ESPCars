#!/usr/bin/env bash
# Fast quality gate: run before every commit (CI runs the same plus compiles).
set -euo pipefail
cd "$(dirname "$0")/.."

PY=.venv/bin/python
ESPHOME=.venv/bin/esphome
if [ ! -x "$PY" ]; then
  echo "No .venv — run: python3 -m venv .venv && .venv/bin/pip install -r requirements_test.txt" >&2
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
find components -name '*.cpp' -o -name '*.h' -o -name '*.tcc' | \
  xargs .venv/bin/clang-format --dry-run --Werror

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

echo "All checks passed."
