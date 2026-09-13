# Using `linbus` — LIN master/slave/sniffer in an ESPHome project

A practical guide: what the component does, how a project is actually put
together, and the end-to-end workflow from a wired-up transceiver to
statistics and frame data flowing into ESPHome. This document only covers the
*how*; the component has no separate design doc — the C++ sources
(`LinProtocolHandler.cpp`, `LINCommunication.cpp`) carry the "why" in their
own comments, and §8 points at the specific ones worth reading.

## 1. The shape of it

Every `linbus:` block is one independent instance — its own UART, its own
pins, its own state — running in exactly one of three modes:

```
mode: listener            mode: slave (no responses:)   mode: slave (+ responses:)   mode: master
──────────────────        ────────────────────────────  ───────────────────────────  ─────────────────────
watches only, never        watches only — no response     answers a matching PID       drives break+sync+
transmits a single bit     registered for any PID          with published data          header on its own
                                                                                          schedule, from boot
```

There is no built-in signal decoder. `linbus` publishes **bus-level
statistics** (frame counts, checksum errors, bus load) as sensors and a
self-test-pass flag as a binary sensor — never a decoded signal value. To read
an actual payload you write your own `on_frame:` automation and pull bytes out
of `data[]` yourself; see §3.2. This is the same division of labor as
`sd_logger`'s "nothing to decode" — `linbus` hands you raw frames, not
interpreted ones.

The other structural difference from `can_gateway`: there is no `ports:` list.
Each LIN segment is its own top-level `linbus:` block — a single mapping for
one segment, or a YAML list under `linbus:` for several (`MULTI_CONF`, no
shared state between instances) — because there is no shared C++ object two
segments could reasonably share; a chip's only real ceiling is how many spare
UARTs it has.

## 2. The three things a working setup needs

1. **A transceiver wired to `tx_pin`/`rx_pin`** on a free UART, plus
   `cs_pin` if the transceiver has an enable/sleep pin (a TJA1021, the
   reference part, does) — `linbus` drives it high once at boot and leaves it
   there; there is no runtime sleep control.
2. **Firmware YAML** — one `linbus:` block per physical segment, in the mode
   that segment actually needs. **The default mode is `master`** — an
   instance with a `schedule:` starts driving the bus from the moment it
   boots, with no separate enable step (§6). Set `mode: listener` explicitly
   while you're still characterizing a bus you don't own the schedule for.
3. **Nothing to read back offline.** Unlike `sd_logger`, there is no
   workstation-side tool — every fact about what a flashed image is actually
   doing comes from the live log (`dump_config` at boot, a statistics block
   every 10 s) and from the `sensor:`/`binary_sensor:` entities themselves.

## 3. Workflow, step by step

### 3.1 Wire it, start as a listener

```yaml
external_components:
  - source: github://kipp-ing/ESPCars
    components: [linbus]

linbus:
  id: lin1
  tx_pin: GPIO17
  rx_pin: GPIO16
  cs_pin: GPIO18       # transceiver enable/sleep pin, if it has one
  baud_rate: 19200      # 1000-20000; 19200 is the common LIN rate
  uart_port: UART_NUM_1 # UART_NUM_0/1/2 — each linbus instance needs its own
  mode: listener         # never answers, never drives the bus — the safe start
```

`tx_pin`/`rx_pin` are the only required keys. `mode: listener` is worth
setting explicitly even though it's not the default: it's the one mode that
is safe to leave on a bus you don't fully understand yet, since it can never
answer a poll or drive a header (`onID()` returns immediately for a listener
— it's not "answers nothing by omission", it's a mode the C++ special-cases).

### 3.2 Sniff traffic — `rx_ids:` and `on_frame:`

```yaml
linbus:
  id: lin1
  tx_pin: GPIO17
  rx_pin: GPIO16
  mode: listener
  rx_ids:
    - lin_id: 0x22
      length: 4
  on_frame:
    - lin_id: 0x22
      then:
        - lambda: |-
            ESP_LOGI("lin", "0x22: %02X %02X %02X %02X",
                     data[0], data[1], data[2], data[3]);
```

`rx_ids:` is a discovery/statistics hint, not a filter — declaring an id
tells the decoder its expected `length`/`checksum` so counters and
rediscovery behave correctly; an entry needs **at least one** of those two
keys. `on_frame:` fires per update on `data` (a byte vector) and
`data_length`; it takes the same optional `length`/`checksum` keys, and
firing is driven by the frame's internal update counter changing — including
resetting back down, so a rediscovered id after an LRU eviction still fires.
Only checksum-valid frames ever reach this stage; there is no
checksum-valid/invalid flag exposed to the automation because an invalid one
never gets this far.

### 3.3 Answer as a slave

```yaml
linbus:
  id: lin1
  tx_pin: GPIO17
  rx_pin: GPIO16
  mode: slave
  responses:
    - lin_id: 0x22
      data: [0x01, 0x02, 0x03, 0x04]
      checksum: enhanced   # default; classic is the other option
```

A slave only ever answers an id it has a `responses:` entry for; every other
poll on the wire is silently not its problem. For a value that changes at
runtime, publish the static entry above (it's what gives the id its length
and checksum mode) and update it from an automation:

```yaml
interval:
  - interval: 1s
    then:
      - linbus.update_response:
          id: lin1
          lin_id: 0x22
          data: !lambda "return std::vector<uint8_t>{0xAA, 0x55, 0xAA, 0x55};"
```

`linbus.update_response` on an id that already has a `responses:` entry keeps
that entry's checksum mode; called on an id with no prior entry it creates
one and defaults to enhanced. There is no `linbus.send` — a `linbus`
instance can only publish response data for an id it's declared, never fire
an arbitrary one-off frame.

### 3.4 Drive the bus as a master

```yaml
linbus:
  id: lin1
  tx_pin: GPIO17
  rx_pin: GPIO16
  mode: master           # drives break+sync+header from boot — see §6
  schedule:
    - lin_id: 0x22
      interval: 100ms
      length: 4
    - lin_id: 0x30
      interval: 50ms
  responses:               # a master can also answer ids in its own schedule
    - lin_id: 0x30
      data: [0xAA]
```

A `schedule:` entry only declares an id and a polling `interval:` (≥1 ms) and
optionally `length:`; it does not declare a checksum. The checksum a master
expects back for that id's response is resolved at runtime — from a matching
`responses:`/`rx_ids:` entry if one exists, otherwise enhanced with a
classic-mode retry on mismatch (§5's `checksum_fallback` counter is exactly
this path, and it's expected, not an error, on a bus that mixes LIN 1.x and
2.x nodes).

### 3.5 Flash, confirm from the log

```
[linbus:XXX]: LIN Bus:
[linbus:XXX]:   UART Port: 1
[linbus:XXX]:   Baud Rate: 19200
[linbus:XXX]:   TX Pin: GPIO17
[linbus:XXX]:   RX Pin: GPIO16
[linbus:XXX]:   CS Pin: GPIO18
[linbus:XXX]:   Mode: Master
[linbus:XXX]:   Self-Test: disabled
```

That's the whole `dump_config` block — **it does not echo the
`schedule:`/`responses:`/`rx_ids:` tables**, only mode, pins, baud rate and
whether self-test is on. To confirm what a flashed image is actually doing on
the wire, read the mode line together with the statistics block (§3.6), not
`dump_config` alone.

### 3.6 Read the statistics

Every 10 s:

```
[I][linbus:XXX]: === LIN Bus Statistics (10s window | lifetime) ===
[I][linbus:XXX]: Master Requests: 41 | 1230
[I][linbus:XXX]: ID Answers: 39 | 1180
[I][linbus:XXX]: Data Bytes RX: 156 | 4720
[I][linbus:XXX]: Frames RX: 39 | 1180
[I][linbus:XXX]: Bus Bytes (window): 640
[I][linbus:XXX]: Unique IDs Seen (lifetime): 5
[I][linbus:XXX]: Checksum Errors: 0 | 2
[I][linbus:XXX]: Send Failures: 0 | 0
[I][linbus:XXX]: Bus Load: 3.2%
[I][linbus:XXX]: Error Rate: 0.15%
```

Every counter is printed twice, deliberately: `window | lifetime`. Read the
window number to judge "is this happening right now"; read the lifetime
number to judge "did this ever happen." A windowed counter sitting at 0 while
the lifetime number is nonzero is not a regression — it's the last 10 s
having been clean. `sensor: platform: linbus` entities publish the **lifetime
totals** as `TOTAL_INCREASING`, matching what Home Assistant expects from a
counter — never the windowed value.

`self_test: true` (master-mode only) is a real bus-driving loopback test, not
a software check — it schedules nine fixed ids against itself and needs a
live transceiver and pull-up looped back to pass. Toggle it at runtime with
`linbus.run_self_test: {id: lin1, enable: true}`; it publishes through
`binary_sensor: platform: linbus, self_test_pass:` once every id has been
seen twice and 5 s have elapsed.

## 4. `packages:` — splitting "this board" from "this LIN setup"

Same pattern as every other component in this repo: a leaf file carries what
is per-board (pins, board type, other automations); a package carries the
`linbus:` block(s), reusable across any board wired the same way.

```yaml
# a board file, layering a LIN segment onto a proven board config
packages:
  board: !include my-board.yaml
  lin: !include my-lin-segment.yaml   # linbus: + its sensor:/binary_sensor: block
```

`tests/build/linbus/common_dual.yaml` is the worked example for **two**
segments on one chip — a slave on `UART_NUM_1` answering a partner's poll,
and a listener on `UART_NUM_2` sniffing the same wire — proving two instances
stay independently addressable (`linbus_id:` on every sensor/action). Two
instances sharing a `uart_port` or a physical pin is a config-time error
(§5), not a runtime surprise.

`tests/hil/mr-purple-lin.yaml` is a real two-transceiver board bring-up: one
LIN0 slave answering a partner's poll, one LIN1 listener on the same wire.
Its sibling file `tests/hil/mr-purple-partner-lin.yaml` records a genuinely
useful cross-bus lesson worth generalizing: on a board that also runs
`can_gateway`, a dead/unterminated CAN segment is not quiet — with nobody to
ACK, every beacon retransmits forever and ESP-IDF's TWAI error interrupt
fires continuously (~4750 events/s measured on a C6). That storm starved the
LIN master's own scheduling task and stalled its schedule dead at 198
requests — "a broken CAN segment silently invalidates a healthy LIN one." If
a master's `Master Requests` count stops climbing on a board that also has
CAN, check the CAN side before assuming the LIN wiring is at fault.

## 5. Config reference

### `linbus:` (one instance per physical segment; list or single mapping)

| key | meaning |
|---|---|
| `id` | this instance's id — required explicitly whenever more than one exists |
| `tx_pin` / `rx_pin` | the only required keys |
| `cs_pin` | optional; transceiver enable/sleep, driven high once at `setup()` and left there |
| `baud_rate` | default `19200`; `1000`–`20000` |
| `uart_port` | default `UART_NUM_1`; one of `UART_NUM_0`/`UART_NUM_1`/`UART_NUM_2` — each instance needs its own, checked at config time |
| `mode` | default `master`; `master` / `slave` / `listener` |
| `schedule` | list, max 16, unique `lin_id`; **master only** |
| `responses` | list, max 16, unique `lin_id`; **not valid with `mode: listener`** |
| `rx_ids` | list, max 32, unique `lin_id` |
| `self_test` | default `false`; **master only** — a real bus-driving loopback test, see §3.6 |
| `on_frame` | automation, fires per frame update — see §3.2 |
| `on_error` | automation, fires on a transport error |

### `schedule:` entries (master)

| key | meaning |
|---|---|
| `lin_id` | required, `0`–`63` |
| `interval` | required, `≥1ms` |
| `length` | optional, `1`–`8`; if omitted, length is resolved from a matching `responses:`/`rx_ids:` entry at runtime |

### `responses:` entries

| key | meaning |
|---|---|
| `lin_id` | required, `0`–`63` |
| `data` | required, `0`–`8` hex bytes |
| `checksum` | default `enhanced`; `classic` or `enhanced` — diagnostic ids `0x3C`/`0x3D` always use classic regardless of this setting, per the LIN spec |

### `rx_ids:` / `on_frame:` entries

| key | meaning |
|---|---|
| `lin_id` | required, `0`–`63` |
| `length` | optional, `1`–`8`; `rx_ids:` needs at least one of `length`/`checksum` |
| `checksum` | optional, `classic`/`enhanced` |

`0` is not a valid `length` anywhere — LIN 2.1 frames carry 1–8 data bytes,
and `0` doubles as the internal "unknown length" sentinel.

### Actions

| action | does |
|---|---|
| `linbus.update_response: {id, lin_id, data}` | publish new response bytes for an id already known to this instance (via `responses:` or a prior call); templatable `data` |
| `linbus.run_self_test: {id, enable}` | start/stop the loopback self-test at runtime; `enable` defaults to `true` |

### `sensor: platform: linbus` (bus statistics, not frame values)

All keys optional, all tied to a parent via `linbus_id:`. Every value is the
**lifetime total** (`TOTAL_INCREASING`), not the windowed count from §3.6's
log block.

| key | meaning |
|---|---|
| `master_requests` | headers this instance sent (master only) |
| `id_requests_answered` | polls this instance answered |
| `data_bytes_received` | payload bytes received |
| `frames_received` | checksum-valid frames received |
| `uart_breaks` | LIN break conditions seen |
| `checksum_errors` | frames where **both** checksum modes failed — a real defect |
| `checksum_fallback` | frames accepted on the non-preferred checksum mode — expected on a mixed LIN 1.x/2.x bus |
| `pid_errors` | parity errors on the id/PID byte |
| `framing_errors` | UART framing/parity/overflow errors |
| `collisions` | a self-transmitted byte didn't echo back as sent — a competing publisher answered the same id |
| `frames_coalesced` | `on_frame:` update-count jumps by more than one — the automation is missing intermediate frames |
| `send_failures` | genuine master TX errors |
| `bus_load` | `%`, estimated from bytes-on-wire vs. `baud_rate` over the window (assumes 8N1) |
| `error_rate` | `%`, `(send_failures + pid_errors + checksum_errors) / (master_requests + id_answers)` |

### `binary_sensor: platform: linbus`

| key | meaning |
|---|---|
| `self_test_pass` | true once every self-test id has updated at least twice and 5 s have elapsed since it started |

## 6. Safety model

**There is no opt-in gate for transmission** — nothing like `uds`'s
`allow_active_services`. `mode: master` defaults, and an instance with a
`schedule:` starts sending break+sync+header on its own cadence the moment
`setup()` runs, with no separate enable action. `mode: slave` only
transmits an answer for an id it has a `responses:` entry for; `mode:
listener` never transmits at all — that check is unconditional in the C++,
not a config option that could be silently widened.

`self_test: true` is exactly as real a transmitter as a `schedule:` — it's a
genuine bus-driving loopback exerciser, not a self-contained software check —
so treat it with the same caution.

Follow the repo-wide rule (`README.md`, `CLAUDE.md`): start a bus you don't
fully understand at `mode: listener`, characterize it with `rx_ids:` /
`on_frame:`, and only move to `mode: slave` or `mode: master` once you know
what you're answering or scheduling. Confirm what's actually flashed from the
`Mode:` line in `dump_config` and from the request/failure counters in §3.6
— not from reading the YAML, since a stale build can outlive a YAML edit.

## 7. Troubleshooting

| symptom | what it means |
|---|---|
| `uart_port N is already used by linbus '<id>'` | two instances on one chip share a UART — give one a different `uart_port` |
| `GPIOn is already used as <key> by linbus '<id>'` | two instances share a physical pin |
| `linbus schedule is only valid with mode: master` | `schedule:` set on a `slave`/`listener` instance |
| `linbus responses are not valid with mode: listener` | `responses:` set on a `listener` instance |
| `linbus self_test requires mode: master` | `self_test: true` on a `slave`/`listener` instance |
| `each rx_ids entry needs at least one of 'length' or 'checksum'` | a bare `rx_ids:` entry with only `lin_id` |
| `duplicate lin_id N in <schedule/responses/rx_ids>` | the same id declared twice in one list |
| `interval must be at least 1ms` | a `schedule:` entry's `interval` rounded to 0 |
| `dump_config` doesn't show my `schedule:`/`responses:`/`rx_ids:` | expected — §3.5; read the statistics block instead to confirm behavior |
| `checksum_fallback` climbing, `checksum_errors` not | expected on a bus mixing LIN 1.x (classic) and 2.x (enhanced) nodes — not a defect |
| `checksum_errors` climbing | a real defect — both checksum modes failed for that frame; check wiring/termination and bus load |
| `collisions` climbing | another node is answering the same id — check for a duplicate slave `responses:` entry, on this board or another |
| `frames_coalesced` climbing | `on_frame:` automations (or the main loop) can't keep up with frame arrival — do less work per automation, or read the id via `sensor:`/manual polling instead |
| `send_failures` climbing | genuine master TX errors — bus wiring, termination, or a segment with no responder |
| `self_test_pass` never goes true | needs `mode: master` **and** a real transceiver looped back with a pull-up — it is not a software-only check |
| a master's request count stops climbing on a board that also runs `can_gateway` | check the CAN side first — a bus-off CAN segment can flood the TWAI driver with error events fast enough to stall the LIN master's own task (§4) |
| `Unknown UART port: ...` warning, falls back to `UART_NUM_1` | `uart_port` named a peripheral this chip wasn't built with (e.g. `UART_NUM_2` on a two-UART target) — passed config validation but has no matching case at runtime; use a port the target actually has |

## 8. Going deeper

- `components/linbus/LinProtocolHandler.cpp` — the checksum algorithm
  (`calculateChecksum`), the classic/enhanced runtime fallback, and why
  diagnostic ids `0x3C`/`0x3D` are always classic.
- `components/linbus/LINCommunication.cpp` — schedule execution, response
  registration, and the self-test's fixed nine-id loopback table.
- `components/linbus/linbus.cpp` — `dump_config()` and the 10 s statistics
  block, if you're chasing an exact log format.
- `tests/build/linbus/common.yaml` — a single master instance exercising
  every config key (`schedule`, `responses`, `rx_ids`, `on_frame`,
  `on_error`, both actions, every sensor).
- `tests/build/linbus/common_dual.yaml` — two independent instances (a slave
  and a listener) on one chip, the worked `packages:`-style example for §4.
- `tests/hil/mr-purple-lin.yaml` — a real two-transceiver board bring-up,
  including the CAN/LIN cross-talk lesson cited in §4.
