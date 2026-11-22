# Changelog

## Unreleased

- can_gateway: suppress the unused TWAI TX-idle notification on single-core
  IDF 5.5.x (C6 at 1,000 frames/s: Tmr_Svc 12.1% → 0%, free CPU +5.3 pp,
  +586 B flash). The gateway-owned nodes cannot use
  `twai_node_transmit_wait_all_done`.
- can_gateway: offload default-drop same-format allowlists to the hardware
  acceptance filter when no RX consumer needs rejected frames; `dump_config`
  reports the decision and reason (C6 at saturation: free CPU +4.2 pp, CAN ISR
  time −17%, +468 B flash).
- can_gateway: count every inject refusal; warn at episode start and stop
  instead of once per second (8 instead of 70 lines per 30 s of overload,
  +276 B flash).
- Evidence: esp_timing D-101 driver studies (ESP32-C6, 3 boots per side).
