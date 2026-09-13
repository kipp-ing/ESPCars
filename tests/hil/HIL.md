# Hardware-in-the-loop bench — the token ring

Three ESP32-C6 reference boards (PCB-ESP32C6-Adapter-CAN-Modbus-LIN) on a USB
hub, permanently attached to the dev machine. They are the delivery proof:
every release must keep the token ring spinning here, on real transceivers.

## The cast

| Board | MAC | Role | LED |
|---|---|---|---|
| **Mr. Green** | …bd:0e:dd:50 | LIN master + CAN seg 1 — ring owner | green |
| **Mr. Blue** | …0a:d0:f0 | LIN slave + CAN seg 2 — ring relay | blue |
| **Mr. Orange** | …0a:d1:e0 | CAN gateway seg1↔seg2 + LIN listener | orange |

No port paths here on purpose — they renumber on every reset and this table has
already drifted twice. `script/hil/ports.py` maps MAC → port live, and both
`flash.sh` and `verify.py` resolve through it.

If the boards get physically reordered on the bench, **every board↔segment
assumption dies at once** and it looks like a component bug: the transmitting
board pegs TEC at 128 with tens of thousands of `err_events` while its supposed
peer reports a perfectly idle bus (`err_events 0`, `TEC 0`). That signature —
one side screaming, the other side seeing nothing — means "cabled to nothing",
not "gateway broken". Re-read the LED colors and re-cable before debugging code.

## Step 0 — `selftest.py`, before you believe any symptom

```bash
.venv/bin/python script/hil/selftest.py             # flash every C6 board, measure, write the report
.venv/bin/python script/hil/selftest.py --board orange
.venv/bin/python script/hil/selftest.py --no-flash  # just re-read what is running
```

One command that flashes `selftest.yaml` onto every attached C6 board, reads all
the consoles at once, cross-references them, and writes `tests/hil/BENCH-STATE.md`.
Boards are selected by bench name, `--mac`, or `--port`; `--color green=00ff00`
overrides an identity hue.

The gates, in dependency order, each one a precondition for the next:

| gate | what it proves |
|---|---|
| `reach` | the board can be talked to at all — it accepted a flash. Above everything below it: an unreachable board has no measurements, only a recovery (see "Bench hygiene") |
| `rail` | 12 V on J8 (P1) **and** GPIO7 high (P2), read off the GPIO6 sense — which is itself gated by GPIO7, so 0 V is genuinely three-way ambiguous |
| `xcvr.<port>` | the +5 V domain (U18, **JP14**), the TJA1044, and the logic-side jumpers — one TXD→RXD echo per port, no loopback cable needed |
| `bus` | that traffic survives on the wire — the controller's own counters, judged on movement between windows |
| *topology* | who is actually on each wire, from the same `[topo]` beacons the rest of the bench uses |
| `lin` | the LIN transceiver, which runs off 12 V and survives a dead +5 V domain |
| `lin.selftest` | master only — the schedule is answered **on time**, not merely answered. A wire that works but jitters passes `lin` and fails this |
| `sd` | the card answering a bit-banged CMD0, with no component in the way |

**Measured all-PASS on 2026-07-29** (exit 0, every gate on all three boards).
Getting there took fixing three separate things, each of which had been read as a
bench fault: the `reach` gate did not exist so an unreachable board aborted the
run; the LIN self-test's own logs were compiled out by
`CONFIG_LOG_MAXIMUM_LEVEL=1`, which made a working exerciser look dead; and its
pass latch never reached the verdict because ESPHome suppresses the trigger on a
binary sensor's *first* published state.

**The report refuses to give a wiring verdict for any port whose `xcvr` gate did
not pass.** That is the whole reason it exists. An unpowered CAN transceiver
reproduces *every* symptom of broken wiring — no echo, no ACK, TEC to bus-off,
nothing received — so at the level of a bus trace "the cable is cut" and "nobody
raised GPIO7" are the same picture. Three sessions have been spent on that
confusion. The tool names the first broken link and stops rather than describing
the wreckage downstream of it.

⚠ Not against a live stress rig: the echo gate pulls each segment dominant for
~200 us at boot, which is ~100 bit times at 500 kbit/s — enough to walk a
saturated partner to bus-off. It is safe during a self-test only because every
board is running the same quiet firmware.

## The topology answers itself — include it in every bench config

Do not derive the wiring from symptoms; the boards report it. Every node beacons
"I am *node*, this is my bus *n*" at 1 Hz on each of its CAN ports, and every
node permanently subscribes to all beacons on all of its ports, so each console
prints, every 10 s:

```
[topo] seg1 <- green.bus0 (912 ms)
[topo] seg2 <- purple.bus0 (1004 ms)
```

That is the measured topology, by name. It also names two faults outright:
`<- nothing` (alone on the wire, or every partner unpowered / in the ROM stub)
and `SAME-WIRE` (two of this board's own ports on one wire — a forwarding loop
if routes cross them). A node cannot receive its own transmissions, so hearing
its own node's beacon means nothing else.

Two packages per CAN port, same two vars — RX (receive + report) and TX (the
beacon); include TX only on ports that may transmit, because a `cyclic_send` on
a listen-only port is rejected at config time:

```yaml
packages:
  topo_rx_seg1: !include { file: topology_rx.yaml, vars: { port_id: seg1, beacon_id: "0x0F4" } }
  topo_tx_seg1: !include { file: topology_tx.yaml, vars: { port_id: seg1, beacon_id: "0x0F4" } }
```

`beacon_id` = `0x0C0 + node*2 + bus`: green 0x0C0/0x0C1, blue 0x0C2/0x0C3,
orange 0x0C4/0x0C5, purple 0x0C6/0x0C7. **Moved from base 0x0F0 on 2026-07-30**
because a real device attached to the bench for testing transmits in the same
range at 50 Hz, on what was Blue's bus0 slot on Blue's own segment — two
senders on one id, and `verify.py` could not run at all while it was attached.
The whole block moved rather than one board, so
`base + node*2 + bus` still holds and an id still decodes by eye. The full slot
map and the design constraints (why one id per (node,bus), why this range) are in
`tests/hil/topology_rx.yaml`. **Bench only** — never in `tests/build/`, an
example, or a car config: it transmits unconditionally on high-priority ids.

### Read it off every board at once

```bash
.venv/bin/python script/hil/topology.py
.venv/bin/python script/hil/topology.py --expect green.bus0=orange.bus0 \
                                        --expect orange.bus1=purple.bus0
```

The value is the **cross-reference**, which is the part reading three consoles
by hand gets wrong: one board's line answers "who do I hear?" and never "does
anyone hear me?", and those two fail separately. Hears-nobody *and*
heard-by-nobody means the port is electrically absent (power, transceiver, bit
rate) — a board question, not a cabling one. Heard-by-nobody alone means its TX
side is dead. The connected components it prints are the measured segments.
Prefer it over `esphome logs` for any "which board is on which wire" question,
and use `--expect` as the gate before a run.

`topology.py` reads the 10 s slot beacons from `topology_rx.yaml`;
`topo-watch.py` reads the live MAC catcher (`topo-live*.yaml`). Same question,
different beacon — the wrong pairing sees nothing.

## Bench hygiene — consoles, resets, and boards that look dead

**Opening a console does not reset the C6 — opening it *carelessly* does.** On
the USB-Serial-JTAG, DTR and RTS are the boot-mode controls, and macOS asserts
both when a port is opened; that is what parks the chip in the ROM download stub
(`rst:0x15 (USB_UART_HPSYS)`), which reads on the bench as a dead board or a
silent bus. Every tool here clears them **before** `open()` — `ports.py`,
`topology.py`, `verify.py`, `selftest.py` all do, and so must anything new.
Attached that way the board runs straight through: on 2026-07-29 a DUT held its
lifetime counters across repeated captures (`err_events` static, TEC still
decaying), which a reset would have zeroed. `esphome logs` is safe too;
`esphome run` / esptool genuinely does reset. So reading a console mid-run is
fine — but a reset while the SD card is being written still wedges the card, so
use tools that clear the lines.

**A board that enumerates is not a board that runs, and there are two ways to be
dead.** Tell them apart before spending time on either; they cost half a session
on 2026-07-29.

| symptom | state | recovery |
|---|---|---|
| esptool connects, no firmware runs | ROM download stub | software: `python -m esptool --port <p> --after hard-reset chip-id` |
| esptool cannot connect on **any** reset mode (`default-reset`, `usb-reset`, `no-reset`) and the console yields zero bytes | not the stub — esptool talks to mask ROM, so this is neither firmware nor a half-written flash | **physical power cycle only** — 12 V *and* re-plug the boards |

All three C6s were in the second state at once while `ports.py` listed the whole
bench as present — it reads the MAC off the USB descriptor, so a full, correct
listing proves only that the USB serial peripheral enumerates and says nothing
about whether firmware runs. Check a non-C6 board first (Purple, on the same
hub): one live board rules out the hub and localises the fault to the C6
adapters. They all came back with every gate reproducing its pre-failure
reading.

`selftest.py` reports this as the **`reach`** gate, which sits above `rail`: a
board that will not take a flash gets a row of its own naming which of the two
states it is in and the matching recovery, the other boards are still measured,
and the report is still written. Until 2026-07-29 a single failed flash aborted
the whole sweep and printed an esphome traceback instead — so the one bench-wide
failure the tool exists to make legible was the one it could not describe.

Four more traps, each of which has cost a session:

- **Never point `topology.py` or `topo-watch.py` at a board mid-run.** They open
  every console on the bench — one reset per board — so a tool meant to *observe*
  the topology will end the run it was called to describe.
- **A reset while the card is being written still wedges it**, however carefully
  the console was opened. It cuts an SDSPI multi-block write and the card sits in
  Receive-Data waiting for a stop token that never comes. `sd_logger`'s in-band
  recovery (`components/sd_logger/card_reset.h`) clears exactly this and is
  proven 12/12 on hardware — a stop token, not patience, and no 12 V cycle.
- **`retention_percent` is destructive on a timer, not on rotation.**
  `retention_pass_()` runs from `RETENTION_POLL_MS` (`sd_logger.cpp`) whether or
  not the rig is writing, so a low threshold on a card holding data you want is
  how you lose it. `readback-orange.yaml` uses 98 for that reason and says so.
- **Topology packages belong on any rig that declares a CAN port** — both
  `topology_rx` and `topology_tx`, once per port. `readback-orange.yaml` is
  exempt only because it declares no CAN port at all; do not copy that exemption
  into a rig that does.

Wiring: one shared 12 V LIN bus (J9.3 daisy-chained; Green carries the master
pull-up). Two separate terminated 125 kbps CAN segments: Green-CAN1 ↔
Orange-CAN2 (jumpers JP22/JP20/JP8/JP10 closed on Orange only), Blue-CAN1 ↔
Orange-CAN1. **12 V on J8 is mandatory** — USB power alone leaves every
transceiver dead and looks exactly like broken wiring.

> **The ring has been ported to the current cabling and `verify.py` runs again
> (2026-07-28, ALL GREEN, 875 laps at 19.4/s).** The table above is the ring's
> *historical* pin assignment and is no longer how Orange is configured.
>
> Measured off Orange's own beacons — `[topo] seg1 <- green.bus0`,
> `[topo] seg2 <- blue.bus0` — Green sits on GPIO2/3 and Blue on GPIO10/11,
> the opposite of the historical wiring, and Purple is on neither wire (it
> beacons, so its absence from those lines is evidence, not silence). So
> `mr-orange.yaml` now maps `green_side` to GPIO2/3 and `blue_side` to
> GPIO10/11.
>
> The failure that swap fixed is worth recognising, because it looks like a
> gateway bug and is not: forwarding does not care about the labels (both routes
> are unfiltered, so the token crossed either way and Green closed laps at the
> full baseline), but the `GW token` **sensor** is bound to a port *by name*.
> Pointed at the wrong wire it never saw 0x3B0, so Orange reported
> `ORANGE degraded: token=0 lin=1` and never logged `RING gw` — a red gate on a
> ring that was spinning perfectly.

Each WS2812 (GPIO8) glows its board's own color and **never** anything else —
hue is identity, brightness and motion are state:

| LED | Meaning |
|---|---|
| identity color, ~10 Hz shimmer between 20% and 35% | healthy, token traffic flowing |
| identity color, steady 20% | healthy, idle |
| identity color, one 35% blip per second | fault |
| dark | not running its firmware — check for ROM download mode |

The ceiling is **under 40% at all times** (a project rule; these boards sit at
eye level). The shimmer peak was 40% and the fault blip 100% until 2026-07-29 —
both are 35% now. What makes a state readable is contrast against the 20% idle
level, not absolute output.

So "which board is this?" is answerable in every state, including a dead bench.
Neither earlier scheme managed that: the white token flash out-ran its own 60 ms
decay at 19 laps/s and left all three boards solid white, and painting faults
red made a stopped bench three identical red boards. The LED is on the plug-in
DevKitC-1 and runs off USB 3V3, so it works with no 12 V on J8.

## The ring ("der tote Ring lebt")

The token is **level-based state, not an edge-triggered frame** — a lost frame
retransmits next cycle, so only a genuinely dead component stalls the ring:

1. Green's LIN master response (ID 0x30, polled every 50 ms) carries lap N.
2. Blue (slave) hears 0x30, stages N into its own response (0x31) and re-emits
   it on CAN 0x3B0 — every cycle.
3. Orange forwards 0x3B0 from Blue's segment to Green's (route `to_green`).
4. Green sees N return on 0x3B0 → closes lap N: sends ack 0x3A0 (crosses the
   gateway the *other* way to Blue), advances its LIN response to N+1.

Every lap therefore exercises: LIN master TX, slave RX, `linbus.update_response`,
listener sniff, `can_gateway.send`, decode-sensor dispatch, and **both gateway
directions** — while high-priority cyclic floods (0x15x/0x16x @ 10/25 ms from
Green and Blue, ~40% bus load per segment once forwarded) try to starve the
lower-priority token. Lap rate ≈ LIN schedule bound (~10–20/s).

## Running it

```bash
script/hil/flash.sh                    # flash all three (or: flash.sh blue)
.venv/bin/python script/hil/verify.py --seconds 30 --min-laps 50 --outdir /tmp/hil
```

`verify.py` captures all three consoles in parallel and asserts: Green's lap
counter progressed ≥ min-laps with no `TOKEN LOST`, Blue relayed and saw acks,
Orange gatewayed tokens and sniffed LIN, and nobody hit bus-off. Exit 0 = the
bench is green — wire it into any release ritual.

## Baseline (2026-07-20, esphome 2026.7.0, commit 634b164)

875 laps / 45 s = **19.4 laps/s** (the LIN 50 ms schedule bound), zero LIN
checksum errors, error rate 0.00%, zero CAN bus errors, TEC/REC 0 on all six
controllers, gateway forwarding exactly symmetric (rx == tx both ports,
~23% bus load per segment). Any regression below `--min-laps` or any error
counter moving is a real finding — this bench has no flaky history to blame.

## SD logger bench (M1 HW test — card proven 2026-07-26, ring gate outstanding)

`tests/hil/mr-orange-sdlog.yaml` layers the `sd_logger` component on top of the
proven Orange config (package include — the ring baseline is untouched) and
feeds it real, load-correlated traffic via the generic `sd_logger.log` action
(the token and LIN-sniff counters, every 50 ms). It is the end-to-end proof that
a card logs under 60 %-segment CAN load + LIN without disturbing the ring.

**M0 is closed (2026-07-26).** The card is wired on the real pins — SCK GPIO21,
MOSI GPIO22, MISO GPIO20, CS GPIO15, VCC sense GPIO6 — traced from the PCB repo
netlist and recorded in `docs/bench-hmi-wiring.md`. RS-485 cannot clash: U20 is
removed from the board. A missing card stays non-fatal (mount fails gracefully,
logger reports `mounted=NO`).

**Card power (GPIO17) is not in that list any more, and that correction matters
(2026-07-29):** the signal reaches header J5.6 through R32 and stops there — the
H5 high-side switch on card VCC was specified but never built. Every config that
declared `card_power_pin: GPIO17` therefore had `power_cycle` on (V19) while
toggling nothing, which is why the recovery ladder's seven attempts on
2026-07-28 all failed identically. The pin is gone from the bench configs.

**Result of the first run:** the card enumerated over SPI at 10 MHz on the first
attempt (`sdmmc_card_init` OK), which confirms H1/H2/H3/H5 on silicon. The card
was factory-blank — `f_mount` returned FRESULT 13 (`FR_NO_FILESYSTEM`) — and was
formatted once on-device via `format_if_mount_failed`. It then mounted on every
boot and logged with `records` climbing at the full 40/s the interval generates
and **`dropped=0`**, under ~10 % LIN bus load and 12.4 % on CAN1.

Two things to know before repeating it:

- **Do not reset the board while `f_mkfs` is running.** Doing so wedges the card:
  it stops enumerating entirely (`sdmmc_card_init failed (0x107)`, i.e.
  `ESP_ERR_TIMEOUT`) and does *not* recover across soft resets or after minutes
  of idle — only a full 12 V power cycle brought it back. Note esptool's own
  hard-reset starts the format immediately after upload, so attach to the console
  **without** resetting.
  The same is true of a reset during any write, and the firmware now has a
  reply to it: from the second recovery attempt on it sends the Stop Tran token
  that ends the interrupted multi-block write, waits out busy on a budget of its
  own, then CMD0 — none of which needs the card's power.
  `tests/hil/probe-orange-wedge.yaml` is the run that tests it: it streams at the
  card, cuts the board mid-write on purpose, and prints what the card says on the
  way back. Its `card reset:` line carries the first measurement anyone has taken
  of a wedged card, and its header explains how to read each outcome.

  **Measured on Mr. Orange, 2026-07-29 — the in-band reset works, and the 12 V
  cycle above is no longer the only way back.** 12 wedge/recover cycles, all
  cleared without touching J8:

  | | |
  |---|---|
  | control (`-s in_band false`) | 6 plain remounts, all `ESP_ERR_TIMEOUT`, card stayed down |
  | with the reset armed | **12/12 recovered** — 10 on attempt 2, 2 on attempt 3 |
  | typical line | `card reset: idle at cmd13 (busy 0 ms, then 0 ms; cmd0 x1 r1=0x01 status=0x00)` |
  | real rig, cut by a reflash mid-write at 6400 rec/s | back up **3.1 s** after boot |
  | FAT damage over all cycles | none — zero `ESP_FAIL`, every mount enumerated |

  **Busy was 0 ms on every single cycle.** The card was never programming, so
  patience was never the fix — the stop token was, which is the one thing no
  layer below `card_reset.h` sends. Budget-raising (`busy_timeout`) is
  belt-and-braces here, not the mechanism.

  So a reset while the card is being written no longer costs a 12 V cycle: the
  board talks it back on its own within a few seconds. The old advice survives
  only for `f_mkfs` — see the caveat above, which this run did not retest.
- **IDF errors are the only ones you see by default.** esphome compiles with
  `CONFIG_LOG_MAXIMUM_LEVEL=1`, so the fatfs `W` line carrying the FRESULT is
  absent and a mount failure looks like a bare `ESP_FAIL`. Add
  `esp32: framework: log_level: INFO` to a throwaway variant to see it — raw
  `sdkconfig_options` will *not* work, because `esp32/__init__.py` sets the
  Kconfig choice symbol `CONFIG_LOG_DEFAULT_LEVEL_<LEVEL>` from that key and it
  wins.

**Known-bad as shipped: `vcc_monitor` trips on a healthy rail.** See the
VCC-calibration finding in `docs/HANDOVER.md`. The YAML carries a
board-specific `divider: 14.67` stopgap (the physical ratio is 10.8673) to keep
the emergency close meaningful on this chip until the component uses `adc_cali`.

```bash
# 1. Flash the logger variant onto Orange (Green/Blue stay on their normal configs)
.venv/bin/esphome run --no-logs tests/hil/mr-orange-sdlog.yaml \
  --device "$(.venv/bin/python script/hil/ports.py orange)"
script/hil/flash.sh green blue

# 2. Ring must stay green with the logger running (release gate unchanged)
#    Runnable again as of 2026-07-29: Blue is back and the gate measures the
#    original 19.4 laps/s baseline. (It was marked NOT RUNNABLE 2026-07-26.)
.venv/bin/python script/hil/verify.py --seconds 45 --min-laps 75

# 3. Logger health: on Orange's console the 5 s stats line must show the card
#    mounted, records climbing, and ZERO drops under load:
#      records=NNNN dropped=0 bytes=NNNN file=L0000000 mounted=1
```

`dropped=0` is the logger's own pass criterion (the ring absorbed every record
under load); any non-zero `dropped` means the writer/SD path fell behind and the
ring sizing or `sync_interval` needs revisiting. (verify.py is intentionally left
as-is — assert the stats line manually for now; folding a `dropped=0` check into
the gate is a later step, once the native taps land.)

### Power-loss procedure (the close-file emergency, §7 — manual/HIL)

With a card wired and `vcc_monitor` pointed at the real 12 V-rail divider:

1. Run the logger bench (above) so records are actively being written.
2. **Cut 12 V on J8** (pull the bench supply — not USB; USB alone already looks
   like broken wiring). The `vcc_monitor` task should trip on the sag and the
   log shows `VCC sag: emergency close (...)` before the rail collapses — this
   only completes cleanly if the board carries **hold-up energy** on 3V3 (spec
   H6); without it, expect Layer A behaviour only.
3. Power back up, pull the card, and verify on a host: every `L*.CSV` parses as
   valid CSV up to the last line, and re-mounting on the next boot succeeds
   (Orange logs `logging to /sdcard/L…CSV` with the next sequence number, no
   `format_if_mount_failed` recovery). Worst acceptable outcome (Layer A): the
   last ≤ `sync_interval` of records is lost, nothing else.

### M6: rotation, retention and `#gap` (the run Phase B is waiting on)

Rotation has **never fired on hardware** and retention has never deleted
anything, so the `#gap` line — the only in-band record that a chunk was thrown
away — has never been written by a real board. The collection server (Phase B) is
deliberately held back until this run exists. Three configs, smallest first:

```bash
# 1. The time bound, on a quiet bus. ~40 rec/s, so 4 MB is 43 minutes away and
#    max_file_seconds is the bound that fires: a fresh file every 60 s.
.venv/bin/esphome run tests/hil/mr-orange-sdlog.yaml \
  --device "$(.venv/bin/python script/hil/ports.py orange)"

# 2. Rotation and retention under load, isolated from CAN. DELETES CHUNKS from
#    the card by default — read that file's header before flashing it.
.venv/bin/esphome run tests/hil/stress-orange-sdrotate.yaml \
  --device "$(.venv/bin/python script/hil/ports.py orange)"

# 3. Production shape: gateway + LIN + both taps, rotating every ~28 s.
.venv/bin/esphome run tests/hil/stress-orange-gateway.yaml \
  --device "$(.venv/bin/python script/hil/ports.py orange)"
```

Pass criteria, in order of what they would catch:

- `#rotate` → `#close,…,rotate` → a fresh padded header in the successor, and
  `file=L…` in the statistics line advancing by **exactly one** per rotation. A
  skipped number is a post-rotation reopen that failed; that used to leave
  `fd_ = -1` with `mounted_` still true and discard every later record without
  moving a counter.
- `dropped=0` and `tap_dropped=0` right through the rotations. A close, an fsync
  and an open every 12 s is the cost being measured, and if it does not fit, it
  shows up as ring drops.
- On the sdrotate rig: `retention: card N% full — discarded L…` on the console,
  the `retention: T never-collected chunk(s) discarded` total in the statistics
  line, and `#gap` lines in the files. **The total and the `#gap` lines must add
  up** — they are computed on either side of the writer, and `script/sdlog.py
  check -v` is what reconciles them. `#gap`'s two seq fields are a window, not a
  list of missing files (design §7a).
- `script/sdlog.py check --strict` clean over every sealed file afterwards, with
  `#close` **present** on all but the last: these files ended by rotation, not by
  a power cut, so a missing `#close` here is the opposite of the M5 expectation.

The card fill is printed once at mount (`card N% full at mount (retention arms at
M%)`) and nowhere else — the first `f_getfree` walks the whole FAT, which is
seconds on a 32 GB card and would stall the writer mid-run. That line is what
says whether retention will arm at all during the run.

## M2 stress bench (500 kbit/s, 90 % load, CPU > 80 %)

Three configs, each usable on its own. All three take substitutions so a sweep
needs no edits. **Always read the achieved number** — `bus load` from the port
statistics line, `records=`/`bytes=` from the logger's — never the configured
one.

```bash
# Load generator: fills the TX queue every main-loop pass. `burst` self-limits
# (it stops at the first refusal), so 58 and 64 behave the same; lower it, or
# raise `burst_interval`, to land on a target load.
.venv/bin/esphome -s burst 58 run tests/hil/stress-green-load.yaml \
  --device "$(.venv/bin/python script/hil/ports.py green)"

# Second node on the segment: ACKs, and reports the load it measures itself.
.venv/bin/esphome run tests/hil/stress-purple-witness.yaml \
  --device /dev/cu.usbserial-0001

# SD writer ceiling, isolated from CAN. Sweep `records` up until `dropped`
# climbs; the last rate with dropped == 0 is the ceiling.
.venv/bin/esphome -s records 12 run tests/hil/stress-orange-sdrate.yaml \
  --device "$(.venv/bin/python script/hil/ports.py orange)"

# The DUT: gateway + LIN + logging together, with the native taps.
.venv/bin/esphome run tests/hil/stress-orange-gateway.yaml \
  --device "$(.venv/bin/python script/hil/ports.py orange)"
```

`tests/hil/cpu_stats.yaml` is an includable package (already in the three
configs above) that reports CPU from the FreeRTOS idle-task share. After any
build that uses it, confirm the options actually took — a raw sdkconfig option
colliding with a Kconfig *choice* is dropped silently:

```bash
grep -E "RUN_TIME_STATS|TRACE_FACILITY" \
  tests/hil/.esphome/build/<name>/sdkconfig.<name>      # both must be =y
```

Measured 2026-07-26 (numbers and the reasoning in M2 stress run §9, git
history): 91.7 % bus load at 500 kbit/s with zero errors; SD writer sustained
~5090 rec/s / ~188 KB/s with `dropped == 0` for 12 minutes across five 32 MB
files at CPU ~85 %.

**Stress topology, settled 2026-07-27** — not the token-ring wiring described
above, which used Blue. Green and Purple hang off *different* Orange ports, so
the DUT is the only path between them:

| Segment | Nodes |
|---|---|
| **Seg1** | Green CAN1 (GPIO2/3) — Orange `seg1` (GPIO2/3) |
| **Seg2** | Orange `seg2` (GPIO10/11) — Blue CAN1 (GPIO2/3) |

**Re-measured 2026-07-28 from Orange's beacons: seg2 is Blue, not Purple.**
Blue is back in the rig and Purple is on neither wire — Purple runs
`topology_tx.yaml`, so it would be heard if it were attached. The row above said
Purple and "Blue is off both wires" until this was checked; re-read the `[topo]`
lines of any run before trusting it again.

Flash the partners before the DUT, and gate on
`probe-orange-links.yaml` reporting both ports `LINKED, clean` — the full
procedure, the expected rx figures and the same-wire check are in
`tests/hil/HIL.md`.

## Known issues found by this bench

- [#1](https://github.com/kipp-ing/ESPCars/issues/1) `can_gateway`: healthy
  port delivers no RX while the sibling port cycles bus-off recovery
  (found in ring round 2; the CAN1-only topology sidesteps it).
- [#2](https://github.com/kipp-ing/ESPCars/issues/2) `can_gateway`:
  `cyclic_sends` intervals floored by the main-loop period; high aggregate load
  needs the many-IDs pattern. **The "~16 ms" in that issue does not hold on the
  current builds** — Green measured ~1000 main-loop passes/s while transmitting
  4100 f/s (2026-07-26), so the floor is nearer 1 ms. Re-measure before reusing
  the issue's 62 f/s-per-entry arithmetic.
- Sizing bug found while building the M2 load generator, now fixed:
  `CAN_GATEWAY_INJECT_SLOTS` ignored `tx_queue_depth`, so any burst of
  `inject()` calls was silently capped at four frames whatever the queue depth
  said. See M2 stress run §9.2 (2026-07-26, git history)

Handovers with repro configs and fix entry points: `docs/HANDOVER.md`.
