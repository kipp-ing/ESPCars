# Using `can_gateway` — a CAN observer, decoder and gateway in an ESPHome project

A practical guide: what the component does, how a project is actually put
together, and the end-to-end workflow from a wired-up transceiver to decoded
entities and (if you need it) a bridged bus. This document only covers the
*how*; there is no separate design doc for `can_gateway` — the C++ sources
(`can_gateway.cpp`, `gateway_core.h`) carry the "why" in their own comments,
and §8 points at the specific ones worth reading.

## 1. The shape of it

```
one port, no routes                  two ports, routes:
────────────────────                 ──────────────────────────────────
observe + decode + send on           port A ──[rule table]──▶ port B
one bus — the safe starting          port B ──[rule table]──▶ port A
shape; nothing can bridge            (bridging is two independent,
because there is nowhere             one-directional routes — declare
else to forward to)                  only the one(s) you need)
```

`can_gateway` runs entirely in the ESP-IDF TWAI (CAN) driver's ISR/loop
context on the ESP32. A **port** is one physical CAN controller: pins, bit
rate, its own RX/TX queues. A **route** is a one-directional link from one
port to another, carrying a rule table (accept/drop/modify, matched
in order); declaring a route is what turns two ports into a bridge. With no
`routes:` at all, one or two ports are independent single-bus nodes: read
with `sensor:`/`binary_sensor:`/`text_sensor:` decode entities or a raw
`on_frame:` sniffer, write with `can_gateway.send`. Bridging genuinely needs
two on-chip TWAI controllers; observing/decoding/sending needs one, which is
why observe-only configs run on far more targets than bridging ones (§15 of
the research behind this guide, distilled into §5's target table).

Two things make this different from a normal ESPHome sensor component:

- **There is no signal-decode step by default.** A decode `sensor:`/
  `binary_sensor:`/`text_sensor:` entry names exactly one CAN ID plus a byte
  or bit position — there is no DBC parser in firmware. `script/dbc2yaml.py`
  (§8) turns a DBC's `SG_`/`BO_`/`VAL_` definitions into that YAML
  mechanically, the same role `uds_catalog.py` plays for `uds`, but the DBC
  itself never ships to the device — only the positions it implies do.
- **Nothing is transmit-gated behind a flag.** Unlike `uds`'s
  `allow_active_services`, there is no opt-in switch that has to be flipped
  before `can_gateway.send`, `cyclic_sends:`, or a `routes:` bridge can put a
  frame on the wire. The gate is the YAML shape itself — declare a second
  port and a route, or a `cyclic_sends:` entry, or call `can_gateway.send`,
  and it transmits from boot. §6 covers what "passive" actually has to look
  like in a config as a result.

## 2. The three things a working setup needs

1. **A transceiver wired to `rx_pin`/`tx_pin`** on a free TWAI-capable
   controller (any ESP32 variant has at least one; §5 lists which variants
   have two). `bit_rate` is the other required key — it must match every
   other node already on that bus.
2. **Firmware YAML** — one or two `can_gateway.ports:` entries, decode
   `sensor:`/`binary_sensor:`/`text_sensor:` entries for the signals you
   want, and (only if you're actually bridging) a `routes:` block naming
   which port forwards to which.
3. **Nothing to read back offline.** Like `linbus` and unlike `sd_logger`,
   there's no workstation tool — every fact about what a flashed image is
   doing comes from the live log: `dump_config` at boot, the periodic
   statistics line, and the entities themselves. `script/dbc2yaml.py` is a
   build-time helper, not a runtime one.

## 3. Workflow, step by step

### 3.1 Wire it, start as a single-bus observer

```yaml
external_components:
  - source: github://kipp-ing/ESPCars
    components: [can_gateway]

can_gateway:
  id: gw
  ports:
    - id: car
      rx_pin: GPIO25
      tx_pin: GPIO26
      bit_rate: 500kbps
      observe_queue_depth: 48    # raise it on a busy bus feeding several decode entities

sensor:
  - platform: can_gateway
    port_id: car
    can_id: 0x2A0
    offset: 2
    length: 2
    byte_order: big
    name: "Engine RPM"
    filters: [{multiply: 0.25}]
```

`rx_pin`/`tx_pin`/`bit_rate` are the only required port keys. One port with
no `routes:` **cannot forward anything** — there's no second port to send
to — which makes it the safe default shape to start a bus you don't fully
understand yet: full sub-component test (`tests/build/can_gateway/
common_monitor.yaml`) even calls `can_gateway.send` from this exact shape,
because a single-bus node is a legitimate read-and-write node, just never a
bridge. Add a second port and a `routes:` block only once you actually need
to move frames between two buses (§3.4).

### 3.2 Read a signal — decode `sensor:`/`binary_sensor:`/`text_sensor:`

A decode entity names a port, a CAN ID, and a position — nothing else. Two
position forms, mutually exclusive:

```yaml
sensor:
  - platform: can_gateway
    port_id: car
    can_id: 0x2A0
    offset: 2          # byte-aligned form: 0-7
    length: 2           # 1-4 bytes (≤32-bit decode only)
    byte_order: big
    signed: false
    name: "Engine RPM"
    filters: [{multiply: 0.25}]
  - platform: can_gateway
    port_id: car
    can_id: 0x2A0
    bit_offset: 24      # bit-level form: 0-63, little-endian only
    bit_length: 4        # 1-32 bits
    name: "Selected gear"
    throttle: 200ms
    sna: all_ones        # raw 0xF here means "not available" -> NAN, not gear 15
```

`sna:` is tested against the **raw extracted value**, before sign extension
and before ESPHome `filters:` — give the unsigned raw bit pattern a DBC
reserves, or the literal `all_ones` for that convention. `binary_sensor:`
takes a whole-frame `bit:` (0-63) instead of offset/length; `text_sensor:`
takes `offset`/`length`/`byte_order` plus a `map:` of raw integer → string,
publishing unmapped values as raw hex rather than failing. A frame shorter
than a decode entity's position is skipped, not zero-filled — that's a
normal, silent no-publish, not a defect.

For a real DBC instead of hand-transcribing bit offsets, generate the
decode YAML instead of writing it by hand:

```bash
script/dbc2yaml.py my-car.dbc --id 0x100 --id 0x101 --port car > decode.yaml
```

See `tests/hil/decode-example.yaml` for a worked example, including the
`sna: all_ones` control-sensor pattern that proves the decode path is
actually live rather than silently NaN-ing everything.

For raw access to every frame regardless of ID, `on_frame:` on a port is a
sniffer, not a decode entity — it fires with `can_id`, `data` (a borrowed
`const uint8_t *`, valid only inside the callback), `dlc`, `extended`, and
`rtr` bound as lambda variables:

```yaml
can_gateway:
  ports:
    - id: car
      rx_pin: GPIO25
      tx_pin: GPIO26
      bit_rate: 500kbps
      on_frame:
        - then:
            - lambda: |-
                ESP_LOGD("sniff", "%s%03X [%u]%s", extended ? "x" : "",
                         (unsigned) can_id, dlc, rtr ? " rtr" : "");
```

### 3.3 Bridge two buses — `routes:` and filters

```yaml
can_gateway:
  id: gw
  ports:
    - id: port_a
      rx_pin: GPIO3
      tx_pin: GPIO2
      bit_rate: 125kbps
    - id: port_b
      rx_pin: GPIO11
      tx_pin: GPIO10
      bit_rate: 125kbps
  routes:
    - id: route_ab
      from: port_a
      to: port_b
      default_action: drop      # bench-safe: nothing crosses unless a rule says so
      filters:
        - can_id: 0x100
          modify:
            can_id: 0x200        # ID-translate on the way through
        - id: charge_limit        # a rule id makes its modify values runtime-updatable
          can_id: 0x355
          modify:
            can_id: 0x581
            data:
              - index: 2
                value: 0x64        # patch byte 2 to a fixed value
        - can_id: 0x700
          can_id_mask: 0x780       # wildcard match
          action: drop
```

A route is one-directional; a two-way bridge is two routes (`from`/`to`
swapped), each with its own rule table. Rules are matched in order; the
first match wins, `default_action` decides everything that falls through.
`modify:` needs at least one of `can_id`/`use_extended_id`/`data`; a byte
index can only be patched once per rule. Give a rule an `id:` only if you
want `can_gateway.set_patch` to update its `modify` values at runtime — a
rule id with no `modify:` block is rejected at config time, since there
would be nothing for `set_patch` to target.

**Bridging needs two ports on a chip with two on-chip TWAI controllers**
(C6, P4 — §5); declaring `routes:` on a chip with only one controller, or
on the ESP32-C5 at all, fails at `esphome config` (§7). `tests/build/
can_gateway/common_bridge.yaml` is the minimal always-on two-port bridge —
deliberately stripped of every diagnostic surface (no `statistics:`, no
decode entity, no switch) to prove the plain forwarding path compiles clean
on its own.

### 3.4 Send frames — one-shot and cyclic

One-shot, from an automation:

```yaml
esphome:
  on_boot:
    then:
      - can_gateway.send:
          port: port_a           # omittable if this is the gateway's only port
          can_id: 0x100
          data: [0x01, 0x02, 0x03]
      - can_gateway.send:            # templatable id and data
          can_id: !lambda "return 0x18DAF110;"
          use_extended_id: true
          data: !lambda 'return {(uint8_t) (millis() & 0xFF), 0x02};'
      - can_gateway.send:               # string payload, like canbus.send
          can_id: 0x101
          data: "abc"
```

`can_gateway.inject` is the same action under its pre-v0.6 name — it still
works, logged once per build as deprecated (`'can_gateway.inject' is
deprecated and will be removed in a future release. Rename it to
'can_gateway.send'; the options are identical.`). Neither action works on a
`listen_only` port, and RTR frames can't carry a literal `data` list (an RTR
frame carries no data by definition).

Cyclic (timed) sends are declared once at the gateway level and start
running from boot unless `enabled: false`:

```yaml
can_gateway:
  cyclic_sends:
    - id: heartbeat
      port: port_a
      can_id: 0x700
      interval: 100ms
      data: [0x01, 0x00]
    - id: keepalive
      port: port_b
      can_id: 0x18FF0102
      use_extended_id: true
      interval: 500ms
      enabled: false
```

```yaml
# update the staged payload and/or start/stop from an automation
- can_gateway.set_cyclic_data:
    id: heartbeat
    data: !lambda "return {(uint8_t) (millis() & 0xFF), 0x00};"
- can_gateway.start_cyclic: heartbeat
- can_gateway.stop_cyclic: keepalive
```

**Cyclic intervals are floored by the ~16 ms ESPHome main-loop period** —
config validation accepts any positive interval, but two configured 4 ms and
10 ms streams both measured at ~62 f/s (≈16 ms effective) on the bench; the
main loop, not the configured `interval:`, is the real pacing source. This
is a known open item (Issue #2, `docs/HANDOVER.md` §3.2) — plan aggregate
cyclic load in frames/second against the loop period, not against the sum of
configured intervals, and reach for many cyclic IDs (proven to ~2000 f/s
aggregate with ~100 IDs) rather than one very fast one.

### 3.5 Bus-off recovery

No YAML tunes the recovery schedule — it's a fixed exponential backoff (100
ms doubling to a 3.2 s cap, `gateway_core.h`'s `RecoveryBackoff`) that only
resets to the base delay after the bus has stayed healthy for 10 s, so a
permanently dead bus settles at retrying every 3.2 s instead of flapping at
~5 Hz forever. The only config surface is a pair of plain (argument-less)
triggers per port:

```yaml
can_gateway:
  ports:
    - id: port_a
      rx_pin: GPIO3
      tx_pin: GPIO2
      bit_rate: 125kbps
      on_bus_off:
        then:
          - lambda: 'ESP_LOGW("app", "port A bus-off");'
      on_recovered:
        then:
          - lambda: 'ESP_LOGI("app", "port A recovered");'
```

The firmware logs the same transition on its own:

```
[W][can_gateway:XXX]: Port 0: bus-off, starting recovery
[I][can_gateway:XXX]: Port 0: recovered
```

A port whose *forwarding* is shed for any reason — its own bus-off, the
destination's bus-off, a full TX queue, or the gateway disabled via the
switch (§6) — keeps observing, tapping and counting the whole time; only the
forward itself stops. That was Issue #1 (`docs/CONVENTIONS.md`'s "known
sharp edges"), fixed 2026-07-25: a healthy port used to go silent on every
counter, not just its forwarding, whenever its sibling shed a frame for any
reason. If a route's `disabled`/`bus_off`/`tx_full` diagnostic counter (§5)
is climbing while the *source* port's own `observed`/`bus_load` counters
keep moving, that's the fixed, expected behavior — the source is fine, the
destination is the one to look at.

### 3.6 Flash, confirm from the log

```
[C][can_gateway:XXX]: CAN gateway:
[C][can_gateway:XXX]:   Enabled: YES
[C][can_gateway:XXX]:   Interrupt priority: 2
[C][can_gateway:XXX]:   Port 0 (TWAI0):
[C][can_gateway:XXX]:     TX pin: GPIO2, RX pin: GPIO3
[C][can_gateway:XXX]:     Bit rate: 125000 bit/s
[C][can_gateway:XXX]:     TX queue depth: 8
[C][can_gateway:XXX]:     Hardware filter offload: NO
[C][can_gateway:XXX]:   Port 1 (TWAI1):
[C][can_gateway:XXX]:     TX pin: GPIO10, RX pin: GPIO11
[C][can_gateway:XXX]:     Bit rate: 125000 bit/s
[C][can_gateway:XXX]:     TX queue depth: 8
[C][can_gateway:XXX]:     Hardware filter offload: NO
[C][can_gateway:XXX]:   Route: port 0 -> port 1
[C][can_gateway:XXX]:     Rules: 4, default: drop
[C][can_gateway:XXX]:     Slots in use: 0/10
[C][can_gateway:XXX]:   Cyclic send: port 0, ID 0x700, every 100 ms, running: YES
[C][can_gateway:XXX]:   Statistics log interval: 30000 ms
[C][can_gateway:XXX]:   Per-ID timings: YES
```

A single-bus observer's block is much shorter — no `Route:` or `Cyclic
send:` lines at all, because those loops are simply empty:

```
[C][can_gateway:XXX]: CAN gateway:
[C][can_gateway:XXX]:   Enabled: YES
[C][can_gateway:XXX]:   Interrupt priority: 2
[C][can_gateway:XXX]:   Port 0 (TWAI0):
[C][can_gateway:XXX]:     TX pin: GPIO26, RX pin: GPIO25
[C][can_gateway:XXX]:     Bit rate: 500000 bit/s
[C][can_gateway:XXX]:     TX queue depth: 8
[C][can_gateway:XXX]:     Hardware filter offload: NO
[C][can_gateway:XXX]:   Statistics log interval: 30000 ms
```

`Hardware filter offload: NO` here is expected, not a problem: any port with
a decode/diagnostic/observe consumer needs every frame to reach software, so
the hardware acceptance filter stays open. `Listen only:`, `Self test
(bench aid):` and `Open-drain TX (bench aid):` lines only print when set —
the latter two are bench aids for sharing one wire between two
transceiver-less controllers, never for a real vehicle bus.

### 3.7 Watch the statistics

With `statistics:` configured, a per-port line lands on `log_interval`
(default 60s if declared at all — the block itself is optional):

```
[I][can_gateway:XXX]: Port 0 stats: bus load 24.3% (rx 11.9% / tx 12.4%), bus_err 0, err_events 3, tx_fail 0, TEC 0, REC 0
```

`bus_err` counts genuine faults only — TEC/REC actually moving upward,
polled every 500 ms — while `err_events` counts every raw controller error
flag including benign arbitration loss, which climbs on any bus this port
transmits into and is not itself a problem. `TEC`/`REC` at 0 with the port
otherwise healthy is normal; climbing TEC without recovering is the bus-off
warning sign (§3.5).

`statistics.id_timings: true` adds a bounded per-ID arrival-timing dump —
at most one line per port per loop pass, so a full table never blocks the
loop, resuming across passes if it doesn't finish in one:

```
[I][can_gateway:XXX]:   Port 0 ID 0x700: avg 100230 us, n 42
[I][can_gateway:XXX]:   Port 0 ID 0x18FF0102: n 1
[W][can_gateway:XXX]:   Port 0 ID table full: 3 frames untracked (raise id_timings_max)
```

Raise `id_timings_max` (default 32) if the table-full warning appears — it
means more distinct IDs crossed the port than the table can track, not that
anything was lost from the bus itself.

### 3.8 Feed `sd_logger`'s native CAN tap

`log_tap: true` on a port arms a dedicated RX-ISR ring for `sd_logger`'s
native tap (S2) — every frame the port receives, forwarded, filtered or
**shed**, lands in the file with a capture-time stamp. `log_tap_queue_depth`
(gateway-level, default 1024, 64–8192) sizes the single ring type every
armed port instantiates — sized by default for one SD write-latency spike
(~280 ms at 90% load on a 500 kbps segment):

```yaml
can_gateway:
  log_tap_queue_depth: 256
  ports:
    - id: port_a
      # ...
      log_tap: true
```

See `docs/sd_logger-user-guide.md` §3.2 for the `sd_logger:` side
(`can_ports:` naming the tapped port, and why it rejects a port with no
`log_tap: true`). `can_gateway` itself places no requirement in the other
direction — an armed tap with no `sd_logger` consumer is legal, just wasted.

## 4. `packages:` — splitting "this board" from "this bus setup"

Same pattern as every other component in this repo: a leaf file carries
what is genuinely per-board (pins, board type, other automations); a
package carries the `can_gateway:` block and its entities, reusable across
any board wired the same way.

```yaml
# tests/hil/mr-orange.yaml — layers can_gateway (+ linbus) onto hil_common.yaml
packages:
  common: !include hil_common.yaml   # board bring-up: power, LED, wifi/api/logger
```

`tests/hil/mr-orange.yaml` is the real worked example: a two-port bridge
plus a decode sensor plus route/port diagnostic sensors, alongside a
`linbus:` listener on the same board — proving the two components coexist
in one firmware without either needing to know about the other.
`tests/hil/decode-example.yaml` is the worked example for the other end of
the spectrum: a genuinely passive single-port decode config, with its own
header comment spelling out exactly why it cannot transmit (§6).

## 5. Config reference

### `can_gateway:` (one instance)

| key | meaning |
|---|---|
| `id` | this gateway's id |
| `ports` | required, 1–2 entries — see below; capped by the target's on-chip TWAI controller count |
| `routes` | optional, ≥1 entry if present — see below; requires 2 ports |
| `interrupt_priority` | default `2`, `1`–`3` |
| `cyclic_sends` | optional, max 32 — see below |
| `log_tap_queue_depth` | default `1024`, `64`–`8192` — one shared ring depth for every port that sets `log_tap: true` |
| `statistics` | optional — `log_interval` (default `60s`, `0` disables the periodic line), `id_timings` (default `false`), `id_timings_max` (default `32`, `1`–`128`) |

### `ports:` entries

| key | meaning |
|---|---|
| `id` | required |
| `rx_pin` / `tx_pin` | required, internal GPIO |
| `bit_rate` | required, `10kbps`–`1000kbps` |
| `listen_only` | default `false` — never transmits, including `can_gateway.send`/cyclic; mutually exclusive with `self_test` |
| `tx_queue_depth` | default `8`, `1`–`64` |
| `observe_queue_depth` | default `32`, `4`–`128` — raise it on a busy bus feeding several decode/observe consumers |
| `log_tap` | default `false` — arms the `sd_logger` native tap ring (§3.8) |
| `max_frames_per_loop` | defaults to `observe_queue_depth`; cannot exceed it |
| `self_test` | default `false` — bench aid, loops a port back on itself; mutually exclusive with `listen_only` |
| `open_drain_tx` | default `false` — bench aid for sharing one wire between two transceiver-less controllers with an external pull-up; never with a real transceiver |
| `on_bus_off` / `on_recovered` | plain (argument-less) automations — §3.5 |
| `on_frame` | automation, fires per received frame with `can_id`/`data`/`dlc`/`extended`/`rtr` — forces the raw observation path on (§3.2) |

### `routes:` entries

| key | meaning |
|---|---|
| `id` | auto-generated |
| `from` / `to` | required port ids, must differ; `to` cannot be a `listen_only` port |
| `default_action` | default `accept`; `accept`/`drop` |
| `filters` | optional, 1–255 entries — see below |

A port can be the `from` of at most one route — one outbound rule table per
source.

### `filters:` entries (one rule)

| key | meaning |
|---|---|
| `id` | optional — only meaningful with `modify:`; makes the rule's values updatable via `can_gateway.set_patch` |
| `can_id` | required, `0`–`0x1FFFFFFF` |
| `can_id_mask` | defaults to the full ID-type bound; narrows the match for a wildcard rule |
| `use_extended_id` | default `false` |
| `remote_transmission_request` | unset = don't check RTR; `true`/`false` to match on it |
| `action` | default `accept`; `accept`/`drop` |
| `modify` | optional — `can_id`, `use_extended_id`, `data: [{index: 0-7, value: 0-255, mask: 0xFF}]`; needs at least one of the three; a byte index can only appear once; incompatible with `action: drop` |

### `cyclic_sends:` entries

| key | meaning |
|---|---|
| `id` | required |
| `port` | required, cannot be `listen_only` |
| `can_id` | required, `0`–`0x1FFFFFFF` |
| `interval` | required, `>0` — floored by the ~16 ms main loop, §3.4 |
| `use_extended_id` / `remote_transmission_request` | default `false` |
| `data` | default `[]`, 0–8 hex bytes |
| `enabled` | default `true` — starts transmitting from boot unless set `false` |

### Actions

| action | needs | does |
|---|---|---|
| `can_gateway.send: {port, can_id, use_extended_id, remote_transmission_request, data}` | — | one-shot TX; `port` omittable with exactly one declared port; `data` accepts a hex list, an ASCII string, or a `!lambda` |
| `can_gateway.inject` | — | pre-v0.6 name for `can_gateway.send`, identical options, deprecated |
| `can_gateway.set_patch: {id, can_id, data}` | the target filter rule declared `id:` + `modify:` | runtime-update a rule's `modify` values; only the fields the rule's `modify:` already declared |
| `can_gateway.set_cyclic_data: {id, data}` | — | update a cyclic sender's staged payload |
| `can_gateway.start_cyclic: <id>` / `can_gateway.stop_cyclic: <id>` | — | start/stop a cyclic sender |

### `sensor: platform: can_gateway` — two shapes

A **decode sensor** names a signal:

| key | meaning |
|---|---|
| `port_id`, `can_id`, `use_extended_id` | which frame |
| `offset` + `length` | byte-aligned form: `offset` `0`–`7`, `length` `1`–`4` (≤32-bit) |
| `bit_offset` + `bit_length` | bit-level form: `0`–`63` / `1`–`32`, little-endian only |
| `byte_order` | default `little`; `big` only valid for the byte-aligned form |
| `signed` | default `false` |
| `throttle` | optional — rate-limit publish of a fast-changing signal |
| `sna` | optional — `all_ones`, or a raw value that must fit the signal's own bit width; tested before sign extension and before `filters:` |

A **diagnostic sensor** names counters/gauges instead — exactly one of
`route_id`/`port_id`, plus at least one of:

| scope | keys |
|---|---|
| route (`route_id`) | `forwarded`, `filtered`, `tx_full`, `bus_off`, `disabled` — all counters |
| port (`port_id`) | `injected`, `tx_fail`, `bus_err`, `recoveries` (counters); `tec`, `rec` (gauges); `bus_load` (%, needs the port to actually receive — not valid on a route-destination-only port); `observed`, `observe_overflow` (counters) |

Owner is a shared polling hub, `update_interval` default `60s`.

### `binary_sensor: platform: can_gateway` — two shapes

| shape | keys |
|---|---|
| bus-off flag | `port_id` + `bus_off:` — `true` on entering bus-off, `false` on recovery |
| decode bit | `port_id`, `can_id`, `use_extended_id`, `bit` (`0`–`63`, whole-frame index) |

### `text_sensor: platform: can_gateway` — two shapes

| shape | keys |
|---|---|
| last frame (debug) | `port_id`, `throttle` (default `1s`) + `last_frame:` — pre-filter formatted frame, e.g. `0x2A0 [8] 01 02 03 04 05 06 07 08` |
| decode enum | `port_id`, `can_id`, `offset`, `length`, `byte_order`, `map:` (raw int → string, unmapped values publish as raw hex) |

### `switch: platform: can_gateway`

One instance total per config, and only valid when the gateway has at least
one route (there'd be nothing to gate otherwise). `write_state` toggles
forwarding only — reception, decode, diagnostics, recovery and
`can_gateway.send`/`cyclic_sends` all keep running regardless. Defaults
`RESTORE_DEFAULT_ON`, and runs its restore *before* the TWAI controllers
come up, so a persisted OFF is honored from boot; **with no switch declared
at all, the gateway is enabled from boot by construction** — see §6.

## 6. Safety model

TX on a vehicle bus is safety-relevant (repo-wide rule) — but `can_gateway`
has no single flag that gates it, unlike `uds`'s `allow_active_services`.
Instead, whether a config can transmit is entirely a property of what you
declare:

- **A single port with no `routes:`, no `cyclic_sends:`, and no
  `can_gateway.send`/`inject` calls anywhere in the config cannot transmit
  at all** — that's the genuinely passive shape, and it's a config-writing
  discipline, not a built-in mode. `tests/hil/decode-example.yaml`'s header
  comment states this explicitly: passive has to mean passive at the
  routing table, not merely the absence of a `cyclic_sends:` block — a
  single port still has a live TX pin, and `can_gateway.send` fires on it
  the instant something calls it.
- **A route transmits the moment both its ports are up in `setup()`** — no
  separate arm step. The optional `switch: platform: can_gateway` gates
  forwarding at runtime, but it defaults **on** (`RESTORE_DEFAULT_ON`), and
  its own config-time validator refuses to let you declare it on a gateway
  with no routes — it exists to let you turn a bridge off, never to require
  turning it on.
- **A `cyclic_sends:` entry runs from boot** unless you set `enabled:
  false` explicitly.
- **`self_test`/`open_drain_tx`** are bench aids for sharing an
  transceiver-less wire between two controllers with an external pull-up —
  never set either on hardware wired to a real vehicle bus.

Start every new bus with a single port, no `routes:`, decode entities only
— exactly §3.1 — and only add a second port plus `routes:` once you've
confirmed what's actually on the wire from the decode/diagnostic entities
and the log.

## 7. Troubleshooting

| symptom | what it means |
|---|---|
| `can_gateway: the ESP32C5 is not supported — it builds the TWAI-FD HAL ...` (config error) | the C5 is rejected outright, at config time and again with a build `#error` — its TWAI-FD HAL completes a frame halted by bus-off, which the forwarding slot tracker isn't verified against. Don't remove the guard; use a C6/P4 for bridging or any other TWAI-capable variant for observe-only |
| `can_gateway: N ports configured but the <variant> has M on-chip TWAI controller(s)` | too many ports for this chip — one-controller variants (ESP32, S2, S3, C3, H2) take exactly one port |
| `routes require two ports; a single-port can_gateway is a monitor/node with no forwarding` | `routes:` declared with only one port — add a second port or drop `routes:` |
| `port '<id>' already has a route; at most one route per direction` | two `routes:` entries share a `from:` — one outbound rule table per source port |
| `route cannot target listen-only port` / `cyclic_send cannot target listen-only port` | `listen_only: true` ports never transmit, including as a route destination or cyclic target |
| `a filter rule id makes its modify values updatable at runtime; without a modify block there is nothing can_gateway.set_patch could target` | gave a rule an `id:` with no `modify:` — drop the id or add a modify block |
| `byte index N may only be patched once` | two `modify.data` entries in one rule target the same index |
| `self_test and listen_only are mutually exclusive` | both set on one port — pick one |
| `max_frames_per_loop (N) cannot exceed observe_queue_depth (M)` | raise `observe_queue_depth` or lower `max_frames_per_loop` |
| `'<action>' needs a 'port': more than one port is declared, so it cannot be inferred` | `can_gateway.send`/`inject` with no `port:` on a gateway with 2 ports |
| `bus_load on port '<id>' would only measure the gateway's own transmissions; the port is a route destination only` | `bus_load` diagnostic sensor on a port this gateway never receives on — put it on the source port instead |
| `sna (N) does not fit this W-bit signal` | `sna:` value wider than the signal itself — give the raw unsigned bit pattern, not a filtered/signed one |
| a decode sensor never publishes | check the frame is actually ≥ `offset+length`/`bit_offset+bit_length` bytes — a too-short frame is skipped, not zero-filled; confirm with a `last_frame` text sensor or `on_frame:` sniffer first |
| `Port N stats: ... err_events` climbing but `bus_err` staying flat | expected — `err_events` counts every raw controller error flag including benign arbitration loss on a bus this port transmits into; `bus_err` (genuine TEC/REC movement) is the one to actually watch |
| `Port N: bus-off, starting recovery` repeating roughly every 3.2 s | the backoff has escalated to its cap — the bus has not stayed healthy for the 10 s needed to reset it; fix the underlying fault rather than the retry cadence |
| a route's `disabled`/`bus_off`/`tx_full` counter climbs while the *source* port's `observed`/`bus_load` keep moving | expected since the 2026-07-25 Issue #1 fix — only the forward is shed, observation/diagnostics on the source stay live; look at the destination port |
| `Port N ID table full: M frames untracked` | more distinct IDs crossed the port than `id_timings_max` tracks — raise it, no frames were dropped from the bus itself |
| cyclic sends measure slower than their configured `interval:` | the ~16 ms main-loop floor (Issue #2, open) — plan aggregate cyclic throughput against the loop period, use more IDs rather than one very fast one |
| `dump_config` shows `Hardware filter offload: NO` even though I only route on a couple of IDs | expected whenever the port also has a decode/diagnostic/observe consumer — those need every frame in software, so the hardware filter stays open |

## 8. Going deeper

- `components/can_gateway/__init__.py` — every config key, schema and
  validator (V1–V29), if a message here doesn't say enough.
- `components/can_gateway/can_gateway.cpp` — `dump_config()`, the ISR data
  plane, bus-off recovery, and the ESP32-C5 build-time guards with their
  full HAL-variant reasoning.
- `components/can_gateway/gateway_core.h` — the host-testable pure core:
  rule engine, slot pools, `RecoveryBackoff`, decode primitives. This is
  what `tests/host/` exercises under ASan/UBSan — read it before touching
  anything ISR-side.
- `script/dbc2yaml.py` — DBC → decode-sensor YAML generator; refuses a
  Motorola (`@0`) signal unless it's byte-aligned rather than silently
  mistranslating it. `tests/can_gateway/test_dbc2yaml.py` pins the SNA and
  Motorola-refusal cases.
- `tests/build/can_gateway/common.yaml` — full-feature worked example:
  routes, cyclic sends, all four decode platforms, statistics, `log_tap`,
  runtime patch, the switch.
- `tests/build/can_gateway/common_bridge.yaml` — minimal always-on two-port
  bridge, diagnostics-free.
- `tests/build/can_gateway/common_monitor.yaml` /
  `common_sniffer.yaml` — single-bus node shapes: decode entities, and a raw
  `on_frame:` sniffer.
- `tests/hil/mr-orange.yaml` — a real board: `packages:` split, two-port
  bridge, decode + diagnostic sensors, alongside `linbus`.
- `tests/hil/decode-example.yaml` — a passive single-port decode config,
  with the `sna:` control-sensor pattern and the "why this can't transmit"
  rationale spelled out in its header.
- `docs/CONVENTIONS.md` §"Known sharp edges" and `docs/HANDOVER.md` §3 — the
  full Issue #1–#4 findings ledger (bus-off/shed-path fix, the cyclic-send
  floor, the C5 HAL contract, and more), with bench verdicts.
