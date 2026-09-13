#!/usr/bin/env bash
# Flash the bidirectional M2 stress round built at PERF (-O2): the soak that follows verify.py.
# Usage: script/hil/flash-soak-perf.sh
#
# Order is not arbitrary — **partners before the DUT**. Orange's ports walk to bus-off against a
# partner that is unpowered, resetting or still in the bootloader, and that is indistinguishable
# from a wiring fault. Green and Blue are both generators here and neither depends on the other,
# so the only ordering that matters is that both precede Orange.
#
# Cabling this is written against, measured 2026-07-28 from Orange's own beacons:
#   seg1 (GPIO2/3)   <-> Green can1      seg2 (GPIO10/11) <-> Blue can1
# Re-confirm with the run's own `[topo]` lines rather than trusting this comment.
set -euo pipefail
cd "$(dirname "$0")/../.."

for x in green blue orange; do
  port="$(.venv/bin/python script/hil/ports.py "$x")"
  echo "== Flashing mr-$x (soak, PERF) on $port =="
  .venv/bin/esphome run --no-logs "tests/hil/soak-perf-$x.yaml" --device "$port"
done
echo "All flashed (soak, PERF)."
