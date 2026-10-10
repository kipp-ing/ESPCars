# Using `sd_logger` — a bus datalogger in an ESPHome project

A practical guide: what the component does, how a project is actually put
together, and the end-to-end workflow from a wired-up card to a checked-out
`.LOG` file on your desk. For *why* it is built this way — the ring/writer
split, the write format, the chunk lifecycle — see
[`sd_logger-spec.md`](sd_logger-spec.md) and
[`sdlog-collection-design.md`](sdlog-collection-design.md). This document only
covers the *how*.

## 1. The shape of it

```
producers (ISR / lambda / action)     writer task              collection server (optional)
──────────────────────────────────    ──────────────────────    ────────────────────────────
sd_logger.log / can_gateway tap  ──▶  [ RAM ring ] ──▶ drain ──▶  FAT file, one line per record
                                        f_sync on a bounded          ▼ seal (rotate/close)
                                        cadence, never per write   SEALED chunk ──▶ served over
                                                                    WiFi, confirmed, renamed .UPL
```

`sd_logger` is a **sink**, never a source. It never transmits on any bus (a
repo-wide rule; the whole point of a datalogger is to be safe to leave running).
Producers push fixed 20-byte records into a RAM ring from ISR/loop context;
a dedicated writer task drains the ring and formats records directly into a
FAT file, so an SD write-latency spike lands on the writer task, never on the
CAN forwarding ISR or the LIN task. Everything else in this guide — chunk
rotation, retention, the collection server — sits downstream of that one
invariant.

Two things make this different from a normal ESPHome sensor component:

- **There is nothing to decode.** `sd_logger` writes raw CAN/LIN frames, its
  own action's data, and (optionally) ESPHome's own log — one typed CSV line
  per record — into a file on the card. It publishes no entities; what comes
  back is a file, read with `script/sdlog.py` on your workstation.
- **The card is the buffer, not RAM.** There is no PSRAM on the ESP32-C6, so
  the 40–80 KB RAM ring only has to absorb one SD latency spike (hundreds of
  ms); the card absorbs hours. "Store everything all the time" is bounded by
  either card size or by collection keeping up — see §3.5.

## 2. The three things a working setup needs

1. **A card wired to four free GPIO in SPI mode** (`clk`/`mosi`/`miso`/`cs`) —
   SD-over-SPI on the C6, capped at 20 MHz. FAT32, one partition, formatted
   (`format_if_mount_failed: true` is a recovery switch, not a routine one).
2. **Firmware YAML** — `sd_logger:` (the card + writer) fed by one or more of
   its three shipped sources: the generic `sd_logger.log` action, a native
   `can_gateway` tap, or ESPHome's own log — plus, optionally, `collection:`
   to serve chunks over WiFi.
3. **`script/sdlog.py`** on your workstation to read a card back — a `.LOG`
   file is a plain, self-describing CSV with a legend in its own header, but
   parsing the delta-coded timestamp column by eye is not something to
   attempt; that is what the reader is for.

## 3. Workflow, step by step

### 3.1 Wire the card, write the `sd_logger:` block

```yaml
external_components:
  - source: github://kipp-ing/ESPCars
    components: [sd_logger]

sd_logger:
  id: sd_log
  clk_pin: GPIO21
  mosi_pin: GPIO22
  miso_pin: GPIO20
  cs_pin: GPIO15
  clock: 10MHz              # capped at 20MHz over SPI on the C6 — start lower with long/flying leads
  buffer_depth: 4096        # records in the RAM ring; raise it on a config that taps a busy bus
  sync_interval: 1s         # bounded fsync cadence => max data-loss window on a hard cut
  max_file_size: 4MB        # rotation threshold — 1-4MB chunks are the collection sweet spot
  max_file_seconds: 60s     # the *other* rotation bound: a quiet bus still seals a chunk
```

`clk_pin`/`mosi_pin`/`miso_pin`/`cs_pin` are the only required keys — a
config with just those and nothing feeding it will mount the card and write
nothing, which is a legal (if pointless) starting point to prove the wiring
before adding a source.

### 3.2 Feed it — pick one or more sources

- **The generic action, `sd_logger.log`** — works from any automation, no
  other component required. This is the only path for LIN today (the native
  `linbus` tap is designed but not built — see §5's `sources:` entry):

  ```yaml
  sources:
    - tag: 5
      kind: lin
      label: lin

  # in a linbus on_frame: automation, or any interval/lambda
  - sd_logger.log:
      source: lin           # a declared label, or the bare numeric tag
      can_id: !lambda "return frame.id;"
      data: !lambda "return frame.data;"
  ```

- **The native `can_gateway` tap (S2)** — a per-port ring filled in the RX
  ISR, so `t_us` is stamped at capture and forwarded/filtered/**shed** frames
  are all captured (the `s` flag is what makes a shed frame visible — the
  reason this tap exists). Needs `log_tap: true` on the gateway port; without
  it `sd_logger`'s own config validation rejects the config rather than
  logging silence:

  ```yaml
  can_gateway:
    id: gw
    ports:
      - id: seg1
        log_tap: true          # required — see V12 in §5
        # ...
      - id: seg2
        log_tap: true

  sd_logger:
    can_ports:
      - seg1                   # bare id: source tag = position (1), label = C1
      - port: seg2
        source: 9               # optional explicit tag
        label: diag              # optional explicit label, <=8 chars
  ```

- **ESPHome's own log (S4)** — every captured line lands as an `X` record in
  the same file, inline with the traffic it explains. Needs a `logger:`
  block (V15) and reads what `logger.level` already let through (V16):

  ```yaml
  logger:
  sd_logger:
    esphome_logs:
      level: INFO
      buffer_depth: 32
  ```

Every producer stamps a 1-byte tag; `can_ports:`/`sources:` name it once so
the file's `#src` header maps it to a type letter and a short label instead
of a bare number meaningful only next to the YAML that produced it. Tag `0`
is the `sd_logger.log` action's own default — declare it under `sources:` to
give the action's own records a name too (`common.yaml`'s `label: act`).

### 3.3 Flash, and confirm from the log

One flashing step — unlike `uds`, there is no separate catalog. `dump_config`
states what was actually found at boot:

```
[C][sd_logger:XXX]: SD logger:
[C][sd_logger:XXX]:   SPI: clk=21 mosi=22 miso=20 cs=15 @ 10000000 Hz
[C][sd_logger:XXX]:   mount=/sdcard buffer_depth=4096 sync_interval=1000ms
[C][sd_logger:XXX]:   max_file_size=4194304 B  mounted=yes
[C][sd_logger:XXX]:   max_file_seconds=60 s (rotation takes whichever bound fires first)
[C][sd_logger:XXX]:   recovery: retry in 1000 ms, max_attempts=0, power_cycle=no
[C][sd_logger:XXX]:   recovery: in-band reset from attempt 2, busy budget 2000 ms doubling to 10000 ms
```

`mounted=NO (logging disabled)` is not fatal — a missing/failed card never
blocks boot, and `recovery:` (on by default) keeps retrying for as long as
the board runs (§6). With `statistics: {log_interval: 5s}` set, a periodic
line is the thing to watch on a bench:

```
[I][sd_logger:XXX]: records=41823 dropped=0 tap_dropped=0 text_dropped=0 card_dropped=0
                     bytes=1738562 file=L0000012 index_refused=0 mounted=1
```

The four drop counters name **different bottlenecks and different fixes** —
never sum them: `dropped` is a producer that found the logger's own ring
full (raise `buffer_depth`), `tap_dropped` is the gateway's RX ISR finding a
tap ring full (raise `can_gateway`'s `log_tap_queue_depth`), `text_dropped`
is a captured log line finding the text ring full (raise
`esphome_logs.buffer_depth`), `card_dropped` is everything lost because
there was no writable file at all — the card is gone, not merely slow, and
`mounted=0` is why. `index_refused` is not a drop: it counts chunks the
collection index couldn't track even after trying to reclaim a confirmed
chunk (§3.5), which is a `collection.max_chunks` / collection cadence
question, never a throughput one.

### 3.4 Read the file back

```bash
script/sdlog.py head    L0000012.LOG                        # the file's own #src/#types legend
script/sdlog.py check   L0000012.LOG                        # verdict: intact? anything dropped?
script/sdlog.py check   --strict /Volumes/SD/*.LOG           # a whole card, fail on any #drop/#gap/no-#close
script/sdlog.py extract L0000012.LOG --type C --label C1     # one interface as a plain decimal CSV
script/sdlog.py extract L0000012.LOG --type C --shed         # every shed frame — the "why didn't it forward" hunt
```

`check` answers the two questions that matter on a bench: **is the file
intact**, and **did anything get dropped**. A missing `#close` as the last
line is the power-cut signature — every orderly end (rotation, shutdown, the
VCC emergency close) writes one, so a file without one was cut; that is
expected after pulling the power and is reported, not treated as failure,
unless `--strict` is asked for (the CI/bench-gate shape). A malformed line
*anywhere but the very end* is a real defect — a formatter bug or a card
returning garbage; one torn trailing line after a cut is expected and
harmless.

Every stream line is `grep`-able directly, because type, tag and flags all
sit in the self-delimiting leading token:

```bash
grep '^C1'          L0000012.LOG   # everything from interface C1
grep '^C[0-9A-F]*s' L0000012.LOG   # every shed frame, whichever interface
grep '^[X#]'        L0000012.LOG   # firmware's own log lines and the meta/legend lines
```

What a one-liner like that **can't** do is resolve an actual timestamp — the
stamp column is delta-coded against the previous line to keep the file small
(§5's `#layout`), so decoding needs every stream line before it. That's what
`extract`/`check` are for; they walk the chain and print absolute decimal µs.

### 3.5 Collect chunks over WiFi (optional)

Rotation seals a file into a chunk (SEALED); a private `esp_http_server`
(never ESPHome's own `web_server` — a deliberate separation, see §5) serves
sealed chunks and takes the confirm that renames one to `.UPL`. **The order
is the whole design and only one order is safe**: serve bytes → puller
verifies locally → confirm → device renames. A crash anywhere in that window
just re-serves the chunk; confirming *before* the bytes are safely on disk
is the one mistake that loses data permanently.

```yaml
wifi:
  ssid: my-car-wifi
  password: !secret wifi_password

sd_logger:
  collection:
    enabled: true          # the card half: chunk index, retention, the #gap marker
    serve: true             # the network half: the esp_http_server (needs wifi:, V22)
    port: 8080               # not 80 — keeps clear of a web_server you might add later (V25)
    retention_percent: 80    # arm "drop oldest un-collected" at 80% card fill
    max_chunks: 512           # in-RAM chunk index; size for the longest run between collections
```

From a Mac (never from the device itself — the puller owns retries,
dedup and the archive; the ECU only ever answers "what exists and what state
is it in"):

```bash
script/sdlog_collect.py index http://mr-orange.local:8080
script/sdlog_collect.py pull  http://mr-orange.local:8080 --into ~/sdlog-archive
```

`pull` fetches every SEALED chunk, verifies it locally with the same reader
as §3.4, dedups on `(device, seq)`, resumes a partial download with `Range`,
and only then confirms. `index` alone confirms nothing — safe to run any
time.

`serve: false` is a real, supported configuration: a device whose card is
collected by pulling it out, with the chunk index, retention and the `#gap`
marker still active. It is also the only shape that lets a bench soak
rotation and retention without WiFi — associating brings tasks above the LIN
task's priority, and the effect on bus timing is unmeasured (see
`sdlog-collection-design.md` §9.1), so don't add `serve: true` to a
bus-critical board casually.

**Watch the card, not just the logs, once retention is armed.** Above
`retention_percent`, retention deletes CONFIRMED chunks first. If none are
available, it deletes never-collected SEALED chunks and states that loss
in-band as a `#gap,<t_us>,<chunks>,<bytes>,<first_seq>,<last_seq>` line so a
card found later doesn't read as a quiet period. `script/sdlog.py check
--verbose` reports it. Separately, if the index fills below the card-fill
threshold, confirmed `.UPL` chunks are also the free list: the oldest
non-serving confirmed file is deleted and the new chunk is tracked. A card
that produces chunks faster than they are confirmed will still hit
`collection.max_chunks`; those refused chunks are untracked and unservable
until a remount can see them. Size `max_chunks` for the longest stretch the
device will run **without confirmations**, not for a single drive.

## 4. `packages:` — splitting "this board" from "this logging setup"

Same pattern as every other component in this repo: a leaf file carries what
is genuinely per-board (pins, board type, the rest of that board's
automations); a package carries the logging config, reusable across any
board wired the same way.

```yaml
# tests/hil/mr-orange-sdlog.yaml — layers sd_logger onto a proven board config
packages:
  orange: !include mr-orange.yaml   # the board: CAN gateway, LIN, its own automations, untouched

sd_logger:
  id: sd_log
  clk_pin: GPIO21
  # ...
```

This is worth doing even for a single board: it keeps a change to the
logging setup (a new `max_file_seconds`, a `collection:` block) from ever
touching the file that proves the board's core behavior still works, and
keeps the reverse true too — see `tests/hil/mr-orange-sdlog.yaml` for the
worked example this guide's §3 examples are drawn from, and
`tests/build/sd_logger/common.yaml` / `common_cantap.yaml` for the two
minimal, fully-annotated configs (action-only, and native CAN-tap) that CI
compiles on every change.

## 5. Config reference

### `sd_logger:` (one instance; ESP32-C6 only)

| key | meaning |
|---|---|
| `clk_pin` / `mosi_pin` / `miso_pin` / `cs_pin` | the only required keys — SD-over-SPI pins, all distinct from each other and from every other pin below |
| `clock` | default `20MHz`; SD-over-SPI on the C6 is capped there (400kHz–20MHz) — start lower with long/flying leads |
| `mount_point` | default `/sdcard` |
| `card_power_pin` | optional high-side switch on the card's supply; enables `recovery.power_cycle` by default |
| `buffer_depth` | records in the RAM ring; default 2048, power of two, 256–16384. Raise it on a config that taps a busy bus — the burst it must absorb is one SD latency spike |
| `sync_interval` | default `2s`; bounded `f_sync` cadence — the maximum data loss on a hard cut is one batch, this is the knob |
| `max_file_size` | default `16MB`; rotation threshold, 64KB–1GB. 1–4MB is the collection sweet spot (§3.5) |
| `max_file_seconds` | no default (rotation is size-only until set); 1–3600s, whole seconds only. A quiet bus otherwise traps its newest records in a chunk that is never SEALED and so never servable |
| `format_if_mount_failed` | default `false`; recovery only, never routine — it erases the card |
| `can_ports` | list — see below; native `can_gateway` tap (S2) |
| `sources` | list — see below; names a tag fed by `sd_logger.log` or a future native tap |
| `esphome_logs` | `level` (default `INFO`) / `buffer_depth` (default 32, power of two, 8–256); needs `logger:` (V15) |
| `recovery` | card-failure retry ladder — see below |
| `vcc_monitor` | the close-file emergency on rail sag — see below |
| `collection` | chunk lifecycle + optional serving over WiFi — see below |
| `statistics.log_interval` | default `60s`; periodic `records=… dropped=…` line (§3.3) |

### `can_ports:` (native CAN tap, S2)

| key | meaning |
|---|---|
| `port` | a `can_gateway` port id; that port must have `log_tap: true` or config validation rejects it (V12) — a port may be tapped once |
| `source` | optional tag, 0–255; defaults to `1 + position` in the list |
| `label` | optional, ≤8 characters (`A-Za-z0-9_-`); defaults to `C1`, `C2`, … by position. Every character costs bytes/s at production load — 4 is the recommendation, 8 the cap |

### `sources:` (named tags for the action, and future native taps)

| key | meaning |
|---|---|
| `tag` | required, 0–255; tag `0` is `sd_logger.log`'s own default source |
| `kind` | required: `can`, `lin`, or `user` — the type letter (`C`/`L`/`U`) the file uses |
| `label` | required, same rule as above |

Tags and labels must be unique across `can_ports` *and* `sources` together.

### `recovery:` (card-failure retry ladder, on by default)

| key | meaning |
|---|---|
| `enabled` | default `true` — off means a card failure disables logging for the rest of the run |
| `initial_delay` / `max_delay` | default `1s` / `30s`; the retry delay doubles between these on each attempt |
| `max_attempts` | default `0` = retry for as long as the board runs — the right default in a car, where nobody is there to power-cycle it |
| `power_cycle` | defaults to whether `card_power_pin` is set (V19); needs it to mean anything |
| `in_band_reset` | default `true`; talks a card that hung mid-write out of it (stop token, then CMD0) without touching power — the step that actually addresses "wedged after a reset mid-write" |
| `busy_timeout` / `max_busy_timeout` | default `2s` / `10s`; how long one in-band reset attempt waits for the card to release the bus, doubling up to the max on further attempts |

### `vcc_monitor:` (the close-file emergency)

Detects the 12 V rail sagging and attempts a clean file close before power
is gone. Best-effort — it needs hold-up energy on the board to mean
anything; the real crash-safety guarantee is `sync_interval` (Layer A),
this is a bonus (Layer B).

| key | meaning |
|---|---|
| `adc_pin` | required; an ADC1-capable GPIO (0–6 on the C6) on the rail's divider |
| `threshold` | required, a voltage; trip **above** the 3V3 regulator's dropout, not at brownout, so warning arrives while 3V3 is still valid |
| `divider` | required; `V_rail = V_adc * divider`, the physical resistor ratio |
| `adc_full_scale` | default `3100`; ADC full-scale in mV at 12dB attenuation. This is a sag detector, not a calibrated meter — if a board logs `UNCALIBRATED estimate` at setup it has no eFuse curve and the threshold is not trustworthy without checking against a meter |

### `collection:` (chunk lifecycle + serving)

| key | meaning |
|---|---|
| `enabled` | default `true`; the **card** half — chunk index, retention, the `#gap` marker. `false` is declared-but-down: nothing indexed, retained or served |
| `serve` | default `true`; the **network** half — whether the `esp_http_server` runs at all. `false` is a real config: chunks collected by pulling the card, or a bench soak that must not add WiFi's effect on bus timing to what it's measuring |
| `port` | default `8080`; must not collide with `web_server`'s port (V25) |
| `retention_percent` | default `80`, strictly 1–99; card fill that arms "drop oldest un-collected" |
| `max_chunks` | default `256`, 16–2048; in-RAM index capacity, 12 bytes/entry. Confirmed chunks act as the free list when the index is full: the oldest non-serving `.UPL` is deleted and the new chunk is tracked. Size this for the longest unconfirmed stretch; if the index fills with only `.LOG` chunks, new chunks are still refused rather than forgetting files that remain on the card |

`collection.enabled: true` with `serve: true` (the default) requires a
`wifi:` block (V22) — otherwise the server binds an interface that never
exists and the card just fills.

### Collection server endpoints (Phase B, private `esp_http_server`)

| route | purpose |
|---|---|
| `GET /sdlog/index` | JSON: every SEALED chunk — name, seq, bytes. OPEN and CONFIRMED are never listed |
| `GET /sdlog/f/<name>` | raw bytes, `Range` supported for resume; SEALED only, `409` for the still-open file |
| `POST /sdlog/done/<name>` | the confirm — renames SEALED → CONFIRMED; idempotent |
| `GET /sdlog/status` | counters, card fill, oldest-un-collected seq, discarded totals (not used by `script/sdlog_collect.py` itself — a manual/monitoring endpoint) |

### Action: `sd_logger.log`

| key | meaning |
|---|---|
| `source` | default `0`; a declared `sources:`/`can_ports:` label, or the bare numeric tag |
| `can_id` | default `0`; templatable, 0–0x1FFFFFFF |
| `data` | required; templatable — a list of up to 8 hex bytes, a `!lambda` returning `std::vector<uint8_t>`, or a plain string (encoded as bytes) |

## 6. Safety model

`sd_logger` is a pure sink and **never transmits** on any bus it observes —
there is no TX path in the component at all, so the repo's TX-safety rule is
satisfied by construction rather than by a config gate. Two things are worth
being deliberate about anyway:

- **A failed card must never take the bus stack down with it.** It doesn't:
  a missing/failed card is non-fatal at boot, `recovery:` retries
  indefinitely by default, and every producer path (the action, the native
  taps) degrades to "records get dropped and counted", never to a stall —
  the drop-newest ring policy exists specifically so a slow writer can never
  back-pressure an ISR or the LIN task.
- **The emergency close is best-effort, and says so.** `vcc_monitor` tries
  to close the file cleanly before a sagging rail collapses, but a FAT
  filesystem is not transactionally power-safe and firmware cannot
  manufacture time it doesn't have. The real guarantee is `sync_interval`:
  a power cut at any instant loses at most the last un-synced batch, and the
  next boot mounts and continues. Don't promise more than that in an
  automation built on top of this component.

## 7. Troubleshooting

| symptom | what it means |
|---|---|
| `mounted=NO (logging disabled)` at boot, `recovery` retrying | no card, or a failed mount — non-fatal; `recovery:` keeps trying (§3.3). Check `card_dropped` to see the cost |
| `dropped` climbing | the logger's own RAM ring is full — raise `buffer_depth`, or the producer rate genuinely exceeds the writer's throughput ceiling |
| `tap_dropped` climbing | the `can_gateway` tap ring is full — raise that port's `log_tap_queue_depth`, a `can_gateway` key |
| `text_dropped` climbing | the captured-log ring is full — raise `esphome_logs.buffer_depth`, or the board is logging faster than the writer can drain |
| `card_dropped` climbing | there was no writable file at all for that stretch — the card is gone, not merely slow; look at `mounted` for why |
| `index_refused` climbing (or nonzero) | the in-RAM index filled and had no confirmed, non-serving chunk to reclaim for at least one rotation — raise `collection.max_chunks`, collect more often, or shorten the unconfirmed stretch. Refused chunks are not listed, served or reclaimed until a remount can see them |
| a `.LOG` file has no trailing `#close` | the power-cut signature — expected after pulling the power, harmless if it's the very last line (one torn line is recoverable by design). `check --strict` is what turns this into a bench-gate failure when it shouldn't be there |
| `script/sdlog.py check` reports a malformed line mid-file | a real defect — a formatter bug, or the card returned garbage; not the torn-trailing-line case above |
| `#drop,<t_us>,ring\|tap:<label>\|text,<delta>,<total>` in the file | the in-band version of the counters above — read together with which counter moved to find the bottleneck |
| `#gap,<t_us>,<chunks>,<bytes>,<first_seq>,<last_seq>` in the file | retention discarded never-collected chunks. `first_seq`/`last_seq` are the **endpoints of the window emptied**, not a promise every seq between them is gone — `script/sdlog.py` states "punctured" when some seqs inside the window were actually collected safely |
| "unknown field"-style config error on `can_ports:` | that port needs `log_tap: true` on the `can_gateway` side (V12) — without it nothing is logged and nothing looks wrong |
| `esphome_logs:` compiles but the file has no `X` lines | check for a missing `logger:` block (V15, a config error), `logger.level` quieter than `esphome_logs.level` (V16, a warning — capture can't see what the logger already dropped), or `logger.task_log_buffer_size: 0` (V17, a warning — messages from the writer/VCC-monitor tasks bypass listeners entirely) |
| `collection:` config error about `wifi:` | `serve: true` (the default) needs a `wifi:` block (V22) — set `serve: false` to keep the card-side index/retention/`#gap` without a server |
| `collection.port` config error | collides with `web_server`'s port (V25) — give one of them a different port |
| `UNCALIBRATED estimate` at setup, `vcc_monitor` | the chip has no eFuse ADC curve; the threshold is a rough estimate — verify against a meter before trusting it |
| `/sdlog/status` times out mid-transfer | known open item — the server is single-threaded, so a status poll can't get in while a pull is streaming (`docs/HANDOVER.md` §1.6) |

## 8. Going deeper

- [`sd_logger-spec.md`](sd_logger-spec.md) — full design rationale: the
  ring/writer split, why SD-over-SPI and never internal flash, the write
  format grammar (`#`-prefixed meta lines, the delta-coded stamp column),
  rotation, and the VCC emergency-close layers.
- [`sdlog-collection-design.md`](sdlog-collection-design.md) — the chunk
  lifecycle (OPEN → SEALED → CONFIRMED → GONE), retention policy, and the
  ownership/locking rules behind the collection server.
- [`sdlog-phase-b-wire-contract.md`](sdlog-phase-b-wire-contract.md) — the
  normative HTTP wire contract the firmware and `script/sdlog_collect.py`
  both have to satisfy, if you're writing an alternative puller.
- `script/sdlog.py --help` / `script/sdlog_collect.py --help` — `check` /
  `extract` / `head`, and `index` / `pull`.
- `tests/build/sd_logger/common.yaml` (action-only) + `common_cantap.yaml`
  (native CAN tap) — minimal, fully-annotated worked examples of every
  config key.
- `tests/hil/mr-orange-sdlog.yaml` — a full real-world configuration:
  `sd_logger` layered over a proven board config via `packages:`, on real
  bus load.
