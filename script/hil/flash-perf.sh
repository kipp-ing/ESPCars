#!/usr/bin/env bash
# Flash the token ring built at PERF (-O2) — the A/B twin of flash.sh.
# Usage: script/hil/flash-perf.sh [green|blue|orange ...]   (default: all three)
#
# Same identity resolution and same `run` (not `upload`) reasoning as flash.sh; the only
# difference is the config set, tests/hil/ring-perf-*.yaml, which is mr-*.yaml plus
# tests/hil/opt_perf.yaml.
#
# Flash ALL THREE before gating. A ring with one board at -O2 and two at -Os measures nothing,
# and because the node names are unchanged the build directory is shared with the -Os build —
# so every switch between flash.sh and this script is a full rebuild per board.
set -euo pipefail
cd "$(dirname "$0")/../.."

boards=("$@")
[ ${#boards[@]} -eq 0 ] && boards=(green blue orange)

for x in "${boards[@]}"; do
  port="$(.venv/bin/python script/hil/ports.py "$x")"
  echo "== Flashing mr-$x (PERF) on $port =="
  .venv/bin/esphome run --no-logs "tests/hil/ring-perf-$x.yaml" --device "$port"
done
echo "All flashed (PERF)."
