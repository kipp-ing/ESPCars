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

echo "== esphome config =="
for y in tests/build/*/test*.yaml; do
  "$ESPHOME" config "$y" > /dev/null
  echo "VALID $y"
done

echo "All checks passed."
