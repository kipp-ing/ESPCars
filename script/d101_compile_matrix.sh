#!/usr/bin/env bash
# Compile only: no upload/serial/hardware operations. Run on an IDF-capable host.
# Logs contain PlatformIO's exact flash/RAM totals; compare each off/on pair.
# Test defines are build-only switches, never YAML component options.
set -euo pipefail
cd "$(dirname "$0")/.."
ESPHOME=${ESPHOME:-.venv/bin/esphome}
COMMAND=compile
if [ "${1:-}" = --config-only ]; then COMMAND=config; shift; fi
if [ "$#" -ne 0 ]; then echo "Usage: $0 [--config-only]" >&2; exit 2; fi
OUT=${D101_RESULTS_DIR:-/tmp/espcars-d101-$COMMAND}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

compile() {
  local yaml=$1 name=$2
  printf '%q %q %q\n' "$ESPHOME" "$COMMAND" "$yaml" | tee "$OUT/$name.command"
  set +e
  "$ESPHOME" "$COMMAND" "$yaml" 2>&1 | tee "$OUT/$name.log"
  local status=${PIPESTATUS[0]}
  set -e
  echo "$status" > "$OUT/$name.exit"
  if [ "$status" -ne 0 ]; then return "$status"; fi
}

# Required default builds: dual TWAI C6 and classic dual-core ESP32 (no wraps).
compile tests/build/can_gateway/test.esp32-c6-idf.yaml c6-default
compile tests/build/can_gateway/test-monitor.esp32-idf.yaml esp32-default

# Use the identical diagnostics-free bridge for every pair: it can engage the
# filter. Each off build disables just one feature; all other defaults stay on.
# Keep generated YAML beside its package so relative external_components paths
# remain valid. Separate build directories prevent stale objects/size reports.
MATRIX_DIR=tests/build/can_gateway
configs=()
cleanup() { rm -f "${configs[@]}"; }
trap cleanup EXIT
for feature in IDLE_NOTIFY HW_FILTER QUIET_REFUSALS; do
  for state in off on; do
    name="d101-$feature-$state"
    yaml="$MATRIX_DIR/.${name}.yaml"
    configs+=("$yaml")
    {
      echo 'packages:'
      echo '  base: !include test-bridge.esp32-c6-idf.yaml'
      echo 'esphome:'
      echo "  build_path: $OUT/build-$name"
      if [ "$state" = off ]; then
        echo '  platformio_options:'
        echo '    build_flags:'
        echo "      - -DCAN_GATEWAY_TEST_DISABLE_$feature"
      fi
    } > "$yaml"
    cp "$yaml" "$OUT/$name.yaml.txt"
    compile "$yaml" "$name"
  done
done
printf '%s logs, commands and exit codes (compile logs include flash/RAM totals): %s\n' "$COMMAND" "$OUT"
