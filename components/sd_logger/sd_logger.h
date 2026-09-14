#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

#include "card_reset.h"
#include "collection_policy.h"
#include "collection_server.h"
#include "log_format.h"
#include "log_record.h"
#include "recovery_policy.h"
#include "sd_diagnostics.h"
#include "sd_confine_policy.h"
#include "utc_anchor.h"

#ifdef USE_SD_LOGGER_CAN_TAP
#include "esphome/components/can_gateway/can_gateway.h"
#include "tap_translate.h"
#endif

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_err.h"

// `spi_device_handle_t`, for the raw-SPI helpers the reset and readiness paths share.
#include "driver/spi_master.h"

extern "C" {
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "sdmmc_cmd.h"
}

namespace esphome {
namespace sd_logger {

#ifdef USE_SWITCH
class DebugWriterFreezeSwitch;
#endif

// LogRecord and the REC_FLAG_* bits live in log_record.h, the tap translation in
// tap_translate.h and the whole of format v1 in log_format.h — all three
// ESPHome- and IDF-free so the host harness can run them. That is where the flag
// encodings collide and where a mis-formatted field yields a perfectly
// well-formed, quietly wrong file; the bench cannot reach either.

// One captured ESPHome log line (source S4, spec D9). Variable-length text
// cannot share the fixed 20-byte LogRecord ring, so it gets its own ring of
// fixed slots. 192 B is the slot; `buf` holds the tag bytes followed by the
// message bytes, both already stripped of ANSI and of the rendered log prefix
// (log_format.h: copy_log_payload).
static const size_t SD_LOGGER_TEXT_SLOT = 192;
static const size_t SD_LOGGER_TEXT_BUF = SD_LOGGER_TEXT_SLOT - 8;

struct TextRecord {
  uint32_t t_us;
  uint8_t level;
  uint8_t tag_len;
  uint8_t len;
  uint8_t truncated;  // the message did not fit the slot; the line gets the '~' mark
  char buf[SD_LOGGER_TEXT_BUF];
};

#ifdef USE_SD_LOGGER_CAN_TAP
#ifndef USE_CAN_GATEWAY_LOG_TAP
#error \
    "sd_logger's can_ports: needs a can_gateway port with 'log_tap: true' — without it the port never allocates a tap ring"
#endif
#endif

/// RAII around the chunk index's mutex (docs/sdlog-collection-design.md §8a).
///
/// `CollectionPolicy` has no concurrency story and deliberately keeps none: it is IDF-free and
/// ESPHome-free so the whole chunk lifecycle runs in the host harness, where every interesting case
/// costs a microsecond instead of hours of a filling card. `find()`, `at()`, `count()` and
/// `is_servable()` are plain unlocked reads of `chunks_[]`, and `discard()` **memmoves the array**
/// to close a hole — so a handler iterating `at()` while the writer discards can be handed a torn
/// entry: a name from one chunk with the length of another. That is why the lock lives here, in the
/// class that already owns the FreeRTOS objects, and not one directory over in a header that has to
/// compile on a laptop.
///
/// A blocking mutex and explicitly **not** a `portMUX` spinlock: the critical sections are linear
/// scans, V26 puts the default index at 256 entries and lets a config ask for 2048, and holding
/// interrupts off across a 2048-entry scan is incompatible with the TWAI ISR discipline
/// `can_gateway` enforces two components over. Blocking is affordable because neither the httpd task
/// nor the writer task is the record producer — producers hand off to the ring and never come here.
///
/// `mux` is null in a build with no collection server, where the writer is again the only thing that
/// touches the index and §8's one-mutator rule holds on its own; the guard is then a null check.
/// **Never hold this across a socket write or card I/O** — every caller in sd_logger.cpp takes it
/// for a scan and gives it back before it touches the world.
///
/// **`xSemaphoreCreateMutex()` is not recursive, and this guard takes it with `portMAX_DELAY`.** A
/// scope that takes it twice does not assert, does not time out and does not log: the task blocks on
/// itself forever, and on the writer task that is the end of logging for the run with no counter
/// moving to say so. No scope in this component encloses a second take today, but the trap is one
/// edit wide and the edit looks harmless — `enter_failed_()` takes this lock (it clears the index)
/// and is reachable from deep inside the write path (`ensure_room_()` -> `flush_block_()` -> a
/// failing `write()`), so widening any existing lock scope to enclose a write call is enough to
/// deadlock the writer. If a nested take ever becomes genuinely necessary, split the inner call into
/// an unlocked `_locked()` helper rather than reaching for `xSemaphoreCreateRecursiveMutex()`: the
/// recursive variant would make the "never hold this across card I/O" rule above unenforceable by
/// inspection, which is the only way it is enforced at all.
class IndexLock {
 public:
  explicit IndexLock(SemaphoreHandle_t mux) : mux_(mux) {
    if (this->mux_ != nullptr)
      xSemaphoreTake(this->mux_, portMAX_DELAY);
  }
  ~IndexLock() {
    if (this->mux_ != nullptr)
      xSemaphoreGive(this->mux_);
  }
  IndexLock(const IndexLock &) = delete;
  IndexLock &operator=(const IndexLock &) = delete;

 private:
  SemaphoreHandle_t mux_;
};

class SdLogger : public Component {
 public:
  // ESPHome lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  // A clean shutdown is the third way a file ends, next to rotation and the VCC
  // emergency. All three write `#close`, because a file *without* one is the
  // power-cut signature (spec F1d) and that only means anything if every
  // orderly exit path does write it.
  void on_shutdown() override;
  // Mount after pins/SPI are ready; before app data producers run.
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Config setters (from Python codegen)
  void set_spi_pins(int8_t clk, int8_t mosi, int8_t miso, int8_t cs) {
    clk_pin_ = clk;
    mosi_pin_ = mosi;
    miso_pin_ = miso;
    cs_pin_ = cs;
  }
  void set_clock_hz(uint32_t hz) { clock_hz_ = hz; }
  void set_mount_point(const std::string &mp) { mount_point_ = mp; }
  void set_buffer_depth(uint32_t depth) { buffer_depth_ = depth; }
  void set_sync_interval(uint32_t ms) { sync_interval_ms_ = ms; }
  void set_max_file_size(uint32_t bytes) {
    max_file_size_ = bytes;
    apply_rotation_bounds_();
  }
  // The second rotation bound, whichever fires first (docs/sdlog-collection-design.md §4). Size
  // alone leaves the newest records trapped in the OPEN file for as long as the bus is quiet, and
  // an OPEN file is never servable. 0 is unbounded and is the default: `max_file_seconds:` has no
  // schema default on purpose, so an already released config keeps rotating on size alone.
  void set_max_file_seconds(uint32_t seconds) {
    max_file_seconds_ = seconds;
    apply_rotation_bounds_();
  }
  void set_format_if_mount_failed(bool v) { format_if_mount_failed_ = v; }
#ifdef USE_SWITCH
  /// Debug-only: request that the writer seal its current chunk and park before
  /// issuing another card command. The switch publishes only the acknowledged
  /// state from the writer task, not this request.
  void set_debug_writer_freeze_requested(bool requested);
  bool is_debug_writer_frozen() const { return this->debug_writer_frozen_.load(std::memory_order_acquire); }
  void set_debug_writer_freeze_switch(DebugWriterFreezeSwitch *sw) { this->debug_writer_freeze_switch_ = sw; }
#endif
  // Chunk collection (design §7). Phase A wires the *policy* — the chunk index, retention and the
  // `#gap` marker; `serve` and `port` belong to the esp_http_server that hands sealed chunks to a
  // puller, which is Phase B. They are recorded rather than dropped so `dump_config` reports what
  // the config asked for instead of a firmware that quietly pretends the keys were never written.
  //
  // `enabled` is the **card** half and `serve` the **network** half, and they are two keys because
  // they fail separately. A device whose card is bounded but whose chunks are fetched by pulling the
  // card out wants the first without the second — and so does the bench: WiFi brings tasks above the
  // LIN task's priority and its effect on bus timing is unmeasured (design §9.1), so a rotation and
  // `#gap` soak that had to associate would be measuring two unknowns at once. V22 and V25 therefore
  // ask for `wifi:` and guard the port exactly when `serve` is true.
  void set_collection(bool enabled, bool serve, uint16_t port, uint8_t retention_percent) {
    collection_serve_ = serve;
    collection_port_ = port;
    if (enabled) {
      collection_.configure(retention_percent);
    } else {
      collection_.disable();
    }
  }
  void set_card_power_pin(int8_t pin) { card_power_pin_ = pin; }
  void set_stats_log_interval(uint32_t ms) { stats_log_interval_ms_ = ms; }
  void set_vcc_monitor(int8_t adc_gpio, float threshold_v, float divider, uint32_t full_scale_mv) {
    vcc_adc_gpio_ = adc_gpio;
    vcc_threshold_v_ = threshold_v;
    vcc_divider_ = divider;
    vcc_full_scale_mv_ = full_scale_mv;
  }
  // Card recovery (spec §7 Layer C). `power_cycle` needs card_power_pin, which V19 enforces.
  void set_recovery(bool enabled, uint32_t initial_delay_ms, uint32_t max_delay_ms, uint32_t max_attempts,
                    bool power_cycle) {
    if (enabled) {
      recovery_.configure(initial_delay_ms, max_delay_ms, max_attempts);
    } else {
      recovery_.disable();
    }
    recovery_power_cycle_ = power_cycle;
  }
  // The in-band reset (card_reset.h), which is what recovers a card left inside an aborted
  // multi-block write — the state a reset mid-write leaves and the one no power switch on this
  // board can clear, because there is no switch on the card's supply.
  void set_recovery_reset(bool enabled, uint32_t busy_timeout_ms, uint32_t max_busy_timeout_ms) {
    recovery_.configure_reset(enabled, busy_timeout_ms, max_busy_timeout_ms);
  }
  void set_origin(const std::string &origin) { origin_ = origin; }
  // Explicit destructive action. It rejects every confirmation except ERASE
  // before it touches the card and is never called from setup/recovery.
  void confine_format(const std::string &confirmation);

  // Called by a time component's on-time-sync callback. It only publishes the
  // correspondence here; the writer task owns block_ and emits the #utc line on
  // its next notified pass, so this callback never races the card write path.
  void set_utc_anchor(uint64_t utc_us, uint64_t boot_us_at_sync);

  // Producer entry point. Non-blocking and safe to call from any task context
  // (guarded by a short critical section); returns false and counts a drop when
  // the ring is full — it never blocks the producer (spec D4).
  bool push_record(const LogRecord &rec);

  // Convenience for the action / future native taps.
  void log_frame(uint8_t source, uint32_t id, uint8_t flags, const uint8_t *data, uint8_t len);

  // Declare a source so the writer can resolve its tag to a type letter and a
  // label, and re-emit it as a `#src` line into every file (spec §6 F1d). Called
  // from codegen before setup(). An undeclared tag still logs — as `U,<decimal>`
  // — so a lambda pushing a tag of its own keeps working.
  void add_source(uint8_t tag, char kind, const char *label);

  // Enable the ESPHome log capture (source S4). `level` is the numeric
  // ESPHOME_LOG_LEVEL_* ceiling; `depth` is the text ring depth in slots.
  void set_esphome_logs(uint8_t level, uint16_t depth) {
    log_level_ = level;
    text_depth_ = depth;
  }

#ifdef USE_SD_LOGGER_CAN_TAP
  // Register a can_gateway port whose tap ring the writer task drains (S2).
  // Called from codegen before setup(). `source` is the tag written into every
  // record from this port, so both segments stay distinguishable in one file,
  // and `label` is how that tag reads in the file and in its `#src` line.
  void add_can_tap(can_gateway::GatewayPort *port, uint8_t source, const char *label);
  // Frames the gateway's RX ISR had to drop because this logger fell behind and
  // a tap ring filled up. Reported apart from dropped_records_ on purpose: the
  // two counters name different bottlenecks (ISR->ring vs producer->ring), and
  // telling them apart is the whole diagnostic value when a run falls behind.
  uint32_t get_tap_dropped_records() const;
#endif

  // Trigger a best-effort close from outside the VCC monitor (e.g. a button).
  // Idempotent. `reason` is what lands in the file's `#close` line, and the
  // only reason a reader can tell an orderly end from a power cut.
  void request_emergency_close(const char *reason = "emergency") {
    close_reason_ = reason;
    dying_.store(true, std::memory_order_release);
  }

  // Getters (stats / template sensors)
  bool is_mounted() const { return mounted_; }
  bool identification_degraded() const { return degraded_identification_; }
  bool capacity_degraded() const { return capacity_state_ != CapacityState::HEALTHY; }
  bool is_degraded() const { return this->identification_degraded() || this->capacity_degraded(); }
  const char *capacity_status() const;
  uint32_t get_records_written() const { return records_written_; }
  uint32_t get_dropped_records() const { return dropped_records_; }
  uint32_t get_bytes_written() const { return bytes_written_; }
  uint32_t get_text_dropped_records() const { return text_dropped_; }
  // Records *and captured log lines* lost because there was no writable file at all — a card that
  // never mounted, one that failed mid-run, or the window while recovery is retrying. Its own
  // counter because it names a different fault from every other drop: no ring was full and no
  // producer was too fast, there was simply nowhere to put the thing. One counter for both units
  // on purpose — the question it answers is "how much did the outage cost", not "of what".
  uint32_t get_card_dropped_records() const { return card_dropped_; }
  /// Frame records formatted into the writer block but discarded when the file failed before their
  /// complete lines reached write(). Kept separate from card_dropped_: these had entered logging.
  uint32_t get_write_lost_records() const { return write_lost_.load(std::memory_order_relaxed); }
  /// Tap entries intentionally left behind at emergency close; one counter per port is retained
  /// internally so their `#drop` markers remain attributable, this is the status total.
  uint32_t get_tap_shutdown_lost_records() const;
  uint32_t get_tap_accepted_records() const;
  uint32_t get_tap_drained_records() const;
  uint32_t get_tap_record_ring_accepted_records() const;
  uint32_t get_recovery_attempts() const { return recovery_.attempts(); }
  // Retention's lifetime losses (design §7): chunks that were never collected and are now gone, and
  // what they held. Mirrored out of CollectionPolicy by the writer task rather than read from it,
  // because the index belongs to that task (§8) and these are read from the main loop.
  //
  // They exist because `#gap` states *deltas* only, unlike `#drop`, whose lifetime `total` column
  // lets a reader recover the truth from the next marker even if one line never reached the card.
  // Without a lifetime total surfaced anywhere, the discards would be visible only in the `#gap`
  // lines themselves — one console line, one card, one chance. In KiB rather than bytes so the
  // counter is a plain 32-bit load on this chip: 4 TiB of range against the ~12 GB/day design §7
  // budgets, and no 64-bit atomic on a 32-bit core to read it.
  uint32_t get_discarded_chunks() const { return discarded_chunks_.load(std::memory_order_relaxed); }
  uint32_t get_discarded_kib() const { return discarded_kib_.load(std::memory_order_relaxed); }
  // Chunks that exist on the card but are not in the index, because it was full when they appeared.
  // Not a data loss on its own — the files are there and the writer keeps writing — but every one of
  // them is a file retention can never reclaim and `/sdlog/index` will never list, so a card with a
  // non-zero count here fills up and stays full. It is a *lifetime* count of refusals, not the
  // current shortfall: a remount rescans and the same file can be refused again. The fix is
  // `collection: max_chunks:`, and the number this reports is what it has to clear.
  uint32_t get_index_refused() const { return index_refused_.load(std::memory_order_relaxed); }

  // The logger callback (S4). Public because it is reached through a plain
  // function pointer from the logger's callback table, not through a member.
  void on_log_message(uint8_t level, const char *tag, const char *message, size_t message_len);

  // --- the collection server's whole view of this component (design §5/§8a). Phase B. ---
  //
  // Every one of these runs on the httpd task and takes `index_mux_` for the length of one index
  // operation. They exist rather than a `friend` declaration because the locking discipline is the
  // interesting part: it is auditable exactly as long as every `this->collection_.` in the codebase
  // sits inside an `IndexLock` scope, and handing a second task a raw reference to the policy is how
  // that stops being true.
  const std::string &mount_point() const { return this->mount_point_; }

  // Ring occupancy, for §6's backpressure: above ~50 % a transfer in flight sleeps before its next
  // card read, because a collection run must never be able to turn into `dropped_records`.
  //
  // Two `volatile` reads without `ring_mux_`, which is the one unsynchronised cross-task read in
  // this component and is said out loud here for that reason. It is a *heuristic*: a torn read
  // misestimates the fill by one producer pass and the next block corrects it, whereas taking the
  // producers' critical section from the httpd task would put the record path behind a socket-facing
  // task. Never use this number for anything that has to be exact.
  uint8_t ring_fill_percent() const {
    if (this->buffer_depth_ == 0)
      return 0;
    const uint32_t head = this->head_;
    const uint32_t tail = this->tail_;
    const uint32_t used = (head - tail) & this->ring_mask_;
    return static_cast<uint8_t>((used * 100u) / this->buffer_depth_);
  }

#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  /// What `POST /sdlog/done/<name>` resolved to. Deliberately not an HTTP status: the mapping to
  /// 200/404/409/503 belongs next to the wire contract in collection_server.cpp, and the decision
  /// belongs next to the index.
  enum class ConfirmResult : uint8_t {
    CONFIRMED,    ///< the chunk is (or already was) CONFIRMED and the rename is queued or done -> 200
    NOT_TRACKED,  ///< no such seq: retention took it, or the index was full when the file appeared
    IS_OPEN,      ///< the file the writer holds an fd to. Confirming it would make it deletable
    BUSY,         ///< the writer's intent queue is full; retryable
  };

  enum class FsDebugResult : uint8_t {
    OK,
    NOT_INDEXED,
    UNAVAILABLE,
  };

  enum class RawReadResult : uint8_t {
    OK,
    INVALID,
    UNAVAILABLE,
  };

  uint16_t collection_count() const;
  /// A **copy** of entry `index`, taken under the lock. `false` past the end, which is also how a
  /// walk that dropped the lock between entries discovers that `discard()` compacted the array.
  bool collection_entry_at(uint16_t index, ChunkEntry *out) const;
  /// One locked transaction: read the state and set the serving flag together. Splitting them
  /// leaves a window in which retention deletes the file that is about to be served, which is why
  /// `mark_serving()` is the one index mutation the httpd task cannot defer to the writer (§8a).
  ///
  /// `false` for anything not SEALED, with `*state_out` set so the caller can tell 409 from 404;
  /// `true` grants the transfer, and the caller then owes `collection_end_serve()` on **every**
  /// exit path.
  bool collection_begin_serve(uint32_t seq, uint32_t *bytes_out, ChunkState *state_out);
  /// **Exactly once per successful `collection_begin_serve()`.** Two things ride on the pairing:
  /// the serving flag retention checks, and the in-flight count `unmount_card_()` waits on.
  void collection_end_serve(uint32_t seq);
  /// Read-only raw FAT32 inspection for `/sdlog/fsdebug`. The caller provides a bounded JSON
  /// buffer; this never writes, mounts, formats, or repairs the card.
  FsDebugResult collection_fs_debug(const char *name, char *out, size_t out_size);
  /// A raw card-read grant with the same unmount gate as fsdebug, but no chunk reservation. The
  /// matching end call is required after every successful begin, including a socket send failure.
  RawReadResult collection_begin_raw_read(uint32_t lba, uint32_t count, char *error, size_t error_size);
  bool collection_raw_read_sector(uint32_t lba, uint8_t *out, char *error, size_t error_size);
  void collection_end_raw_read();
  /// A page of the raw FAT chain for an indexed chunk. This is read-only and keeps its chunk
  /// reservation while walking, so retention cannot rename or discard the directory entry under it.
  FsDebugResult collection_chain_debug(const char *name, uint32_t from, uint32_t count, char *out, size_t out_size);
  /// The SDSPI command ring is intentionally lock-free: the reader snapshots it while transactions
  /// continue, so its tail may be torn rather than ever delaying the card command path.
  SdSpiTraceRing &collection_sdspi_trace();
  /// The confirm, index-side. Flips SEALED -> CONFIRMED and hands the *rename* to the writer as an
  /// intent, because every filesystem mutation belongs to that task (§8).
  ConfirmResult collection_confirm(uint32_t seq);
  bool collection_oldest_uncollected(uint32_t *seq_out) const;
  uint32_t collection_discarded_chunks() const;
  uint64_t collection_discarded_bytes() const;
  /// Card fill for `GET /sdlog/status`, published by the writer. Above 100 means "not known yet" —
  /// the httpd task must not call `card_fill_percent_()` itself, because that is an SD access and
  /// the first `f_getfree` on a 32 GB card walks the whole FAT.
  uint32_t collection_card_percent() const { return this->card_fill_pub_.load(std::memory_order_relaxed); }
#endif

 protected:
  static void writer_trampoline(void *arg) { static_cast<SdLogger *>(arg)->writer_loop_(); }
  static void monitor_trampoline(void *arg) { static_cast<SdLogger *>(arg)->monitor_loop_(); }
  void writer_loop_();
#ifdef USE_SWITCH
  // Cooperatively park at the end of a writer pass. It seals rather than
  // suspending a task mid-FatFs call, so a frozen card has neither a held
  // filesystem lock nor a partially buffered record.
  void debug_freeze_writer_();
  void publish_debug_writer_frozen_(bool frozen);
#endif
  void monitor_loop_();
  // Attach the eFuse calibration curve to the VCC channel, or say loudly that there is none.
  void init_vcc_calibration_(adc_channel_t channel);
  // Convert one raw ADC count to a rail voltage, through the curve when there is one.
  float vcc_rail_volts_(int raw) const;

  // `allow_format` is false on every recovery attempt, whatever format_if_mount_failed says: a
  // retry ladder that reformats would erase the very logs the card is being recovered for, and it
  // would do it repeatedly. Only the boot mount honours the option (V21 says so out loud).
  bool mount_card_(bool allow_format);
  void unmount_card_();
  // Run one card-initialisation attempt, and on the one timeout which can mean a latched ACMD41
  // handshake, require card_probe_ready() evidence before arming the response mask and retrying.
  // `cleanup_failed_attempt` releases anything the attempt owns before the raw-SPI probe borrows
  // the bus; the VFS mount cleans up internally, while confine-format owns an SDSPI device itself.
  esp_err_t init_card_with_latched_idle_recovery_(esp_err_t (*attempt)(void *), void (*cleanup_failed_attempt)(void *),
                                                  void *context);
  bool run_capacity_self_test_(uint64_t *volume_bytes_out);
  bool logging_permitted_() const { return this->capacity_state_ == CapacityState::HEALTHY; }
  // Talk the card out of an aborted transaction before the next mount, over a raw SPI device on the
  // same four pins at 400 kHz. Runs only between an unmount and a mount, when nothing else owns the
  // bus. Returns what the card said; the mount is attempted regardless, because the reset's own
  // reading of "mute" is a diagnosis and not a verdict.
  CardResetResult run_card_reset_(uint32_t busy_timeout_ms);
  // Ask the card to prove it works when identification has timed out: OCR bit 31 plus a real block
  // read. True only on evidence, and only then may the caller mask the latched idle bit.
  bool run_card_probe_ready_(CardReadyResult *out);
  // Raw 400 kHz SPI on the logger's own pins with CS as a plain GPIO, shared by both of the above.
  bool open_raw_spi_(spi_device_handle_t *dev, bool *owns_bus);
  void close_raw_spi_(spi_device_handle_t dev, bool owns_bus);
  bool open_next_file_();
  void rotate_file_();
  // Both rotation bounds, tested against the writer pass's single clock read. Rotation is decided
  // in collection_policy.h so the host suite can reach it; this is the part that has to know what
  // time it is and what the file already holds.
  void maybe_rotate_(uint64_t now64);
  // The one place `max_file_size` and `max_file_seconds` reach the policy, called from both setters
  // so the pair cannot end up half-applied whichever order codegen emits them in.
  void apply_rotation_bounds_() { collection_.configure_rotation(max_file_size_, max_file_seconds_); }
  // Card fill in percent, straight off the mounted FAT volume. False when the volume cannot answer.
  bool card_fill_percent_(uint8_t *fill_out) const;
#ifdef USE_SD_LOGGER_COLLECTION
  // Retention (design §7): above the fill threshold, ask the policy for a victim and unlink it.
  // **Writer task only** — §8 gives every filesystem mutation to the writer, and an unlink from
  // loop() would race the remount that card recovery runs on exactly this card.
  void retention_pass_();
#endif
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // Execute the confirms the httpd task posted: `.LOG` -> `.UPL`, on the writer task, for the same
  // reason retention's unlink is here. Drained next to `retention_pass_()` and only while a file is
  // open — a rename against an unmounted card would "succeed" as ENOENT and silently spend the
  // intent, leaving a `.LOG` file the rebuilt index calls SEALED and the collector fetches again.
  void drain_confirms_();
#endif
  uint32_t scan_next_seq_();
  void emergency_close_();
  bool pop_record_(LogRecord &out);
  bool pop_text_(TextRecord &out);

  // --- the failure/recovery path (spec §7 Layer C). Writer task, plus setup(). ---
  // The single transition into "no writable file": closes the fd, stops producers, and arms the
  // retry ladder. Everything that can discover a dead card routes through here, so `mounted_` and
  // `fd_` can never disagree about whether the logger is running.
  void enter_failed_(const char *why);
  // One bounded remount attempt: unmount, optionally power-cycle the card, mount, open a file.
  bool try_recover_();
  // Is there a file to write into right now? The writer's drain loops all gate on this, because a
  // write that fails mid-pass must not have the rest of the pass silently discarded into it.
  bool file_ok_() const { return fd_ >= 0; }

  // --- the write path (spec §5 D6, §6). Writer task only. ---
  // Records are formatted directly into block_ at the write cursor; block_ hands
  // whole sectors to write(). Nothing here uses stdio: a FILE* and setvbuf buy
  // nothing once the writer owns its own buffer, and they cost a lock per line.
  void write_record_(const LogRecord &rec, uint64_t now64);
  void write_text_(const TextRecord &rec, uint64_t now64);
  // Reserve room for one line, flushing whole sectors first if the buffer is
  // running low. Returns false when the file is not writable.
  bool ensure_room_(size_t need);
  void commit_line_(size_t len);
  // Push whole sectors to the card. `all` also writes the sub-sector remainder,
  // which is only ever correct as the last write of a file.
  bool flush_block_(bool all);
  void sync_file_();
  // Header block: #sdlog, one #src per declared source, #types, #flags, #pad.
  void write_file_header_();
  void write_utc_marker_();
  // `#drop` markers, emitted only when a counter actually moves (spec D4/F1d).
  void write_drop_markers_(uint64_t now64);

  // --- the `#gap` window between "retention deleted it" and "the card says so" (design §7/§7a) ---
  // A window leaves CollectionPolicy once and never goes back, so from `take_pending_discards()`
  // until the line stating it has actually reached the card, the writer is the only thing holding
  // it. These three keep that window alive across every accident in between.
  //
  // Why it cannot simply be formatted and forgotten: `commit_line_()` moves bytes into `block_`,
  // not onto the card, and `enter_failed_()` resets `block_` — so a write error in the same 20 ms
  // pass discards up to 4 KB of committed lines. `#drop` survives that because `format_drop()`
  // carries a lifetime `total` column a reader can recover from; `format_gap()` carries deltas only,
  // and a lost line means the loss is stated nowhere on the card, ever.
  //
  // Fold a window retention just emptied into the one the writer still owes a line for.
  void owe_gap_(const DiscardStats &window);
  // Spend the in-flight window once the bytes behind its line have gone through write().
  void retire_gap_if_durable_();
  // The block buffer is about to lose what is committed in it: any in-flight window goes back to
  // being owed, so it lands in the next file instead of vanishing. Called at **every** `block_`
  // reset — that is the whole invariant, and a new reset that forgets it re-opens this hole.
  void drop_unflushed_gap_();
  // The single close path: `#close,<t_us>,<reason>`, flush, fsync, close. False when the final
  // flush failed, in which case the file is already down and recovery is armed — the caller must
  // not go on to open a successor.
  bool close_file_(const char *reason);

  // Pins / SD config
  int8_t clk_pin_{-1}, mosi_pin_{-1}, miso_pin_{-1}, cs_pin_{-1};
  int8_t card_power_pin_{-1};
  uint32_t clock_hz_{20000000};
  std::string mount_point_{"/sdcard"};
  bool format_if_mount_failed_{false};

  // Ring
  LogRecord *ring_{nullptr};
  uint32_t buffer_depth_{2048};
  uint32_t ring_mask_{0};
  volatile uint32_t head_{0};  // producer index
  volatile uint32_t tail_{0};  // consumer index
  portMUX_TYPE ring_mux_ = portMUX_INITIALIZER_UNLOCKED;

  // Text ring (source S4, spec D9). Its own ring because a log line is
  // variable-length and cannot share the fixed-size record ring; drained by the
  // same writer in the same pass.
  TextRecord *text_ring_{nullptr};
  uint16_t text_depth_{0};  // 0 = capture disabled
  uint16_t text_mask_{0};
  volatile uint16_t text_head_{0};
  volatile uint16_t text_tail_{0};
  portMUX_TYPE text_mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t log_level_{0};

  // File / write policy
  int fd_{-1};
  BlockBuffer block_;
  UnflushedRecordTracker unflushed_records_;
  char *block_storage_{nullptr};
  uint32_t seq_{0};
  uint32_t max_file_size_{16u * 1024 * 1024};
  uint32_t max_file_seconds_{0};  // 0 = unbounded, i.e. rotation on size alone
  uint32_t file_bytes_{0};        // bytes appended to the *current* file; drives rotation
  // When the open file was opened, for the time bound. Microseconds off the same monotonic clock
  // every record is stamped from, so the two bounds are read against one timebase.
  uint64_t file_opened_us_{0};
  uint32_t sync_interval_ms_{2000};
  uint32_t last_sync_ms_{0};
  uint32_t last_retention_ms_{0};

  // Tag -> type letter + label, built at codegen and re-emitted as `#src` lines
  // into every file so a card explains itself without the YAML (spec §6 F1d).
  SourceTable sources_;

  // Configured identity, paid once in each header rather than on every record.
  std::string origin_;

  // A sync callback runs outside the writer task. This tiny critical section
  // publishes its two 64-bit values as one pair; record formatting never takes it.
  portMUX_TYPE utc_mux_ = portMUX_INITIALIZER_UNLOCKED;
  UtcAnchor utc_anchor_;
  uint32_t utc_generation_{0};
  uint32_t utc_emitted_generation_{0};

  // The stamp column's delta chain (spec §6 F1h). Writer-task only, like `block_`, and reset by
  // write_file_header_() so every sealed chunk decodes without the chunks before it.
  DeltaClock stamps_;

  // Counters (writer-task only, except the dropped ones which producers bump)
  uint32_t records_written_{0};
  uint32_t bytes_written_{0};
  std::atomic<uint32_t> dropped_records_{0};
  std::atomic<uint32_t> text_dropped_{0};
  std::atomic<uint32_t> card_dropped_{0};
  std::atomic<uint32_t> write_lost_{0};
  // Chunks the index would not take: the boot scan past capacity, and every rotation once the index
  // is full. Written by the writer task (and setup's scan), read from loop() — atomic for the same
  // reason the drop counters are, not because it is hot. It moves once per rotation at worst.
  std::atomic<uint32_t> index_refused_{0};
  // Last values written into a `#drop` marker, so a marker costs nothing on a
  // healthy run and appears the moment a counter moves.
  uint32_t marked_dropped_{0};
  uint32_t marked_text_dropped_{0};
  uint32_t marked_card_dropped_{0};
  uint32_t marked_write_lost_{0};
  uint32_t marked_tap_shutdown_lost_{0};

  // VCC monitor / emergency
  int8_t vcc_adc_gpio_{-1};
  float vcc_threshold_v_{0.0f};
  float vcc_divider_{1.0f};
  uint32_t vcc_full_scale_mv_{3100};
  std::atomic<bool> dying_{false};
  // Written before the release-store on dying_, read after the acquire-load, so
  // the writer task always sees the reason that set the flag.
  const char *close_reason_{"emergency"};
  adc_oneshot_unit_handle_t adc_handle_{nullptr};
  // The eFuse calibration curve. Without it the raw-count estimate below reads ~26 % low on the
  // C6 — measured 8.94 V for a rail an esphome `adc:` on the same pin put at 12.07 V — which
  // trips the emergency close on a perfectly healthy supply and latches logging off for the run.
  // Null means no scheme was available and the linear fallback is in use; the fallback is
  // announced at setup precisely because it is the broken path.
  adc_cali_handle_t adc_cali_{nullptr};

  // Card recovery (spec §7 Layer C)
  RecoveryPolicy recovery_;
  bool recovery_power_cycle_{false};

  // Chunk lifecycle, rotation bounds and retention (design §3/§4/§7). The *card* has one mutator,
  // the writer task (§8): the boot scan fills the index, close_file_() seals, retention_pass_()
  // discards, drain_confirms_() renames. The *index* is shared with the collection server's httpd
  // task, which is what §8 alone does not settle and §8a does — every access below goes through
  // `index_mux_`, including the writer's own, and `dump_config`/the stats line still report
  // configured values only because those are written by codegen before any task exists.
  CollectionPolicy collection_;
  // §8a. Created in setup() only when a server was asked for; null otherwise, and `IndexLock` is
  // then a null check. Guards every `this->collection_.` in this component bar the accessors that
  // only read configuration — `enabled()`, `retention_percent()`, `should_rotate()` — which codegen
  // writes before any task exists and nothing changes afterwards.
  SemaphoreHandle_t index_mux_{nullptr};
  // The collection server's port, and whether a server was asked for at all. Kept even in a build
  // without the server so dump_config can say what the config asked for.
  uint16_t collection_port_{0};
  bool collection_serve_{false};
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  CollectionServer collection_server_{this};
  // Set by setup(), consumed by the first loop() pass — the server may NOT be started from setup().
  //
  // This component is `setup_priority::DATA` (600) and esphome's wifi component is
  // `setup_priority::WIFI` (250), so **sd_logger::setup() runs first**, and at that moment lwip does
  // not exist yet: no `esp_netif_init()`, no TCPIP core mutex. `httpd_start()` opens its control
  // socket immediately, so the first lwip call locks a null `sys_mutex` and the board dies in
  // FreeRTOS rather than in any code here — `assert failed: xQueueSemaphoreTake queue.c:1709
  // (( pxQueue ))`, on every boot, ~1.3 s in. Measured on Mr. Orange 2026-07-29, the first time
  // Phase B was ever flashed; nothing in the test suite can see it, because
  // tests/build/sd_logger/common.yaml compiles this path and CI never runs it on hardware.
  //
  // The earlier "the socket binds on 0.0.0.0, so it does not need WiFi to be associated yet" is
  // still true and is not what was wrong: *association* is not needed, *initialisation* is, and the
  // two are different events. esphome runs loop() only once every component's setup() has returned,
  // so the first pass is the earliest point at which the stack is guaranteed up — and it is still
  // long before any association, which keeps the original intent intact.
  bool collection_server_pending_{false};
  // `confirm <seq>` intents, httpd task -> writer task (§8). Shallow on purpose: the collector is
  // strictly sequential, so more than one confirm can only be in flight if the writer has not run,
  // and a full queue is a 503 the client retries rather than a backlog nobody is draining.
  QueueHandle_t confirm_q_{nullptr};
  // Card fill, published by the writer for `GET /sdlog/status`. > 100 is "not known yet". Published
  // rather than read on demand because reading it is an SD access and the httpd task may not make
  // one (§9.4).
  std::atomic<uint32_t> card_fill_pub_{0xFFFFFFFFu};
  // Chunk transfers the httpd task currently holds a read fd for. Not a diagnostic: it is what
  // `unmount_card_()` waits on, because unmounting frees the VFS context those fds live in.
  std::atomic<uint32_t> transfers_in_flight_{0};
  // **The serving gate.** "New transfers may be granted", and the reason `unmount_card_()`'s drain
  // is a barrier rather than a hope. Guarded by `index_mux_` and read in the *same* critical section
  // as the `find()`/`mark_serving()` pair in `collection_begin_serve()`, because the whole point is
  // that the two decisions cannot be split: the writer clears this flag before it starts draining,
  // so once the drain observes zero in-flight transfers, no later grant can create one.
  //
  // Without it, waiting for `transfers_in_flight_ == 0` proves only that no transfer was in flight
  // at the instant it was read. `collection_begin_serve()` granted on index state alone, so the
  // httpd task could open a read fd *after* the drain loop exited and `esp_vfs_fat_sdcard_unmount()`
  // would then free the VFS context that owns it — a use-after-free on the next `::read()` and a
  // second one on `~FdGuard`'s `::close()`. A longer timeout cannot fix that; only a closed state
  // can. Also gates `collection_confirm()`, so a confirm cannot be answered 200 into an intent queue
  // whose draining task is on its way out (the emergency path).
  //
  // Cleared by `unmount_card_()` before the drain and set by `mount_card_()` after a successful
  // mount, so it is false for the whole teardown/remount window and for the whole emergency close.
  bool serving_open_{false};
#endif

  // The `#gap` window the writer owes a line for, and the one a committed line already states.
  // Both survive `enter_failed_()`, a rotation and a remount, for the same reason
  // `marked_card_dropped_` survives `write_file_header_()`: the accounting outlives the file, and
  // the first marker pass of the next file is where the loss gets stated. `chunks == 0` is "none".
  DiscardStats gap_owed_{};
  DiscardStats gap_inflight_{};
  // File offset just past the committed `#gap` line, i.e. how much of *this* file has to reach the
  // card before `gap_inflight_` is spent. Meaningless without an in-flight window, and cleared with
  // it, because file offsets restart at every open.
  uint32_t gap_line_end_{0};
  // Lifetime discard totals, mirrored off the writer task for the stats line and template sensors.
  std::atomic<uint32_t> discarded_chunks_{0};
  std::atomic<uint32_t> discarded_kib_{0};

  // sdmmc / tasks
  sdmmc_card_t *card_{nullptr};
  TaskHandle_t writer_task_{nullptr};
  TaskHandle_t monitor_task_{nullptr};
#ifdef USE_SWITCH
  // Both are runtime-only diagnostics. The switch always starts OFF and its
  // state is deliberately not restored across a reboot.
  std::atomic<bool> debug_writer_freeze_requested_{false};
  std::atomic<bool> debug_writer_frozen_{false};
  bool debug_reopen_after_freeze_{false};
  DebugWriterFreezeSwitch *debug_writer_freeze_switch_{nullptr};
#endif
  // When the previous writer pass started (µs since boot); 0 until the first pass. Writer task
  // only. Feeds the pass-gap warning — the direct measurement of the writer outages §4.3 could
  // only infer from tap-ring burst sizes.
  int64_t last_pass_us_{0};
  volatile bool mounted_{false};
  bool spi_bus_ok_{false};
  // True while the current mount only exists because the card's latched "in idle state" bit is
  // being masked. Worth surfacing rather than hiding: the card is working but has not completed a
  // real initialisation since it last had power, so the next power cycle is still owed to it.
  bool degraded_identification_{false};
  enum class CapacityState : uint8_t {
    HEALTHY,
    VOLUME_TOO_BIG,
    SELF_TEST_FAILED,
  };
  CapacityState capacity_state_{CapacityState::HEALTHY};
  std::atomic<bool> confine_format_stop_requested_{false};
  std::atomic<bool> confine_format_in_progress_{false};

  // Stats logging
  uint32_t stats_log_interval_ms_{60000};
  uint32_t last_stats_log_ms_{0};

#ifdef USE_SD_LOGGER_CAN_TAP
  struct CanTap {
    can_gateway::GatewayPort *port{nullptr};
    uint8_t source{0};
    const char *label{""};
    // Last value written into this tap's `#drop` marker. Per tap, not summed:
    // one segment falling behind is a different diagnosis from both doing so.
    uint32_t marked_dropped{0};
    uint32_t marked_shutdown_lost{0};
    // The gateway owns per-ring accepted atomics. These are per-port aggregates maintained by the
    // sole drain task: accepted -> drained -> record-ring accepted localises the first divergence.
    std::atomic<uint32_t> drained{0};
    std::atomic<uint32_t> record_ring_accepted{0};
    std::atomic<uint32_t> shutdown_lost{0};
  };
  CanTap can_taps_[SD_LOGGER_CAN_TAP_MAX]{};
  uint8_t can_tap_count_{0};
  // The tap rings' single consumer (the SPSC contract allows exactly one), and it is NOT the
  // writer: the writer spends up to hundreds of ms inside one card call — sdspi busy-polls a
  // card's internal write stall with the FATFS volume mutex held — and for as long as it does,
  // whoever drains the taps is not running. §4.3 measured 4765 records lost that way in 6
  // stall-shaped bursts. So a dedicated task pops the tap rings into the record ring, which never
  // touches the card and is sized to absorb ~585 ms of writer outage at full bus rate — and whose
  // fill is exactly what the collection server's backpressure_() throttles transfers on.
  static void tap_drain_trampoline(void *arg) { static_cast<SdLogger *>(arg)->tap_drain_loop_(); }
  void tap_drain_loop_();
  TaskHandle_t tap_drain_task_{nullptr};
#endif
};

#ifdef USE_SWITCH
/// A diagnostic-only control. Its published state is an acknowledgement from
/// the writer task, not merely the request made by a remote client.
class DebugWriterFreezeSwitch : public switch_::Switch, public Component, public Parented<SdLogger> {
 public:
  void setup() override {
    // Explicitly ignore every persisted switch value: a reboot must always
    // bring the logger up able to write.
    this->parent_->set_debug_writer_freeze_requested(false);
    this->publish_state(false);
  }

  void publish_frozen(bool frozen) { this->publish_state(frozen); }

 protected:
  void write_state(bool state) override { this->parent_->set_debug_writer_freeze_requested(state); }
};
#endif

// sd_logger.log action (source S1).
template<typename... Ts> class LogAction : public Action<Ts...> {
 public:
  explicit LogAction(SdLogger *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(uint32_t, can_id)
  void set_source(uint8_t source) { source_ = source; }
  void set_data_static(const std::vector<uint8_t> &data) { data_static_ = data; }
  void set_data_template(std::function<std::vector<uint8_t>(Ts...)> func) { data_func_ = func; }

  void play(const Ts &...x) override {
    std::vector<uint8_t> data = data_func_.has_value() ? (*data_func_)(x...) : data_static_;
    uint8_t flags = 0;
    uint8_t len = data.size() > 8 ? 8 : static_cast<uint8_t>(data.size());
    if (data.size() > 8)
      flags |= REC_FLAG_TRUNCATED;
    parent_->log_frame(source_, this->can_id_.value(x...), flags, data.data(), len);
  }

 protected:
  SdLogger *parent_;
  uint8_t source_{0};
  std::vector<uint8_t> data_static_;
  optional<std::function<std::vector<uint8_t>(Ts...)>> data_func_;
};

/// Explicit destructive operation; it has no boot path and only runs when an
/// automation invokes `sd_logger.confine_format` with confirmation: ERASE.
template<typename... Ts> class ConfineFormatAction : public Action<Ts...> {
 public:
  explicit ConfineFormatAction(SdLogger *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, confirmation)

  void play(const Ts &...x) override { this->parent_->confine_format(this->confirmation_.value(x...)); }

 protected:
  SdLogger *parent_;
};

}  // namespace sd_logger
}  // namespace esphome
