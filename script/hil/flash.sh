#!/usr/bin/env bash
# Flash HIL bench boards over USB (sequential — shared hub bandwidth).
# Usage: script/hil/flash.sh [green|blue|orange ...]   (default: all three)
#
# Ports are resolved by board identity at run time via script/hil/ports.py and
# never hardcoded here: the hub renumbers whenever a board resets or drops into
# the bootloader, so a baked-in path either fails outright or — worse — flashes
# the wrong board. `run` rather than `upload`: upload ships the last binary
# built, which silently reflashes stale firmware after a YAML edit.
set -euo pipefail
cd "$(dirname "$0")/../.."

boards=("$@")
[ ${#boards[@]} -eq 0 ] && boards=(green blue orange)

for x in "${boards[@]}"; do
  port="$(.venv/bin/python script/hil/ports.py "$x")"
  echo "== Flashing mr-$x on $port =="
  .venv/bin/esphome run --no-logs "tests/hil/mr-$x.yaml" --device "$port"
done
echo "All flashed."
