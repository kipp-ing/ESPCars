# sd_logger — chunk lifecycle, collection and retention (M6 design)

2026-07-28. Extends `docs/sd_logger-spec.md`; validation rules continue from V21.

**Build state.** §3/§4/§7 (chunk lifecycle, rotation bounds, retention, `#gap`)
are Phase A and are built. §5/§6/§8/§8a — the `esp_http_server`, the four
endpoints, `Range`, the serving guard, ring-fill backpressure and the index mutex
— are Phase B and are built as of this line; see §5d for the questions it had to
answer and how. **Nothing in either phase has been on hardware.** No board has
served a chunk, no `#gap` line has been written to a card, and the acceptance
criteria in §6 and §9 are all still owed.

Requirement, in the user's words: **"store everything all the time"** and
**"upload automatically via WiFi"**. This document is how those two get
satisfied on an ESP32-C6 that also runs a CAN gateway and a LIN stack.

---

## 1. The premise correction: there is no PSRAM tier

The intuitive shape — buffer log chunks in PSRAM, DMA them to the card, read
them back into PSRAM to upload — is unavailable on this hardware, and would not
pay if it were.

- **The C6 has no PSRAM.** No external SPIRAM interface on the die. Checked
  against pinned esphome: `psram/__init__.py` supports ESP32, C5, **C61**, H4,
  P4, S2, S3, S31. `VARIANT_ESP32C6` is absent. Note the trap — a substring grep
  for `VARIANT_ESP32C6` matches `VARIANT_ESP32C61`, which is a different chip.
  Budget is 512 KB HP SRAM total, perhaps 100–200 KB free once WiFi, IDF,
  esphome, `can_gateway` and `linbus` have taken theirs.
- **DMA would relieve the wrong bottleneck.** §3 H1 as corrected on 2026-07-27:
  the writer saturates at ~373 KB/s against a card good for 1–2 MB/s, because
  the cost is **per-record CPU in the writer**, not card bandwidth. The sdspi
  driver already uses DMA underneath. An app-level DMA path buys nothing.

**Therefore the card is the buffer.** The RAM ring (40 KB at the default depth)
absorbs bursts; the card absorbs hours. There is no third tier, and adding one
would only introduce a second place for data to be lost.

---

## 2. The invariant

> **The writer only ever appends to the open file. The collector only ever reads
> sealed files. They never touch the same file.**

Concurrent read and write on this platform is genuinely bad, but not for a RAM
reason: there is one SPI bus, one FATFS lock, and a card controller whose
garbage collection stalls when read and write streams interleave. Splitting by
*file* removes the interesting half of that contention for free, without any
buffering. What remains is bus sharing, handled by §6.

This invariant is the whole design. Everything below is bookkeeping that keeps
it true.

---

## 3. Chunk lifecycle

A chunk is a file. Four states, and the transitions are the only filesystem
mutations in the system:

| State | On card | Meaning | Who may touch it |
|---|---|---|---|
| **OPEN** | `L0000433.LOG`, `fd_ >= 0` | The writer is appending. Not listed, not servable. | Writer only |
| **SEALED** | `L0000433.LOG`, closed | `#close` written, fsynced. Complete and readable. | Collector may read |
| **CONFIRMED** | `L0000433.UPL` | The puller acknowledged receipt. Eligible for deletion. | Retention may delete |
| **GONE** | — | Deleted to reclaim space. | — |

**SEALED → CONFIRMED is a rename**, never an in-file edit. Rewriting bytes
inside a sealed log to flag it "collected" is a read-modify-write on the file
you are trying to protect, and it costs a FAT sector write plus a data sector
write for one bit of state. A rename is one directory-entry write and is
power-cut atomic: either it happened or it did not.

**The order is fixed and only one order is safe:**

```
serve bytes  →  puller confirms  →  rename to .UPL
```

A crash anywhere in that window re-serves the chunk. That is correct — the
puller dedups on `(device, seq)`. The inverse order (rename first, serve later)
loses data permanently on the same crash. This is the single most important
ordering constraint in the document.

`seq` is already card-derived and monotonic across reboots — the boot scan
walked 431 → 432 → 433 rather than restarting — so `(device, seq)` is a usable
dedup key today, without a format change. See §10 for whether a boot id is worth
adding anyway.

---

## 4. Rotation by time as well as size

Rotation is currently size-only (`max_file_size_`, 16 MB default, 32 MB on the
bench). That is not sufficient for collection: a quiet period leaves the newest
data trapped in an OPEN file indefinitely, and OPEN files are never servable.

Add `max_file_seconds` next to `max_file_size`, whichever fires first. Sizing:

| | at production ~148 KB/s |
|---|---|
| 1 MB chunk | ~7 s |
| 4 MB chunk | ~28 s |
| 16 MB (today's default) | ~110 s |

**1–4 MB is the target.** Small enough that a failed transfer is a cheap retry
over car WiFi, large enough that header block, FAT metadata and HTTP round-trip
stay in the noise. Time bound of 60 s or so, so an idle bus still seals.

One caution: rotation has **never fired on hardware** (handover §4b.5), and the
bug where a failed post-rotation reopen silently ate records was found by reading,
not by running. Shrinking chunks to 1–4 MB makes rotation fire every few seconds
instead of never. That path needs bench time before it carries the collection
design on top of it.

---

## 5. The collection surface (pull)

Decision: **the device serves, something else collects.** The puller owns
retries, credentials, dedup and the archive. The ECU owns only "which chunks
exist and what state are they in". This keeps the hard parts on a machine with a
filesystem and a debugger.

### 5a. Run our own `esp_http_server`

Not esphome's `web_server`. `web_server_base`'s handler API
(`add_handler(AsyncWebHandler *)`) is the Arduino path; on ESP32 the component
takes an early return into the IDF web server, and what that path exposes to
external components was not verified and is not contractual. Coupling to it
would risk the hard rule that components stay **external-component-safe**.

A private `esp_http_server` instance on its own port is IDF-native,
self-contained, supports chunked responses and `Range`, and costs roughly 10 KB
heap plus a task stack.

**Start it from `loop()`, never from `setup()`** — settled on hardware, and the
failure mode is a boot loop with no error path to harden. `sd_logger` is
`setup_priority::DATA` (600) and esphome's wifi component is `WIFI` (250), so
`setup()` returns before lwip exists. `httpd_start()` opens its control socket
immediately, and the first lwip call then locks a null TCPIP mutex:
`assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))`. There is
nothing to check for, because `httpd_start()` never returns. Pinned by
`test_the_server_is_started_from_loop_and_never_from_setup`.

The related thing that is **not** a requirement and should not be "fixed":
binding without an association is deliberate and works. The socket takes
`0.0.0.0`, and a board whose card failed is exactly the one an operator wants to
reach on `/sdlog/status`. Initialisation was needed; association was not.

### 5b. Endpoints

| Route | Purpose |
|---|---|
| `GET /sdlog/index` | JSON: every SEALED chunk — name, seq, bytes, first/last `t_us`. OPEN and CONFIRMED are **not listed**. |
| `GET /sdlog/f/<name>` | Raw bytes, `Content-Length` set, `Range` supported. SEALED only; 409 for the open file. |
| `POST /sdlog/done/<name>` | The confirm. Renames SEALED → CONFIRMED. Idempotent. |
| `GET /sdlog/status` | Counters, card fill, oldest un-collected seq, discarded totals. |

`Range` support is the retry story and it comes nearly free in the pull model: a
dropped connection resumes at the byte offset instead of restarting the chunk.
The push model has no equivalent without server cooperation — a real argument
for pull beyond the state-machine savings.

### 5c. Reads are streamed, not buffered

4–8 KB from the card straight into the socket, repeat. The chunk is never
resident, so the absent PSRAM costs nothing here either.

Worth recording why the obvious shortcut is closed in the push direction too:
esphome's `http_request` takes the body as a `std::string` and its own header
documents that the read helpers block the main event loop, recommending
*"esp_http_client directly on a separate FreeRTOS task"*. Either direction needs
its own task. Pull needs one anyway for the httpd.

### 5d. What Phase B had to decide that §5b did not

Every one of these was open when the endpoints were written, and each is written
down here because the alternative is the next session re-deriving it from the
client's source. The authority throughout is `script/sdlog_collect.py` and the
`FakeDevice`/`_Handler` pair in `tests/sd_logger/test_collect.py`, which prove the
*client* and stay green no matter what the firmware does.

- **`Content-Length` on a streamed body.** Neither IDF helper can do it:
  `httpd_resp_send()` sets the header but wants the whole body in RAM, and
  `httpd_resp_send_chunk()` streams but emits `Transfer-Encoding: chunked` and no
  length at all — which deletes the client's primary cut detection (`got <
  promised`). The chunk body therefore writes its status line and headers by hand
  and pushes the body with `httpd_send()`. **On a 206 the length is that of the
  body, not of the file**; the inverse makes every successful resume look like a
  cut connection, permanently. The index, by contrast, *is* chunked: 256 entries
  of JSON does not fit the §5a heap budget, and `index()` reads to EOF and never
  looks at the header.
- **Device name** (§11.2): `App.get_name()`. It is half the collector's dedup key
  and its fallback is the URL hostname, which is not stable across DHCP leases. No
  new config key, and the consequence is that renaming the node changes the
  archive directory the collector writes into.
- **`first_t_us` / `last_t_us`** (§11.1): omitted. `ChunkEntry` has no timestamp
  and reading each file's `#sdlog` header at index time would put card I/O on the
  httpd path for a display nicety. The keys are optional to the client; only
  `sdlog_collect.py index` notices, printing `span=-`.
- **A confirm for an untracked seq** (§11.5): 404, for both readings — retention
  discarded it mid-transfer, or the index was full when the file appeared and the
  firmware cannot rename what it is not tracking. Neither loses data.
- **An unparseable `Range`** (§3.7 of the wire contract): ignored, and the whole
  file is served as 200. The client tolerates exactly that; the fake's 400 would
  be fatal for the chunk, because every non-416 4xx is.
- **Renaming a file a handler holds an open fd to** (§11.4): deferred rather than
  assumed. `drain_confirms_()` puts the intent back on the queue while
  `is_serving(seq)` is set, so no claim is made about whether an open FATFS `FIL`
  survives a rename of its directory entry. Costs one 20 ms writer pass in the
  only case that reaches it.
- **Unmounting under an open read fd.** Not in the design at all and introduced by
  Phase B: `esp_vfs_fat_sdcard_unmount()` frees the VFS context that owns every
  open `FIL`, so a card failing mid-transfer put the httpd task one block away
  from a use-after-free. `unmount_card_()` now waits, bounded, for in-flight
  transfers, and proceeds anyway when the bound expires — logging wins, which is
  the same rule §6 states about the SPI bus.

---

## 6. Backpressure — logging always wins

The collector and the writer still share SPI2. One rule, using a signal that
already exists:

> Between blocks, the serving handler checks record-ring fill. Above ~50 %, it
> sleeps a few ms before the next read.

The ring is the backpressure sensor and it is already instrumented. A collection
run must never be able to turn into `dropped_records`. This is the assertion the
bench has to prove, and the criterion is the existing one: `dropped == 0` while a
transfer is in flight at production rate.

---

## 7. Retention — what makes "store everything" bounded

At ~148 KB/s: **~520 MB/hour, ~12.4 GB/day continuous.** A 32 GB card holds
about 60 hours. So "store everything all the time" is bounded by collection, and
the policy for what happens when collection does not keep up *is* the feature.

**Decision: drop oldest un-collected.** Above a fill threshold, delete
oldest-first — CONFIRMED chunks preferentially, then SEALED ones that were never
collected. The card is a circular buffer in which collected chunks are the free
list. Logging never stops; the oldest history loses.

**The gap must be stated in-band.** This mirrors the design already in the file
format: `#drop` markers are emitted when a counter moves, and `card_dropped` is
the one baseline the header writer deliberately does not reset, so a recovered
file states the cost of the outage. Retention gets the same treatment — a
`discarded_chunks_` / `discarded_bytes_` counter pair, and a `#gap` line emitted
by the existing marker pass when it moves:

```
#gap,<t_us>,<chunks>,<bytes>,<first_seq>,<last_seq>
```

A log that silently skips six hours is worse than one that says it skipped them.

**The window is held by the writer, not by the policy.** Between
`take_pending_discards()` and the line actually reaching the card there is a
buffer that a failing write throws away — `commit_line_()` moves bytes into the
block buffer and `enter_failed_()` resets it — so the writer keeps the window in a
member until the bytes behind its line have gone through `write()`, and folds it
back into the next file if they never do. `#drop` does not need this because it
carries a lifetime `total` a reader can recover from; `#gap` carries deltas, so a
line that is lost is a loss stated nowhere, ever. The mirror of that is equally
binding: a window whose line *did* reach the card must never be stated twice.

### 7a. What the `#gap` fields mean — and what they deliberately do not

`chunks` and `bytes` are the delta since the last marker, the same column `#drop`
carries. `first_seq`/`last_seq` are **the endpoints of the window retention
emptied, in the order it emptied it** — not a numeric min and max, and *not* a
promise that every sequence number between them is missing.

Two properties follow, and both have to be honoured by every reader:

- **The endpoints are unsorted.** `seq` wraps at 9999999. A two-chunk window that
  straddles the ceiling is `9999998..0`; sorting it first prints `0..9999999` and
  claims the entire card went away.
- **The window can be punctured.** Only never-collected (SEALED) chunks are billed
  to the counters — deleting a CONFIRMED chunk is not a gap, because the collector
  already has it. So a confirm landing in the middle of a retention run leaves a
  hole in the window that is not a hole in the archive: seal 10, 11, 12; discard
  10; the puller confirms 11; discard 12 — and the line reads `chunks=2` over the
  window `10..12`, while `L0000011` is safely collected. Whenever
  `chunks != span(first_seq, last_seq)` (computed modularly, per the point above)
  the window is punctured.

The rule for readers: **state the loss and the window as two separate numbers, and
never expand the span into a list of files to go looking for.** `#gap` exists to
state loss honestly in-band, so over-reporting it is the one failure mode the
marker must not have — a line that claims six hours it did not lose sends someone
hunting for files that are not missing, and the next honest `#gap` is then the one
nobody believes. `script/sdlog.py` renders the punctured window above as
`seqs 10..12 (punctured: the window is 3 seqs wide)` — two chunks stated as the
loss, three seqs stated as the window, and the two never conflated. The
contiguous case (`chunks=3` over `412..414`) prints plain, with no puncture
clause at all; that asymmetry is the point, and both spellings are pinned by
tests in `tests/sd_logger/test_sdlog_reader.py`.

This section exists because it was missing: `log_format.h` and
`collection_policy.h` each wrote down their own reading of these two fields, the
readings disagreed about whether the span was a loss claim, and nothing in the
design doc arbitrated. The header docstrings now defer here.

**Deleting must not race a read.** Retention only ever deletes CONFIRMED chunks,
and a chunk becomes CONFIRMED only after the puller finished with it — so under
normal flow the two never overlap. The exception is the pressure case where
retention deletes SEALED chunks that were never collected; that path needs a
"currently serving" guard so it skips a file with a transfer in flight.

---

## 8. Ownership — one mutator

All filesystem mutations (open, append, rotate, rename, unlink) belong to the
**writer task**. The httpd task only opens files read-only and posts intents
(`confirm <name>`) into a small queue the writer drains.

This is not ceremony. The component already has a hard-won invariant that
`mounted_` and `fd_` can never disagree, enforced by routing every discovery of
a dead card through a single `enter_failed_()`. A second task calling `rename()`
or `unlink()` behind the writer's back reintroduces exactly the class of bug
that invariant exists to prevent — and it would do so during card recovery, when
the writer is already unmounting and remounting underneath.

### 8a. But the index is shared, and §8 alone does not make it safe

"One mutator" is about the *card*. It says nothing about `CollectionPolicy`, and
the two are not the same object. Writing this down because §8 reads as though it
settles the question and it does not — the omission was found by extracting the
wire contract from the already-passing collector tests, not by running anything.

The httpd handler needs `find()`, `at()`, `count()` and `is_servable()` to answer
`/sdlog/index` and `/sdlog/f/<name>`. Those are plain `const` reads of `chunks_[]`
and `count_`, taken with no lock, while the writer task may be inside
`discard()` — which **memmoves the array** to close a hole rather than swapping
the last entry in, precisely so `at()` keeps insertion order. A handler iterating
`at()` during that memmove can hand out a torn entry: a name from one chunk with
the length of another. That is a genuine data race, and `sd_logger.h`'s current
comment — *"Nothing outside that task may read it either"* — is today an accurate
description of the code and becomes false the moment a handler exists.

Worse, one of the accesses cannot be deferred into the intent queue at all.
`mark_serving()` is a mutator that must take effect **before the first block is
read**, or retention has a window in which it deletes the file being served. A
`confirm` can wait for the writer to drain a queue; a serving guard cannot.

**Decision: a blocking `SemaphoreHandle_t` mutex around every `CollectionPolicy`
access, from both tasks.** Explicitly *not* a `portMUX` spinlock: the critical
sections are linear scans (`find()`, `oldest_()`, `next_victim()`), V26 puts the
default index at 256 entries and lets a config ask for 2048, and holding
interrupts off across a 2048-entry scan is incompatible with the TWAI ISR
discipline `can_gateway` enforces two components over. A blocking mutex is
affordable because neither the httpd task nor the writer task is the record
producer — producers hand off to the ring and never touch the index.

Two alternatives were considered and rejected for v1: a writer-published seqlock
snapshot of the index (the shape `SnapshotRing` already uses in
`gateway_core.h`), which is less locking but more machinery; and routing every
access through the writer with a synchronous ack, which is the most faithful to
§8 and gives a natural 500-on-timeout, but pays a task round-trip per request.
Either may be worth it later; neither is worth being clever with first.

**The mutex does not live in `collection_policy.h`.** That header is deliberately
IDF-free and ESPHome-free so it runs in the host harness, which is where the
whole chunk lifecycle is actually tested. The lock belongs to `SdLogger`, which
already owns the FreeRTOS objects, and `CollectionPolicy` stays a passive data
structure that knows nothing about tasks. Per the repo's definition of done, a
concurrency contract gets a host case with a real thread and not a reasoned
argument — that is how the `SnapshotRing` reader fence was found.

---

## 9. What the bench has to answer before any of this is trusted

WiFi is entirely unscoped in the current spec, and these are measurements, not
arguments:

1. **WiFi vs. the ISR discipline.** `can_gateway` forces the TWAI ISR cache-safe
   and FreeRTOS into IRAM specifically to protect wire timing. WiFi brings tasks
   at priority ~23 — **above `lin_uart_evt` at 18** (spec §2). On a single core
   that is a preemption path straight into LIN timing. Measure LIN jitter and
   `verify.py` laps/s with WiFi associated and a transfer running.
2. **Current draw vs. hold-up.** WiFi TX bursts to ~300 mA. The H6 hold-up cap is
   sized to finish one SD write, and `vcc_monitor` trips an emergency close on
   rail sag — a monitor whose calibration bug was only just fixed and which has
   not yet been confirmed healthy on hardware. A TX burst on a marginal rail is
   the coincidence to look for.
3. **TLS heap.** mbedTLS costs ~40–50 KB per connection. On a C6 already hosting
   two bus stacks, plain HTTP on a trusted local network may be the only thing
   that fits. Decide before designing auth.
4. ~~**Rotation at 1–4 MB.** Per §4 — it has never fired at all.~~
   **ANSWERED 2026-07-28.** The PERF soak (`tests/hil/opt_perf.yaml`, 11 min 57 s,
   4 MB / 60 s bounds, both segments at ~89 %, writer at rate) rotated **29 times
   with `dropped 0`, `card_dropped 0` and no bus-off**. The path §4 flagged as
   never having fired on hardware — and whose silent-record-eating reopen bug was
   found by reading rather than running — now has an hour of rotations behind it.
   Collection can be built on top of it.

   It also found the thing that would have made collection useless anyway: the
   chunk index was **full at the first of those 29 rotations**, so every chunk was
   untracked, retention could reclaim nothing, and `/sdlog/index` would have
   listed nothing at all. Fixed by V26 (§11) — 12 B an entry, 256 by default,
   `collection: max_chunks:` up to 2048 — and the refusals are now counted in the
   periodic stats line (`index_refused=`) rather than costing one warning that
   scrolls past.

Estimates flagged as such: the 148 KB/s and 373 KB/s figures are **measured**
(handover §2), and rotation is now measured too. Card read throughput, WiFi
throughput and heap costs above are **estimates** and none has been measured on
this board — `tests/hil/wifi_probe.yaml` and the two rigs including it exist to
turn items 1 and 3 into measurements, and report free heap *and largest free
block* because a fragmented heap can be 80 KB free and still refuse a 10 KB
httpd allocation.

---

## 10. Open decisions

1. **Collection trigger.** Continuous whenever a known SSID is present, or gated
   on "vehicle idle" (record rate below a threshold)? The arithmetic favours
   gating: an hour of driving is ~520 MB, which at a plausible few-hundred-KB/s
   is ~20 minutes of parked time. **You cannot collect a full-rate log in real
   time while writing it** — the working model is log while driving, drain while
   parked.
2. **Boot id in the header.** `#sdlog,<fmtver>,<seq>,<t_us>,<esphome_ver>` has no
   boot id. `seq` is monotonic per card, so dedup works without one; a boot id
   would additionally detect card swaps and reboot boundaries. It is a format
   version bump (reader, host tests, golden lines) for a nice-to-have — probably
   defer, but decide deliberately rather than by omission.
3. **Fill threshold and reserve.** What card percentage arms retention.
4. **Auth.** None, shared token, or esphome's existing web auth — gated on
   decision 9.3.

---

## 11. New validation rules (sketch, continuing from V21)

- **V22** a `collection:` block that asks for a **server** requires `wifi:` in the
  config. Two keys, because the block has two halves that fail separately:
  `enabled:` is the *card* half (chunk index, retention, `#gap`) and `serve:` the
  *network* half (the `esp_http_server` of §5, defaulting on). V22 and V25 apply
  when both are true. `serve: false` is a real configuration — a device whose card
  is bounded and whose chunks are fetched by pulling the card out — and it is the
  only shape in which the bench can soak §4's rotation and §7's retention at all,
  since associating to WiFi would put §9.1's unmeasured preemption risk inside the
  very run that is measuring rotation.
- **V23** `max_file_seconds` ≥ 1, and rotation must have at least one bound.
- **V24** `retention_percent` strictly between 1 and 99 — both ends exclusive
  (`cv.int_range(min=1, max=99, min_included=False, max_included=False)`), so the
  accepted values are 2..98. At 99 % the headroom left is smaller than one chunk
  and retention would arm only once the card is full and logging has already
  stopped; at 1 % it deletes chunks about as fast as the writer seals them, which
  is a permanent gap dressed up as a policy. `CollectionPolicy::configure()`
  additionally clamps to **[1, 99]** for callers that never went through the
  schema (host tests construct the class directly). The schema is the *narrower*
  of the two deliberately: no value a user can write is ever silently rewritten by
  the clamp, and widening a range later is not a breaking change while narrowing
  one is.
- **V25** `port` must not collide with `web_server`'s.
- **V26** `max_chunks` between **16 and 2048**, default 256. It sizes the static
  chunk index (`SD_LOG_MAX_CHUNKS`, emitted as a define from the component's
  Python, never from esphome core), so it is RAM against coverage: 12 B an entry,
  hence 3 KB at the default and 24 KB at the ceiling. The default was 64 until a
  PERF soak on the bench (2026-07-28) found the index full at the *first* of 29
  rotations — every chunk of that run untracked, so retention could reclaim
  nothing and `/sdlog/index` would have listed nothing. At the 4 MB / 60 s bounds
  a chunk lands roughly every 25 s: 64 entries was ~27 minutes of cumulative
  logging, 256 is ~1.8 hours. Refusals are counted and printed as
  `index_refused=` in the periodic stats line, because the one warning per
  rotation that used to be the only witness scrolls past on a soak.

Per the repo's definition of done, each ships with acceptance *and* rejection
tests in `tests/sd_logger/`, and every new key appears in a `tests/build/` yaml
so CI compiles it.
