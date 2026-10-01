# sd_logger — SD-card bus datalogger (component spec, draft)

Drafted 2026-07-22. Net-new ESPHome component for ESPCars: log CAN (via
`can_gateway`) and LIN (via `linbus`) traffic to an SD card on the ESP32-C6,
under load, without disturbing bus timing. This is a design spec, not code —
promote sections to `components/sd_logger/` as they are built. Written in the
repo's English/esphome-docs style so it stays upstream-friendly.

Unlike `can_gateway`/`linbus`/`isotp`, there is **no upstream fork to import
from**. The closest ESPHome prior art (`n-serrette/esphome_sd_card`,
`Mnark/sdmmc`) is **ESP32/ESP32-S3 only and SDMMC-based** — it does not run on
the C6. So this is built from scratch, SPI-mode, on our own architecture.

**Implementation status (2026-07-22):** M1 landed — `components/sd_logger/`
(SDSPI mount with the H2 settle delay, lock-guarded RAM ring, writer task at
prio 6, batched `fsync`/rotation, the `sd_logger.log` action, and the VCC ADC
monitor → emergency close of §7 Layer B). Schema tests (`tests/sd_logger/`,
V1-V6/V8) and a build yaml (`tests/build/sd_logger/`) are green; the standalone
image and the Orange integration image both compile on the C6.

**Update (2026-07-26): M0 is closed and M1 is proven on hardware.** The card is
wired to Mr. Orange on the `docs/bench-hmi-wiring.md` pins and the logger ran on
the bench: mount + on-device format, `mounted=1`, `records` climbing at the full
40/s the test interval generates, **`dropped=0`**, writing `/sdcard/L0000000.CSV`
while LIN carried ~10 % bus load and CAN1 12.4 %. Card init succeeded at the
first attempt at 10 MHz, so H1/H2/H3/H5 are all confirmed on silicon.

Two things came out of that run:

- **§7 Layer B is broken as implemented (see the VCC-calibration finding in
  `docs/HANDOVER.md`).** `monitor_loop_()` converts the ADC with a naive
  `raw/4095 * adc_full_scale` and no `adc_cali` curve; on the C6 that reads 26 %
  low (raw 1087 → 8.94 V computed vs 12.07 V actual), so the emergency close
  fires on a healthy rail and, because `dying_` latches one-way, permanently
  disables logging at boot. `adc_full_scale` cannot compensate — the schema caps
  it at 3300 mV, which still only reaches 9.51 V. **M3 cannot be called done
  until this is fixed**, and M1's own vcc_monitor path is only working on the
  bench via a board-specific `divider` stopgap.
- ~~The full ring gate (SW-6's second bullet) did **not** run: Mr. Blue was
  decommissioned on 2026-07-26 and `verify.py` needs all three boards.~~
  **Retired 2026-07-29:** Blue is back and the gate runs green at the original
  19.4 laps/s. The gate has since been run against the merged `sd_logger` work
  (PR #5) — 875 laps, zero errors — so the logger is now proven against the ring
  baseline, not only under LIN + single-segment CAN load.

Still open: **M2** (the `can_gateway` tap S2 is in the tree — `add_can_tap()` /
`drain_can_taps_()`, `tap_translate.h` and its host cases — with the bench run
outstanding; the `linbus` tap S3 does not exist, LIN is still fed by the generic
action), **M3** (Layer A pre-allocation hardening + hold-up cap, plus the VCC
calibration fix above), and SW-6's ring gate under the new bench topology.

**Update (2026-07-27): the write format is designed — §6 below is v1.** What
shipped in M1 was improvised, not designed: one `snprintf` plus one `snprintf`
*per payload byte* into a `char line[80]`, and a bare numeric `source` column
whose meaning lives only in the YAML that produced the card. §6 replaces it with
a typed-line format that carries both buses, the action and **ESPHome's own log**
in one file, and §5 gains the writer rules (block buffer, no `snprintf`, stateless
time reconstruction) that make it cheaper than what it replaces. The break is
clean rather than migrated: `script/hil/verify.py` does not parse the files, no
reader existed anywhere in the repo, and the format was never promised to anyone.

**Update (2026-07-28): format v1 is built — M5 steps 1-6 (§12).**
`log_format.h` plus `tests/host/test_log_format.cpp`, the writer rewrite (fd,
block buffer, meta lines, `#drop`, `#close` on every exit path), the text ring
and the ESPHome log capture (S4), the schema keys with V13-V18, and
`script/sdlog.py` as the reader. `pytest` (354 cases), `make -C tests/host` (179)
and both C6 build targets are green.

**M5.7 is done, same day — and v1 beat its own spec (table in F1g).** On Mr.
Orange the writer saturates at **~9 300 rec/s / 373 KB/s** and runs entirely clean
to **~8 460 rec/s / 338 KB/s at 64.6 % CPU**, against the M1 format's
5300-5700 rec/s / 190-211 KB/s at ~85 %. The line measures 41.0 B, inside the ~42 B
estimate — so removing ~9 `snprintf` parses per record buys **1.5× the record
ceiling and 1.75× the throughput at 20 points less CPU**, and pays for the wider
line several times over. Production load now has 2.3× headroom, against 1.5×.

Still open: M5.8 (the `HIL.md` readout criterion), the §10.6 ring gate against v1,
and **one card read back on a host** — nothing on the board can read its own file,
so the bytes v1 actually writes are verified today only by the host cases and the
reader suite, never end to end.

Writing the D8 host cases immediately found a real defect in the spec's own
timestamp formula — see the correction in D8 below. That is the case for keeping
the formatter host-testable in one line: on the bench it would have been a green
run and a card full of confident nonsense.

---

## 1. Goal & scope

- **G1** Persist CAN frames and LIN frames to an SD card at up to **60 %
  CAN-gateway bus load plus LIN**, on the single-core ESP32-C6, with **zero
  effect on CAN forwarding timing and LIN schedule timing**.
- **G2** Observe-first: the logger **never transmits** on any bus (aligns with
  the CLAUDE.md TX-safety rule). It is a pure sink.
- **G3** Survive power loss without corrupting the card beyond *at most the
  last un-synced batch*, and attempt a clean file close when the supply sags
  ("close-file emergency", §7).
- **G4** External-component-safe: no edits to esphome core; all `USE_*`/sizing
  defines emitted from Python via `cg.add_define(...)`; IDF components pulled
  with `include_builtin_idf_component(...)`.

Out of scope for the MVP: decoding/DBC-aware logging (log raw frames; decode is
a later layer), an on-device web UI.

---

## 2. Why this is feasible on a single-core C6 (the architecture fit)

The load study (see the investigation that spawned this spec) established the
one fact everything else depends on: **neither bus's wire timing lives in the
ESPHome main loop.**

- **CAN forwarding** runs in the TWAI RX ISR (`can_gateway` re-transmits in
  interrupt context; `can_gateway/__init__.py` sets `TWAI_ISR_CACHE_SAFE`,
  `TWAI_IO_FUNC_IN_IRAM`, and forces FreeRTOS into IRAM for exactly this).
- **LIN** runs in its own FreeRTOS task `lin_uart_evt` at priority 18
  (`LINCommunication.cpp` `uart_event_task_trampoline`), which carries RX
  decode, the master header cadence, and response TX. Schedule jitter bounded
  to ~2 ms.

Consequence: **SD write-latency spikes (the classic real-time-logger killer)
hit only the main loop / a writer task, never the buses.** Two corollaries:

- **C2a** SD over the *external* SPI does **not** disable the flash cache, so it
  does **not** threaten the gateway's IRAM/cache-safe discipline.
  → **Design rule: log to SD (external SPI), never to internal flash
  (LittleFS/NVS).** Internal-flash logging *would* create cache-off windows and
  directly endanger the gateway ISR path. SD is the correct medium *because* of
  this, not merely for capacity.
- **C2b** The design must **capture off the hot paths into a RAM ring** and let
  a **dedicated writer task** drain it. Never write to the card from a
  component `loop()` or a YAML lambda — that would convert SD latency into
  main-loop jitter and make Issue #2 (cyclic ~16 ms floor) worse and delay
  decode-sensor updates.

---

## 3. Hardware constraints (C6-specific, confirmed by research)

- **H1 — No SDMMC host on the C6.** SD must run in **SPI mode (SDSPI, 1-bit)**
  on the one general-purpose SPI (SPI2_HOST / GPSPI2). Clock is capped at
  `SDMMC_FREQ_DEFAULT` (**≤ 20 MHz**) over SPI. That is ~2.5 MB/s theoretical,
  ~1–2 MB/s realistic — versus a logging need of ~130–150 KB/s (§5 D3), so the
  **card** is not the limit.

  > **Corrected 2026-07-27.** This bullet used to say average throughput was a
  > non-issue and latency spikes were the whole problem. The bench says
  > otherwise: the writer tops out at **~5300–5700 rec/s ≈ 190–211 KB/s at
  > CPU ~85 %** — an order of magnitude below the card, because the cost is
  > *per-record CPU in the writer*, not card bandwidth. Sustained rate is a real
  > constraint; it is just not the card's. See §6 F1g.
- **H2 — Init ordering / C6 boot quirk.** `spi_bus_initialize()` must run
  **before** the mount. On the C6 a **few-ms delay between bus init and
  `esp_vfs_fat_sdspi_mount()`** is needed for reliable init on *every* boot
  (documented community gotcha). Bake a short settle delay into `setup()`.
- **H3 — Pins.** SDSPI needs 4 GPIO (MOSI/MISO/SCK/CS) + 3V3 + GND. On the
  busiest reference config (Mr. Orange = CAN gateway + LIN listener) the taken
  pins are GPIO2/3 + GPIO10/11 (CAN), GPIO18/19/7 (LIN), GPIO8 (LED), plus
  GPIO12/13 (USB-JTAG). Four free GPIO for SDSPI must therefore come from the
  **PCB** — this is a precondition, not a given (see M0 in §12).
- **H4 — ADC for VCC sensing** (§7): the C6 has one SAR ADC (ADC1) on
  **GPIO0–GPIO6**. The VCC-divider tap must land on a free ADC1 pin (GPIO0/1/4/5
  are typically free on Orange).
- **H5 — Card power (recommended, NOT built, and no longer needed).** A high-side load switch on
  card VCC (+1 GPIO) would let firmware hard-cut and re-init a wedged card, and
  de-power the card cleanly on the emergency path (§7). On the reference board
  the GPIO exists and the switch does not: SD_PWR reaches header J5.6 through
  R32 and terminates there. Every config that declared `card_power_pin: GPIO17`
  was therefore driving a header pin, and the `power_cycle` branch of Layer C
  has never removed this card's power. Corrected 2026-07-29; the configs no
  longer declare the pin. Note also that a switch alone would not be enough:
  `gpio_reset_pin()` leaves the bus pins pulled up and the board adds 2.2k/4.7k
  externals, so a card whose VCC is cut still back-powers through its I/O clamp
  diodes unless all four lines are driven low for the whole off window. Moot as
  of 2026-08-28: the wedge H5 was wanted for is handled in software — §7 Layer D.
- **H6 — Hold-up energy (required for §7 to mean anything).** A bulk cap /
  supercap on 3V3 behind a series diode, sized to finish one in-flight SD write
  + one `f_sync` + `f_close` after the supply starts sagging. Firmware cannot
  manufacture time; without hold-up, "emergency close" is best-effort at best.

**The chip can do all of this; whether the `PCB-ESP32C6-Adapter-CAN-Modbus-LIN`
board (git.kipp.ing) exposes 4 free GPIO + an ADC pin + hold-up + optional card
power, and whether the Modbus/RS-485 branch already claims wanted pins, is the
gating open question.**

---

## 4. Component shape & integration seams ("subkomponente")

New top-level component **`components/sd_logger/`**. Mirrors the siblings:
`DEPENDENCIES = ["esp32"]`, `only_on_variant([VARIANT_ESP32C6])` for now,
`require_framework_version(esp_idf >= 5.5)`. It is a *sink* other components
feed. Five input **sources**, smallest-coupling first (S1/S2 ship; S3–S5 are
designed here so the format does not have to change when they land):

- **S1 — generic action `sd_logger.log`** (templated `data`, optional `tag`):
  any automation can push a record. Ships in the MVP; also the seam LIN uses at
  first (call it from an existing `linbus` `on_frame:` lambda).
- **S2 — `can_gateway` tap — shipped, and it landed *below* what this section
  originally proposed.** The draft called for a sink callback on the observe
  drain (loop context). What was built is a per-port **`ObserveRing<…,
  TapRecord>` filled in the RX ISR** (`log_tap: true`; `run_rx_taps_()`), which
  the writer task drains as the ring's single consumer. Two consequences the
  format depends on: `t_us` is stamped **at capture in the ISR**, so SD write
  latency cannot shift the time base (D8), and forwarded, filtered and shed
  frames are all captured — the `s` flag of F1b is what makes a shed frame
  visible.
- **S3 — native `linbus` source** (later): a registered sink equivalent to S2
  so LIN frames skip the lambda round-trip. The hook point is
  `LinProtocolHandler::onData`, right after the frame is accepted: that is the
  only place where the PID *with* parity, the resolved checksum mode and the
  fallback flag are still live — none of them survive into `lin_discovered_id_t`,
  and the `on_frame` trigger the logger uses today fires from the **main loop**,
  where frames arriving faster than a loop pass are coalesced away
  (`note_frames_coalesced()`). A LIN log fed through the trigger is therefore
  lossy by construction; S3 is what makes it complete.
- **S4 — ESPHome's own log** (format v1, §6 F1c). The component registers a
  logger callback and writes every captured message as an `X` line in the same
  file. Mechanics in §4a — every one of them is a *silent* failure if missed.
- **S5 — `isotp` messages** (later, `I` lines): `isotp` already exposes
  `add_on_message_callback` / `add_on_error_callback` with a non-owning
  `MessageView` valid only for the callback. Reassembled messages run to
  `max_message_size` (256 B default), so they do not fit the 20-byte record and
  need the variable-length path (D9) — and the `IsoTpError` codes are the
  failures a raw frame log cannot show. Reserved in the grammar so adding it
  later does not change the format.

Keep S2/S3 hooks minimal and generic (a `FrameSink` interface with a
`push(record)` that is **ISR-safe and non-blocking**) so they stay
upstream-mergeable and never stall the producer.

### 4a — S4: capturing ESPHome's log (verified against pinned esphome 2026.7.0)

Recorded here because each of these fails *quietly*:

- The API is **`Logger::add_log_callback(void *instance, void (*)(void *,
  uint8_t level, const char *tag, const char *message, size_t len))`**.
  `add_on_log_callback` and the `LogListener` interface are **gone** in this
  version; copy `components/syslog/`, the closest analogue in tree.
- It compiles to a **no-op** unless the Python side calls
  `request_log_listener()` in `to_code` — that is what emits `USE_LOG_LISTENERS`
  and `ESPHOME_LOG_MAX_LISTENERS`. Miss it and capture silently does nothing;
  under-count it and `StaticVector::push_back` silently drops the callback.
- `message` is the **fully formatted line**: ANSI colour, then `[I][tag:123]: `,
  plus an extra bracketed thread name (with its own escapes) for logs emitted off
  the main task. It is null-terminated and points into the logger's shared
  `tx_buffer_`, reused by the next line — **copy synchronously, never store the
  pointer**. Strip escapes generically, then drop the header at the **first**
  `]: ` (the first occurrence is always the end of the prefix); `level` and `tag`
  arrive as separate arguments anyway.
- Callbacks **only ever fire on the main loop task**. Messages from other tasks
  route through `TaskLogBuffer` and are replayed in `Logger::loop()` — but with
  `task_log_buffer_size: 0` they bypass listeners entirely, which would silently
  lose exactly the sd_logger writer and VCC-monitor tasks' own messages (V17).
- Recursion is guarded **by dropping**: an `ESP_LOGW` from inside the callback
  vanishes. The capture path must never log — failures get a counter.
- ESP-IDF's own output arrives too, under tag `esp-idf`.
- The level filter is `level <= configured` (numerically lower = more severe),
  and it can only ever see what `logger.level` already let through (V16).

Draft YAML (the shipped keys, not the M0-era sketch — there is no `spi:`
sub-block and no `rotate:` block; new in v1 are `label:`, `sources:` and
`esphome_logs:`):

```yaml
sd_logger:
  id: logger
  clk_pin: GPIO21
  mosi_pin: GPIO22
  miso_pin: GPIO20
  cs_pin: GPIO15
  clock: 10MHz              # capped at SDMMC_FREQ_DEFAULT over SPI (H1)
  card_power_pin: GPIO17    # optional high-side switch (H5)
  buffer_depth: 4096        # records in the RAM ring (§5 D3)
  sync_interval: 2s         # bounded fsync cadence => max loss window (§5,§7)
  max_file_size: 32MB       # rotation threshold (§6 F2)
  vcc_monitor:              # the close-file emergency (§7)
    adc_pin: 6              # ADC1 channel on the 12 V-rail divider (H4)
    threshold: 10.5V        # trip above regulator dropout, not at brownout
    divider: 10.8673
  can_ports:                # S2 — one entry per tapped can_gateway port
    - port: seg1            # label defaults to C1, C2, ... by position
    - port: seg2
    - port: seg3
      label: diag           # <= 8 chars; every character costs bytes/s (F1g)
  sources:                  # tags fed by the action today, by S3 later
    - tag: 5
      kind: lin             # can | lin | user -> the L/C/U type letter
      label: lin
  esphome_logs:             # S4 — ESPHome's own log as `X` lines
    level: INFO
    buffer_depth: 32
```

Marking, end to end: the producer stamps a 1-byte tag, the writer resolves it
through this table to a **type letter + label**, and each file re-emits the whole
table as `#src` lines (§6 F1d). A card found on a bench therefore explains itself
without the YAML that produced it — which the bare numeric column it replaces
could not. `sd_logger.log`'s `source:` accepts a declared label or the existing
integer; tag derivation (positional `index + 1`, 0 reserved for the action) does
not change.

---

## 5. Data path & buffering (the core design)

```
 producers (ISR / LIN task / lambda)     writer task (prio ~6, no-affinity)
 ────────────────────────────────────    ──────────────────────────────────
 push fixed record ──▶ [ RAM ring ] ──▶  drain ▶ 512B-aligned batch ▶ FAT
                          (SPSC/MPSC,     f_sync every sync_interval
                           lock-free)     (never IMMEDIATE_FSYNC — see P3)
```

- **D1 — Record (packed, 20 B, as built):** `t_us` (uint32, `esp_timer`), `id`
  (uint32, CAN or LIN id), `source` (uint8 tag), `flags` (uint8, `REC_FLAG_*`),
  `len` (0–8), `data[8]` — `log_record.h`. **Format v1 does not change it.** The
  tag stays a single byte on the producer side, where every byte is ISR budget;
  the *writer* resolves it to a kind letter and a label through the table built
  at codegen (§4), because that resolve is pure formatting and belongs on the
  consumer side. An unmapped tag degrades to kind `U` and its decimal value, so
  a lambda calling `log_frame(1, …)` directly keeps working.
- **D2 — Ring:** static, in internal RAM, power-of-two, single-producer-safe per
  source (use an MPSC ring or per-source SPSC rings merged by the writer).
  Spec it **ISR-safe** (no malloc, no task-held lock on the producer side) even
  though the MVP CAN tap runs in loop/drain context — this future-proofs an
  ISR-side tap.
- **D3 — Sizing:** worst burst = one SD latency spike. **Measured**, not
  estimated (M2 stress run §9.6 (2026-07-26, git history)/§9.7): production load on a
  forwarding gateway at 90 % of 500 kbit/s is **~3600 rec/s** (not the 7200 an
  earlier draft assumed — a TWAI node does not receive its own transmissions),
  and the writer sustains **~5300–5700 rec/s** before the ring sheds. A 500 ms
  stall at 3600 rec/s is ~1800 records ≈ 36 KB at 20 B. `buffer_depth: 4096`
  (~80 KB) is therefore ~1.1 s of absorption at production load, ~570 ms at the
  forwarding-off worst case. Default 2048 is the standalone figure; a tapped
  gateway config should raise it.
- **D4 — Overflow policy:** **drop-newest, never block the producer** (blocking
  would stall the ISR or the LIN task — the whole thing we are protecting).
  Increment a `dropped_records` counter exposed as a sensor, **and write the
  `#drop` marker of F1d into the file itself** so the gap is visible to whoever
  reads the card. The marker half was specified in M1 and never built: today a
  dropped burst is indistinguishable from a quiet bus once the console scrollback
  is gone. Emit it on counter *change*, so a healthy run costs nothing.
- **D5 — Writer task:** `xTaskCreatePinnedToCore(..., prio ≈ 6, tskNO_AFFINITY)`
  — **below** LIN (18) and the TWAI ISR so those always preempt, **above** the
  esphome main loop (prio 1) so draining is steady. It **blocks on the ring**
  (task notification / semaphore) and only runs when a batch exists, so it is
  bursty and cannot busy-starve the main loop; during a real SD write it yields
  on SPI-DMA waits. Document this priority tradeoff in the code.
- **D6 — Batching, and the formatter (v1 makes this concrete):** the writer owns
  a ~4 KB buffer and **formats records directly into it at the write cursor** —
  no intermediate line buffer, no per-line `memcpy`, no stdio lock, and no
  `char line[80]` cap. It flushes `floor(pos / 512) * 512` bytes and `memmove`s
  the sub-sector remainder to the front, so **every write is a whole number of
  sectors** and FATFS never does a read-modify-write on a partial one; the
  `#pad` header line (F1d) puts the stream on a sector boundary at file open so
  the invariant holds from the first write. Drop stdio for
  `open()/write()/fsync()/close()` — `FILE*`/`setvbuf` buy nothing once the
  writer owns its buffer.

  > **No `snprintf` in the writer.** Today it costs ~9 format-string parses per
  > record (one for the header fields, one *per payload byte*), which at the
  > measured ceiling is where the 85 % CPU goes. Replace with table-driven
  > emitters — u64 decimal, u32 hex without leading zeros, byte → two hex chars.
  > This is the single biggest lever on the record ceiling, and the reason v1 can
  > afford a wider line than what it replaces (F1g).

  **Pre-allocating the file contiguously** (`f_expand`) would remove mid-stream
  FAT cluster allocation, a known spike source — but it is a FatFs API not
  exposed through the VFS layer the component writes through, so the mitigation
  in place is `allocation_unit_size = 16 KB` at mount. The soak run found
  rotation and allocation are *not* stall sources at production load, so this
  stays a documented non-fix rather than a reason to bypass the VFS.
- **D7 — fsync cadence:** **do not** enable `CONFIG_FATFS_IMMEDIATE_FSYNC` — it
  guarantees consistency but forces a disk op per write and destroys throughput.
  Instead call `f_sync()` on a **bounded cadence** (`sync_interval`, and/or
  every N KB). This makes the **maximum data loss on a hard cut = one batch**,
  which is the knob §7 tunes.
- **D8 — Timestamps: 32-bit on the producer, 64-bit in the file, stateless in
  between.** Records carry a uint32 `t_us` — the CAN tap stamps it in the RX ISR
  (`can_gateway.h`, `run_rx_taps_()`), where 4 bytes matter and a 64-bit read
  does not belong. It wraps every ~71 min, so lines would march backwards in any
  log that runs longer than that. The writer samples `esp_timer_get_time()` once
  per drain pass and reconstructs the full value from a **signed** difference:

  ```c
  int32_t age = (int32_t) ((uint32_t) now64 - t32);   // + = older than the sample
  int64_t full = (int64_t) now64 - age;               // clamped at 0
  ```

  **Corrected 2026-07-28 — the version this replaces was wrong, and wrong in the
  common case.** It read `hi = now64 >> 32; if ((uint32_t) now64 < t32) hi--;`,
  which assumes every record predates the clock sample. They do not: the writer
  samples `now64` once and *then* drains, while the RX ISR keeps stamping records
  into the tap rings for the whole pass — so a record a few µs newer than the
  sample is the normal case at load, not an edge. With `hi == 0`, which is the
  first ~71 min after boot and therefore every bench run, the decrement
  underflowed to `0xFFFFFFFF` and stamped the line ~584 000 years out. The signed
  form reads that as the small negative age it is. Found by the property case in
  `tests/host/test_log_format.cpp`, which is the whole argument for keeping the
  formatter host-testable: on the bench it would have been a green run and a card
  full of confident nonsense.

  Exact for any record within **±~36 min** of the sample, in either direction
  (2^31 µs, the range a signed 32-bit delta spans), and records are milliseconds
  old. **Deliberately not a wrap counter**: a counter desyncs across a quiet
  period and is fooled by the few-ms out-of-order arrivals between two rings
  (F1f), while this is a pure function of two numbers and host-testable across
  the boundary in three lines.
- **D9 — Text ring (source S4).** ESPHome log lines are variable-length and
  cannot share the fixed 20-byte record ring, so S4 gets a second ring of fixed
  slots — `{uint32_t t_us; uint8_t level; uint8_t tag_len; uint8_t len; char
  buf[…]}` at `SD_LOGGER_TEXT_SLOT` 192 B, depth 32 (~6 KB) — drained by the same
  writer in the same pass. Same overflow policy as D4: drop-newest into a
  `text_dropped_` counter that surfaces as `#drop,…,text`. Longer messages are
  truncated with the `~` marker of F1e rather than split, so one log line stays
  one file line.

---

## 6. File format & rotation

**Format v2, designed 2026-07-29** — v1 (2026-07-27) with a delta-coded stamp
column (F1h) and nothing else moved. A v1 reader handed a v2 file calls every
record line malformed, so `script/sdlog.py` now **refuses a write format it does
not know** instead of only printing the number; that gap, not the byte saving, is
what made the version field worth having.

**Format v1, designed 2026-07-27.** What M1 shipped was improvised: a bare
`t_us,source,id_hex,flags,dlc,data_hex` line whose `source` column is a number
meaningful only next to the YAML that produced the card, emitted through one
`snprintf` plus one `snprintf` *per payload byte* into a `char line[80]`. v1
replaces it. There is nothing to migrate — `script/hil/verify.py` does not parse
these files and no reader exists in the repo — so this is a clean break. M1-era
files carry no version of their own, just a `# t_us,source,…` legend line, which
is why the `.CSV` → `.LOG` rename in F2 is worth its small cost: it tells the two
eras apart on a card that holds both, without sniffing.

- **F1 — Container: one file, typed lines.** Both buses, the generic action and
  **ESPHome's own log** go into the same file. Every line is comma-separated,
  newline-terminated, unquoted, and starts with a **type letter** that dispatches
  the rest of the line; only the last field may contain a comma. One open file
  means one fsync cadence, one rotation and one power-loss story, and it puts a
  firmware message inline with the traffic it explains — which is the whole point
  during a bus-off window or an emergency close, where a separate log file would
  have to be re-merged by hand.

  ```
  #sdlog,2,7,@64960,2026.7.0
  #src,C,C1,1,can_gateway:gw port0,500000
  #src,C,C2,2,can_gateway:gw port1,500000
  #src,L,lin,5,linbus:lin,19200
  #types,C=can,L=lin,U=user,X=esphome-log,#=meta
  #flags,x=extended,r=rtr,t=tx,s=shed,~=truncated,none=absent
  #layout,rec=<K><tag><flags>:t:id:data,log=X<level>:t:tag:msg,t=@abs|step us HEX,tag/id/data HEX,dlc=len(data)/2
  #pad,<spaces to the next 512-byte boundary>
  C1x,@64AB9,1A2,0011223344556677
  C2xs,2B,1A2,0011223344556677
  XI,72,sd_logger,records=4120 dropped=0 bytes=198112
  L5,6C,3C,55AA0000000000FF
  #drop,@65132,ring,17,17
  #close,@F3ED0,emergency
  ```

- **F1a — Record lines** — one layout for every bus, so one parser covers all of
  them. **Four fields, three commas**, because a separator only earns its place
  between fields whose width is not self-evident:

  ```
  <K><tag><flags>,<t>,<id>,<data>
  ```

  | field | radix | meaning |
  |---|---|---|
  | `K` | — | `C` CAN, `L` LIN, `U` user/unmapped (`I` reserved for isotp, §4 S5). The only thing a reader needs to dispatch the line. |
  | `tag` | **hex** | the producer's 1-byte source tag. `#src` maps it to a label and a kind, so the *name* is stated once per file instead of on every line. |
  | `flags` | — | concatenated letters, **nothing at all** when none |
  | `t` | **hex** | the stamp column (F1h): `@<µs>` absolute, or a bare step |
  | `id` | **hex** | uppercase, no leading zeros (`1A2`, `18DAF110`) |
  | `data` | **hex** | 2 uppercase chars per byte, opaque; empty for a zero-length frame |

  > **Every number on a record or text line is uppercase hex**, no `0x`, no
  > padding. Not a style rule: a decimal field is a divide-and-modulo *per digit*
  > on a path that runs ~3600 times a second, and one mixed-radix column is a trap
  > for whoever reads the card later. One rule for the whole line is the only
  > version that stays true. `#` meta lines are the exception and F1d states it.

  > **The leading token is self-delimiting**, which is what lets it fuse three of
  > v1's columns. The tag is uppercase hex, the flag letters are lowercase or `~`,
  > and **the two alphabets do not overlap** — so a reader takes hex characters
  > until one is not, and the rest is flags. `C1Ax` is tag `0x1A` with the extended
  > flag, never tag `0x1` with flags `Ax`. Keep it that way: a flag letter that is
  > also a hex digit would silently re-partition every token in the file.

  > **The payload is opaque bytes and is never decoded into a number.** No
  > widening, no byte-swap, no endianness — `hex_bytes()` walks bytes through a
  > nibble table, and that is the whole of it. Base64 would fit 8 bytes in 12
  > characters instead of 16, and is **rejected**: it is slower (bit-shuffling
  > across byte boundaries against a per-nibble lookup), it complicates every
  > reader, and a payload you can read by eye is worth 4 bytes on a bench log.

  > **`dlc` is gone**: it was always `len(data)/2`. The one thing that costs is an
  > **RTR frame's requested length, which v2 does not preserve** — a remote frame
  > has no payload for its width to be read off. Deliberate: RTR is not used on
  > these buses, and a conditional last field would buy a frame type that never
  > appears a branch on the hot path and a special case in every reader. The `r`
  > flag stays, so such a frame is still visible as one.

- **F1b — Flag letters**, from the `REC_FLAG_*` bits already in `log_record.h`
  (the 20-byte RAM record does not change): `x` extended (bit0), `r` rtr (bit1),
  `t` tx (bit2), `s` shed (bit3), `~` truncated (bit7). Bits 4–6 stay reserved
  for LIN error marking when S3 lands. `s` is the flag this file exists for — a
  frame that was on the wire and was *not* forwarded, the Issue #1 signature.

  **No flags emits nothing** — v1's `-` placeholder went with the column it
  padded. The letters are the tail of the leading token (F1a), not a field, so an
  unflagged frame costs zero bytes here instead of a `-` and a comma; most frames
  on this bench are unflagged, so it is the saving that lands on the most lines.
  **Every letter is lowercase or `~`, and never a hex digit** — that disjointness
  is what makes the token self-delimiting, so it is a constraint on any future
  flag letter and not a coincidence.
  Letters rather than a number because the column is read far more often than it
  is parsed, and because the encoding trap in `tap_translate.h` (`TAP_FLAG_SHED`
  is 0x04, `REC_FLAG_TX` is 0x04, same bit opposite meanings) is invisible in a
  decimal column and obvious in a letter one.

- **F1c — Text lines: ESPHome's own log** (source S4, §4).

  ```
  X<L>,<t>,<tag>,<message>
  ```

  `L` is the ESPHome level letter (`E W I C D V`; VERY_VERBOSE collapses to `V`),
  and it rides in the leading token for the same reason the record's flags do: one
  fixed byte, so the comma after it was paying for nothing. `message` is the last
  field, so its commas need no escaping. The stamp column is the same one record
  lines carry and shares **one chain** with them (F1h).

- **F1d — Meta lines** all start `#`, so any reader that already skips comments
  keeps working:

  | line | when |
  |---|---|
  | `#sdlog,<fmtver>,<seq>,<t_us>,<esphome_ver>` | file open |
  | `#src,<K>,<label>,<tag>,<origin>,<detail>` | file open, one per declared source |
  | `#origin,<string>` | file open, once when `origin:` is configured; board/bench identity |
  | `#utc,<boot_us_anchor_hex>,<utc_us_hex>` | file open after a time sync, and immediately on each later sync; boot-to-UTC correspondence |
  | `#types,…` / `#flags,…` / `#layout,…` | file open — the legend, so a card found on a bench is self-describing |
  | `#pad,<spaces>` | file open — pads the header block to a 512-byte boundary (D6) |
  | `#drop,<t_us>,<what>,<delta>,<total>` | only when a drop counter moves; `what` ∈ `ring` \| `tap:<label>` \| `text` |
  | `#rotate,<t_us>,<next-file>` | immediately before rotation |
  | `#close,<t_us>,<clean\|rotate\|emergency>` | last line of a file |

  Every `t_us` above is an **anchor-marked absolute** `@<hex>`, and outside the
  delta chain of F1h. Outside, because that is what lets a reader skip the markers
  it does not care about: a meta line that advanced the chain would make anyone
  ignoring `#drop` decode every record after one of them wrongly. Anchor-marked,
  because `@` then means one thing everywhere in the file — an absolute hex µs —
  rather than being a record-line convention a reader has to remember not to apply
  here.

  `#utc` is also outside that chain, but its two fields are bare uppercase hex
  values rather than stamp encodings: `boot_us_anchor_hex` is the same
  reconstructed 64-bit boot-µs domain as `reconstruct_us()`, and `utc_us_hex` is
  UTC microseconds. It states `record_utc = utc_us_anchor + (record_boot_us -
  boot_us_anchor)`. Its absence is normal before a time sync or when no
  `time_id:` is configured. `#origin` is emitted only when configured; both lines
  pay once per file instead of adding work or bytes to a record.

  **Radix on meta lines**: times are hex (they carry `@`), and `#src`'s `tag` is
  hex **because it is the same number the record token carries** — a decimal `10`
  here against a token of `CA` there is the kind of mismatch nobody notices until
  they are chasing a source that seems to have no records. Everything else on a
  meta line is **decimal**: `fmtver`, `seq`, the drop counters, `#gap`'s chunk and
  byte totals. Each of those names something outside the file — `seq` names a
  zero-padded decimal filename, the counters match the firmware's own stats line —
  and they are written a handful of times per file, so no CPU argument applies.

  `#layout` is new in v2 and states the column list and these rules in-band. It
  costs nothing: the header block is padded to a 512-byte boundary either way, so
  the line comes out of `#pad`'s budget. That is the trade the format makes
  everywhere — spend bytes on self-description in the header, where they are paid
  once, never in the record lines, where they are paid ~3600 times a second.

  `#drop` is D4's overflow marker, which was specified and never built — today a
  gap in the file is indistinguishable from a quiet bus. The three drop counters
  stay **separate** (logger ring / gateway tap ring / text ring) for the same
  reason `sd_logger.cpp`'s stats line refuses to sum them: they name different
  bottlenecks with different fixes.

  **A missing `#close` is the power-cut signature.** It costs one line and turns
  the manual bench criterion in `tests/hil/HIL.md` ("parses as valid CSV up to
  the last line") into something a script can decide.

- **F1e — Escaping, and the one invariant.** Applied to `tag`, `message` and
  string payloads: `\`→`\\`, LF→`\n`, CR→`\r`, TAB→`\t`, other bytes < 0x20 or
  0x7F→`\xNN`; bytes ≥ 0x80 pass through so UTF-8 survives. A comma in a `tag`
  becomes `_` (tags are identifiers); commas in `message` pass through. Lines are
  capped at `SD_LOG_MAX_LINE` (512 B) with a trailing `~` on truncation.

  > **The formatter never emits a bare newline except the line terminator.**
  > That single invariant is what makes a torn file recoverable: after a power
  > cut the reader discards one partial trailing line and everything before it is
  > sound. It is also the concrete reason v1 stays text — a binary container
  > needs a resync scanner to make the same promise. Host-test it against
  > adversarial payloads, because nothing on the bench will ever catch it.

- **F1f — Ordering: the file is not strictly time-sorted.** The writer drains the
  logger ring, then each gateway tap ring, then the text ring, once per pass, so
  skew is bounded by the writer poll period (`WRITER_POLL_MS`, 20 ms). Every line
  carries `t_us`, so a bounded-window sort recovers exact order. Documented
  rather than fixed: a merge across ring heads would couple the writer to every
  producer for no operational gain.

- **F1g — Byte budget, and why v1 is still text.** The measured ceiling
  (M2 stress run §9.6 (2026-07-26, git history)) is **≈5300–5700 rec/s ≈ 190–211 KB/s with
  `dropped == 0`, at ~36 B/line and CPU ~85 %** — so the binding constraint is
  **CPU, not card bandwidth** (the card does 1–2 MB/s). Production load is
  ~3600 rec/s (that handover's §9.7: a forwarding TWAI node does not receive its
  own transmissions, so the tap sees 3600, not the 7200 its earlier §5 assumed),
  i.e. ~130 KB/s at the old line width — about 30 % headroom.

  v1 spends ~6 B/line more than that: `C,` (+2), the 64-bit timestamp (+1–2 vs a
  wrapping uint32), and a 4-char label instead of a 1-char tag (+3). ~42 B/line
  ⇒ ~151 KB/s at 3600 rec/s. **That headroom is only affordable because D6
  removes every `snprintf` from the writer** — at ~9 format-string parses per
  record, that is where the 85 % CPU goes, and it is the same lever that sets the
  ceiling.

  **Measured on Mr. Orange, 2026-07-28 (`stress-orange-sdrate.yaml`, `sources:`
  declared so the line carries a real 4-character label):**

  | records/pass | rec/s | KB/s | B/line | dropped/s | CPU |
  |---|---|---|---|---|---|
  | 4 | 3 430–3 474 | 135–138 | 41.0 | 0 | 33.5 % |
  | 8 | 6 215–6 234 | 244–249 | 41.0 | 0 | 53 % |
  | 12 | **8 424–8 464** | **337–338** | 41.0 | **0** | 64.6 % |
  | 16 | 9 283–9 388 | 372–376 | 41.0 | ~1 500 | 70–76 % |

  **Ceiling: the writer saturates at ~9 300 rec/s ≈ 373 KB/s.** Past that the ring
  sheds a steady ~1 500 rec/s no matter how hard the producer pushes. The last
  rate that runs entirely clean is **~8 460 rec/s / 338 KB/s at 64.6 % CPU**.

  **Both predictions in this section were wrong in the same direction — v1 is
  better than it promised.** The line does measure ~41 B (inside the ~42 B
  estimate), but where this section expected that width to *cost* something, it
  buys a **1.5× higher record ceiling and 1.75× the byte throughput, at 20 points
  less CPU** than the M1 format's 5300–5700 rec/s / 190–211 KB/s at ~85 %. Removing
  ~9 format-string parses per record is worth far more than 5 extra bytes of line.
  Production load (~3 600 rec/s) now runs at a third of the CPU with **2.3× headroom
  to the clean ceiling**, against 1.5× before.

  (An earlier reading of the first two points extrapolated a ~12 600 rec/s knee.
  That was ~35 % optimistic — CPU is not linear in record rate once the SD path
  and the main loop start competing. The table above is measured; the
  extrapolation is gone.)

  **v2 takes 13 B off that line — 43 B to 30 B, ~30 %.** Measured off the real
  emitter, not estimated, at the bench's own steady state (~1 h uptime, so v1's
  absolute stamp is at its typical 10 digits, and the guest's ~238 rec/s on seg2,
  so the step is ~4200 µs):

  | | line | B |
  |---|---|---|
  | v1 | `C,3600412345,seg1,1A2,x,8,0011223344556677` | 43 |
  | v2, anchor | `C1x,@D699EEB9,1A2,0011223344556677` | 35 |
  | v2, step | `C1x,1068,1A2,0011223344556677` | **30** |
  | v2, step, no flags | `C1,1068,1A2,0011223344556677` | **29** |

  Where the 13 B come from: the stamp column ~6 B (a step instead of an absolute,
  in hex), the label 2 B (`seg1` -> the tag `1`), `dlc` and its comma 2 B, the
  flags comma 1 B, the label and flags commas 2 B.

  **Anchor rate: 0.389 %** — 3892 anchors in 1 000 000 records at 4200 µs spacing.
  Nothing on this profile trips the 1 s resync cap, so every one of those is the
  unconditional every-257th-line re-anchor, and it costs **0.023 B/record, 0.08 %
  of the line**. Amortised width **30.02 B/record**. That is the price of a
  garbled stamp being unable to shift timestamps past the end of its own 256-line
  window, and it is worth paying.

  **And it is faster, which is the same change and not a second one.** v1 spent
  one divide-and-modulo per decimal digit on its widest column — ~10 per record —
  plus one on `dlc`, and up to eight bounds-checked byte writes copying the label
  string. v2 has **none of that: zero `mul`/`div`/`rem` instructions and no
  libgcc division helper in the whole record path**, verified by disassembling it
  for the target ISA (`riscv32-esp-elf-g++ -O2 -march=rv32imac`); the same
  disassembly of v1's shape has 10. Host A/B of the two emitters at -O2:
  **22.2 ns/record -> 17.4 ns/record, 21.6 % less time**. Shorter and faster are
  the same edit here, which is why the format was worth breaking.

- **F1h — The stamp column (v2).** One field, two spellings, **both hex**:
  `@<µs>` is an **absolute anchor**, a bare `<µs>` is a **step from the previous
  stream line of the same file**. The chain runs over record and text lines in file
  order; `#` meta lines are anchor-marked absolutes and stay **outside** it, so a
  reader may skip a `#drop` it does not care about and still decode the records
  after it.

  **Diff-to-previous is the rule, not one option among several.** Every anchor
  costs bytes and works against the point of the column, so anchors exist only
  where correctness requires one — the four cases below and the resync bound. On
  this bench's traffic that is 0.389 % of lines (F1g).

  A step is 4 hex digits where v1's absolute was 10 decimal ones, and — the half
  that matters — it **does not widen as uptime grows**. An absolute stamp is at its
  widest during exactly the long soak where the log is worth having.

  **A delta chain is only safe if every way time misbehaves is handled by name.**
  All four of these are real on this bench, and each one anchors:

  | case | why it cannot be arithmetic |
  |---|---|
  | 32-bit wrap (~71.6 min) | steps are computed on `reconstruct_us()`'s 64-bit value, never the raw stamp — the wrap never reaches the chain |
  | a record older than the line before it | the writer samples the clock once and drains ring after ring (F1f), so this is routine at load; an unsigned step would underflow to ~1.8e19 µs |
  | a card stall | 1672 ms measured with records buffered through it — legal traffic, but past `SD_LOG_RESYNC_US` (1 s) a step is no narrower than the absolute |
  | a chunk boundary | the collection server serves chunks individually and retention deletes old ones, so **every chunk anchors its own first stream line** |

  A fifth anchor is unconditional: at most `SD_LOG_ANCHOR_EVERY` (256) steps hang
  off one anchor. A busy bus never trips the 1 s cap — 4 ms between frames, for the
  whole file — so without it a 32 MB chunk would be one unbroken chain and a single
  garbled stamp would shift every timestamp to the end of it. **Measured cost:
  0.023 B/record, 0.08 % of the line** (F1g), for bounding the damage to one
  256-line window.

  **A reader always recovers exact absolute µs**: the saving is in the file, not in
  what anyone gets back out of it. `script/sdlog.py extract` resolves the chain and
  prints absolute decimal µs, and puts back the label, the flags and `dlc` too —
  those cost ~3600 lines a second on the card and nothing at all in a CSV.

  The chain advances **inside the line formatters, after they have decided to write
  a line**. A chain advanced for a line that was never written is this format's
  quietest failure: the file parses, every column is present, and every timestamp
  after it is wrong by the step of a record that is not there.

- **F1i — Reading it back.** Type-first dispatch keeps the common questions to one
  line of shell, and `script/sdlog.py` (M5) does the rest — parse-check, report
  the `#drop` markers, and flag a missing `#close`:

  ```bash
  script/sdlog.py extract L0000007.LOG --type C   # a clean single-schema CAN CSV
  script/sdlog.py extract L0000007.LOG --type C --label C1 --shed
  grep '^C1'                   L0000007.LOG   # one interface — the token starts the line
  grep '^C[0-9A-F]*s'          L0000007.LOG   # shed frames — the Issue #1 hunt
  grep '^[X#]'                 L0000007.LOG   # firmware's side of the story
  ```

  > **Line-selecting one-liners got *easier*; time-selecting ones are gone.**
  > Everything that identifies a record — interface, kind, flags — is now in the
  > leading token, so `grep '^C1'` is a whole interface and `grep '^C[0-9A-F]*s'`
  > is every shed frame, both anchored at the start of the line. But `awk -F,
  > '$1=="C"'` yields a column of *steps*, and no line-at-a-time filter can resolve
  > those: decoding needs every stream line before it, whatever type it is. That is
  > what `extract` is for — it walks the whole chain and prints absolute decimal µs
  > with the label, flags and `dlc` columns put back.
- **F2 — Rotation:** new file at `max_file_size` and at each boot. Filename =
  `L<7-digit seq>.LOG`, an 8.3 name so it is safe under FatFs with long names
  disabled (`CONFIG_FATFS_LFN_NONE`, which is what ESPHome builds).

  > **The extension changed from `.CSV` with format v1** (F1). It is the only
  > version marker an M1-era file carries — those have no `#sdlog` header line —
  > and it stops a reader having to sniff. **The boot scan must match both
  > extensions**, so the sequence stays monotonic across the change and F2a's
  > failure mode cannot reappear on a card holding files from both eras.

  The sequence is recovered at boot by **scanning the mount point for the
  highest existing `L#######.{LOG,CSV}` and adding one** — no NVS boot counter and no
  RTC, and nothing to keep in sync with the card. The spec originally called for
  an NVS counter; the scan replaced it because a card moved between boards, or a
  card wiped while NVS survives, both make a stored counter lie, and the
  directory is the single source of truth about what is actually on the card.

  > **F2a — the scan requires `CONFIG_VFS_SUPPORT_DIR`, and it is off by
  > default.** ESPHome disables it (~0.5 KB saving; core ESPHome never
  > enumerates directories), and with it off `opendir()` is compiled out and
  > returns `nullptr` **without setting errno** — an entirely silent failure.
  > The component therefore calls `esp32.require_vfs_dir()` from its
  > `_final_validate`; it must be a validator, because the esp32 component reads
  > that flag inside its own `to_code()` and the two coroutines are not
  > priority-ordered against each other.
  >
  > This is not a cosmetic dependency. `open_next_file_()` opens for append and
  > seeds `file_bytes_` from a real `ftell()`, so a scan that wrongly answers 0
  > makes every already-full file trip the `max_size` check the instant it is
  > opened: the writer walks the whole existing file set one `fopen` at a time
  > (~14/s measured) and **every record produced during the walk is dropped**,
  > getting one file worse on each boot. Measured on the bench before the fix:
  > 201 693 records lost across a 397-file walk. After: 1 rotation, `dropped ==
  > 0`. Any future change to how the sequence is recovered must keep a loud
  > failure path — `scan_next_seq_()` logs a warning naming the consequence.

- **F2b — sequence exhaustion is not handled.** `L9999999.LOG` is the last
  representable name; the counter is `uint32_t` and `open_next_file_()` would
  start emitting names wider than 8.3. At the M2 rotation rate (one 32 MB file
  per ~3 min at 171 KB/s) that is years away, and cards are wiped long before —
  but it is unbounded by design, so it is written down rather than assumed safe.
- **F3 — Partition:** one card-spanning FAT32 partition. **Small partitions are
  explicitly discouraged** (they restrict wear distribution). Use
  `format_if_mount_failed` as *recovery only*, never as routine.

---

## 7. Close-file emergency on VCC sag (the requested feature)

The honest framing first: **a FAT filesystem is not transactionally
power-safe, and a bare brownout gives µs–low-ms while a FAT flush + the card's
own internal write can take tens to ~500 ms.** So this feature is **two
independent layers** — the first is the real guarantee, the second is a
best-effort nicety that only works with hold-up energy.

### Layer A — crash-safe write discipline (always on, the real protection)
Pre-allocated contiguous file (D6) + append-only + bounded `f_sync` cadence
(D7) means: a power cut at **any** instant loses **at most the last un-synced
batch**, and the next boot mounts and continues (or, worst case,
`format_if_mount_failed` recovers). This makes a clean close a *bonus*, not a
correctness requirement. **Ship Layer A even if the PCB has no hold-up cap.**

### Layer B — the emergency close (best-effort, needs hold-up energy H6)
Detect the supply sagging **early**, then do the minimum to close cleanly
before the rail collapses.

- **Detection — three options, recommend #1:**
  1. **Software ADC on the 12 V (J8) rail** via a divider to a free ADC1 pin
     (H4). Sample from a high-priority timer/task; trip the threshold **above
     the 3V3 regulator dropout** so warning arrives while 3V3 is still valid.
     The 12 V bulk cap discharges slowly → **most lead time**. Recommended,
     since these boards are 12-V-fed (the bench already treats USB-only power as
     "broken wiring").
  2. **Hardware brownout detector (BOD) in interrupt mode** on 3V3 — enable IDF
     BOD as an *interrupt* (not reset), attach a handler. Fast but **late**
     (fires near 2.5–3.0 V, little time left). Use as a last-resort trigger, not
     the primary.
  3. **External supervisor / comparator** driving a PWRGOOD GPIO interrupt —
     most deterministic, costs a BOM part.
- **Handler discipline:** the detector (ISR/BOD/timer) does **only** set an
  atomic `dying` flag and notify the writer — no filesystem work in ISR context
  (same ethos as the repo's ISR-flash-audit discipline). The writer, on seeing
  `dying`: stop accepting records → flush the block-aligned remainder →
  `f_sync` → `f_close` → `unmount` → (optional) de-assert `card_power_pin`.
  **Every step bounded** (timeouts, like `linbus` `sendBreak`'s 20 ms cap) so
  the path can never outlive the hold-up budget.
- **Hold-up sizing (HW ask on the PCB repo):**
  `E = ½·C·(V0² − Vmin²)` must cover
  `(worst-case in-flight SD write completion) + f_sync + f_close` at the 3V3
  load current. Worked starting point: budget ~150–500 ms of hold-up at the
  board's 3V3 current; a series diode prevents the cap back-feeding the sagging
  input rail. **This is a hardware requirement the firmware cannot substitute.**
- **Card power switch (H5):** with a high-side switch on card VCC, the emergency
  path can also cut card power *after* unmount so the card is never mid-write
  when the rail finally dies; and normal operation can hard-recover a wedged
  card by power-cycling it.

**Stated plainly for the record:** even with all of the above, worst case is the
loss of the last batch and an occasional `format_if_mount_failed` on next boot.
Layer A bounds that; Layer B lowers its probability. Do not promise more.

### Layer C — recovery (always on by default, and the one that pays daily)
Layers A and B are about the *end* of a run. Layer C is about the middle of one:
a card that failed does not stay failed for the rest of the boot.

Three failures used to be permanent, and all three are now retried:

| failure | what it looked like before |
|---|---|
| mount fails at boot | `mounted=NO (logging disabled)` for the whole run |
| a `write()` fails mid-run | file dropped, `mounted_` false, nothing ever retried |
| the post-rotation `open()` fails | **worse than permanent** — `mounted_` stayed *true*, so producers kept pushing into an `fd` of −1 and every record vanished without touching a counter |

The writer task owns recovery, which is why it now starts even when the card
never mounted. On a failure it takes the file down through the single
`enter_failed_()` path — the one place that can make `mounted_` and `fd_`
disagree, and now the one place that cannot — and arms a doubling retry ladder
(`recovery_policy.h`, host-tested). Each attempt unmounts, frees the SPI bus,
optionally drops card power for 150 ms, remounts, rescans the sequence and opens
a fresh file. **A retry never formats**, whatever `format_if_mount_failed` says
(V21).

**The step that actually addresses a wedge is the in-band reset**
(`card_reset.h`, V27–V28, host-tested), from the second attempt on — the first
stays a plain remount, which is all a transient failure needs. What a reset
mid-write leaves behind is not a dirty filesystem and not a card that lost its
supply: it is a card sitting inside an open CMD25, in Receive-Data state, waiting
for either another data token or the Stop Tran token. A CMD0 frame is neither, so
its bytes are consumed as write data and never answered — which is why the SD
spec's reset-without-repower procedure, which ESP-IDF already performs on every
mount (80 clocks with CS deasserted, then CMD0), cannot clear it. The sequence
sends the stop token, waits out busy on a budget the config sets instead of the
driver's fixed 40 ms (`wait_for_miso`, capped at 127 ms), then CMD12, CMD0 and
CMD13. It reports the measured busy time and the card's own status byte, which
are the two numbers that separate "wedged", "merely slower than 40 ms" and
"absent" — a distinction the bare `mount failed: ESP_ERR_TIMEOUT` never carried.

**Proven on hardware 2026-07-29** (`tests/hil/probe-orange-wedge.yaml`, Mr.
Orange, log in wedge run (2026-07-29, 12/12, git history)). The control run — the same
wedge with `in_band_reset: false` — left the card down through six plain
remounts, so the failure is real and a remount cannot clear it. With the reset
armed, **12 of 12 staged wedges recovered without touching the card's power**,
ten on attempt 2 and two on attempt 3; the real stress rig cut by a reflash
mid-write at 6400 rec/s was logging again 3.1 s after boot. Every successful
attempt reported `idle at cmd13 … status=0x00`.

Two things the bench corrected about this design. **Busy was 0 ms on every
cycle** — the card was sitting in the open CMD25, not programming, so the stop
token is the entire mechanism and the busy budget is belt-and-braces; the
"patience bug" hypothesis (a card slower than the driver's fixed 40 ms) is dead
for this card. And **`no response at cmd0` and `mute at cmd0` both turned out to
be transient**, each appearing once and clearing on the next attempt — neither
is evidence that the card is latched, and neither justifies fitting H5. That is
why the ladder retries the reset rather than reporting the first outcome as
final.

The power cycle was originally documented here as "the firmware doing what
'cycle 12 V on J8' did by hand". It never was: see H5 — the switch it drives was
never built. The branch is kept for boards that do fit one.

#### Layer D — the latched idle bit (2026-08-28, and it removed the last reason to want H5)

A second wedge, distinct from the open CMD25 above and immune to every part of
Layer C. After a soft reset the card keeps R1's "in idle state" bit set forever:
5455 ACMD41 polls over 60 s, HCS set and clear, the full voltage window, a 1 s
settle, twenty CMD0s and CMD1 — none of them clear it. Meanwhile the same card
reports `OCR=0xC0FF8000` with its own power-up-complete flag **set**, returns
CSD and CID, serves CMD17 block reads with a valid 55AA signature, and accepts
CMD24 writes at 400 kHz and 10 MHz. It is a working card with a latched
handshake, and only removing its power unlatches it.

ESP-IDF has no way to be told that, and treats the bit as fatal in exactly two
places: `sdmmc_send_cmd_send_op_cond()`, which polls it 300 times and returns
ESP_ERR_TIMEOUT, and `sdmmc_write_sectors_dma()`'s post-write CMD13 gate, which
demands `status == 0` while SPI-mode `SD_SPI_R2()` puts R1 in that status's low
byte. Fix only the first and the card mounts read-only.

So `mount_card_()` answers an `ESP_ERR_TIMEOUT` mount by running
`card_probe_ready()` (card_reset.h) — OCR bit 31 **and** a real block read — and
only on that evidence installs a `do_transaction` wrapper that clears bit 0 of
those two responses, armed for the life of the mount. Every other bit, including
R2's whole high byte, passes through untouched, so a card that is genuinely
failing still fails and an absent one is refused exactly as before. This is why
H5 is no longer worth fitting: the case it was wanted for is now handled in
software, on evidence, with no hardware.

Nothing is drained while there is no file. Producers are gated on `mounted_` and
count their losses into a fourth counter, `card_dropped` — separate from
`dropped`/`tap_dropped`/`text_dropped` for the same reason those are separate
from each other: it names a different fault (*nowhere to put it*, not *too
slow*). Its `#drop,…,card,…` marker is the **only** baseline `write_file_header_()`
does not reset, because the loss it reports happened while there was, by
definition, no file to report it in — carrying it across makes the first marker
pass in the recovered file state the cost of the gap.

---

## 8. Config schema & validation rules (repo Vxx discipline)

Enumerate numbered validators (matching the `can_gateway` convention), each with
an accept-path and reject-path schema test (§10):

- **V1** exactly 4 SPI pins, all distinct, all valid GPIO; CS distinct from the
  data pins.
- **V2** `clock` ≤ `SDMMC_FREQ_DEFAULT` (20 MHz) — reject higher with the H1
  reason (do not silently clamp).
- **V3** `vcc_monitor.adc_pin` must be an **ADC1-capable** pin (GPIO0–6, H4)
  when `source: adc`; reject otherwise with the pin list.
- **V4** `vcc_monitor.threshold` and the divider must be consistent with the ADC
  full-scale (reject a threshold the divider can't represent).
- **V5** `buffer_depth` power-of-two within [256, 16384]; warn if the ring is
  smaller than one worst-case latency-spike burst at the highest configured CAN
  bit rate (borrow the frame-rate estimate from the source `can_gateway`).
- **V6** `sync_interval` positive; **warn** if it is long enough that the
  worst-case loss window exceeds a documented bound (ties the knob to §7).
- **V7** every `sources:` entry references a declared `can_gateway`/`linbus` id.
- **V8** `card_power_pin`, if present, distinct from all SPI/ADC pins.
- **V9** `only_on_variant` C6 + IDF ≥ 5.5 (component-level, mirrors siblings).
- **V10** reject `IMMEDIATE_FSYNC` being force-enabled elsewhere together with a
  configured `sync_interval` (contradictory intent) — or at least warn.
- **V11** `can_ports` source tags unique, a port tapped at most once, tag 0
  reserved for the action (shipped).
- **V12** a tapped port must carry `log_tap: true` on the `can_gateway` side —
  final-validate against the resolved config (shipped).

New with format v1 (§6). Each still needs both an accept and a reject test:

- **V13** `label` matches `[A-Za-z0-9_-]{1,8}` and is unique across `can_ports`
  **and** `sources` — two wires answering to one label makes every line
  ambiguous, and the file is the only artifact that survives the run.
- **V14** `sources[].tag` in 0–255 and disjoint from every `can_ports` tag;
  `kind` ∈ {`can`,`lin`,`user`}. Same reasoning as V11, other direction.
- **V15** `esphome_logs:` requires the `logger:` component — without
  `request_log_listener()` the callback compiles to a **no-op** and capture
  silently does nothing (§4a).
- **V16** *warn* when `logger.level` is more restrictive than
  `esphome_logs.level`: capture can never see what the logger already filtered.
- **V17** *warn* when `logger.task_log_buffer_size: 0` — messages from non-main
  tasks never reach listeners, which is exactly the writer and VCC-monitor tasks.
- **V18** `esphome_logs.buffer_depth` a power of two in [8, 256].

New with card recovery (§7 Layer C):

- **V19** `recovery.power_cycle: true` requires `card_power_pin:` — without a
  high-side switch there is nothing to cycle, and a card wedged mid-write does
  not come back from a remount alone. It **defaults** from whether the pin is
  present, because cycling the card is the reason the switch is on the board.
- **V20** `recovery.initial_delay` and `recovery.max_delay` in [100 ms, 300 s],
  and `max_delay >= initial_delay` — the ladder doubles from the first to the
  second, so a lower maximum would step backwards. Below 100 ms a retry lands
  inside the card's own power-on ramp and fails for that reason alone.
- **V21** *warn* when `format_if_mount_failed: true` and recovery is enabled:
  retries deliberately **never** format, only the boot mount does. A ladder that
  reformatted would erase the logs it is being run to save, once per attempt.
- **V27** `recovery.busy_timeout` / `recovery.max_busy_timeout` must be between
  100 ms and 60 s. Below 100 ms the sequence is no more patient than the SDSPI
  driver's own 40 ms, which is the limitation it exists to lift; above 60 s one
  attempt outlasts what `on_shutdown()` can wait for the writer.
- **V28** `max_busy_timeout` cannot sit below `busy_timeout` — the budget doubles
  up to the maximum on each further attempt, so a lower maximum would hand a card
  that needs more patience less of it. Same shape as V20.

**Honest status:** V4, V9 and V10 are listed above and **not implemented**
(`components/sd_logger/__init__.py` says so too). V9's variant/framework guard is
enforced by `only_on_variant` + `require_framework_version` at the component
level rather than as a numbered validator. V13-V18 landed 2026-07-28 with format
v1; V16 and V17 are warnings by design, so they are asserted by capturing the
warning rather than by a rejection. V19-V21 landed 2026-07-28 with card
recovery; V21 is likewise a warning.

---

## 9. Interaction with existing constraints & open issues

- **Issue #2 (cyclic ~16 ms floor):** the logger must keep **all** SD work off
  the main loop (C2b/D5). Done right it adds **zero** main-loop jitter and does
  not worsen #2.
- **Issue #1 (RX-loss while a sibling port cycles bus-off):** the logger
  faithfully records whatever the observe path delivers. During that bug's
  window it logs **nothing** for the affected port. Note it in the docs so a
  gap there is not mis-attributed to the logger dropping frames.
- **TX-safety hard rule:** logger never transmits — satisfied by construction
  (G2).
- **ISR-priority interplay:** keep SPI on **DMA** (short ISRs) and keep the
  gateway `interrupt_priority` ≥ the SD-SPI ISR priority so TWAI RX-ISR latency
  is not stretched at high CAN frame rates.
- **RAM budget:** ring (~64 KB) + FAT work buffers (~4–8 KB) + writer stack
  (~4 KB) + a sector buffer, **on top of** the ~8 KB the gateway already spends
  forcing FreeRTOS into IRAM. The C6 has 512 KB SRAM — comfortable, but budget
  it and watch the build's RAM report.
- **external-component-safe:** emit `USE_SD_LOGGER`, ring/buffer sizes, and any
  `FF_*`/`FATFS_*` needs via `cg.add_define` / `add_idf_sdkconfig_option`;
  `include_builtin_idf_component("fatfs")`. No core edits.

---

## 10. Definition of done (mirrors CLAUDE.md)

1. Schema tests in `tests/sd_logger/` — **accept and reject** paths for every
   Vxx in §8.
2. A `tests/build/sd_logger/` yaml exercising every new key/action so CI runs
   `esphome config` + `esphome compile` on it (C6 target).
3. `pytest` green + `esphome config` green on all build yamls before commit.
4. C++ touched → `esphome compile` at least one C6 target locally.
5. clang-format clean. **Format changes also mean host cases in `tests/host/`:**
   `log_format.h`/`log_record.h`/`tap_translate.h` are ESPHome- and IDF-free
   precisely so they can be tested there, because a mis-encoded field yields a
   perfectly well-formed file that no bench run will ever flag (§6 F1e).
6. **HIL (release gate):** add `sd_logger` to **Mr. Orange** (already CAN
   gateway + LIN listener → the ideal host), logging both buses to the card, and
   run `verify.py --seconds 45 --min-laps 75`. Assert **both**: the ring stays
   green (logger must not regress the 19.4 laps/s baseline) **and** the
   `dropped_records` counter stays at 0 under the ~40 %/segment ring load.
7. **Power-loss procedure (manual/HIL, not CI):** with 12 V on J8, pull power
   mid-log; confirm the file replays valid up to the last synced batch and the
   next boot mounts clean. Document as a repeatable bench step in
   `tests/hil/HIL.md`.

---

## 11. Open decisions for Jan

- **VCC-sense path** — recommend the **12 V-rail ADC + hold-up cap** (#1 in §7);
  depends on the PCB.
- **Record format** — ✅ **decided 2026-07-27: text, one file, typed lines**
  (§6 v1). Binary stays M4 and, per the measured ceiling, is not needed for the
  production topology. Also decided in the same pass: everything in one file
  including ESPHome's own log (S4), and sources marked by **type letter + label**
  rather than the bare numeric tag.
- **Component shape** — recommend **standalone `sd_logger` + sink hooks**, over
  making it a `can_gateway` platform (LIN also needs to feed it).
- **CAN tap point** — recommend the **observe drain (loop context)** for the
  MVP, but spec the ring **ISR-safe** anyway so a future ISR-side tap is a drop-in.

---

## 12. Milestones

- **M0 — PCB precondition — ✅ DONE 2026-07-26.** Pins traced from the PCB repo's
  `production/netlist.ipc` and recorded in `docs/bench-hmi-wiring.md`; RS-485
  cannot clash because U20 is removed from the board. Card wired and proven on
  Mr. Orange. Hold-up is the one part that is **not** satisfied — D6 is a shunt
  clamp, not a series element, so there is no hold-up energy on this board (see
  "Not covered by software" in `docs/TODO-bench-hmi.md`).
- **M1 — Standalone logger:** SDSPI mount (H2 settle delay), `sd_logger.log`
  action, ring + writer task, text format, rotation. Gate: config + compile.
- **M2 — Native sources + HIL:** `can_gateway` (S2) and `linbus` (S3) sinks,
  `dropped_records` sensor, HIL on Mr. Orange, wired into the release gate.
- **M3 — Power loss:** Layer A discipline + Layer B (VCC-sag detect + hold-up +
  card-power switch); documented power-loss bench procedure.
- **M4 — Later:** binary encoding of the v1 model (§6 F1g — *not* needed for the
  production topology), decode/DBC-aware logging, time sync.
- **M5 — Write format v1 (§6) — specified 2026-07-27; steps 1-6 built
  2026-07-28, steps 7-8 outstanding (they need the bench).** In order:

  1. ✅ **`components/sd_logger/log_format.h`** — ESPHome-free and IDF-free, for
     the same reason `log_record.h` and `tap_translate.h` are: the formatter is
     where a wrong byte produces a well-formed, quietly wrong file, which is
     exactly what a bench run does not catch. Holds the emitters, the escape
     pass, the source-table lookup, `reconstruct_us()` (D8) and the block buffer
     (D6). It earned its keep immediately — see the D8 correction above.
  2. ✅ **`tests/host/test_log_format.cpp`** — 40 cases, picked up automatically
     by the Makefile's `wildcard test_*.cpp`. A golden line per type; the
     **never-a-bare-newline** invariant (F1e) under adversarial payloads;
     cap/truncation; ANSI + bracketed-thread prefix stripping; unmapped-tag
     fallback; `reconstruct_us` across the 2^32 boundary *and* for a record
     stamped after the clock sample; the block buffer's "every flush is a whole
     number of sectors" invariant end to end; and `parse_log_seq` over both
     extensions.
  3. ✅ **Writer rewrite** in `sd_logger.cpp`: stdio → `open`/`write`/`fsync`/
     `close`, block buffer at the write cursor, header/meta emission, `#drop` on
     counter change, `#close` on *every* exit path — rotation, `on_shutdown()`
     and `emergency_close_()`, which now share one `close_file_(reason)`.
  4. ✅ **Text ring + logger listener** (D9, §4a): a second ring of 192 B slots,
     `add_log_callback` in `setup()`, `request_log_listener()` in `to_code`, and
     a `text_dropped_` counter surfacing as `#drop,…,text`.
  5. ✅ **Schema**: `can_ports[].label`, `sources:`, `esphome_logs:`; V13–V18 with
     accept and reject tests in `tests/sd_logger/` (48 new cases); every new key
     exercised in both `tests/build/sd_logger/common*.yaml`. The action's
     `source:` now also takes a declared label.
  6. ✅ **`script/sdlog.py`** — the counterpart reader: parse-check, report
     `#drop` markers apart, flag a missing `#close`, extract one type as a plain
     CSV, `head` the self-describing header. `--strict` is the bench-gate
     spelling (a drop or a missing `#close` fails). Tested against the same
     golden lines the host suite pins the writer to, so the two halves cannot
     drift — `tests/sd_logger/test_sdlog_reader.py`.
  7. ✅ **Re-measure the ceiling** — done 2026-07-28 on Mr. Orange, full sweep in
     the F1g table. v1 saturates at **~9 300 rec/s / 373 KB/s** and runs clean to
     **~8 460 rec/s / 338 KB/s at 64.6 % CPU**, against the M1 format's
     5300–5700 rec/s / 190–211 KB/s at ~85 %. The wider line more than pays for
     itself. `stress-orange-sdrate.yaml` now ramps its own rate and stops on the
     first drop, so the whole curve costs one flash instead of one reset per
     point.
  8. ⬜ **Then** update `tests/hil/HIL.md`'s readout criterion and the bench
     configs' comments. Now unblocked on the measurement side; still wants **one
     card read back on a host** first — nothing on the board can read its own
     file, so the bytes v1 actually writes are verified only by the host cases
     and `tests/sd_logger/test_sdlog_reader.py`, never end to end. The criterion
     should become `script/sdlog.py check --strict`.

### M6 — collection over WiFi ("store everything, upload automatically")

Designed 2026-07-28 in **`docs/sdlog-collection-design.md`**; nothing built.
Summary of what that document settles, because each point closes off a plausible
wrong turn:

- **There is no PSRAM tier and cannot be** — the C6 has no SPIRAM interface, and
  DMA would relieve card bandwidth, which §3 H1 already shows is not the
  bottleneck. **The card is the buffer.**
- **One invariant carries the design:** the writer only appends to the open
  file, the collector only reads sealed files, never the same file. That removes
  the FATFS-lock and card-GC contention without any buffering.
- **Pull, not push** — the device serves sealed chunks over its own
  `esp_http_server`; the puller owns retries, dedup and the archive. `Range`
  makes transfers resumable, which the push direction cannot match for free.
- **Collected state is a rename** (`.LOG` → `.UPL`), never an in-file edit, and
  the order `serve → confirm → rename` is the one that cannot lose data.
- **Retention drops oldest un-collected** and states the hole in-band with a
  `#gap` line, the same way `#drop` markers already state theirs.
- Rotation gains a time bound, and chunks shrink to 1–4 MB — which means the
  rotation path, still never fired on hardware (§4b.5), starts firing every few
  seconds.

---

## 13. Sources (pitfall research, 2026-07-22)

- ESP-IDF FATFS (C6), `CONFIG_FATFS_IMMEDIATE_FSYNC`, small-partition warning,
  `disk_status_check_enable`:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/storage/fatfs.html
- ESP-IDF SD SPI host (C6), ≤`SDMMC_FREQ_DEFAULT` over SPI, `spi_bus_initialize`
  ordering, bus sharing:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/sdspi_host.html
- C6 boot quirk — ms delay between `spi_bus_initialize` and mount; SD-SPI init
  pitfalls: https://esp32.com/viewtopic.php?t=38014 ,
  https://github.com/espressif/esp-idf/issues/1362
- SD write latency spikes (~5 ms typical, ~320 ms, up to ~0.5 s) + ring-buffer /
  dedicated-drain pattern: https://esp32.com/viewtopic.php?t=30232 ,
  https://calsol.berkeley.edu/2011/03/04/optimizing-sd-card-writes/
- Brownout interrupt mode + ADC Vcc monitoring divider:
  https://esp32.com/viewtopic.php?t=6855 ,
  https://www.robmiles.com/journal/2020/1/20/disabling-the-esp32-brownout-detector
- Hold-up cap / supercap clean-shutdown strategy (early-warning + small hold-up
  + fast critical flush): https://forums.raspberrypi.com/viewtopic.php?t=253104 ,
  https://thecavepearlproject.org/2017/05/21/switching-off-sd-cards-for-low-power-data-logging/
- ESPHome SD prior art is ESP32/S3-only (SDMMC), does not cover the C6:
  https://github.com/n-serrette/esphome_sd_card
</content>
