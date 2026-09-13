# Handover — M6 headroom: where it went, what buys it back. 2026-07-29

Follows M6 load run §11 (2026-07-29, git history), which measured the thin headroom this session
was asked to explain: tap records lost in stall-shaped bursts, heap
min-since-boot at 1668 B, CPU 62–75 %, and a server that answers nothing else
for the length of a transfer. This document is the root-cause analysis (three
independent code audits plus an external-sources sweep), the fix that came out
of it, and the ranked list of levers not yet pulled.

The one-line verdict: **nothing is out of capacity — the losses are a
coupling problem.** The tap consumer lived on the one task that spends
hundreds of milliseconds inside card calls, and the backpressure that should
have protected it watched a ring the production traffic never touched.

---

## 1. The mechanism, assembled

§11 blamed a 169 ms *main-loop* stall overrunning a 146 ms tap ring. That
number was real but it was the shadow, not the object. The chain, with every
link verified in source:

1. **The tap drain never ran in the main loop.** Its only consumer was the
   *writer task* (`drain_can_taps_`, called from `writer_loop_` and nowhere
   else that matters). So tap survival was coupled to writer-task latency, not
   loop latency.
2. **The writer episodically disappears for 220–360 ms**, inferred from burst
   sizes (a 1493-record burst in one ring at ~3500 f/s is a ~430 ms outage
   including the fill time; the smaller bursts price 220–360 ms). Worse than
   the 169 ms the loop instrument printed — the loop was a *correlated victim*
   of the same event, not the mechanism.
3. **The event is a card-internal stall spun through a no-yield busy-wait.**
   IDF's sdspi driver polls the card's busy state with
   `spi_device_polling_transmit` in a do-while — no `vTaskDelay`, no yield —
   at the caller's priority, for up to `SDMMC_WRITE_CMD_TIMEOUT_MS` = 5000 ms
   per data block (`esp_driver_sdspi/src/sdspi_host.c:611-647`, spin sites at
   `:988`, `:1011`, and the pre-command MISO wait at `:506` which sd_logger
   raises to 127 ms). A consumer SD card that is fed interleaved
   read/write streams — exactly what a transfer during logging is — runs its
   write-cache flush / allocation-unit GC for 100–400 ms at a time
   (canonical numbers; pure sequential append, which every writer-only soak
   was, is the card's happy path, which is why no earlier run saw this).
4. **The FATFS volume mutex serializes everything behind that spin.** One
   FreeRTOS mutex per volume, held for the *entire* `f_read`/`f_write` call
   (`fatfs/src/ff.c` — `lock_volume` at entry, released at `LEAVE_FF`;
   `FF_FS_REENTRANT` hardwired on in IDF). Priority inheritance applies but
   does not help: the writer *holds* the mutex while spinning, so the httpd
   task waits out the full stall, and the transfer pauses with it.
5. **Everything below priority 5 starves meanwhile** — the ESPHome loop (1)
   prints its `[loop] max`; the idle task starves invisibly
   (`ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0` is off). LIN (18) and WiFi (23)
   preempt the spin, which is exactly why both stayed clean through every
   burst. The measured split — `tap_dropped` moving, everything else
   pristine — is this chain and nothing else.

Cross-checks that carried the diagnosis:

- 245 KB/s (read under load) + 163 KB/s (write) ≈ 415 KB/s (read idle): the
  throughput split is pure time-slicing on the volume mutex, no capacity
  missing.
- Burst count (6 in 11 minutes) matches episodic card GC, not a per-operation
  cost. The first ~8 transfer minutes lost nothing.
- CPU never left 62–75 % — a quarter idle. Neither CPU nor SPI nor the card
  was exhausted. Latency, not throughput, start to finish.

## 2. The finding inside the finding: §6's PASS was trivially true

`backpressure_()` throttles a transfer on `ring_fill_percent()` — the *record
ring's* occupancy. In the §4.3 rig the record ring had almost no producers:
tap traffic bypassed it entirely (tap ring → `write_record_` directly, in the
writer), and LIN fed a lambda that set a timestamp. The ring sat at ~0 %, the
backpressure never slept once, and `dropped_records == 0` — §6's criterion —
was satisfied by a ring nothing was using. The criterion is right; the
plumbing made it unfalsifiable.

## 3. The fix (this branch)

**A dedicated tap-drain task** (`sdlog_tap`, priority 7, 3 KB static stack in
.bss — the heap floor is 1.6 KB, the .bss has 65 % headroom): pops the tap
rings and feeds `push_record()`, i.e. the record ring, which never touches the
card. The writer keeps draining the record ring into the file exactly as
before. What this buys, at zero new heap and ~1–2 % CPU:

- The tap rings' consumer gap collapses from "however long a card stall lasts"
  to the drain task's scheduling latency (~5 ms cadence, preemptible only by
  LIN/WiFi/lwIP bursts). 1024 slots against that gap is oceanic.
- The **already-paid, previously-idle 80 KB record ring becomes the stall
  absorber**: 4096 records ≈ 585 ms at the full ~7000 f/s — sized to ~2× the
  worst observed outage, per the rule "size against the writer's real gaps
  (220–360 ms), not the loop instrument's 169 ms".
- **Backpressure becomes real**: tap traffic now flows through the ring that
  `backpressure_()` watches, so a transfer gets throttled exactly when the
  writer falls behind. The §6 feedback loop closes for the first time.
- **Every drop counter keeps its meaning.** The drain gates on `mounted_`:
  card outages still park records in the tap rings and count into
  `tap_dropped` at the ISR, as before. A full record ring counts into
  `dropped_records`, which is precisely §6's counter. On a full ring the drain
  stops its pass and leaves the remaining records in the tap rings — another
  ~146 ms of second-stage buffer instead of a drop-counter feed.

**Instrumentation, permanent** (the numbers §11 had to infer are now
measured):

| instrument | where | what it names |
|---|---|---|
| `write() held N ms` | `flush_block_` | the card stall itself, at the call that spins it |
| `fsync() held N ms` | `sync_file_` | FAT/dir-sector writes sending the card into GC |
| `read <chunk> held N ms` | `serve_chunk_` | the httpd side inheriting the mutex wait |
| `writer pass gap N ms` | `writer_loop_` | the writer's total outage, the number every ring is sized against |

All at the same 50 ms threshold ESPHome uses for loop blocking, so one grep
collects a stall's whole signature. Expected pattern under a §4.3 rerun:
`write() held` + `writer pass gap` + `read held` clustered on one timestamp,
`tap_dropped` **not** moving.

**Host test** (`tests/host/test_tap.cpp`): a virtual-time consumer-stall
simulation — depth 4096 survives 500 ms at 7000 f/s with zero refusals, depth
1024 loses ~2500 records, reproducing §11's burst arithmetic exactly. No
threads, no flake; it pins the sizing rule, not the scheduler.

Not changed, deliberately: `log_tap_queue_depth` stays 1024 (raising it to
4096 on both ports would cost 123 KB that does not exist — the §11 suggestion
"1024 → 4096 buys ~580 ms for ~80 KB" undercounted by 2× and against the wrong
budget); `buffer_depth` stays 4096 (it is now load-bearing); the rig YAML is
byte-identical, so the A/B against §11 is clean.

## 4. Where the heap actually goes

No leak. Boot-to-baseline (88.8 → 31.5 KB) is WiFi association (~51 KB) plus
the server (~11 KB). The walk from 31.5 KB to the 1668 B floor is **WiFi
dynamic TX buffers** — up to 32 × ~1.6 KB stacking during radio-contention
bursts while a transfer is in flight, all freed afterwards. That is a
transient with a 51 KB ceiling colliding with a 30 KB baseline. The knob is
`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` (see §5); nothing in sd_logger or
can_gateway is fragmenting anything.

## 5. Levers not yet pulled, ranked — the tips-and-tricks catalog

Everything here is YAML `sdkconfig_options` or our-own-component code;
nothing needs an esphome-core patch. Links kept per the house rule.

**Rig/config tier (one line each, try first):**

1. `wifi: power_save_mode: none` — ESPHome's ESP32 default is LIGHT
   (`WIFI_PS_MIN_MODEM`, DTIM sleep, up to ~beacon-interval RX latency).
   Plausibly a chunk of the 415→245 KB/s gap and assorted latency spikes; zero
   heap cost; power is irrelevant on 12 V.
   <https://esphome.io/components/wifi.html>
2. WiFi buffer caps for the heap floor:
   `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM: "18"`,
   `DYNAMIC_RX_BUFFER_NUM: "12"`, `STATIC_RX_BUFFER_NUM: "6"` — Espressif's
   own "minimum rank" for C6 is 6/6/3, so this is a conservative middle.
   Directly raises the measured 1668 B floor by capping the transient.
   <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/wifi-driver/wifi-performance-and-power-save.html>
3. `CONFIG_LWIP_TCPIP_CORE_LOCKING: "y"` — in Espressif's C6 iperf defaults,
   cuts tcpip-task context switches; and note
   `CONFIG_LWIP_TCPIP_TASK_PRIO` defaults to **18, an exact tie with
   `lin_uart_evt`** — lowering lwIP to 17 is the one-line experiment that
   turns §9.1's tie into an ordering, if LIN jitter is ever observed again.
   <https://github.com/espressif/esp-idf/blob/release/v5.5/examples/wifi/iperf/sdkconfig.defaults.esp32c6>
4. `sync_interval: 1s → 5s` in the stress rig — every fsync writes FAT and
   directory sectors away from the append stream, the exact access pattern
   that triggers card GC. Fewer triggers, fewer stalls; costs 5 s of tail on
   power loss. The new `fsync() held` instrument prices it.
5. `clock: 10MHz → 20MHz` (schema ceiling, and the driver's own cap —
   `SDMMC_FREQ_DEFAULT`; SDSPI cannot go higher on C6). Roughly halves every
   mutex hold. Signal-integrity risk on the bench harness is the reason it
   sits at 10; try once, watch `bus_err`.
   <https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/sdspi_host.html>
6. An A1-class/industrial card. Consumer-card GC is the stall; A1 cards bound
   random-write latency by spec. One card swap, then read the
   `write() held` histogram again.

**Component-code tier (each is its own change with its own proof):**

7. **Serve-path pacing**: `SD_LOG_SERVE_BLOCK` 4 KB → 16–32 KB read into a
   DMA-capable buffer, with `vTaskDelay(1)` between blocks. Fewer, longer
   mutex holds beat many short ones for throughput while the delay keeps the
   writer's entry latency bounded; IDF's own file-serving example (8 KB, no
   yields) maximizes contention by design — do not copy it.
   <https://github.com/espressif/esp-idf/blob/master/examples/protocols/http_server/file_serving/main/file_server.c>
8. **`/sdlog/status` during transfers**: either a second httpd instance on
   another port for the cheap routes (in-tree precedent:
   `esp_https_server` coexists via `ctrl_port+1`; ~4 KB stack + a few lwIP
   FDs) or the official async-handler pattern
   (`httpd_req_async_handler_begin/complete`, IDF ≥ 5.2, worker-task example
   upstream). The async route costs worker stacks against a thin heap —
   second-instance is the better first move here.
   <https://github.com/espressif/esp-idf/tree/master/examples/protocols/http_server/async_handlers>
9. **Writer batching + contiguous preallocation**:
   `esp_vfs_fat_create_contiguous_file()` (wraps `f_expand`) at rotation
   removes FAT-chain allocation from every subsequent write; batches of
   16–32 KB give the card long single-sided runs (CMD25 multi-block) instead
   of GC-thrash. SdFat's logger examples measured max write latency dropping
   ~33 % from preallocation alone.
   <https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/storage/fatfs.html>
10. **`CONFIG_FATFS_USE_FASTSEEK: "y"`** — read-only opens get a CLMT, making
    `lseek` into the 64 MiB legacy chunks O(1) instead of a FAT-chain walk
    *under the volume mutex*. Matters exactly for ranged re-pulls of big
    chunks.
11. **`CONFIG_WL_SECTOR_SIZE_512: "y"`** — FF_MAX_SS is 4096 via the
    wear-levelling default, so every open FIL carries a 4 KB sector buffer for
    a card whose sectors are 512 B. ~3.5 KB back per open file.

**Verified dead ends, so nobody re-walks them:**

- Two FATFS volumes/partitions to split the lock: the sdmmc layer beneath has
  *no* lock — two volumes on one card means unserialized sector ops =
  corruption; and no IDF mount helper does SD partition-N anyway.
- Making sdspi's `poll_busy` yield: both polling flags are hardwired `true`
  in the driver (`sdspi_host.c:69-70`), no Kconfig — fixing it means patching
  IDF, which this repo does not do. Worth an upstream issue.
- Raising any app task to ≥ 20: destabilizes RF per Espressif's own guidance.
- PSRAM tier of any kind: the C6 has none (design doc §1 already says so).

## 6. What the bench owed — and answered, same day

Both gates ran on 2026-07-29 evening, on the real §4.3 bench (Green generating
88.8 % on seg1, Purple ACKing seg2, LIN live, `sdlog_collect.py pull` by IP).

**1. The §4.3 rerun: PASS, and not narrowly.** Rig YAML byte-identical to §11,
only the firmware changed. 28 minutes, ~26 of them with the transfer in
flight, **6.80 M records, every drop counter 0 the whole run** — dropped,
tap_dropped, text_dropped, card_dropped, all of them, one distinct value.
The instrumentation meanwhile logged **468 stall events**: 170 × 50–99 ms,
237 × 100–199 ms, **59 × 200–399 ms — §4.3's killer class, which lost 4765
records across just 6 occurrences — and one 1672 ms monster** (a single 4 KB
`read` on a legacy chunk held 1634 ms: card GC in full form; the writer's
pass gap logged 1672 ms in the same instant, the mutex coupling §1 describes,
photographed). Under the old architecture that one event alone costs more
records than all of §4.3 did. LIN held 197–198 RX/10 s, checksum errors 0.
Heap min-since-boot **2108 B — better than §11's 1668 B** with the record
ring loaded and the drain task running, which is the static-stack decision
doing what it was for. First stall pair appeared **18 s** after the pull
started; the instrument-free §11 run needed burst forensics to see any of
this.

**2. The release gate: `verify.py --seconds 45 --min-laps 75` → ALL GREEN,
875 laps, ~19.4/s** — the pre-fix baseline exactly, all three boards green,
`LIN sniff 48` ok. The drain task costs the ring nothing.

Also answered on the way through: a pulled 4 MB chunk (`L0000036.LOG`,
102 245 records) parses **clean** — `drops none, gaps none, close rotate` —
so records that transited tap ring → drain task → record ring → writer are
byte-good end to end, not merely counted.

Still genuinely open from the old list: the `ring N%` observation — the
stats line does not print ring fill, so "backpressure was active" is inferred
from the survival of the 1672 ms stall rather than read off a gauge. Worth a
`ring=` field in the stats line some quiet day.

## 7. The idea scoreboard — tested, acknowledged, discarded

**Tested on hardware, this session:**

| idea | verdict |
|---|---|
| drain task decoupling (§3) | **PROVEN** — 0 losses through 468 stalls incl. 1672 ms; ring gate unchanged at 875/19.4 |
| stall instrumentation at 50 ms | **PROVEN** — attributes every §11 mystery number; first signal 18 s into a transfer |
| static task stack vs heap floor | **PROVEN** — floor 2108 B, better than §11 despite the new task |
| backpressure watching a ring taps actually use | **ACTIVE** — inferred from the 1672 ms survival (no fill gauge yet, see §6) |
| SD `clock: 10MHz → 20MHz` | run this session, result recorded below (§7a) |

**Acknowledged — catalogued in §5 with links, ranked, not yet run:** WiFi
`power_save_mode: none` (§5.1), WiFi buffer caps for the heap floor (§5.2),
`LWIP_TCPIP_CORE_LOCKING` + the lwIP-18-vs-LIN-18 priority tie (§5.3),
`sync_interval` 1 s → 5 s (§5.4), an A1/industrial card (§5.6), serve-block
16–32 KB + `vTaskDelay(1)` pacing (§5.7), second httpd instance for `/status`
(§5.8), writer batching + `f_expand` contiguous preallocation (§5.9),
FASTSEEK for ranged re-pulls (§5.10), `WL_SECTOR_SIZE_512` (§5.11). Each is
one experiment with its own observable; none blocks the merge.

**Discarded, with the reason on record:** raising `log_tap_queue_depth` to
4096 both ports (+123 KB against a heap that does not have it — §3); two
FATFS volumes to split the lock (no lock below the FS layer → corruption);
patching sdspi's no-yield busy-poll (requires an IDF fork — upstream issue
material, not repo material); any app task at priority ≥ 20 (Espressif RF
guidance); PSRAM anything (C6 has none); a pre-opened FILE handle per chunk
(conflicts with the unmount fd-ownership barrier, saves µs); smaller serve
reads *alone* (cannot defeat a higher-priority spin — pacing without
decoupling was the §11 death); ESPHome loop-interval knobs for this problem
(the loop was preempted, not slow — no cadence knob shortens a 169 ms
preemption).

## 7a. The 20 MHz run — tried, clean at the card, not adopted

Same evening, `clock: 10MHz → 20MHz` in the collect rig, ~40 minutes on the
card (about 30 of them under load and transfer). Three results and two
confounds:

- **The card is fine at 20 MHz.** Clean mount, not one `write failed`, not one
  recovery, zero drops on every counter, across two pull windows. The 10 MHz
  signal-integrity caution is disproven for this harness.
- **The stall profile got visibly milder** — 174 warn events in the loaded
  10-minute window, worst **242 ms**; the 10 MHz soak's histogram had a 1672 ms
  monster and 59 events in the 200–399 ms class over its window. Halved mutex
  holds are the plausible mechanism, but see the confounds.
- **The throughput question stays open.** The loaded window measured ≥126 KB/s
  with a 64 MiB legacy chunk still in flight uncounted (true figure up to
  ~230 KB/s) — not comparable to §11's 245 KB/s because the load shape
  drifted: the window ran **bidirectional** (~94 % both segments, rx+tx, Blue
  bursting on seg2) where §4.3 was Green-solo, and the legacy chunks §11
  streamed were mostly drained by then.

What the harder shape surfaced, and 10 MHz never did: **heap min-since-boot
152 B** (vs 2108 B in the 10 MHz soak) and the **first two LIN checksum errors
ever seen under load** (19:55:01 and 19:55:41, ≈83 ppm of that window's
frames, zero in the following 8.5 min). Whether that is 20 MHz, the
bidirectional shape, or the legacy-chunk streaming is unresolved — but it
makes §5.2's WiFi buffer caps a prerequisite, not an optimization. The rig
keeps 10 MHz so its numbers stay anchored to §11; re-raise the clock
deliberately, after the buffer caps, with a same-shape A/B.

## 7b. The card swap — a second card, live, and what it proved. Same evening, 20:17–21:35

Jan hot-swapped a new SD card mid-write. Everything below happened with the
merged firmware, load running throughout.

**The swap itself: the architecture held.** Pull-out mid-write → the writer
failed its write, dropped the file, buses unaffected, recovery ladder retrying
on backoff with the in-band reset — and the counters attributed the outage
exactly as designed: `dropped=0`, records shed at the RX ISR into
`tap_dropped` (~740 k over the unmounted window), `card_dropped` for the one
in-flight record. Nothing wedged, nothing crashed.

**The new card had no FAT** (`the card answered but its filesystem did not
mount; retrying cannot fix a FAT` — the ladder's message is exactly right).
Remedy: one flash with `format_if_mount_failed: true` — V21 formats on the
boot mount only — then one flash with it removed, so a transient mount
failure can never format a data card. The on-device format (16 KB allocation
units, `sd_logger.cpp` mount_cfg) took seconds; `card 0% full at mount`.

**The A/B answer — the card owns the stall signature, and it no longer
matters:**

| | old card (26 min, with transfer) | new card (45 min, writer-only) |
|---|---|---|
| stall events | 468 | **1200** |
| 200–399 ms class | 59 | **117** |
| worst | 1672 ms | 556 ms |
| records | 6.8 M | 11.67 M |
| every drop counter | **0** | **0** |

The new card stalls **more often** with a **shorter tail** (so far — its GC
history is one evening old). Under the pre-fix architecture its 117
mid-class stalls would each have been a burst; with the drain task the
difference between the two cards is invisible in the data. Also: the fresh
card serves uniform 4 MB chunks and `index_refused` pinned at **0** — §11.2's
stranding problem does not exist on it. Its transfer window: 35 chunks,
143.9 MB, ~180 KB/s, 35/35 confirmed.

**Two events to carry forward, distinct from each other:**

1. **Orange hard-wedged at ~21:18**, right around the pull's end: USB console
   silent (from 21:05 already, while WiFi/SD/CAN/LIN demonstrably kept
   working), then httpd gone (lwIP still answering RST), then no esptool sync
   on any mode — the `hil-c6-dead-not-rom-stub` signature, cleared only by the
   12 V cycle. **Unexplained.** The reset cause was unrecoverable (a power
   cycle overwrites it). Lead suspect is the thin heap under the transfer
   shape (§7a measured a 152 B floor), unproven. If it recurs: console
   attached from boot, 1 s heap sampling, and the §5.2 buffer caps as the
   first counter-measure.
2. **The bench-wide 12 V cycle tripped `vcc_monitor` at the next boot** —
   four boards' inrush sagged the shared rail past 10.5 V for the 10 ms
   (2 samples × 5 ms) the trip needs, and the now-correctly-calibrated
   monitor did its specced one-way close: board up, server answering,
   logging off, no recovery, all three sd_logger tasks gone. A solo reset
   with the rail stable boots clean. Two readings: after any bench-wide
   power cycle, expect to reset the DUT once more; and in the car, key-on
   inrush is the same shape — a boot-arming grace period for the monitor is
   a small, legitimate improvement to weigh against §9.2's hold-up story.

**Log format decision (Jan, this session): v1 is frozen until release.** The
agreed post-release v2 direction, recorded so it is not re-derived: delta
timestamps (signed — the stream is not strictly monotonic across sources;
absolute anchors at file open, after every `#`-marker, and every Nth record
so one torn line cannot poison the file), record type merged with the source
tag (`C1,` instead of `C,` + `seg1,` — the `#src` header already carries the
mapping), and CAN IDs in hex. ≈30 % smaller lines, still greppable ASCII,
*faster* to format than v1. Needs the `#sdlog,2` version bump and a v2
parser in `sdlog.py check`. Above it on the leverage ladder, also post-v1:
change-filtering at the tap (5–20× on real vehicle traffic) and an ID
allowlist.

Bench-hygiene finding from the same detour, now in the agent memory as well:
`soak-perf-blue.yaml` and `mr-blue.yaml` share the node name `mr-blue`, so
they share a build directory — `esphome upload` after compiling the *other*
yaml flashes the wrong firmware and reports success. That put a 125 kbps ring
binary on seg2's 500 kbps ACK partner mid-session: Orange showed `REC 128`,
`err_events` +4 k/s, seg2 at 0.0 %, and Blue's own console showed
`inject failed (tx queue full)`. M6 load run §1 (2026-07-29, git history) documents the same
trap for `mr-orange`. Compile and upload the same yaml, always, and read the
DUT's port stats before starting any measurement window.
