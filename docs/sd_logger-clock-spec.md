# sd_logger — a wall-clock basis in every file

2026-10-11, **draft for review**. Extends `docs/sd_logger-spec.md` §6 (F1d meta
lines) and §8 (schema). Every requirement has an ID and the test that proves it.
**A requirement without a test is not done.**

## 1. Problem

A log is only useful later if you can say *when* it was written. Today a file
gets its UTC basis only from a `time_id:` clock that has synced during the
current boot (`#utc` line, `set_utc_anchor()`). The current state:

| Situation | Today | Consequence |
|---|---|---|
| a `time_id:` clock synced this boot | `#utc` in the header, or mid-file at the sync | fine, but resolved to 1 s only (`utcnow().timestamp * 1e6`) |
| no sync yet (car, no WiFi; SNTP not reached yet) | no `#utc` at all | the whole file has boot-relative µs only, so it **cannot be placed on a calendar** |
| a warm reset after a sync (OTA, panic, watchdog, `esphome` reboot) | no `#utc` until the next sync | the system clock survived the reset, but the logger does not use it |
| several clocks (SNTP at home, RTC chip in the car) | `time_id:` takes only one | resyncs from the other clock never reach the file |

None of the bench configs sets `time_id:`. So in practice **no file on any card
carries a UTC basis today.**

The ask (Jan, 2026-10-11): UTC belongs at the start of every file. Use a real
clock when one is there. If none is there (cold boot), take the previous run's
last UTC as the start, plus the time since this boot.

## 2. The answer, in one rule

> **Every file opens with a `#clock` line.** It states the best wall-clock
> knowledge the device has, and says how good that knowledge is: a **real
> reading** (`utc`) or a **lower bound** (`est`). The line is restated as the
> file grows, so the tail of any file says when that file ended. The next boot
> reads that tail when it has nothing better.

A lower bound is the only honest estimate. No one knows how long the power was
off. The previous run's last flushed line was written before this boot powered
up, so `prev_end + uptime` is guaranteed not to be later than the true time.
Any fixed "+x" on top of that could be a lie (think of a 50 ms power blip). So
**the "+x" is the measured uptime of this boot, not a guessed constant.** The
real offset becomes known once a real clock syncs, and the reader reports it then.

## 3. Sources, best first

| Tier | Source | Basis | When it applies |
|---|---|---|---|
| T1 `sync` | a `time_id:` clock synced in this boot (SNTP, Home Assistant, an RTC chip, GPS — any ESPHome `time:` platform) | `utc` | from the sync on; the header if the sync came first |
| T2 `warm` | the ESP's own system clock kept running through a non-power-on reset | `utc` | at boot, when `gettimeofday()` is plausible (C1) and no sync has come yet |
| T3 `card` | the newest file on the card: the last `#clock` in its tail, projected to that file's last timed line | `est` | cold boot, no T1/T2 |
| T4 `build` | `App.get_build_time()` — firmware cannot run before it was built | `est` | fresh card and no clock; also the floor under T3 |

T2 is not a guess. ESP-IDF keeps the system time in the RTC timer, which
"can also persist time keeping across any resets … with the exception of
power-on resets which reset the RTC timer"
([ESP-IDF System Time, ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/system_time.html)).
ESPHome's `RealTimeClock` reads that same clock (`::time()`), but it fires
`on_time_sync` only on a fresh sync, so the logger has to look for itself.

T3 and T4 are combined as `max()`. The `src` field names whichever one won.

## 4. Format additions (additive — `fmtver` stays 2)

```
#clock,@<t_hex>,<utc_us_hex>,<basis>,<src>[,<seq>]
```

- `t_hex` is an anchor-marked absolute boot-µs value, like every other timed meta
  line. It stays outside the delta chain (F1h).
- `utc_us_hex` is the UTC in µs at boot time `t`. For `basis=utc` this is a
  reading. For `basis=est` it is a lower bound.
- `basis` ∈ `utc` | `est`. `src` ∈ `sync` | `warm` | `card` | `build`. `seq` (decimal)
  is present only for `card` and names the file the floor came from.
- **Reading rule:** a `#clock` line is an anchor pair, like `#utc`:
  `record_utc = utc_us + (record_boot_us − t)`. With `est` the result is a lower
  bound for that record. A later `basis=utc` line, or a `#utc` line, overrides
  every `est` before it.

`#utc` is **unchanged** (`#utc,<boot_hex>,<utc_hex>`, still written at the sync).
`script/sdlog.py` rejects `#utc` with any other field count, and an existing
consumer must never take an estimate for a reading. Both reasons rule out
reusing `#utc` for estimates. Older readers skip unknown `#` lines by design, so
they ignore `#clock`. That is the safe default.

**When a `#clock` line is written:**

1. In the header of every file, right after `#origin`/`#utc` (C2).
2. **Restated** when 32 KiB of file output or 10 s have passed since the last one,
   whichever comes first (`SD_LOG_CLOCK_RESTATE_BYTES`, `SD_LOG_CLOCK_RESTATE_US`).
   This bounds how far back the boot reader has to look (C5) and how stale a
   card floor can be (≤ 10 s plus one `sync_interval`).
   Cost: at production load (171 KB/s) one ~45 B line per 32 KiB, so 0.14 %.
3. Right after a `#utc` line, when a sync changes the basis mid-file.

## 5. Requirements

| ID | Requirement | Test |
|---|---|---|
| C1 | **T2 plausibility.** The system clock counts as warm only if `now ≥ build_time` **and** `now ≥ card_floor − 2 s`. Otherwise it is rejected with a `W` log line, and T3/T4 apply. | host: `clock_basis` cases *warm accepted / before build / before card floor* |
| C2 | Every file opened while mounted starts with exactly one `#clock` header line. `basis`/`src` follow the tier table in §3. | host: `format_clock` golden lines; HIL H1–H3 |
| C3 | The card floor is `max(build, prev_end)` at boot µs 0, so at file-open time `t` it is `floor + t`. This is the "+x", and nothing else is added. | host: floor arithmetic, incl. saturation |
| C4 | `prev_end` = the newest valid `#clock` in the tail of the newest file, projected to the latest `@`-timed meta line after it (`#close`, `#rotate`, `#drop`, a later `#clock`). A torn last line (no `\n`) and any line that fails to parse are ignored, never guessed. | host: tail parser — clean close, power-cut tail, torn line, garbage, no `#clock` (old v2 file), `est` chained from `est` |
| C5 | The boot read is bounded: up to 3 newest files (`.LOG`/`.UPL`), the last 64 KiB of each, read only once per boot and before the first file opens. It never fails logging. A read error means "no card floor" plus one `W` line. | host: bounds; HIL H4: `dropped=0` across boot under load |
| C6 | **No estimate ever reaches the system clock.** The logger never calls `settimeofday()`. Otherwise T2 would launder a T3 guess into a "real" time after the next soft reset. | code review + grep test in `tests/sd_logger` |
| C7 | A sync sets the anchor from `gettimeofday()` (µs), not `timestamp * 1e6`. Any second-resolution error then comes from the clock source, not from the logger. | compile; HIL H2 shows a non-zero µs part after SNTP |
| C8 | A sync that lands **before** the current floor by more than 2 s is still accepted, because a real clock outranks an estimate. It is logged `W` and counted (`clock_back=` in the stats line). `sdlog.py check` reports it. | host + reader test |
| C9 | `time_id:` accepts one id **or a list** (e.g. an RTC chip plus SNTP). Each clock's `on_time_sync` updates the anchor. A single id keeps working unchanged. | schema test |
| C10 | `sdlog.py`: `check` and `head` print the file's time basis (`utc sync/warm`, `est card:<seq>`/`build`, or none). When a real `utc` follows an `est`, they print the correction (= how long the device was really off). `extract` keeps the `utc_us` column **real only**. A new opt-in `--estimates` flag adds `utc_floor_us`. | `tests/sd_logger/test_sdlog_reader.py` |
| C11 | Spec, user guide and the F1d table in `sd_logger-spec.md` describe `#clock` and the recipe in §6. | review |

## 6. "It needs a clock": hardware and config

The firmware side is source-agnostic. Any ESPHome `time:` platform works through
`time_id:`. What the car actually has:

- **At home (WiFi):** `sntp`. Collection already needs WiFi, so every visit home
  corrects the clock and shows how far the estimates were off.
- **In the car:** no network. Without an RTC chip, the car gets T2 across resets
  and T3/T4 across power cuts. That is a lower bound, honestly labelled, but not
  a calendar time. **For true UTC in the car, the board needs a battery- or
  supercap-backed RTC on I²C** (ESPHome:
  [`pcf8563`](https://esphome.io/components/time/pcf8563.html),
  [`pcf85063`](https://esphome.io/components/time/pcf85063/),
  [`ds1307`](https://esphome.io/components/time/ds1307/),
  [`rx8130`](https://esphome.io/components/time/rx8130/)). These docs show the
  usual pattern: `read_time` on boot, `write_time` on each SNTP sync.
  **The bench boards have no free pin pair for I²C** (`docs/bench-hmi-wiring.md`:
  I²C was dropped, GPIO0 is the only unused pin). So the RTC is a **PCB ask for
  the next revision**. It is not part of this firmware change.
- **Vehicle bus time:** many vehicles broadcast date/time on CAN. That is
  vehicle-specific and belongs in the consuming project. It can feed the logger
  through a lambda that calls `set_utc_anchor()`, or through a custom `time:`
  source.

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
| H2 | `time: sntp` + `time_id` → boot with WiFi | `#utc` + `#clock,…,utc,sync`; µs part ≠ 0 (C7) |
| H3 | After H2, soft reset (`esphome` upload/reboot) | new file: `#clock,…,utc,warm` **before** any sync line; difference to the following `sync` ≤ 1 s |
| H4 | Cold boot (no time source reachable — WiFi off / SNTP blocked) | `#clock,…,est,card,<prev seq>`; floor ≥ the previous file's last `#clock` projection; `dropped=0` |
| H5 | WiFi back → sync | `#utc` + `#clock …utc,sync`; `sdlog.py check` prints the correction ≥ 0 |
| H6 | `verify.py --seconds 45 --min-laps 75` | exit 0 (release gate, CLAUDE.md) |

A true power-on reset needs USB re-plugged as well as 12 V. Before H3/H4 are
read, it has to be confirmed on hardware whether an esptool/EN reset counts as
power-on on the C6 (the docs above suggest it does). Until that is checked,
H4 runs as "no time source", which exercises the same T3 path.

## 8. Not in scope

- No RTC hardware on the current PCB (see §6).
- No reordering or renaming of files by date. `seq` stays the only order (F2).
- No time zone. Everything is UTC, and display is the reader's business.
- A future-dated real clock (an RTC set to 2099) poisons every later floor on
  that card until a sync corrects it. C8 makes it visible, and `check` flags it.
  The logger does not try to judge which "real" clock is lying.
