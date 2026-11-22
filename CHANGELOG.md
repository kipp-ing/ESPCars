# Changelog

## Unreleased

- can_gateway: automatically suppress unused gateway TX-idle notifications on
  single-core IDF 5.5.x (C6: Tmr_Svc 12.3% → 0%, ≈7 pp CPU at 1,000 frames/s).
  The gateway-owned nodes cannot use `twai_node_transmit_wait_all_done`.
- can_gateway: offload default-drop same-format allowlists when no RX consumer
  needs rejected frames; report the filter decision and reason. C6 saturation:
  CAN ISR count −25%, ISR time −17%.
- can_gateway: count every inject refusal; warn at episode start/stop rather
  than periodically (removing periodic warnings reduced worst loop pass
  1,080 → 626 µs). Evidence: esp_timing D-101 driver A/B studies.
- Field payload cost before structure padding: +16 bytes/port and +32 global
  bytes with idle suppression.
  Final flash/RAM deltas await `script/d101_compile_matrix.sh` measurements.
