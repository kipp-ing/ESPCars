# sdlog_publish: the pipeline's state in Home Assistant

2026-10-11. The step after `sdlog_decode.py`. A long-running service publishes, over MQTT discovery:

- the state of the collect → archive → decode chain (is it healthy, when was the last pull, how fresh is the data)
- the **last known value** of a configured list of decoded signals per vehicle, so Home Assistant still knows
  them when the car is away. Live values while the car is home come from the device's own ESPHome API, not from here.
- a button that asks for a collection pass now.

Every requirement carries an ID and its test (`tests/sd_logger/test_publish.py`, against a fake source and a fake MQTT
client; no broker, no database). **A requirement without a test is not done.**

## 1. Shape

```
fleet heartbeats (<state>/<device>.json) ─┐
sdlog_decode tables (chunks/frames/signals)┼─▶ collect() ─▶ evaluate problem ─▶ <node>/state (one JSON, retained)
archive free space ───────────────────────┘
discovery: <prefix>/<component>/<node>/<object>/config (retained, on every connect)
availability: <node>/status  online | offline (last will)
button: <node>/cmd/collect ─▶ touch <trigger> ─▶ sdlog-fleet.path starts sdlog-fleet.service
```

```
sdlog_publish.py run   --config publish.toml   # the service (systemd, Restart=always)
sdlog_publish.py once  --config publish.toml   # print the state JSON once, no MQTT (debugging)
```

### 1.1 Config

```toml
node = "sdlog"                                   # HA device name and topic root
decode_config = "/etc/sdlog-decode/decode.toml"  # devices, vehicles, ClickHouse: one source of truth
fleet_state = "/var/lib/sdlog-fleet"
archive_min_free_mb = 1024
trigger = "/run/sdlog-fleet/trigger"
interval_s = 60
expire_after_s = 1800
discovery_prefix = "homeassistant"

[mqtt]
host = "192.0.2.42"
port = 1883
user = "sdlog"
password_file = "/etc/sdlog-publish/mqtt.pass"   # never inline

[[vehicle]]
name = "demo-car"                                # as in decode.toml
[[vehicle.sensor]]
key = "soc"                                      # object id and JSON key
name = "State of charge"
signal = "DEMO_STATUS.Soc"                       # as in the signals table: <Message>.<Signal>, or <ECU>.<name> (decode spec §7)
unit = "%"                                       # optional
device_class = "battery"                         # optional, HA sensor device class
```

## 2. Requirements

| ID | Requirement | Test |
|---|---|---|
| P1 | **Config.** Rejects unknown keys, a missing password file, a vehicle not in the decode config, a duplicate sensor key, an inline password, with exit 2 naming the problem. | `test_config_rejects` (parametrized) |
| P2 | **Chain state per device**: from the fleet heartbeat `last_success` (ISO 8601 UTC) and `last_result`; from the decode tables: chunks ok / defect / pending (the same numbers as `sdlog_decode.py status`), newest `utc` in `frames`, `#gap` count of chunks from the last 7 days. A device never collected or never decoded has nulls, not errors. | `test_chain_state_per_device`, `test_a_fresh_device_is_null_not_error` |
| P3 | **Vehicle values**: per configured sensor, the value with the newest `utc` and that time. A signal with no row yet is null. Rows without `utc` are not considered (their wall time is unknown). | `test_latest_value_by_utc`, `test_missing_signal_is_null` |
| P4 | **Problem** = on iff one of: ClickHouse unreachable; archive free space under `archive_min_free_mb`; a device's fleet verdict is `error`; a device has a defect chunk on record. The reasons are listed (`clickhouse`, `space`, `collect:<device>`, `defect:<device>`). A device that is away (`offline`) and `#gap`s are **not** problems. | `test_problem_table` (parametrized) |
| P5 | **ClickHouse down is not fatal**: fleet and disk values are still published, decode values are null, the problem is `clickhouse`. | `test_clickhouse_down_still_publishes_the_rest` |
| P6 | **Discovery**: one retained config per entity, published on every (re)connect: a `binary_sensor` (problem, reasons as attributes), sensors for free space, per device (last pull, result, last data, pending, defect, gaps), per vehicle sensor, and a `button`. Every entity has a `unique_id`, the availability topic and, except the button, `expire_after`. | `test_discovery_payloads` |
| P7 | **State**: one retained JSON on `<node>/state` every `interval_s`. | `test_state_is_published_retained` |
| P8 | **Availability**: last will `offline` (retained); `online` on connect. | `test_last_will_and_online` |
| P9 | **Button**: a message on `<node>/cmd/collect` creates the trigger file and nothing else. The fleet's `.path` unit starts the pass; `sdlog-fleet.service` removes the trigger when it starts, so a press during a pass gives exactly one more pass. | `test_button_touches_the_trigger_only` |
| P10 | **Read-only** towards archive, fleet state and database. | `test_collect_writes_nothing` |
| P11 | `paho-mqtt` (2.x API) is imported only by `sdlog_publish.py`, and only in `run`. It is not a test requirement: esphome pins paho 1.x in the same venv, so the service gets its own venv on the host. | `test_once_needs_no_mqtt` |
| P12 | Example config and units in `deploy/sdlog-publish/` are fictional and scanned by the private-data guard. | `test_the_example_config_loads`, `script/check_no_private_data.py` |

## 3. Out of scope

Trips, battery assessment and diagnostics: those are queries and services of their own. The publisher only reports
the chain and last values. Which signals a vehicle shows is the consuming project's config (§1.1).

## 4. Verified end to end (2026-10-11)

`run` against a local Mosquitto and ClickHouse 26.8, paho-mqtt 2.1.0: discovery for every entity, `online`, the state
every `interval_s`, a button press creating the trigger, and after `kill -9` the broker publishing the last will `offline`.
