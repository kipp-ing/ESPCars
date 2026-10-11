# sd_logger — a wall-clock basis in every file

2026-10-11, **draft for review (rev 2: deep sleep on KL30)**. Extends
`docs/sd_logger-spec.md` §6 (F1d meta lines) and §8 (schema). Every requirement
has an ID and the test that proves it. **A requirement without a test is not
done.**

## 1. Problem

A log is only useful later if you can say *when* it was written. Today a file
gets its UTC basis only from a `time_id:` clock that has synced during the
current boot (`#utc` line, `set_utc_anchor()`). The current state:

| Situation | Today | Consequence |
|---|---|---|
| a `time_id:` clock synced this boot | `#utc` in the header, or mid-file at the sync | fine, but resolved to 1 s only (`utcnow().timestamp * 1e6`) |
| no sync yet (car, no WiFi; SNTP not reached yet) | no `#utc` at all | the whole file has boot-relative µs only, so it **cannot be placed on a calendar** |
| wake from deep sleep, or a warm reset after a sync | no `#utc` until the next sync | the system clock kept running, but the logger does not use it |
| several clocks (SNTP at home, RTC chip in the car) | `time_id:` takes only one | resyncs from the other clock never reach the file |

None of the bench configs sets `time_id:`. So in practice **no file on any card
carries a UTC basis today.**

The ask (Jan, 2026-10-11): UTC belongs at the start of every file. Use a real
clock when one is there. If none is there (cold boot), take the previous run's
last UTC as the start, plus the time since this boot.

**Operating mode (Jan, 2026-10-11):** in the car the device sits on **KL30
(permanent battery) and runs its own deep sleep**. It has to, or it could never
wake the car. So the usual start is a **wake from deep sleep, not a cold boot**.
A real cold boot happens only when the battery is disconnected, on a brownout
(cranking), or on a fresh install.

## 2. The answer, in one rule

> **Every file opens with a `#clock` line.** It states the best wall-clock
> knowledge the device has, and says how good that knowledge is: a **reading**
> (`utc`), a **reading that has drifted** (`drift`), or a **lower bound**
> (`est`). The line is restated as the file grows and right before the file is
> closed. That way the tail of any file says when that file ended, and the next
> boot reads that tail when it has nothing better.

A lower bound is the only honest estimate after a cold boot. No one knows how
long the power was off. The previous run's last flushed line was written before
this boot powered up, so `prev_end + uptime` is guaranteed not to be later than
the true time. Any fixed "+x" on top of that could be a lie (think of a 50 ms
power blip). So **the "+x" is the measured uptime of this boot, not a guessed
constant.** The real offset becomes known once a real clock syncs, and the
reader reports it then.

## 3. Sources, best first

| Tier | Source | Basis | When it applies |
|---|---|---|---|
| T1 `sync` | a `time_id:` clock synced in this boot (SNTP, Home Assistant, an RTC chip, GPS — any ESPHome `time:` platform) | `utc` | from the sync on; the header if the sync came first |
| T2a `warm` | the system clock kept running through a reset that was not a deep-sleep wake and not a power-on (SW restart/OTA, panic, watchdog) | `utc` | at boot, when the clock is plausible (C1) and no sync has come yet |
| T2b `sleep` | the system clock kept running through **deep sleep** (`esp_reset_reason() == ESP_RST_DEEPSLEEP`) | `drift` | same, after a wake. **The normal case in the car** |
| T3 `card` | the newest file on the card: the last `#clock` in its tail, projected to that file's last timed line | `est` | cold boot, no T1/T2 |
| T4 `build` | `App.get_build_time()` — firmware cannot run before it was built | `est` | fresh card and no clock; also the floor under T3 |

### Why T2 splits in two (research, 2026-10-11)

ESP-IDF keeps the system time in the RTC timer. It "allows time keeping in
various sleep modes, and can also persist time keeping across any resets … with
the exception of power-on resets which reset the RTC timer". Its frequency
deviation "affects the accuracy only in sleep modes"
([ESP-IDF System Time, ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/system_time.html)).
While the chip is awake, the timer is corrected against the crystal (< ±10 ppm).
In deep sleep it counts on the slow clock.

- **Our board's slow clock is the internal ~136 kHz RC** (the default,
  [clk_tree](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/peripherals/clk_tree.html)).
  Espressif gives no ppm figure for it, only that it depends on temperature, and
  ESP-IDF calls it suitable when "time drifting in the range of several seconds
  per day is acceptable". One user's measurement on a C6 is worse: ~25 s after
  10 h of deep sleep on the RC, against 0 s after 14 h on a 32 kHz crystal;
  Espressif staff answer that only an external 32 kHz crystal fixes it
  ([esp32.com thread](https://esp32.com/viewtopic.php?p=127974); anecdotal,
  single runs).
- **A car parked for two weeks can therefore wake minutes off, in either
  direction.** That is not a lower bound, so it cannot be `est`. It is also not
  the ±1 s of a `warm` reset, so it must not be `utc`. Hence the third basis,
  `drift`, which carries the slept duration so a reader can size the error.
- **The internal 32 kHz RC is no alternative.** The C6 datasheet removed it in
  v1.3 under advisory AR2024-011
  ([ESP32-C6 datasheet v1.5](https://documentation.espressif.com/esp32-c6_datasheet_en.pdf),
  revision history).
- **A 32 kHz crystal needs GPIO0/GPIO1** (XTAL_32K_P/N, datasheet pin table). On
  this PCB those are the FUSB302 INT and DISP_DC (`docs/bench-hmi-wiring.md`),
  so a crystal is a PCB change. It costs about 1 µA more in deep sleep (ESP-IDF
  System Time).
- **Not every reset is equal.** The datasheet says a *System Reset* "resets the
  whole digital system, including the LP system", while ESP-IDF says the time
  survives "any reset" except power-on. Which reset reasons really keep the time
  (RTC watchdog, brownout, an EN-pin reset from esptool) is **measured on the
  bench (H7), not assumed**. C1 makes the firmware safe either way: a clock that
  did not survive reads as 1970 + uptime, fails the plausibility check and falls
  through to T3.

T3 and T4 are combined as `max()`. The `src` field names whichever one won.

## 4. Format additions (additive — `fmtver` stays 2)

```
#clock,@<t_hex>,<utc_us_hex>,<basis>,<src>[,<detail>]
```

- `t_hex` is an anchor-marked absolute boot-µs value, like every other timed meta
  line. It stays outside the delta chain (F1h).
- `utc_us_hex` is the UTC in µs at boot time `t`.
- `basis`:
  - `utc` — a reading; error is that of the source (`sync`, `warm`).
  - `drift` — a reading that ran unsupervised; the error grows with `detail`
    and can have either sign (`sleep`).
  - `est` — a lower bound (`card`, `build`).
- `src` ∈ `sync` | `warm` | `sleep` | `card` | `build`.
- `detail` (decimal): for `card` the seq of the file the floor came from; for
  `sleep` the seconds slept by the device's own clock (wake time minus the card's
  last `#clock`, when there is one); absent otherwise.
- **Reading rule:** a `#clock` line is an anchor pair, like `#utc`:
  `record_utc = utc_us + (record_boot_us − t)`. It inherits the line's basis. A
  later line with a better basis overrides worse ones (`utc` > `drift` > `est`).
  A later line with the same basis overrides an earlier one.

`#utc` is **unchanged** (`#utc,<boot_hex>,<utc_hex>`). It is written for
`sync` and `warm` only, **never for `drift` or `est`**. `script/sdlog.py` rejects
`#utc` with any other field count, and an existing consumer must never take an
estimate or a minutes-off sleep clock for a reading. Both reasons rule out
reusing `#utc`. Older readers skip unknown `#` lines by design, so they ignore
`#clock`. That is the safe default.

**When a `#clock` line is written:**

1. In the header of every file, right after `#origin`/`#utc` (C2).
2. **Restated** when 32 KiB of file output or 10 s have passed since the last one,
   whichever comes first (`SD_LOG_CLOCK_RESTATE_BYTES`, `SD_LOG_CLOCK_RESTATE_US`).
   This bounds how far back the boot reader has to look (C5) and how stale a
   card floor can be after a power cut (≤ 10 s plus one `sync_interval`).
   Cost: at production load (171 KB/s) one ~45 B line per 32 KiB, so 0.14 %.
3. Right after a `#utc` line, when a sync changes the basis mid-file.
4. Right before `#rotate` and `#close`. That covers the deep-sleep path:
   ESPHome's `deep_sleep` runs the shutdown hooks before `esp_deep_sleep_start()`,
   and `SdLogger::on_shutdown()` closes with `#close,clean`. So the last line
   before sleep states the exact time the device went to sleep, and the wake
   can compute the slept duration from it.

## 5. Requirements

| ID | Requirement | Test |
|---|---|---|
| C1 | **T2 plausibility.** The system clock counts as `warm`/`sleep` only if `now ≥ build_time` **and** `now ≥ card_floor − 2 s`. Otherwise it is rejected with a `W` log line, and T3/T4 apply. The reset reason picks `sleep` (deep-sleep wake) or `warm` (everything else). | host: `clock_basis` cases *warm / sleep / before build / before card floor / 1970 after power-on* |
| C2 | Every file opened while mounted starts with exactly one `#clock` header line. `basis`/`src` follow the tier table in §3. | host: `format_clock` golden lines; HIL H1–H4 |
| C3 | The card floor is `max(build, prev_end)` at boot µs 0, so at file-open time `t` it is `floor + t`. This is the "+x", and nothing else is added. | host: floor arithmetic, incl. saturation |
| C4 | `prev_end` = the newest valid `#clock` in the tail of the newest file, projected to the latest `@`-timed meta line after it (`#close`, `#rotate`, `#drop`, a later `#clock`). A torn last line (no `\n`) and any line that fails to parse are ignored, never guessed. | host: tail parser — clean close, power-cut tail, torn line, garbage, no `#clock` (old v2 file), `est` chained from `est` |
| C5 | The boot read is bounded: up to 3 newest files (`.LOG`/`.UPL`), the last 64 KiB of each, read only once per boot and before the first file opens. It never fails logging. A read error means "no card floor" plus one `W` line. | host: bounds; HIL H5: `dropped=0` across boot under load |
| C6 | **No estimate ever reaches the system clock.** The logger never calls `settimeofday()`. Otherwise T2 would launder a T3 guess into a "real" time after the next reset or wake. | grep test in `tests/sd_logger` |
| C7 | A sync sets the anchor from `gettimeofday()` (µs), not `timestamp * 1e6`. Any second-resolution error then comes from the clock source, not from the logger. | compile; HIL H2 |
| C8 | A sync that lands **before** the current floor by more than 2 s is still accepted, because a real clock outranks an estimate. It is logged `W` and counted (`clock_back=` in the stats line). `sdlog.py check` reports it. | host + reader test |
| C9 | `time_id:` accepts one id **or a list** (e.g. an RTC chip plus SNTP). Each clock's `on_time_sync` updates the anchor. A single id keeps working unchanged. | schema test |
| C10 | `sdlog.py`: `check` and `head` print the file's time basis (`utc sync/warm`, `drift sleep <s>`, `est card:<seq>`/`build`, or none). When a `utc` line follows a `drift` or `est` line, they print the measured correction. For `drift` they also print it as a rate (s per day slept). `extract` keeps the `utc_us` column **from `#utc` only**. A new opt-in `--estimates` adds `utc_best_us,utc_basis`. | `tests/sd_logger/test_sdlog_reader.py` |
| C11 | Spec, user guide and the F1d table in `sd_logger-spec.md` describe `#clock`, and the recipe in §6. | review |

## 6. "It needs a clock": hardware and config

The firmware side is source-agnostic. Any ESPHome `time:` platform works through
`time_id:`. What the car actually has:

- **At home (WiFi):** `sntp`. Collection already needs WiFi, so every visit home
  corrects the clock and, through C10, measures how far the sleep clock and the
  estimates were off.
- **In the car, today's PCB:** T2b across deep sleep (minutes of drift over a
  long park, see §3) and T3/T4 across a battery disconnect. Every value is
  honestly labelled, but none is a calendar-grade time.
- **Next PCB revision, two independent asks:**
  1. **A 32 kHz crystal on XTAL_32K_P/N (GPIO0/1)**, plus
     `CONFIG_RTC_CLK_SRC_EXT_CRYS`. This turns T2b from minutes into
     seconds-per-day class, which is what fixes the normal case in the car.
     It does not help a cold boot.
  2. **A battery- or supercap-backed I²C RTC chip** for cold boots (battery
     disconnected). ESPHome:
     [`pcf8563`](https://esphome.io/components/time/pcf8563.html),
     [`pcf85063`](https://esphome.io/components/time/pcf85063/),
     [`ds1307`](https://esphome.io/components/time/ds1307/),
     [`rx8130`](https://esphome.io/components/time/rx8130/). These docs show the
     usual pattern: `read_time` on boot, `write_time` on each SNTP sync.

  The bench boards have no free pins for either (`docs/bench-hmi-wiring.md`).
  Neither is part of this firmware change.
- **Vehicle bus time:** many vehicles broadcast date/time on CAN. That is
  vehicle-specific and belongs in the consuming project. It can feed the logger
  through a lambda that calls `set_utc_anchor()`, or through a custom `time:`
  source.
- **Deep sleep itself** (when to sleep, the wake sources, waking the car) is
  **not** in this spec. It is TX on a vehicle bus and is reviewed on its own.
  Two facts for that future spec: ext1 wakeup works only on LP GPIO0–7, so LIN
  (GPIO18/19) cannot wake the chip. CAN1 (GPIO2/3) could, but its transceiver
  sits on the GPIO7-gated +5 V domain and is unpowered in sleep unless GPIO7 is
  held high.

Recipe (also goes into the user guide):

```yaml
time:
  - platform: sntp
    id: sntp_time
    on_time_sync:
      - pcf8563.write_time: rtc_time     # only if an RTC is fitted
  - platform: pcf8563                    # optional, needs i2c:
    id: rtc_time
sd_logger:
  time_id: [rtc_time, sntp_time]         # C9
```

## 7. Hardware tests (Mr. Orange, card wired, WiFi in range)

| ID | Procedure | Pass |
|---|---|---|
| H1 | Fresh card (or one without `#clock`), no `time:` → boot | header `#clock,…,est,build`, `utc_us` = the build time |
| H2 | `time: sntp` + `time_id` → boot with WiFi | `#utc` + `#clock,…,utc,sync`; the µs part ≠ 0 (C7) |
| H3 | After H2, soft reset (OTA / `esphome` reboot) | new file: `#clock,…,utc,warm` **before** any sync line; difference to the following `sync` ≤ 1 s |
| H4 | After H2, `deep_sleep` for 10 min with SNTP blocked, wake | old file ends `#clock` + `#close,clean`; new file opens `#clock,…,drift,sleep,~600`; after unblocking SNTP, `check` prints the correction and the rate in s/day |
| H5 | Cold start with no time source reachable (WiFi off / SNTP blocked) | `#clock,…,est,card,<prev seq>`; floor ≥ the previous file's last `#clock` projection; `dropped=0` under load |
| H6 | WiFi back → sync | `#utc` + `#clock,…,utc,sync`; `sdlog.py check` prints the correction ≥ 0 |
| H7 | Reset matrix: SW restart, esptool/EN reset, a 12 V **and** USB re-plug | the table of which reasons kept the clock goes into §3. C1 behaves correctly for each |
| H8 | `verify.py --seconds 45 --min-laps 75` | exit 0 (release gate, CLAUDE.md) |

H4 is informational for the drift *rate*. Its pass criterion is the format and
the reported correction, not a ppm figure. A longer park-soak (hours, cold
room) is worth doing once the format exists, to put a number on §3.

## 8. Not in scope

- No 32 kHz crystal and no RTC chip on the current PCB (see §6).
- Deep-sleep policy, wake sources and waking the car (see §6).
- No reordering or renaming of files by date. `seq` stays the only order (F2).
- No time zone. Everything is UTC, and display is the reader's business.
- A future-dated real clock (an RTC set to 2099) poisons every later floor on
  that card until a sync corrects it. C8 makes it visible, and `check` flags it.
  The logger does not try to judge which "real" clock is lying.
