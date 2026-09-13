# ESPCars

**Monitor and modify your car with ESPHome — CAN and LIN, on cheap ESP32 hardware.**

ESPCars is a collection of automotive ESPHome components. It turns an ESP32 into a
vehicle bus node: observe traffic, decode signals into Home Assistant entities,
bridge/filter/rewrite frames between buses, and talk to LIN devices — all configured
in plain ESPHome YAML.

## Components

| Component | What it does | Targets |
|---|---|---|
| `can_gateway` | ISR-level CAN⇄CAN gateway: bridge, filter, ID-translate and patch frames between two buses; single-bus observe mode with signal decode into sensors; `can_gateway.send`, cyclic sends, per-ID statistics, bus-off recovery automations. See [`docs/can_gateway-user-guide.md`](docs/can_gateway-user-guide.md) | Any TWAI-capable ESP32 — observe/decode on one bus needs one controller (ESP32, S2, S3, C3, H2), forwarding needs two (C6, P4). Not the C5, see below |
| `linbus` | LIN bus master/slave: frame schedule, slave responses, sniffing (`rx_ids` / `on_frame`), checksum handling (classic/enhanced), self-test, detailed bus statistics. See [`docs/linbus-user-guide.md`](docs/linbus-user-guide.md) | ESP32, ESP32-C3, ESP32-C6 |
| `isotp` | ISO-TP (ISO 15765-2) transport for diagnostics: segments requests and reassembles responses on top of a `can_gateway` port; normal / normal-fixed / extended addressing, flow control, `isotp.send`, `on_message` / `on_error`, per-instance diagnostic counters | ESP32-C6 (on `can_gateway`) |
| `uds` | UDS (ISO 14229) diagnostic client on top of `isotp`: requests, decodes and schedules polling for fields defined in a flashed `.dcat` catalog — no decode rules in firmware or YAML — and publishes them as sensors/text sensors; `uds.read` / `uds.execute` / `uds.raw` / `uds.set_enabled`. See [`docs/uds-user-guide.md`](docs/uds-user-guide.md) | ESP32-C6 (on `isotp`) |
| `sd_logger` | SD-card bus datalogger: buffers CAN/LIN frames (via the generic `sd_logger.log` action or a native `can_gateway` tap) and ESPHome's own log into a RAM ring, drained by a dedicated writer task so SD latency never touches bus timing; rotation, crash-safe writes, card-failure recovery, and optional chunk collection over WiFi. Pure sink — never transmits. See [`docs/sd_logger-user-guide.md`](docs/sd_logger-user-guide.md) | ESP32-C6 |

## Quick start

```yaml
external_components:
  - source: github://kipp-ing/ESPCars
    components: [can_gateway, linbus]

can_gateway:
  ports:
    - id: car
      rx_pin: GPIO3
      tx_pin: GPIO2
      bit_rate: 500kbps

sensor:
  - platform: can_gateway
    port_id: car
    can_id: 0x2A0
    offset: 2
    length: 2
    name: "Battery Voltage"
    filters:
      - multiply: 0.01
```

See `tests/build/` for complete, compiling example configurations of every feature.

## Hardware

Developed against the ESP32-C6 reference adapter board (CAN + Modbus + LIN
transceivers). Any ESP32 with the right transceiver works: a CAN transceiver
(e.g. TJA1051, SN65HVD230) for `can_gateway`, a LIN transceiver (e.g. TJA1021)
for `linbus`.

> **Warning**: These components can *transmit* on vehicle buses. Writing to a
> car's CAN or LIN bus can trigger real actuators. Start in observe/listen-only
> configurations and only send frames you understand, on buses you own.

## Development & testing

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements_test.txt
script/check.sh                              # pytest + clang-format + every config
.venv/bin/python -m pytest tests/ -q         # component schema/codegen tests
make -C tests/host                           # C++ unit tests for the ISR-side core
.venv/bin/esphome config tests/build/can_gateway/test.esp32-c6-idf.yaml   # validate
.venv/bin/esphome compile tests/build/can_gateway/test.esp32-c6-idf.yaml  # full build
```

`tests/host/` compiles `can_gateway/gateway_core.h` — the rule engine, slot
pool, frame rings and recovery backoff that run inside the RX interrupt — with a
plain host compiler under ASan/UBSan, because that header is deliberately free
of ESPHome and ESP-IDF dependencies.

CI runs the schema tests and the host tests, validates every build config, and
compiles the full target matrix on every PR.

Beyond CI there is a permanent hardware-in-the-loop bench — three reference
boards (Mr. Green, Mr. Blue, Mr. Orange) running a token ring across LIN and
two gateway-bridged CAN segments under load. See `tests/hil/HIL.md`;
`script/hil/verify.py` must exit green before a release.

## License

Same dual-license scheme as ESPHome: C++ under GPLv3, Python and everything else
under MIT. See [LICENSE](LICENSE).
