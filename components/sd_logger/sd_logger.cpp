#include "sd_logger.h"
#include "esphome/core/log.h"
// ESPHOME_VERSION, for the `#sdlog` header line: a card should say which
// firmware wrote it without anyone having to remember.
#include "esphome/core/version.h"

#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

extern "C" {
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
}

namespace esphome {
namespace sd_logger {

static const char *const TAG = "sd_logger";

// Writer task cadence: it wakes at least this often to flush/rotate even when
// the ring trickles. The ring absorbs bursts between wakes (spec D5).
static const uint32_t WRITER_POLL_MS = 20;
// VCC monitor sampling period. The 12 V rail sags slowly behind its bulk cap,
// so a few ms of latency here still leaves the hold-up budget intact (spec §7).
static const uint32_t MONITOR_POLL_MS = 5;
// Consecutive under-threshold samples before we declare the supply dying, to
// reject a single noisy read.
static const uint8_t VCC_TRIP_SAMPLES = 2;
// The writer's own output buffer (spec D6). Lines are formatted directly into
// it at the write cursor and it hands write() whole 512 B sectors, so FATFS
// never does a read-modify-write on a partial one. Eight sectors: big enough
// that a full drain pass rarely flushes twice, small enough to stay off the
// heap's back.
static const size_t BLOCK_BUF_SIZE = 8 * SD_LOG_SECTOR;
// How long card power stays off during a recovery power cycle. A wedged card
// holds its state through a brief dip — this has to be long enough for its
// internal rail to actually collapse, which is the whole point of the exercise.
static const uint32_t CARD_POWER_OFF_MS = 150;
// Power-on ramp before the first SPI transaction, same as setup() gives it.
static const uint32_t CARD_POWER_SETTLE_MS = 10;
// Identification speed. The in-band reset runs here and nowhere near the configured clock: every
// card is required to answer CMD0 at 400 kHz, and a reset that failed because the bus was too fast
// would be indistinguishable from the wedge it is trying to clear.
static const int CARD_RESET_CLOCK_HZ = 400000;
// Bytes clocked between yields during the reset. At 400 kHz a byte is 20 µs, so this is ~2.5 ms of
// uninterrupted polling: long enough that the busy poll is not mostly context switches, short
// enough that a ten-second wait does not starve the idle task into a watchdog reset. The writer
// task runs at priority 6, above the main loop, so nothing else would get in.
static const uint32_t CARD_RESET_YIELD_BYTES = 128;
// How often the writer asks the card how full it is. Not every pass: card fill only moves at
// rotation speed (a chunk every few seconds at best), while esp_vfs_fat_info() is a FATFS call on
// the same SPI bus the writer is streaming over. One pass in 250 is plenty to stay ahead of a
// writer producing ~150 KB/s against chunks of megabytes.
static const uint32_t RETENTION_POLL_MS = 5000;
// Any single card/file call held longer than this gets a warning naming the call and its duration.
// 50 ms is the ESPHome loop-blocking threshold, reused deliberately: the same magnitude that would
// be shouted about in loop context should not pass silently in a task. This is the instrument that
// attributes a tap burst to the card stall that caused it (§4.3: bursts implied 220-360 ms writer
// outages, and nothing in the system measured them directly).
static const int64_t STALL_WARN_US = 50000;
#ifdef USE_SD_LOGGER_CAN_TAP
// Tap-drain task cadence. At 7000 f/s (two segments saturated at 500 kbit/s) one pass moves ~35
// records; a 1024-slot tap ring gives ~146 ms of slack, so 5 ms of drain latency leaves a wide
// margin even against preemption by lwip/WiFi bursts.
static const uint32_t TAP_DRAIN_POLL_MS = 5;
// Statically allocated (.bss, where RAM sits at ~34 %) rather than heap: §4.3 measured the free
// heap's min-since-boot at 1668 B under load, and a task stack taken from the heap at setup would
// move that floor down by its whole size.
static const uint32_t TAP_DRAIN_STACK_BYTES = 3072;
static StaticTask_t tap_drain_tcb;                                                // NOLINT
static StackType_t tap_drain_stack[TAP_DRAIN_STACK_BYTES / sizeof(StackType_t)];  // NOLINT
#endif
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
// `confirm <seq>` intents waiting for the writer to do the rename (design §8). Four is generous:
// the collector is strictly sequential — one request in flight, ever — so a second entry already
// means the writer has not run for a whole confirm round trip, and the writer runs every 20 ms.
// A full queue answers 503, which the client retries; it is not a backlog anyone is draining.
static const uint8_t SD_LOGGER_CONFIRM_QUEUE_DEPTH = 4;
// How long unmount_card_() waits for an in-flight chunk transfer to let go of its read fd before
// pulling the filesystem out from under it. See the note there for why this is bounded and why it
// gives up rather than blocking logging indefinitely.
//
// Derived from the server's own send timeout rather than picked: a handler stalled on a socket write
// is released by `SO_SNDTIMEO`, so a drain bound *shorter* than that expires on every slow peer by
// construction — which is what the previous 500 ms did against a 2 s send timeout, four times over.
// The half second on top covers the card read that follows the send returning.
static const uint32_t UNMOUNT_DRAIN_MS = SD_LOG_SEND_WAIT_S * 1000 + 500;
#endif

void SdLogger::setup() {
  ESP_LOGCONFIG(TAG, "Setting up SD logger...");

  // The policy is the only rotation authority from here on, and its bounds default to unbounded.
  // Codegen always calls set_max_file_size(), so this is belt and braces — but the failure it
  // guards against is a file that never rotates at all, which on a card looks exactly like a bus
  // that never produced enough to fill one.
  this->apply_rotation_bounds_();

#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // Before anything touches the chunk index — the boot scan below is the first thing that does, and
  // by the time the httpd task exists there must already be a lock for it to take (design §8a).
  // Both objects are static-sized FreeRTOS primitives; failing to get one is not fatal to logging,
  // so it costs the server rather than the run.
  this->index_mux_ = xSemaphoreCreateMutex();
  this->confirm_q_ = xQueueCreate(SD_LOGGER_CONFIRM_QUEUE_DEPTH, sizeof(uint32_t));
  if (this->index_mux_ == nullptr || this->confirm_q_ == nullptr)
    ESP_LOGE(TAG, "collection: index mutex/queue alloc failed — no server this run");
#endif

  // Card power first, if a high-side switch is present (H5), and give the card
  // its power-on ramp before we talk SPI.
  if (this->card_power_pin_ >= 0) {
    gpio_set_direction(static_cast<gpio_num_t>(this->card_power_pin_), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(this->card_power_pin_), 1);
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // Allocate the ring up front. Failure here is fatal to the component (no
  // point running), but not to the rest of the firmware.
  this->ring_mask_ = this->buffer_depth_ - 1;
  this->ring_ = new (std::nothrow) LogRecord[this->buffer_depth_];
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "ring alloc (%" PRIu32 " records) failed", this->buffer_depth_);
    this->mark_failed();
    return;
  }

  // The writer's block buffer. Allocated rather than a member so the component
  // object stays small and the 4 KB shows up in the heap report where the RAM
  // budget of spec §9 is actually read.
  this->block_storage_ = new (std::nothrow) char[BLOCK_BUF_SIZE];
  if (this->block_storage_ == nullptr) {
    ESP_LOGE(TAG, "write buffer alloc (%u B) failed", static_cast<unsigned>(BLOCK_BUF_SIZE));
    this->mark_failed();
    return;
  }
  this->block_.attach(this->block_storage_, BLOCK_BUF_SIZE);

  // Text ring for the ESPHome log capture (S4), only when it was configured.
  if (this->text_depth_ > 0) {
    this->text_mask_ = this->text_depth_ - 1;
    this->text_ring_ = new (std::nothrow) TextRecord[this->text_depth_];
    if (this->text_ring_ == nullptr) {
      ESP_LOGW(TAG, "text ring alloc (%u slots) failed - esphome log capture disabled", this->text_depth_);
      this->text_depth_ = 0;
    }
  }

  // Bring the card up. A failure here is no longer the end of the run: with `recovery:` on (the
  // default) the writer task starts regardless and retries on its own ladder. That is what turns
  // the bench's oldest sharp edge — a reset mid-write wedges the card until 12 V on J8 is cycled
  // — from a session-ender into a gap of a few seconds.
  if (this->mount_card_(this->format_if_mount_failed_)) {
    this->seq_ = this->scan_next_seq_();
    if (this->open_next_file_()) {
      this->last_sync_ms_ = millis();
      this->mounted_ = true;
    } else {
      ESP_LOGW(TAG, "card mounted but no log file could be opened");
      this->unmount_card_();
    }
  }

  if (!this->mounted_) {
    // A missing/failed card must NOT take down the buses: keep the component alive so producers
    // just drop, counted as `card` rather than silently.
    if (!this->recovery_.enabled()) {
      ESP_LOGW(TAG, "SD card not available and recovery is off — logging disabled, buses unaffected");
      return;
    }
    ESP_LOGW(TAG, "SD card not available — retrying in %" PRIu32 " ms, buses unaffected", this->recovery_.delay_ms());
    this->recovery_.arm(millis());
  }

  // Writer task: below LIN (18) and the TWAI ISR so bus timing always wins,
  // above the esphome main loop (1) so draining is steady (spec D5). Started even with no card,
  // because it is also what drives recovery.
  xTaskCreatePinnedToCore(&SdLogger::writer_trampoline, "sdlog_wr", 4096, this, 6, &this->writer_task_, tskNO_AFFINITY);

#ifdef USE_SD_LOGGER_CAN_TAP
  // Tap drain: one step above the writer, so a writer stuck inside a card call (sdspi busy-polls a
  // card's internal stall at full CPU, FATFS mutex held) cannot take the tap consumer down with it
  // — that coupling is what §4.3 measured as 4765 tap records lost in 6 stall-shaped bursts. Still
  // far below LIN (18), whose wire timing always wins. Static stack: see TAP_DRAIN_STACK_BYTES.
  if (this->can_tap_count_ > 0) {
    this->tap_drain_task_ = xTaskCreateStatic(&SdLogger::tap_drain_trampoline, "sdlog_tap", TAP_DRAIN_STACK_BYTES, this,
                                              7, tap_drain_stack, &tap_drain_tcb);
  }
#endif

  // VCC monitor task (optional): high priority so the sag is caught promptly,
  // but below LIN so it can never starve the bus.
  if (this->vcc_adc_gpio_ >= 0) {
    adc_oneshot_unit_init_cfg_t init_cfg = {};
    init_cfg.unit_id = ADC_UNIT_1;
    const adc_channel_t channel = static_cast<adc_channel_t>(this->vcc_adc_gpio_);
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &this->adc_handle_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "ADC init failed (%s) — VCC emergency-close disabled", esp_err_to_name(err));
    } else {
      adc_oneshot_chan_cfg_t chan_cfg = {};
      chan_cfg.atten = ADC_ATTEN_DB_12;
      chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
      // Checked, not discarded: a channel that never got configured still reads — it returns
      // whatever the unit defaults to — so the monitor would trip on a number that means nothing.
      err = adc_oneshot_config_channel(this->adc_handle_, channel, &chan_cfg);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel %d config failed (%s) — VCC emergency-close disabled", this->vcc_adc_gpio_,
                 esp_err_to_name(err));
      } else {
        this->init_vcc_calibration_(channel);
        xTaskCreatePinnedToCore(&SdLogger::monitor_trampoline, "sdlog_vcc", 3072, this, 17, &this->monitor_task_,
                                tskNO_AFFINITY);
      }
    }
  }

  // S4 last: the capture must not be live before the file exists. It *is* registered when the
  // card is absent but recovery is pending, because the point of recovery is that the file
  // arrives later; on_log_message() gates on mounted_ and counts what it drops, so until then the
  // callback costs a level compare and a counter.
  //
  // Registration is a plain function pointer into the logger's callback table
  // (add_on_log_callback and the LogListener interface are both gone in the
  // pinned version). It compiles to a no-op unless the Python side called
  // request_log_listener(), which is why __init__.py does — miss it and capture
  // silently does nothing at all (spec §4a).
#ifdef USE_LOGGER
  if (this->text_depth_ > 0 && logger::global_logger != nullptr) {
    logger::global_logger->add_log_callback(
        this, [](void *self, uint8_t level, const char *tag, const char *message, size_t message_len) {
          static_cast<SdLogger *>(self)->on_log_message(level, tag, message, message_len);
        });
  }
#endif

#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // Armed here, started on the first loop() pass — **never from setup()**. See
  // `collection_server_pending_` in the header for why: at DATA priority lwip has not been
  // initialised yet and `httpd_start()` takes a null TCPIP mutex, which is a FreeRTOS assert and a
  // boot loop, not a failed start this code could report.
  //
  // Unconditional on `serve: true` and deliberately not gated on `mounted_`. A board whose card
  // failed is exactly the board an operator wants to ask `GET /sdlog/status`, and recovery may hand
  // it a card later. It is not gated on association either — the socket binds on 0.0.0.0; V22 is
  // what makes sure WiFi is in the config at all.
  //
  // A server that will not start is not fatal: logging goes on and the chunks are collected by
  // taking the card out, which is `serve: false`'s whole shape.
  this->collection_server_pending_ =
      this->collection_serve_ && this->index_mux_ != nullptr && this->confirm_q_ != nullptr;
#endif

  ESP_LOGCONFIG(TAG, "SD logger up: mount=%s file seq=%" PRIu32, this->mount_point_.c_str(), this->seq_);
}

void SdLogger::init_vcc_calibration_(adc_channel_t channel) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id = ADC_UNIT_1;
  cali_cfg.chan = channel;
  cali_cfg.atten = ADC_ATTEN_DB_12;
  cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  const esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &this->adc_cali_);
  if (err == ESP_OK) {
    ESP_LOGCONFIG(TAG, "vcc_monitor: eFuse curve-fitting calibration active");
    return;
  }
  this->adc_cali_ = nullptr;
  ESP_LOGW(TAG,
           "vcc_monitor: no eFuse calibration on this chip (%s) — falling back to the raw-count "
           "estimate, which reads low on the C6 and can trip the emergency close on a healthy "
           "rail. Verify the threshold against a meter before trusting it.",
           esp_err_to_name(err));
#else
  (void) channel;
  ESP_LOGW(TAG, "vcc_monitor: this target has no curve-fitting ADC calibration — falling back to the "
                "raw-count estimate. Verify the threshold against a meter before trusting it.");
#endif
}

float SdLogger::vcc_rail_volts_(int raw) const {
  // The calibrated path is the correct one. Uncalibrated, `raw / 4095 * full_scale` reads ~26 %
  // low on the C6 — measured 8.94 V for a rail an esphome `adc:` on the same pin put at 12.07 V
  // — which is below any threshold worth setting, so the monitor fires at boot and latches
  // logging off for the whole run (docs/HANDOVER.md).
  if (this->adc_cali_ != nullptr) {
    int mv = 0;
    if (adc_cali_raw_to_voltage(this->adc_cali_, raw, &mv) == ESP_OK)
      return (static_cast<float>(mv) / 1000.0f) * this->vcc_divider_;
  }
  const float v_adc = (static_cast<float>(raw) / 4095.0f) * (this->vcc_full_scale_mv_ / 1000.0f);
  return v_adc * this->vcc_divider_;
}

void SdLogger::unmount_card_() {
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // The collection server made this teardown someone else's business. `esp_vfs_fat_sdcard_unmount()`
  // frees the VFS context that owns every open `FIL`, so unmounting while the httpd task holds a
  // read fd is a use-after-free on its next block — and the case that gets there is precisely the
  // interesting one, a card that failed mid-transfer.
  //
  // **The barrier is the flag, not the timeout.** Closing `serving_open_` under `index_mux_` — in
  // the same critical section `collection_begin_serve()` grants in, and *before* the first read of
  // `transfers_in_flight_` — is what turns "no transfer was in flight when I looked" into "no
  // transfer can start". Waiting alone proves only the former: the drain loop can observe zero, the
  // httpd task can then be granted a transfer and `::open()` the chunk, and the unmount below frees
  // the VFS context that owns its `FIL`. That is a use-after-free on the handler's next `::read()`
  // and a second one when `~FdGuard` closes it, and no timeout however long can prevent it.
  //
  // The index goes with it, in the same critical section: after this the entries describe a card
  // that is not mounted, so a `GET /sdlog/index` racing the teardown would list chunks whose every
  // fetch is a 500 until the next recovery pass rebuilds them. `clear()` deliberately leaves the
  // `#gap` window alone — that loss still owes a line in the first file that opens afterwards.
  {
    IndexLock lock(this->index_mux_);
    this->serving_open_ = false;
    this->collection_.clear();
  }
  // Bounded, and it proceeds anyway when the bound expires: the handler is either inside a card
  // read that is about to fail (fast) or inside a socket send (bounded by the server's
  // send_wait_timeout, which is what UNMOUNT_DRAIN_MS is derived from), and a recovery that waited
  // indefinitely for a stalled socket would stop logging to protect a transfer. Logging wins — that
  // is the same rule §6 states about the SPI bus. What the bound risks is now only the *one*
  // transfer that was already inside a read when the gate closed, never a fresh one.
  for (uint16_t waited = 0; this->transfers_in_flight_.load(std::memory_order_acquire) != 0; waited++) {
    if (waited >= UNMOUNT_DRAIN_MS / 10) {
      ESP_LOGW(TAG, "unmount: a chunk transfer is still in flight after %u ms — tearing down anyway",
               static_cast<unsigned>(UNMOUNT_DRAIN_MS));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
#endif
  // The full teardown, in the order a recovery needs it: file, then filesystem, then bus. Leaving
  // the SPI bus initialised would make the next spi_bus_initialize() fail with INVALID_STATE and
  // every retry after the first would report a bus fault rather than a card one.
  if (this->fd_ >= 0) {
    close(this->fd_);
    this->fd_ = -1;
  }
  this->drop_unflushed_gap_();
  this->block_.reset();
  if (this->card_ != nullptr) {
    esp_vfs_fat_sdcard_unmount(this->mount_point_.c_str(), this->card_);
    this->card_ = nullptr;
  }
  if (this->spi_bus_ok_) {
    spi_bus_free(static_cast<spi_host_device_t>(SDSPI_DEFAULT_HOST));
    this->spi_bus_ok_ = false;
  }
}

bool SdLogger::mount_card_(bool allow_format) {
  spi_host_device_t host_slot = static_cast<spi_host_device_t>(SDSPI_DEFAULT_HOST);  // SPI2_HOST
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = static_cast<int>(this->clock_hz_ / 1000);

  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = this->mosi_pin_;
  bus_cfg.miso_io_num = this->miso_pin_;
  bus_cfg.sclk_io_num = this->clk_pin_;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 4096;

  esp_err_t err = spi_bus_initialize(host_slot, &bus_cfg, SPI_DMA_CH_AUTO);
  // INVALID_STATE means the bus is already up — a retry that got here without a clean teardown,
  // or another component sharing SPI2. Neither is a reason to refuse the mount.
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return false;
  }
  this->spi_bus_ok_ = (err == ESP_OK);

  // H2: the C6 needs a short settle between bus init and mount to init the card
  // reliably on every boot.
  vTaskDelay(pdMS_TO_TICKS(20));

  sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev_cfg.gpio_cs = static_cast<gpio_num_t>(this->cs_pin_);
  dev_cfg.host_id = host_slot;
  // How long the driver waits for MISO to go high before it sends a command. The default is 0,
  // which means 40 ms — and 40 ms is the entire patience the stack has ever shown a card that is
  // still finishing an interrupted write. The API caps this at 127 ms; anything longer is the
  // in-band reset's job, which is why it does the waiting itself and this only widens the door.
  dev_cfg.wait_for_miso = 127;

  esp_vfs_fat_mount_config_t mount_cfg = {};
  // Never the member directly: a recovery attempt passes false regardless, because a retry ladder
  // that reformats would erase the logs it is being run to save, once per attempt (V21).
  mount_cfg.format_if_mount_failed = allow_format;
  mount_cfg.max_files = 5;
  // Larger allocation unit => fewer mid-stream FAT cluster allocations, which
  // are a latency-spike source (spec D6; no f_expand over the stdio path).
  mount_cfg.allocation_unit_size = 16 * 1024;

  err = esp_vfs_fat_sdspi_mount(this->mount_point_.c_str(), &host, &dev_cfg, &mount_cfg, &this->card_);
  if (err != ESP_OK) {
    // Which layer failed decides whether retrying can possibly help, and the two are one esp_err
    // apart. The fatfs FRESULT that would say more is logged by IDF at W level, which esphome
    // compiles out (CONFIG_LOG_MAXIMUM_LEVEL), so without this hint a filesystem fault and a card
    // that never answered are the same bare line.
    if (err == ESP_ERR_TIMEOUT) {
      ESP_LOGE(TAG,
               "mount failed: %s — the card never answered identification (wedged mid-write, absent, or unpowered)",
               esp_err_to_name(err));
    } else if (err == ESP_FAIL) {
      ESP_LOGE(TAG, "mount failed: %s — the card answered but its filesystem did not mount; retrying cannot fix a FAT",
               esp_err_to_name(err));
    } else {
      ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
    }
    this->card_ = nullptr;
    if (this->spi_bus_ok_) {
      spi_bus_free(host_slot);
      this->spi_bus_ok_ = false;
    }
    return false;
  }
#ifdef USE_SD_LOGGER_COLLECTION
  // Read the fill once, here, while nothing is being logged yet: the *first* f_getfree walks the
  // whole FAT — seconds on a 32 GB card — and is maintained incrementally afterwards. Paying that
  // on the writer task mid-run would stall the drain long enough to overflow the ring, which is
  // the one thing retention must never cost (design §6). Both callers of mount_card_() reach it
  // with producers still gated on `mounted_`.
  if (this->collection_.enabled()) {
    uint8_t fill = 0;
    if (this->card_fill_percent_(&fill))
      ESP_LOGI(TAG, "card %u%% full at mount (retention arms at %u%%)", fill, this->collection_.retention_percent());
  }
#endif
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // The gate `unmount_card_()` closed. Reopened only on the success path and only once the VFS
  // context an `::open()` from the httpd task would land in actually exists. The index is empty at
  // this point — the unmount cleared it and the boot/recovery scan has not run yet — so nothing is
  // servable until the scan says so; what this restores is the *permission*, not the content.
  {
    IndexLock lock(this->index_mux_);
    this->serving_open_ = true;
  }
#endif
  return true;
}

bool SdLogger::card_fill_percent_(uint8_t *fill_out) const {
  uint64_t total = 0;
  uint64_t free_bytes = 0;
  if (esp_vfs_fat_info(this->mount_point_.c_str(), &total, &free_bytes) != ESP_OK || total == 0)
    return false;
  const uint64_t used = total - (free_bytes < total ? free_bytes : total);
  if (fill_out != nullptr)
    *fill_out = static_cast<uint8_t>((used * 100) / total);
  return true;
}

uint32_t SdLogger::scan_next_seq_() {
  // Files are 8.3 names "L#######.LOG" (format v1) or "L#######.CSV" (M1-era);
  // pick max seq + 1 so we never overwrite a previous boot's logs (no RTC/NVS
  // needed). Both extensions match — the sequence has to stay monotonic across
  // the format change, on a card that may well hold files from both eras. The
  // matcher itself is parse_log_seq() in log_format.h, host-tested there.
  //
  // The same walk also rebuilds the chunk index (design §3): every `L#######.LOG` on the card is a
  // SEALED chunk and every `L#######.UPL` a CONFIRMED one. One walk, two jobs — a second opendir()
  // pass would double the cost of the boot scan and, on the recovery path, run it a second time
  // against the card that just failed.
  uint32_t next = 0;
  // The index describes one card; a recovery may well have remounted a different one. Locked
  // because a recovery runs this with the collection server already live (§8a) — and taken and
  // given back around each individual call rather than held across the walk, because readdir() and
  // stat() below are card I/O and this lock must never be held across any.
  {
    IndexLock lock(this->index_mux_);
    this->collection_.clear();
  }
  // Trailing slash so that what reaches FatFs, once the VFS has stripped the
  // mount prefix, is "/" rather than the empty string.
  //
  // This needs CONFIG_VFS_SUPPORT_DIR, which ESPHome disables by default —
  // sd_logger's _final_validate() calls esp32.require_vfs_dir() to force it back
  // on. Without that, opendir() is compiled out and returns nullptr *and leaves
  // errno untouched*, so the failure is invisible: the scan quietly returns 0,
  // every boot reopens L0000000.CSV, and because open_next_file_() seeds
  // file_bytes_ from a real ftell(), each already-full file trips the rotate
  // check immediately. The writer then walks the entire existing file set one
  // open at a time (~14/s) and every record produced meanwhile is dropped —
  // 201 693 of them across a 397-file walk on the bench, growing every boot.
  // Hence the warning below: this must never fail silently again.
  const std::string root = this->mount_point_ + "/";
  DIR *dir = opendir(root.c_str());
  if (dir == nullptr) {
    ESP_LOGW(TAG,
             "scan: opendir(%s) failed (errno %d) - starting at seq 0; expect a slow "
             "rotate-walk and heavy record loss if the card already holds logs",
             root.c_str(), errno);
    return 0;
  }
  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    uint32_t seq = 0;
    if (parse_log_seq(ent->d_name, &seq) && seq + 1 > next)
      next = seq + 1;

    // parse_chunk_name() is the *chunk* matcher and is deliberately not the same one: it takes
    // `.UPL`, which the sequence scan above cannot see, and refuses M1-era `.CSV`, which has no
    // header and no seq to dedup on — not a chunk, so never listed and never a retention victim.
    uint32_t chunk_seq = 0;
    ChunkState state = ChunkState::NONE;
    if (!parse_chunk_name(ent->d_name, &chunk_seq, &state))
      continue;
    // A confirmed chunk still owns its sequence number. Without this, a card carrying
    // `L0000433.UPL` would hand 433 straight back to the next file, and `(device, seq)` — the key
    // the puller dedups on — would name two different files.
    if (chunk_seq + 1 > next)
      next = chunk_seq + 1;
    // One slot is held back for the file this boot is about to open: the OPEN chunk is the only
    // entry the index cannot recover later, and a soak card holds SD_LOG_MAX_CHUNKS chunks after
    // about an hour. Without the reservation every boot after that would start with an untracked
    // open file, which is then never sealed and never reclaimed. The index refuses rather than
    // evicts by design — see the capacity note at the top of collection_policy.h.
    uint16_t tracked;
    {
      IndexLock lock(this->index_mux_);
      tracked = this->collection_.count();
    }
    if (tracked + 1 >= SD_LOG_MAX_CHUNKS) {
      // Counted, not just skipped: this is a file on the card that nothing will ever list, serve or
      // reclaim, and the only other trace of it is a scan line saying how many chunks were indexed —
      // which looks the same whether the card held that many or that was as far as the index got.
      this->index_refused_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // One stat() per tracked chunk, bounded by the index capacity, and read-only: the size is what
    // lets a later `#gap` say how much the discard cost rather than reporting zero bytes.
    struct stat st;
    const std::string path = root + ent->d_name;
    const uint64_t bytes = (::stat(path.c_str(), &st) == 0 && st.st_size > 0) ? static_cast<uint64_t>(st.st_size) : 0;
    {
      IndexLock lock(this->index_mux_);
      this->collection_.add(chunk_seq, bytes, state);
    }
  }
  closedir(dir);
  uint16_t indexed;
  {
    IndexLock lock(this->index_mux_);
    indexed = this->collection_.count();
  }
  ESP_LOGI(TAG, "scan: next seq=%" PRIu32 ", %u chunk(s) indexed", next, indexed);
  return next;
}

bool SdLogger::open_next_file_() {
  // First, while `file_bytes_` and `block_` still describe the file being left behind: settle any
  // `#gap` line committed into it. Every path that gets here has already done so (close_file_() on
  // the way out, enter_failed_() on the way down), but the answer depends on state this function is
  // about to overwrite, and a stale `gap_line_end_` carried into a fresh file — whose offsets start
  // again from ~0 — would be satisfied immediately and spend a window whose line was never written.
  this->drop_unflushed_gap_();
  char name[SD_LOG_NAME_LEN];
  format_log_name(name, this->seq_);
  const std::string path = this->mount_point_ + "/" + name;
  // O_APPEND, not truncate: the scan says this sequence is free, but if it is
  // ever wrong the right failure is a file that grows, not one that is erased.
  this->fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (this->fd_ < 0) {
    ESP_LOGE(TAG, "open %s failed (errno %d)", path.c_str(), errno);
    return false;
  }
  // Seed the rotation counter from the real size, once: the file is opened for
  // append, so a resumed sequence may already hold data. This is also what the
  // header pad is measured against, so an appended file stays sector-aligned.
  const off_t pos = lseek(this->fd_, 0, SEEK_END);
  this->file_bytes_ = pos > 0 ? static_cast<uint32_t>(pos) : 0;
  // Settled at the top of this function, deliberately not here: by now `file_bytes_` names the new
  // file and the question "did that `#gap` line reach the card" has no answer any more.
  this->block_.reset();
  this->write_file_header_();
  ESP_LOGI(TAG, "logging to %s", path.c_str());
  // Not a bare `true`: writing the header goes through the same flush path as everything else, so
  // a card that refuses its very first write has already taken the fd away underneath us. The
  // honest answer to "did a file open" is whether one is there now.
  if (!this->file_ok_())
    return false;
  // The file the writer holds the fd to is the OPEN chunk: never listed, never served, never a
  // retention victim (design §3). It becomes SEALED in close_file_(), which is the only place that
  // transition can honestly be made, and the clock starts here for the time bound.
  bool tracked;
  uint16_t held;
  {
    IndexLock lock(this->index_mux_);
    tracked = this->collection_.add(this->seq_, this->file_bytes_, ChunkState::OPEN);
    held = this->collection_.count();
  }
  if (!tracked) {
    // The counter is the part a bench run can actually see: this warning is printed once per
    // rotation and a run that scrolls looks identical to a healthy one, which is exactly how a
    // 12-minute soak came back with 29 rotations and an index that was full at the first of them.
    // `index_refused=` in the periodic stats line is the same fact, standing still where it can be
    // read. Raise `collection: max_chunks:` when it moves.
    this->index_refused_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(TAG,
             "chunk index full (%u of %u entries): L%07" PRIu32 " is untracked, so retention cannot reclaim it "
             "(raise collection.max_chunks; %" PRIu32 " refused so far)",
             held, static_cast<unsigned>(SD_LOG_MAX_CHUNKS), this->seq_,
             this->index_refused_.load(std::memory_order_relaxed));
  }
  this->file_opened_us_ = static_cast<uint64_t>(esp_timer_get_time());
  return true;
}

void SdLogger::write_file_header_() {
  // The header block, then a `#pad` line sized so the stream sits on a 512-byte
  // boundary from the first record onwards (spec D6/F1d). Every later flush is a
  // whole number of sectors, so this one line is what keeps FATFS off the
  // read-modify-write path for the entire file.
  //
  // The `#src` table is why a card found on a bench explains itself: the bare
  // numeric source column it replaces was meaningful only next to the YAML that
  // produced the card.
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

  // A fresh delta chain per file, which is what makes every sealed chunk decodable on its own
  // (F1h). The collection server hands out chunks individually, so a chain carried across a
  // rotation would make each file depend on every file before it — including the ones retention
  // has already deleted. This is also the reset that covers `enter_failed_()`: it drops the
  // unflushed block and closes the file, and the next file to open comes through here.
  this->stamps_.reset();

  if (this->ensure_room_(SD_LOG_MAX_LINE))
    this->commit_line_(format_header(this->block_.cursor(), this->block_.room(), this->seq_, now, ESPHOME_VERSION));

#ifdef USE_SD_LOGGER_CAN_TAP
  for (uint8_t i = 0; i < this->can_tap_count_; i++) {
    if (!this->ensure_room_(SD_LOG_MAX_LINE))
      break;
    this->commit_line_(format_src_num(this->block_.cursor(), this->block_.room(), KIND_CAN, this->can_taps_[i].label,
                                      this->can_taps_[i].source, "can_gateway", this->can_taps_[i].port->bit_rate()));
  }
#endif

  // Sources declared in YAML but not backed by a native tap: fed by the
  // sd_logger.log action today, by the linbus tap (S3) later.
  for (uint8_t i = 0; i < this->sources_.size(); i++) {
    const SourceEntry &entry = this->sources_.at(i);
#ifdef USE_SD_LOGGER_CAN_TAP
    bool from_tap = false;
    for (uint8_t t = 0; t < this->can_tap_count_; t++)
      from_tap = from_tap || this->can_taps_[t].source == entry.tag;
    if (from_tap)
      continue;  // already emitted above, with its real bit rate
#endif
    if (!this->ensure_room_(SD_LOG_MAX_LINE))
      break;
    this->commit_line_(
        format_src(this->block_.cursor(), this->block_.room(), entry.kind, entry.label, entry.tag, "action", nullptr));
  }

  if (this->ensure_room_(SD_LOG_MAX_LINE))
    this->commit_line_(format_types(this->block_.cursor(), this->block_.room()));
  if (this->ensure_room_(SD_LOG_MAX_LINE))
    this->commit_line_(format_flags_legend(this->block_.cursor(), this->block_.room()));
  // The column list and the hex/step rules. It comes out of the `#pad` line's budget rather than
  // out of the card, so a v2 file explains its own compact shape for free.
  if (this->ensure_room_(SD_LOG_MAX_LINE))
    this->commit_line_(format_layout(this->block_.cursor(), this->block_.room()));

  // The pad is measured against the file offset the line will land at, which is
  // what was already on disk plus what is buffered ahead of it.
  if (this->ensure_room_(2 * SD_LOG_SECTOR))
    this->commit_line_(format_pad(this->block_.cursor(), this->block_.room(), this->file_bytes_));

  // Reset the drop baselines: markers are relative to the file they appear in,
  // so a counter that moved before this file opened is not re-reported into it.
  //
  // `marked_card_dropped_` is deliberately NOT reset, and that is the whole point of it. It only
  // ever moves while there is no file to write into, so whatever it has accumulated by the time a
  // file opens is exactly the cost of the outage that preceded it — and there was, by definition,
  // no file to report that in. Carrying the baseline across makes the first marker pass in the
  // new file state the gap. On a rotation it has not moved, so nothing is emitted.
  //
  // The `#gap` window gets exactly that treatment, and gets it by not appearing here at all: it
  // lives in CollectionPolicy, nothing in this function touches it, and clear() does not reset it
  // either. A retention discard between the marker pass and a rotation therefore lands in the next
  // file rather than being erased by the header that opened it.
  this->marked_dropped_ = this->dropped_records_.load(std::memory_order_relaxed);
  this->marked_text_dropped_ = this->text_dropped_.load(std::memory_order_relaxed);
#ifdef USE_SD_LOGGER_CAN_TAP
  for (uint8_t i = 0; i < this->can_tap_count_; i++)
    this->can_taps_[i].marked_dropped = this->can_taps_[i].port->log_tap_dropped();
#endif
}

bool SdLogger::close_file_(const char *reason) {
  if (this->fd_ < 0)
    return false;
  // `#close` is the last line of every file that ends in an orderly way, which
  // is what makes its *absence* mean something: a file with no `#close` was cut
  // by a power loss (spec F1d). One line, and the bench criterion "parses as
  // valid CSV up to the last line" becomes something a script can decide.
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  if (this->ensure_room_(SD_LOG_MAX_LINE))
    this->commit_line_(format_close(this->block_.cursor(), this->block_.room(), now, reason));
  // The one place a partial sector may be written: there is nothing left to
  // align for, and losing the tail to keep the invariant would be absurd.
  if (!this->flush_block_(/*all=*/true))
    return false;  // the flush already took the file down and armed recovery
  // Everything committed to this file is on the card now, a `#gap` line among it: the window that
  // line states is spent, and the writer stops owing it. This is also what guarantees the successor
  // file opens with no in-flight window and no stale end offset.
  this->retire_gap_if_durable_();
  fsync(this->fd_);
  close(this->fd_);
  this->fd_ = -1;
  // OPEN -> SEALED: `#close` is written and fsynced, so the chunk is complete and the collector may
  // read it (design §3). Only here — a file sealed before its last sector reached the card would be
  // served with a `Content-Length` it cannot honour. `false` means the chunk was never tracked (a
  // full index), which open_next_file_() already said out loud.
  //
  // This is the exact moment the chunk becomes visible to `GET /sdlog/index` and fetchable by
  // `GET /sdlog/f/<name>`, so it is also the moment the httpd task can start reading the entry —
  // hence the lock (§8a).
  {
    IndexLock lock(this->index_mux_);
    this->collection_.seal(this->seq_, this->file_bytes_);
  }
  return true;
}

void SdLogger::enter_failed_(const char *why) {
  // The single transition into "no writable file". Everything that can discover a dead card comes
  // through here, which is what keeps `mounted_` and `fd_` from disagreeing — the state where
  // producers keep pushing into a file that is not there is exactly how a failed rotation used to
  // discard every subsequent record without counting one of them.
  const bool was_running = this->mounted_;
  this->mounted_ = false;
  if (this->fd_ >= 0) {
    close(this->fd_);
    this->fd_ = -1;
  }
  // Before the reset, never after: the block holds committed lines that never reached the card, and
  // a `#gap` line among them is the only in-band record that retention deleted anything. This is the
  // tightest case there is — the marker pass commits `#gap` and the very next line's `ensure_room_()`
  // discovers the dead card.
  this->drop_unflushed_gap_();
  this->block_.reset();
  // The index describes a card that is no longer reachable, and its OPEN entry names a file whose
  // fd has just gone. Dropping it here is what keeps the *next* open file trackable — there is only
  // ever one OPEN entry, so a stale one would make add() refuse the successor — and it is what a
  // remount does anyway, since scan_next_seq_() rebuilds the index from the card. clear()
  // deliberately leaves the discard window alone: a `#gap` that has not been written yet still has
  // to reach the first file that opens after the outage, exactly like `card_dropped`'s baseline.
  //
  // A transfer may be in flight against one of the entries being dropped. That is safe and needs no
  // coordination: the handler's `clear_serving()` tolerates a seq that is no longer tracked, and its
  // next read off the dead card fails, which closes the connection and lets the client retry.
  {
    IndexLock lock(this->index_mux_);
    this->collection_.clear();
  }
  if (!was_running)
    return;  // already failed; a ladder is running and must not be restarted

  if (this->recovery_.enabled()) {
    ESP_LOGE(TAG, "log file lost (%s) — retrying in %" PRIu32 " ms", why, this->recovery_.delay_ms());
    this->recovery_.arm(millis());
  } else {
    ESP_LOGE(TAG, "log file lost (%s) — recovery is off, logging stops for this run", why);
  }
}

namespace {

/// `CardResetIo` over a raw SPI device on the logger's own four pins.
///
/// CS is a plain GPIO here and not the driver's `spics_io_num`, because the sequence has to hold
/// the card selected across many separate transactions — the busy poll is one byte at a time — and
/// deselected across others, and a hardware CS that toggles per transaction can express neither.
class SpiCardResetIo : public CardResetIo {
 public:
  SpiCardResetIo(spi_device_handle_t dev, gpio_num_t cs_pin, const std::atomic<bool> &dying)
      : dev_(dev), cs_pin_(cs_pin), dying_(dying) {}

  void cs(bool assert) override { gpio_set_level(this->cs_pin_, assert ? 0 : 1); }

  uint8_t xfer(uint8_t out) override {
    // Yield on a fixed byte count, not on elapsed time: this runs in the writer task at priority 6,
    // so a multi-second busy poll that never blocks starves the idle task and the watchdog reboots
    // the board — turning a recoverable card into a reset, which is how it got wedged in the first
    // place.
    if (++this->bytes_ % CARD_RESET_YIELD_BYTES == 0)
      vTaskDelay(1);
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.tx_data[0] = out;
    if (spi_device_polling_transmit(this->dev_, &t) != ESP_OK)
      return 0xFF;  // reads as "nothing drove MISO", which is exactly what a dead bus is
    return t.rx_data[0];
  }

  void delay_ms(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
  uint32_t now_ms() override { return millis(); }
  bool aborted() override { return this->dying_.load(std::memory_order_acquire); }

 private:
  spi_device_handle_t dev_;
  gpio_num_t cs_pin_;
  const std::atomic<bool> &dying_;
  uint32_t bytes_{0};
};

}  // namespace

CardResetResult SdLogger::run_card_reset_(uint32_t busy_timeout_ms) {
  CardResetResult result;
  const spi_host_device_t host_slot = static_cast<spi_host_device_t>(SDSPI_DEFAULT_HOST);

  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = this->mosi_pin_;
  bus_cfg.miso_io_num = this->miso_pin_;
  bus_cfg.sclk_io_num = this->clk_pin_;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 64;

  esp_err_t err = spi_bus_initialize(host_slot, &bus_cfg, SPI_DMA_CH_AUTO);
  // INVALID_STATE is another owner's bus, which we borrow and must not free.
  const bool owns_bus = (err == ESP_OK);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "card reset: spi_bus_initialize failed: %s", esp_err_to_name(err));
    return result;
  }

  spi_device_interface_config_t dev_cfg = {};
  dev_cfg.clock_speed_hz = CARD_RESET_CLOCK_HZ;
  dev_cfg.mode = 0;
  dev_cfg.spics_io_num = -1;
  dev_cfg.queue_size = 1;
  spi_device_handle_t dev = nullptr;
  err = spi_bus_add_device(host_slot, &dev_cfg, &dev);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "card reset: spi_bus_add_device failed: %s", esp_err_to_name(err));
    if (owns_bus)
      spi_bus_free(host_slot);
    return result;
  }

  // Level before direction, not the other way round: writing the output latch while the pin is
  // still an input is harmless, whereas enabling the driver first would put whatever the latch
  // happened to hold onto CS — a stray assertion into a card that is mid-transaction, which is the
  // one thing this function exists not to do.
  const gpio_num_t cs = static_cast<gpio_num_t>(this->cs_pin_);
  gpio_set_level(cs, 1);
  gpio_set_direction(cs, GPIO_MODE_OUTPUT);

  CardResetConfig cfg;
  cfg.busy_timeout_ms = busy_timeout_ms;
  SpiCardResetIo io(dev, cs, this->dying_);
  result = card_reset(io, cfg);

  spi_bus_remove_device(dev);
  if (owns_bus)
    spi_bus_free(host_slot);
  // Hand CS back: sdspi_host_init_device() configures the pin itself on the mount that follows, and
  // a pin left as a driven output would fight it.
  gpio_reset_pin(cs);
  return result;
}

bool SdLogger::try_recover_() {
  // One bounded attempt. The teardown is unconditional, and what follows it depends on how many
  // attempts have already failed: the first is a plain remount, which is all a transient write
  // error needs; from the second on the card is talked out of whatever an aborted transaction left
  // it in, because that state — not a dirty filesystem — is what a reset mid-write produces.
  this->unmount_card_();
  if (this->recovery_power_cycle_ && this->card_power_pin_ >= 0) {
    gpio_set_level(static_cast<gpio_num_t>(this->card_power_pin_), 0);
    vTaskDelay(pdMS_TO_TICKS(CARD_POWER_OFF_MS));
    gpio_set_level(static_cast<gpio_num_t>(this->card_power_pin_), 1);
    vTaskDelay(pdMS_TO_TICKS(CARD_POWER_SETTLE_MS));
  }
  if (this->recovery_.reset_due()) {
    const CardResetResult reset = this->run_card_reset_(this->recovery_.busy_timeout_ms());
    // `NONE` means the sequence never reached the card — the bus itself refused, and
    // run_card_reset_() has already said which call failed. A verdict line here would attribute
    // that to the card, which was never asked anything.
    if (reset.reached != CardResetStep::NONE) {
      // The one line that says what the card actually did. `busy` is a measurement nothing else in
      // this project has ever produced: a card that answers after 900 ms was never wedged, it was
      // waited on for 40 ms and declared dead.
      char status[24] = "";
      if (reset.status_valid)
        snprintf(status, sizeof(status), " status=0x%02X", reset.status);
      ESP_LOGW(TAG, "card reset: %s at %s (busy %" PRIu32 " ms, then %" PRIu32 " ms; cmd0 x%u r1=0x%02X%s)",
               card_reset_outcome_str(reset.outcome), card_reset_step_str(reset.reached), reset.busy_ms,
               reset.busy_after_stop_ms, reset.cmd0_tries, reset.r1, status);
    }
    // Only an abort skips the mount: "mute" is this sequence's reading of the bus, and the mount is
    // the better instrument. Shutting down is different — there is nothing left to mount for.
    if (reset.outcome == CardResetOutcome::ABORTED)
      return false;
  }
  if (!this->mount_card_(/*allow_format=*/false))
    return false;
  // Rescan rather than reusing seq_: the card may be a different one, and appending to a
  // sequence that belonged to the old card would interleave two runs in one file.
  this->seq_ = this->scan_next_seq_();
  if (!this->open_next_file_()) {
    this->unmount_card_();
    return false;
  }
  this->last_sync_ms_ = millis();
  this->mounted_ = true;
  return true;
}

void SdLogger::maybe_rotate_(uint64_t now64) {
  // Not while dying_ — an emergency close must not open a fresh file on a collapsing rail.
  if (this->fd_ < 0 || this->dying_.load(std::memory_order_relaxed))
    return;
  // Both bounds, whichever fires first, decided by collection_policy.h (design §4). `now64` is the
  // writer pass's single clock read rather than a fresh esp_timer_get_time(): this runs once per
  // record at up to ~9 300 rec/s, and the deadline is a whole number of seconds — which is exactly
  // why V23 refuses sub-second values instead of rounding them away in silence.
  const uint64_t elapsed = now64 > this->file_opened_us_ ? now64 - this->file_opened_us_ : 0;
  if (this->collection_.should_rotate(this->file_bytes_, elapsed))
    this->rotate_file_();
}

void SdLogger::rotate_file_() {
  // `#rotate` names the successor, so a reader walking a card knows the stream
  // continues and where — then `#close` ends this file (spec F1d).
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  if (this->ensure_room_(SD_LOG_MAX_LINE))
    this->commit_line_(format_rotate(this->block_.cursor(), this->block_.room(), now, this->seq_ + 1));
  if (!this->close_file_("rotate")) {
    // The final flush failed; enter_failed_() has already taken the file down and armed the
    // ladder. Opening a successor on a card that just refused a write would only make a second
    // broken file, and `#rotate` above already names a file that will not exist — which
    // `sdlog.py check` reports rather than silently believing.
    return;
  }
  this->seq_++;
  if (!this->open_next_file_()) {
    // The failure that used to be invisible: the old file is closed, the new one never opened,
    // `mounted_` stayed true, and from here on every record was dropped into a -1 fd without
    // touching a single counter.
    ESP_LOGE(TAG, "rotate: could not open L%07" PRIu32 ".LOG", this->seq_);
    this->enter_failed_("rotate");
  }
}

bool SdLogger::push_record(const LogRecord &rec) {
  // No writable file: count here rather than filling the ring, so the loss is attributed to the
  // card instead of to buffer_depth. The three drop counters only earn their keep if each one
  // names the fault it actually is.
  if (!this->mounted_) {
    this->card_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  taskENTER_CRITICAL(&this->ring_mux_);
  uint32_t next = (this->head_ + 1) & this->ring_mask_;
  if (next == this->tail_) {
    taskEXIT_CRITICAL(&this->ring_mux_);
    this->dropped_records_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  this->ring_[this->head_] = rec;
  this->head_ = next;
  taskEXIT_CRITICAL(&this->ring_mux_);
  return true;
}

bool SdLogger::pop_record_(LogRecord &out) {
  taskENTER_CRITICAL(&this->ring_mux_);
  if (this->tail_ == this->head_) {
    taskEXIT_CRITICAL(&this->ring_mux_);
    return false;
  }
  out = this->ring_[this->tail_];
  this->tail_ = (this->tail_ + 1) & this->ring_mask_;
  taskEXIT_CRITICAL(&this->ring_mux_);
  return true;
}

void SdLogger::on_log_message(uint8_t level, const char *tag, const char *message, size_t message_len) {
  // Source S4. Runs on the main loop task only — messages from other tasks are
  // routed through the logger's TaskLogBuffer and replayed there — so this is
  // one producer against the writer's one consumer, guarded like the record
  // ring. With `task_log_buffer_size: 0` those other tasks bypass listeners
  // entirely, which would silently lose exactly the writer and VCC-monitor
  // tasks' own messages; V17 warns about it at config time.
  //
  // Two hard rules from spec §4a live in here: `message` points into the
  // logger's shared tx_buffer_, reused by the very next line, so the copy is
  // synchronous and the pointer is never stored; and **this path must never
  // log** — recursion is guarded by dropping, so an ESP_LOGW from here would
  // simply vanish. Failures get a counter instead.
  if (this->text_ring_ == nullptr || level > this->log_level_)
    return;
  // Same rule as push_record(): with no writable file this is a card loss, not a ring that is too
  // small, and text_dropped_ must keep meaning "the text ring depth is too low".
  if (!this->mounted_) {
    this->card_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  taskENTER_CRITICAL(&this->text_mux_);
  const uint16_t next = (this->text_head_ + 1) & this->text_mask_;
  if (next == this->text_tail_) {
    taskEXIT_CRITICAL(&this->text_mux_);
    this->text_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  TextRecord &slot = this->text_ring_[this->text_head_];
  slot.t_us = static_cast<uint32_t>(esp_timer_get_time());
  slot.level = level;

  size_t tag_len = 0;
  if (tag != nullptr) {
    while (tag[tag_len] != '\0' && tag_len < 24)
      tag_len++;
  }
  if (tag_len > SD_LOGGER_TEXT_BUF)
    tag_len = SD_LOGGER_TEXT_BUF;
  std::memcpy(slot.buf, tag, tag_len);
  slot.tag_len = static_cast<uint8_t>(tag_len);

  bool truncated = false;
  const size_t len =
      copy_log_payload(slot.buf + tag_len, SD_LOGGER_TEXT_BUF - tag_len, message, message_len, &truncated);
  slot.len = static_cast<uint8_t>(len);
  slot.truncated = truncated ? 1 : 0;

  this->text_head_ = next;
  taskEXIT_CRITICAL(&this->text_mux_);
}

bool SdLogger::pop_text_(TextRecord &out) {
  if (this->text_ring_ == nullptr)
    return false;
  taskENTER_CRITICAL(&this->text_mux_);
  if (this->text_tail_ == this->text_head_) {
    taskEXIT_CRITICAL(&this->text_mux_);
    return false;
  }
  out = this->text_ring_[this->text_tail_];
  this->text_tail_ = (this->text_tail_ + 1) & this->text_mask_;
  taskEXIT_CRITICAL(&this->text_mux_);
  return true;
}

void SdLogger::log_frame(uint8_t source, uint32_t id, uint8_t flags, const uint8_t *data, uint8_t len) {
  // No `mounted_` check of its own — push_record() owns that decision, so the "no card" drop is
  // counted exactly once and in one place. It used to return here silently, which made an outage
  // and a quiet bus produce identical statistics.
  LogRecord rec;
  rec.t_us = static_cast<uint32_t>(esp_timer_get_time());
  rec.id = id;
  rec.source = source;
  rec.flags = flags;
  rec.len = len > 8 ? 8 : len;
  std::memset(rec.data, 0, sizeof(rec.data));
  if (data != nullptr && rec.len > 0)
    std::memcpy(rec.data, data, rec.len);
  this->push_record(rec);
}

bool SdLogger::flush_block_(bool all) {
  if (this->fd_ < 0)
    return false;
  size_t len = all ? this->block_.drain_all() : this->block_.flush_len();
  if (len == 0)
    return true;
  // Loop on a short write rather than carrying the remainder into the next
  // flush: a partial write would leave the file offset off a sector boundary,
  // and every later whole-sector write would then straddle two sectors — the
  // read-modify-write the block buffer exists to avoid, silently, for the rest
  // of the file.
  while (len > 0) {
    // Timed because this is where a card's internal stall (write-cache flush / GC) surfaces:
    // sdspi busy-polls it at full CPU with the FATFS volume mutex held, for up to its 5 s
    // timeout, and nothing above the driver sees anything but a write() that took that long.
    const int64_t t0 = esp_timer_get_time();
    const ssize_t written = write(this->fd_, this->block_.data(), len);
    const int64_t held_us = esp_timer_get_time() - t0;
    if (held_us > STALL_WARN_US) {
      ESP_LOGW(TAG, "write() held %" PRId64 " ms (%u B) — card internally busy", held_us / 1000,
               static_cast<unsigned>(len));
    }
    if (written <= 0) {
      // A failing card must not take the buses down: drop the file, keep the component alive, and
      // let producers keep counting drops. enter_failed_() decides whether the card gets retried.
      ESP_LOGE(TAG, "write failed (%d, errno %d)", static_cast<int>(written), errno);
      this->enter_failed_("write");
      return false;
    }
    this->block_.consume(static_cast<size_t>(written));
    this->bytes_written_ += static_cast<uint32_t>(written);
    len -= static_cast<size_t>(written);
  }
  return true;
}

bool SdLogger::ensure_room_(size_t need) {
  if (this->fd_ < 0)
    return false;
  if (this->block_.room() >= need)
    return true;
  if (!this->flush_block_(/*all=*/false))
    return false;
  // A sub-sector remainder can still leave less room than a worst-case line
  // needs. Writing it out would break the alignment invariant, so grow nothing
  // and let the caller skip: BLOCK_BUF_SIZE is 8 sectors against a 512 B line
  // cap, so this is unreachable rather than a silent cap.
  return this->block_.room() >= need;
}

void SdLogger::commit_line_(size_t len) {
  if (len == 0)
    return;
  this->block_.commit(len);
  // Counted, never lseek()'d: at production load this runs ~3600 times a second
  // and the file offset is knowable without asking the VFS.
  this->file_bytes_ += static_cast<uint32_t>(len);
}

void SdLogger::write_record_(const LogRecord &rec, uint64_t now64) {
  // Formatted straight into the block buffer at the write cursor: no
  // intermediate line buffer, no per-line memcpy, no stdio lock, and no
  // snprintf — which at ~9 format-string parses per record was where the
  // measured 85 % CPU at the record ceiling went (spec D6/F1g).
  if (!this->ensure_room_(SD_LOG_MAX_LINE)) {
    // The file went away underneath this pass — a flush that failed a moment ago. The record is
    // already out of the ring and cannot go back, so count it; letting it evaporate here is what
    // made a card failure look like a logger that had merely gone quiet.
    this->card_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const size_t n = format_record(this->block_.cursor(), this->block_.room(), this->stamps_, rec,
                                 reconstruct_us(now64, rec.t_us), this->sources_);
  this->commit_line_(n);
  this->records_written_++;
  this->maybe_rotate_(now64);
}

void SdLogger::write_text_(const TextRecord &rec, uint64_t now64) {
  if (!this->ensure_room_(SD_LOG_MAX_LINE)) {
    this->card_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The slot holds the tag bytes followed by the message bytes; both were
  // copied raw at capture and are escaped here, on the consumer side.
  // A message cut at the slot boundary carries the same '~' the line cap uses,
  // so a reader can tell a short log line from one that was clipped (spec F1e).
  const char *tag = rec.buf;
  const char *msg = rec.buf + rec.tag_len;
  const size_t n =
      format_text(this->block_.cursor(), this->block_.room(), this->stamps_, reconstruct_us(now64, rec.t_us), rec.level,
                  tag, rec.tag_len, msg, rec.len, rec.truncated != 0);
  this->commit_line_(n);
  // Deliberately NOT counted in records_written_. That counter is the rec/s
  // figure the bench compares against the measured ceiling (spec F1g), and
  // folding firmware log lines into it would quietly inflate exactly the number
  // the M5.7 re-measure is supposed to decide. Their volume shows up in
  // bytes_written_, where it belongs.
  this->maybe_rotate_(now64);
}

void SdLogger::write_drop_markers_(uint64_t now64) {
  // D4's overflow marker, and the half of D4 that M1 specified and never built:
  // today a dropped burst is indistinguishable from a quiet bus once the console
  // scrollback is gone. Emitted only on a counter *change*, so a healthy run
  // costs nothing.
  //
  // The three counters stay separate for the same reason the stats line refuses
  // to sum them: a producer that found the logger ring full (SD throughput /
  // buffer_depth), the gateway's RX ISR that found a tap ring full
  // (log_tap_queue_depth / drain cadence) and a log line that found the text
  // ring full are three different bottlenecks with three different fixes.
  // `card` first, because it is the one that explains a gap rather than a slowdown: everything it
  // counts was lost while there was no file at all, so this marker is the *only* record that the
  // outage happened. It is deliberately the one baseline write_file_header_() does not reset —
  // see the note there.
  const uint32_t card_dropped = this->card_dropped_.load(std::memory_order_relaxed);
  if (card_dropped != this->marked_card_dropped_ && this->ensure_room_(SD_LOG_MAX_LINE)) {
    this->commit_line_(format_drop(this->block_.cursor(), this->block_.room(), now64, "card", nullptr,
                                   card_dropped - this->marked_card_dropped_, card_dropped));
    this->marked_card_dropped_ = card_dropped;
  }

  // `#gap` sits next to `card` because it answers the same question — what happened to the data
  // that is not here — and it is emitted on the same terms as every `#drop`: only when the counters
  // moved, so a healthy run costs nothing. What the fields mean is settled in
  // docs/sdlog-collection-design.md §7a and is deliberately not restated here — least of all the
  // span, which is a width and not a loss claim.
  //
  // The window is taken out of the policy as soon as it moves, but into a **member**, not into a
  // line: `commit_line_()` only advances the block cursor, so a window taken at commit time is one
  // `enter_failed_()` away from being erased with the rest of the block. It stays owed until the
  // bytes behind its line have gone through write().
  this->retire_gap_if_durable_();
  // Locked like every other index access even though the discard window is writer-only state the
  // httpd task never looks at: the rule "every `this->collection_.` sits inside an `IndexLock`" is
  // worth more than the nanoseconds an uncontended take costs once per 20 ms writer pass, because
  // it is the only form of this invariant a reviewer can check by grepping.
  {
    IndexLock lock(this->index_mux_);
    if (this->collection_.has_pending_discards())
      this->owe_gap_(this->collection_.take_pending_discards());
  }
  // One line in flight at a time. Re-stating a window whose line *did* reach the card would
  // over-report the loss, and §7a is explicit that over-reporting is the one failure this marker
  // must not have: a line claiming six hours it did not lose sends someone hunting for files that
  // are not missing, and the next honest `#gap` is then the one nobody believes.
  if (this->gap_owed_.chunks != 0 && this->gap_inflight_.chunks == 0 && this->ensure_room_(SD_LOG_MAX_LINE)) {
    const size_t n = format_gap(this->block_.cursor(), this->block_.room(), now64, this->gap_owed_.chunks,
                                this->gap_owed_.bytes, this->gap_owed_.first_seq, this->gap_owed_.last_seq);
    // A refused format leaves the window owed rather than silently spent — the whole point of
    // holding it in a member. commit_line_(0) would be a no-op and gap_line_end_ would then name an
    // offset the file has already passed, retiring a line that was never written.
    if (n > 0) {
      this->commit_line_(n);
      this->gap_inflight_ = this->gap_owed_;
      this->gap_owed_ = DiscardStats{};
      // commit_line_() has just advanced file_bytes_ past the line, which is exactly how much of
      // this file has to be on the card before the window is spent.
      this->gap_line_end_ = this->file_bytes_;
    }
  }

  const uint32_t dropped = this->dropped_records_.load(std::memory_order_relaxed);
  if (dropped != this->marked_dropped_ && this->ensure_room_(SD_LOG_MAX_LINE)) {
    this->commit_line_(format_drop(this->block_.cursor(), this->block_.room(), now64, "ring", nullptr,
                                   dropped - this->marked_dropped_, dropped));
    this->marked_dropped_ = dropped;
  }

  const uint32_t text_dropped = this->text_dropped_.load(std::memory_order_relaxed);
  if (text_dropped != this->marked_text_dropped_ && this->ensure_room_(SD_LOG_MAX_LINE)) {
    this->commit_line_(format_drop(this->block_.cursor(), this->block_.room(), now64, "text", nullptr,
                                   text_dropped - this->marked_text_dropped_, text_dropped));
    this->marked_text_dropped_ = text_dropped;
  }

#ifdef USE_SD_LOGGER_CAN_TAP
  for (uint8_t i = 0; i < this->can_tap_count_; i++) {
    const uint32_t tap_dropped = this->can_taps_[i].port->log_tap_dropped();
    if (tap_dropped == this->can_taps_[i].marked_dropped || !this->ensure_room_(SD_LOG_MAX_LINE))
      continue;
    this->commit_line_(format_drop(this->block_.cursor(), this->block_.room(), now64, "tap", this->can_taps_[i].label,
                                   tap_dropped - this->can_taps_[i].marked_dropped, tap_dropped));
    this->can_taps_[i].marked_dropped = tap_dropped;
  }
#endif
}

void SdLogger::owe_gap_(const DiscardStats &window) {
  // `chunks` and `bytes` add. The endpoints keep the order retention emptied them in — the older
  // window's `first_seq`, the newer window's `last_seq` — because §7a makes them endpoints rather
  // than a numeric min and max: `seq` wraps at 9999999, and sorting a pair that straddles the
  // ceiling prints `0..9999999` and claims the entire card went away.
  //
  // Merging two windows into one line is honest by the same section: the span is only the width of
  // what retention walked, `chunks` is the loss, and a merged window is punctured in exactly the way
  // §7a already requires every reader to handle (`chunks != span`). One line understating nothing is
  // better than two lines of which the first may never be written.
  if (window.chunks == 0)
    return;
  if (this->gap_owed_.chunks == 0) {
    this->gap_owed_ = window;
    return;
  }
  this->gap_owed_.chunks += window.chunks;
  this->gap_owed_.bytes += window.bytes;
  this->gap_owed_.last_seq = window.last_seq;
}

void SdLogger::retire_gap_if_durable_() {
  if (this->gap_inflight_.chunks == 0)
    return;
  // `file_bytes_` counts everything committed to this file (seeded from its real size at open) and
  // `block_.pending()` is the part of that still sitting in RAM, so the difference is where the file
  // ends *on the card*. Once that reaches the end of the committed `#gap` line, the line went
  // through write() and the window it states is spent. Nothing here needs fsync: this is the same
  // standard every other byte in the file is held to.
  const uint32_t buffered = static_cast<uint32_t>(this->block_.pending());
  const uint32_t on_card = this->file_bytes_ > buffered ? this->file_bytes_ - buffered : 0;
  if (on_card < this->gap_line_end_)
    return;
  this->gap_inflight_ = DiscardStats{};
  this->gap_line_end_ = 0;
}

void SdLogger::drop_unflushed_gap_() {
  // The block buffer is about to lose whatever is committed in it — a dead card, a remount, a fresh
  // file. A `#gap` line in there never reached the card, so the window it states goes back to being
  // owed and is stated by the first marker pass of the next file that opens. That is exactly the
  // treatment `marked_card_dropped_` gets from write_file_header_(): the accounting outlives the
  // file, because the file is the thing that failed.
  //
  // Ask about durability *first*, and this is not belt and braces. A line flushed by one of the
  // later markers in a pass is on the card, but the window it states is only retired at the top of
  // the next pass — and a write failure inside that gap would otherwise fold a window that was
  // safely written back into the owed one and state it a second time in the next file. §7a: a `#gap`
  // that claims a loss it did not have is worse than none, because the next honest one is then the
  // one nobody believes. This is why every caller has to run while `file_bytes_` and `block_` still
  // describe the file the line was written into.
  this->retire_gap_if_durable_();
  if (this->gap_inflight_.chunks == 0) {
    this->gap_line_end_ = 0;
    return;
  }
  const DiscardStats newer = this->gap_owed_;
  this->gap_owed_ = this->gap_inflight_;  // the in-flight window is the older of the two
  this->gap_inflight_ = DiscardStats{};
  this->gap_line_end_ = 0;
  this->owe_gap_(newer);
}

void SdLogger::add_source(uint8_t tag, char kind, const char *label) {
  if (!this->sources_.add(tag, kind, label)) {
    // Not fatal: an unmapped tag still logs, as `U,<decimal>` (spec D1). Loud
    // anyway, because the file would silently stop naming that bus.
    ESP_LOGW(TAG, "source table full (%d entries) - tag %u logs as U,%u instead of '%s'", SD_LOG_MAX_SOURCES, tag, tag,
             label);
  }
}

#ifdef USE_SD_LOGGER_CAN_TAP
void SdLogger::add_can_tap(can_gateway::GatewayPort *port, uint8_t source, const char *label) {
  if (this->can_tap_count_ >= SD_LOGGER_CAN_TAP_MAX)
    return;  // codegen sizes the array from the same list; unreachable by construction
  this->can_taps_[this->can_tap_count_].port = port;
  this->can_taps_[this->can_tap_count_].source = source;
  this->can_taps_[this->can_tap_count_].label = label;
  this->can_tap_count_++;
  this->add_source(source, KIND_CAN, label);
}

uint32_t SdLogger::get_tap_dropped_records() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < this->can_tap_count_; i++)
    total += this->can_taps_[i].port->log_tap_dropped();
  return total;
}

void SdLogger::tap_drain_loop_() {
  // The tap rings' single consumer, decoupled from the card on purpose: this loop never makes a
  // filesystem call, so a card that goes internally busy for hundreds of ms (and the writer
  // spinning it out) cannot stop tap records from reaching the record ring. From there the writer
  // drains them like every other source, and the ring's fill is what backpressure_() throttles a
  // transfer on — tap traffic used to bypass that ring entirely, which is why §6's criterion
  // passed while tap_dropped failed.
  for (;;) {
    if (this->dying_.load(std::memory_order_acquire)) {
      // Same exit the writer takes: emergency_close_ has set mounted_ false, so nothing is being
      // consumed anymore; whatever is left in the tap rings (a few ms of tail) stays there.
      this->tap_drain_task_ = nullptr;
      vTaskDelete(nullptr);
      return;
    }
    // Gated on mounted_, not file_ok_: with no card the records stay in the tap rings and the RX
    // ISR sheds into tap_dropped, exactly as before this task existed — an outage keeps its
    // counter. (push_record() would count them as card_dropped, which says "the card lost this",
    // and during a mount retry that is true of every source but double-speak for a ring drain.)
    if (this->mounted_) {
      bool record_ring_full = false;
      for (uint8_t i = 0; i < this->can_tap_count_ && !record_ring_full; i++) {
        auto *ring = this->can_taps_[i].port->log_tap_ring();
        if (ring == nullptr)
          continue;  // port armed at codegen but never enabled
        can_gateway::TapRecord tap;
        LogRecord rec;
        while (this->mounted_ && ring->pop(tap)) {
          // Field by field, never a memcpy — see tap_translate.h for why, and
          // tests/host/test_tap_translate.cpp for the cases that hold it there.
          tap_to_log_record(tap, this->can_taps_[i].source, rec);
          if (!this->push_record(rec)) {
            // Record ring full (writer stalled long enough to fill ~585 ms of it). Stop the whole
            // pass: records left *in the tap rings* are another ~146 ms of buffer, whereas popping
            // them now would feed them straight into the full ring's drop counter. The one record
            // already popped is lost and was counted by push_record(); at 5 ms per pass that caps
            // the misattribution at ~200 records/s against the ~7000/s it replaces.
            record_ring_full = true;
            break;
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(TAP_DRAIN_POLL_MS));
  }
}
#endif

#ifdef USE_SD_LOGGER_COLLECTION
void SdLogger::retention_pass_() {
  // "Store everything all the time" is bounded by collection, and this is what happens when
  // collection does not keep up (design §7): above the fill threshold the card becomes a circular
  // buffer in which collected chunks are the free list. Logging never stops; the oldest history
  // loses, and says so in-band through the `#gap` line the next marker pass writes.
  //
  // Writer task only (§8). The fd is required, not because the delete needs it, but because
  // `!file_ok_()` means the card is already the suspect and a recovery is being retried against it.
  // And never while dying_: the hold-up cap (spec §7 H6) is sized to finish one write, not to spend
  // itself on an unlink for space the run is about to stop needing.
  if (!this->file_ok_() || this->dying_.load(std::memory_order_relaxed))
    return;
  const bool retaining = this->collection_.enabled();
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // The fill is read even with retention disabled, because `GET /sdlog/status` reports it and the
  // httpd task may not ask the card itself (§9.4). `collection: {enabled: false, serve: true}` is a
  // real shape — a bounded index and a server, with the card left to fill — and its status endpoint
  // should not report a number frozen at mount time.
  const bool need_fill = true;
#else
  const bool need_fill = retaining;
#endif
  if (!need_fill)
    return;
  uint8_t fill = 0;
  if (!this->card_fill_percent_(&fill)) {
    // Not enter_failed_(): a volume that will not report its free space says nothing about whether
    // the open file is still writable, and taking logging down over it would lose data to protect
    // free space.
    ESP_LOGW(TAG, "retention: %s did not report its fill — nothing discarded this pass", this->mount_point_.c_str());
    return;
  }
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  this->card_fill_pub_.store(fill, std::memory_order_relaxed);
#endif
  if (!retaining)
    return;
  // The victim is chosen, copied out **and reserved** in one critical section: `next_victim()` hands
  // back a pointer into `chunks_[]`, which the httpd task can invalidate the moment the lock is
  // released — and the `#gap` accounting needs the size of a file that will no longer exist. The
  // unlink itself is card I/O and happens with the lock given back.
  //
  // **Choosing and reserving cannot be two transactions.** Between them, `collection_begin_serve()`
  // can grant a transfer for the very chunk about to be unlinked, and every check downstream then
  // fails safe in the wrong direction: `discard()` refuses a serving chunk, so the file is deleted
  // and its index entry survives, naming a file that is gone. `CONFIG_FATFS_FS_LOCK = 0` in this
  // build means FatFs does not refuse `f_unlink` on an open file either, so the handler keeps
  // reading a freed FAT chain — into clusters the writer may already have reallocated — and serves
  // whatever is there at the `Content-Length` it promised. The client's only cut detector is
  // `got < promised`, so that response verifies clean and lands in the archive as log data.
  //
  // The reservation is the existing `serving` flag rather than a new `condemned` one: it already
  // means "hands off this entry" to `next_victim()` and `discard()`, `collection_begin_serve()` now
  // refuses on it, and it costs no byte in `ChunkEntry` (pinned at 12) and no edit in
  // collection_policy.h. What it does cost is precision in one console line — a chunk being deleted
  // and a chunk being read are the same state to `next_victim()` for the length of one unlink.
  uint32_t seq = 0;
  uint64_t bytes = 0;
  bool never_collected = false;
  bool reserved = false;
  char name[SD_LOG_NAME_LEN];
  {
    IndexLock lock(this->index_mux_);
    const ChunkEntry *victim = this->collection_.next_victim(fill);
    if (victim == nullptr)
      return;
    seq = victim->seq;
    bytes = victim->bytes;
    // False once the collection server exists and a puller has confirmed something: a CONFIRMED
    // chunk is not a gap, because the collector already has it. Without a server this is always
    // true, which is what makes the `#gap` path soak-testable on a bench with no network — every
    // retention pass there is a real, never-collected loss.
    never_collected = victim->state == ChunkState::SEALED;
    format_chunk_name(name, seq, victim->state);
    // `next_victim()` already skipped everything serving, so this cannot be stealing a transfer's
    // chunk; it is claiming one nobody holds. Cleared on **every** path out of here, which is why
    // the failed-unlink branch below is not a bare `return`.
    reserved = this->collection_.mark_serving(seq);
  }
  const std::string path = this->mount_point_ + "/" + name;
  // The result is captured rather than re-read off `errno`, which a *successful* unlink leaves
  // untouched at whatever the last failing call set it to.
  errno = 0;
  const bool unlinked = unlink(path.c_str()) == 0;
  const int unlink_errno = errno;
  if (!unlinked && unlink_errno != ENOENT) {
    // Left tracked on purpose. The file is still there, and an index that forgets a file that
    // exists is the one failure retention cannot recover from — that chunk would then never be
    // listed, never served and never deleted while the card fills behind it. The next pass retries.
    // The reservation goes back first: a chunk left flagged is a chunk `next_victim()` walks past
    // forever *and* one no transfer can ever be granted for — the file would be stranded in both
    // directions by a failure that only means "not this pass".
    if (reserved) {
      IndexLock lock(this->index_mux_);
      this->collection_.clear_serving(seq);
    }
    ESP_LOGW(TAG, "retention: unlink %s failed (errno %d)", path.c_str(), unlink_errno);
    return;
  }
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  if (!never_collected && !unlinked) {
    // The entry is CONFIRMED but its `.UPL` is not there — which happens exactly when a confirm's
    // rename failed for something other than ENOENT and drain_confirms_() logged it and moved on.
    // The `.LOG` file is then still on the card under a name nothing tracks any more, so reclaim it
    // here: the collector already has these bytes, which is what CONFIRMED means.
    char sealed_name[SD_LOG_NAME_LEN];
    format_chunk_name(sealed_name, seq, ChunkState::SEALED);
    const std::string sealed_path = this->mount_point_ + "/" + sealed_name;
    if (unlink(sealed_path.c_str()) == 0)
      ESP_LOGW(TAG, "retention: %s was never renamed; reclaimed it as %s", name, sealed_name);
  }
#endif
  // ENOENT lands here too: the file is gone either way, and an entry naming a file that does not
  // exist would be chosen again on every pass.
  //
  // The reservation is released and the entry dropped in one critical section: `discard()` refuses a
  // chunk that is still flagged, so clearing it in a separate scope would re-open — for exactly the
  // width of two lock acquisitions — the window this function was rewritten to close.
  uint32_t discarded_chunks;
  uint64_t discarded_bytes;
  bool billed;
  {
    IndexLock lock(this->index_mux_);
    if (reserved)
      this->collection_.clear_serving(seq);
    billed = this->collection_.discard(seq);
    discarded_chunks = this->collection_.discarded_chunks();
    discarded_bytes = this->collection_.discarded_bytes();
  }
  // Mirrored out of the index rather than read from it: the index belongs to this task (§8) and the
  // stats line runs on the main loop. Unconditional, because `discard()` bills only never-collected
  // chunks — deleting a CONFIRMED one leaves both totals exactly where they were.
  this->discarded_chunks_.store(discarded_chunks, std::memory_order_relaxed);
  this->discarded_kib_.store(static_cast<uint32_t>(discarded_bytes / 1024), std::memory_order_relaxed);
  if (!billed) {
    // The file is gone but the entry was not the writer's to drop — `discard()` refuses an unknown
    // seq and the OPEN chunk. Neither is reachable from a reserved victim on this task, so if this
    // ever prints, the index and the card have disagreed about what was deleted and the console is
    // the only place that will say so. Said once, and **not** as a discard: the lines below claim
    // `#gap` accounting that did not happen, which is precisely the over-reporting design §7a exists
    // to prevent — a console that bills a loss twice (once when the discard was refused, again when
    // it finally took) sends someone hunting for files that are not missing.
    ESP_LOGW(TAG, "retention: unlinked %s but the index would not drop it", name);
  } else if (never_collected) {
    ESP_LOGW(TAG, "retention: card %u%% full — discarded %s, %" PRIu32 " KB never collected (total %" PRIu32 ")", fill,
             name, static_cast<uint32_t>(bytes / 1024), discarded_chunks);
  } else {
    ESP_LOGI(TAG, "retention: card %u%% full — deleted collected %s", fill, name);
  }
}
#endif

#ifdef USE_SD_LOGGER_COLLECTION_SERVER
void SdLogger::drain_confirms_() {
  // The other half of `POST /sdlog/done/<name>`. The handler decided — under the index lock, from
  // the index and never from the filesystem — that the chunk is CONFIRMED; this is the rename that
  // makes the card agree, and it is here because §8 gives every filesystem mutation to this task.
  //
  // The order this preserves is the one the whole design rests on: serve bytes -> puller confirms ->
  // rename. A crash anywhere inside that window re-serves the chunk, which is correct and cheap; the
  // inverse order loses it permanently on the same crash.
  if (this->confirm_q_ == nullptr || !this->file_ok_())
    return;
  uint32_t seq = 0;
  // **Peek, decide, then consume.** The intent stays in the queue until this task has committed to
  // executing it, so the deferral path below needs no re-queue at all — and that matters, because
  // the re-queue it replaces was documented as unable to fail and could: the httpd task may
  // `xQueueSend()` into the slot the receive just freed, between the receive and the send-to-front.
  // `xQueueSendToFront()` then returns `errQUEUE_FULL`, the intent is dropped, and the index is left
  // CONFIRMED over a file still named `.LOG` — which retention's ENOENT branch reclaims **only if
  // retention is enabled**. `collection: {enabled: false, serve: true}` is a supported shape in
  // which `retention_pass_()` returns before it ever looks, and that file is then never listed,
  // never served and never reclaimed for the rest of the card's life.
  //
  // Peeking is safe because this task is the queue's only consumer: nothing else can take the head
  // between the peek and the receive, and producers only ever append.
  while (xQueuePeek(this->confirm_q_, &seq, 0) == pdTRUE) {
    bool reserved = false;
    bool renameable = true;
    {
      // Not while a handler still holds a read fd on the file. `confirm()` deliberately leaves the
      // serving flag alone — the fixed order is serve -> confirm -> rename, so a confirm can land
      // while the transfer is still finishing — but whether an open FATFS `FIL` survives a rename
      // of its directory entry is **not verified on this platform**. Deferring costs one writer pass
      // (20 ms) in the only case that can reach it: a second puller, or a client that confirmed a
      // chunk it is still reading. The design doc offers exactly this choice — "verify, or defer the
      // rename until `is_serving(seq)` is false" — and this is the half that needs no hardware.
      IndexLock lock(this->index_mux_);
      if (this->collection_.is_serving(seq))
        break;  // still at the head of the queue; the rest waits with it rather than reordering
      // Reading `is_serving()` under the lock and then renaming outside it decides nothing on its
      // own: a card failure between the POST and here clears the index, the rescan re-adds this seq
      // as SEALED from the `.LOG` still on the card, and the httpd task can be granted a transfer
      // for it in the gap. So the same reservation retention uses — claim the entry with the flag
      // `collection_begin_serve()` now refuses on, and hold it across the card I/O.
      const ChunkEntry *entry = this->collection_.find(seq);
      // A rescan can also hand this seq to the file the writer currently holds an fd to (the `.LOG`
      // it names was deleted, so the scan's max+1 came back around to it). Renaming that one moves
      // the directory entry out from under an open `FIL`, which is the exact accident the deferral
      // above exists to avoid — drop the intent instead. `mark_serving()` refuses the OPEN chunk for
      // the same reason, so this is not merely belt and braces: without the check the rename would
      // run *unreserved*.
      renameable = entry == nullptr || entry->state != ChunkState::OPEN;
      if (renameable)
        reserved = this->collection_.mark_serving(seq);
    }
    // Committed: take it off the queue. Its value is already in `seq` from the peek, and this task
    // is the only consumer, so the receive cannot fail — but a peek that keeps succeeding while the
    // receive does not is an unbounded loop on the *writer* task, which is a worse failure than the
    // dropped intent it would be spinning over. Bounded by construction instead.
    uint32_t taken = 0;
    if (xQueueReceive(this->confirm_q_, &taken, 0) != pdTRUE) {
      if (reserved) {
        IndexLock lock(this->index_mux_);
        this->collection_.clear_serving(seq);
      }
      break;
    }
    if (!renameable) {
      ESP_LOGW(TAG, "confirm: L%07" PRIu32 " is the open file now — intent dropped, not renamed", seq);
      continue;
    }
    char from[SD_LOG_NAME_LEN];
    char to[SD_LOG_NAME_LEN];
    format_chunk_name(from, seq, ChunkState::SEALED);
    format_chunk_name(to, seq, ChunkState::CONFIRMED);
    const std::string src = this->mount_point_ + "/" + from;
    const std::string dst = this->mount_point_ + "/" + to;
    // Captured rather than re-read off `errno`, which a *successful* rename leaves at whatever the
    // last failing call set it to.
    errno = 0;
    const bool renamed = ::rename(src.c_str(), dst.c_str()) == 0;
    const int rename_errno = errno;
    {
      IndexLock lock(this->index_mux_);
      if (reserved)
        this->collection_.clear_serving(seq);
      // Applied here as well as in the handler, and idempotently, because the two can disagree: a
      // card failure between the POST and this rename clears the index, and the rescan that follows
      // re-adds the chunk as SEALED from a `.LOG` file this is about to rename away. Without this
      // the index would list a name that no longer exists and every fetch of it would 500 forever.
      if (renamed || rename_errno == ENOENT)
        this->collection_.confirm(seq);
    }
    if (!renamed && rename_errno != ENOENT) {
      // Not retried and not re-queued: a rename that fails for a reason other than "already gone"
      // is a card fault, and spinning on it would turn one bad directory entry into a permanent
      // busy loop on the writer task. The entry stays CONFIRMED, so retention will pick it — and
      // the ENOENT branch there reclaims the `.LOG` this left behind.
      ESP_LOGW(TAG, "confirm: rename %s -> %s failed (errno %d)", from, to, rename_errno);
      continue;
    }
    ESP_LOGI(TAG, "collected: %s -> %s", from, to);
  }
}
#endif

#ifdef USE_SD_LOGGER_COLLECTION_SERVER
// ------------------------------------------------------------------------------------------------
// The collection server's view of the index (design §8a). httpd task; each takes `index_mux_` for
// exactly one index operation and gives it back before the caller touches a socket or the card.
// ------------------------------------------------------------------------------------------------

uint16_t SdLogger::collection_count() const {
  IndexLock lock(this->index_mux_);
  return this->collection_.count();
}

bool SdLogger::collection_entry_at(uint16_t index, ChunkEntry *out) const {
  IndexLock lock(this->index_mux_);
  const ChunkEntry *entry = this->collection_.at(index);
  if (entry == nullptr)
    return false;
  // A copy, not the pointer: `discard()` memmoves the array the moment this lock is given back, and
  // the caller is going to spend a socket write on whatever it holds.
  *out = *entry;
  return true;
}

bool SdLogger::collection_begin_serve(uint32_t seq, uint32_t *bytes_out, ChunkState *state_out) {
  IndexLock lock(this->index_mux_);
  // The serving gate, first and in this same critical section — that adjacency is the whole
  // mechanism. The writer clears `serving_open_` under this lock before it drains
  // `transfers_in_flight_`, so a grant either happens entirely before the clear (and the drain then
  // waits for it) or is refused here. Splitting the check out of this scope, or checking it in the
  // handler before calling, restores exactly the window the drain barrier exists to close.
  //
  // Reported as NONE rather than the entry's real state, so the handler answers the existing 404
  // "no such chunk" and never a 409: during a teardown the honest answer is "not from here", and 404
  // is the status the client already handles by failing that chunk and going on to the next.
  if (!this->serving_open_) {
    *state_out = ChunkState::NONE;
    return false;
  }
  const ChunkEntry *entry = this->collection_.find(seq);
  *state_out = entry == nullptr ? ChunkState::NONE : entry->state;
  // SEALED only (§2.3/§3.2). OPEN is the file the writer holds an fd to and is never servable at
  // any point; CONFIRMED has been collected already and is 404 rather than a second copy.
  if (entry == nullptr || entry->state != ChunkState::SEALED)
    return false;
  // **Already claimed — refuse.** `mark_serving()` returns true for a chunk that is already serving
  // (it simply sets the flag again), so it cannot carry this decision on its own. Two things ride on
  // checking it here:
  //
  //   * the writer *reserves* a chunk with this same flag before it unlinks (retention) or renames
  //     (`drain_confirms_()`) it, with the lock given back across the card I/O in between. Refusing
  //     here is what makes that reservation mean something — otherwise a transfer is granted for a
  //     file whose `unlink()` is already committed to, and `CONFIG_FATFS_FS_LOCK = 0` means FatFs
  //     will not refuse it either. The handler then reads a freed FAT chain, at the promised
  //     `Content-Length`, and the client's only cut detector is `got < promised`;
  //   * the flag is a boolean, not a refcount, so a second concurrent transfer of the same chunk
  //     would have its guard cleared by whichever handler finished first — and the survivor would be
  //     serving a chunk retention no longer believes is busy. The collector is strictly sequential,
  //     so refusing the second costs a 404 on a request no client makes.
  if (this->collection_.is_serving(seq))
    return false;
  *bytes_out = entry->bytes;
  if (!this->collection_.mark_serving(seq))
    return false;
  // Paired with collection_end_serve(), which the handler's RAII guard runs on every exit path.
  // Two different things depend on the pairing: the serving flag (retention must skip this chunk)
  // and this count (unmount_card_() must not free the VFS context under an open read fd).
  this->transfers_in_flight_.fetch_add(1, std::memory_order_release);
  return true;
}

void SdLogger::collection_end_serve(uint32_t seq) {
  {
    IndexLock lock(this->index_mux_);
    // Idempotent, and tolerates a seq that is no longer tracked — which is what an enter_failed_()
    // during a transfer leaves behind.
    this->collection_.clear_serving(seq);
  }
  this->transfers_in_flight_.fetch_sub(1, std::memory_order_release);
}

SdLogger::ConfirmResult SdLogger::collection_confirm(uint32_t seq) {
  IndexLock lock(this->index_mux_);
  // The same gate, for the same reason one step further on: an accepted confirm is an intent in a
  // queue that only the writer task drains, and the emergency path (`dying_` -> `emergency_close_()`
  // -> `vTaskDelete`) ends that task. Answering 200 after the gate has closed is the device saying
  // "collected" about a rename that will never happen — and 200 is exactly what makes the collector
  // delete its own copy's `.part` bookkeeping and never ask again. BUSY (503) instead: the confirm is
  // idempotent and the client retries it, which is right when the gate is closed for a remount and
  // honest when it is closed for good.
  if (!this->serving_open_)
    return ConfirmResult::BUSY;
  const ChunkEntry *entry = this->collection_.find(seq);
  if (entry == nullptr)
    return ConfirmResult::NOT_TRACKED;
  if (entry->state == ChunkState::OPEN)
    return ConfirmResult::IS_OPEN;
  if (entry->state == ChunkState::CONFIRMED)
    return ConfirmResult::CONFIRMED;  // idempotent: the rename is queued or already done
  // The intent is accepted *before* the state flips, and that order is the point. An index that
  // says CONFIRMED with no rename queued would leave a `.LOG` file on the card that retention later
  // tries to unlink under its `.UPL` name and never finds — a file nothing lists, serves or
  // reclaims, which is the one failure mode this whole index exists to prevent.
  if (this->confirm_q_ == nullptr || xQueueSend(this->confirm_q_, &seq, 0) != pdTRUE)
    return ConfirmResult::BUSY;
  this->collection_.confirm(seq);
  return ConfirmResult::CONFIRMED;
}

bool SdLogger::collection_oldest_uncollected(uint32_t *seq_out) const {
  IndexLock lock(this->index_mux_);
  return this->collection_.oldest_uncollected(seq_out);
}

uint32_t SdLogger::collection_discarded_chunks() const {
  IndexLock lock(this->index_mux_);
  return this->collection_.discarded_chunks();
}

uint64_t SdLogger::collection_discarded_bytes() const {
  IndexLock lock(this->index_mux_);
  return this->collection_.discarded_bytes();
}
#endif

void SdLogger::sync_file_() {
  if (this->fd_ < 0)
    return;
  this->flush_block_(/*all=*/false);
  // Timed for the same reason as the write() in flush_block_: an fsync lands FAT and directory
  // sectors away from the data stream, which is precisely the access pattern that sends a card
  // into its garbage collector.
  const int64_t t0 = esp_timer_get_time();
  fsync(this->fd_);
  const int64_t held_us = esp_timer_get_time() - t0;
  if (held_us > STALL_WARN_US)
    ESP_LOGW(TAG, "fsync() held %" PRId64 " ms — card internally busy", held_us / 1000);
}

void SdLogger::writer_loop_() {
  for (;;) {
    // One 64-bit clock read per pass; every record's 32-bit stamp is rebuilt
    // against it (spec D8). The reconstruction is signed precisely because
    // producers keep stamping records *after* this read for the whole pass.
    const uint64_t now64 = static_cast<uint64_t>(esp_timer_get_time());
    const uint32_t now = millis();

    // The writer's own outage meter. §4.3 had to infer 220-360 ms writer stalls from tap burst
    // sizes because nothing measured the gap between passes; now a pass that starts late says so
    // and by how much. Late means: more than the poll period plus a generous scheduling allowance
    // — anything past that is time spent inside a card call or preempted, which is exactly the
    // window every ring in the system has to be sized against.
    if (this->last_pass_us_ != 0) {
      const int64_t gap_us = static_cast<int64_t>(now64) - this->last_pass_us_;
      if (gap_us > static_cast<int64_t>(WRITER_POLL_MS) * 1000 + STALL_WARN_US) {
        ESP_LOGW(TAG, "writer pass gap %" PRId64 " ms (poll is %" PRIu32 " ms)", gap_us / 1000,
                 static_cast<uint32_t>(WRITER_POLL_MS));
      }
    }
    this->last_pass_us_ = static_cast<int64_t>(now64);

    if (this->file_ok_()) {
      // Drain everything currently queued. All sources feed the same file; t_us
      // (stamped by the producer, in the RX ISR for a tap) is what puts the
      // interleaved records back in order on read-out. Skew is bounded by this
      // poll period, which is what F1f documents rather than fixes.
      //
      // Every loop re-checks file_ok_(): a failing write takes the file down mid-pass, and the
      // remaining pops would otherwise go into a closed fd.
      LogRecord rec;
      while (this->file_ok_() && this->pop_record_(rec))
        this->write_record_(rec, now64);
      TextRecord text;
      while (this->file_ok_() && this->pop_text_(text))
        this->write_text_(text, now64);

      this->write_drop_markers_(now64);

      // The time bound again, once per pass. write_record_() covers the busy case, but the whole
      // point of the bound is the *quiet* one — a bus that produces nothing reaches no record path
      // at all, and its last records would sit in an OPEN file, unservable, until traffic resumed
      // (design §4). This is also what the 20 ms poll makes the deadline's real granularity.
      this->maybe_rotate_(now64);

#ifdef USE_SD_LOGGER_COLLECTION_SERVER
      // Every pass, not on the retention cadence: a confirm the collector is waiting on should not
      // sit in a queue for five seconds, and an empty queue costs one non-blocking xQueueReceive.
      this->drain_confirms_();
#endif
#ifdef USE_SD_LOGGER_COLLECTION
      if (now - this->last_retention_ms_ >= RETENTION_POLL_MS) {
        this->last_retention_ms_ = now;
        this->retention_pass_();
      }
#endif

      if (now - this->last_sync_ms_ >= this->sync_interval_ms_) {
        this->sync_file_();
        this->last_sync_ms_ = now;
      } else {
        // Push whole sectors out as they accumulate even between syncs, so the
        // block buffer stays a latency absorber rather than a second ring.
        this->flush_block_(/*all=*/false);
      }
    } else if (!this->dying_.load(std::memory_order_acquire) && this->recovery_.due(now)) {
      // No file, and the ladder says it is time to try again. Nothing is drained in this state:
      // producers are gated on `mounted_` and count their own losses as `card`, so the rings hold
      // still rather than being emptied into nowhere.
      this->recovery_.note_attempt(now);
      // Say which of the two attempts this is. "power-cycling the card" was printed for a year by
      // benches whose card_power_pin drove a header pin and nothing else; a line that names the
      // step it is about to take is checkable against what happens next.
      ESP_LOGW(TAG, "card recovery: attempt %" PRIu32 " (%s%sremount)", this->recovery_.attempts(),
               this->recovery_power_cycle_ ? "power cycle + " : "",
               this->recovery_.reset_due() ? "in-band reset + " : "");
      if (this->try_recover_()) {
        ESP_LOGI(TAG, "card recovered after %" PRIu32 " attempt(s) — logging to L%07" PRIu32 ".LOG",
                 this->recovery_.attempts(), this->seq_);
        this->recovery_.reset();
      } else if (this->recovery_.exhausted()) {
        ESP_LOGE(TAG, "card recovery gave up after %" PRIu32 " attempts — logging off for this run",
                 this->recovery_.attempts());
      } else {
        ESP_LOGW(TAG, "card recovery failed — next attempt in %" PRIu32 " ms", this->recovery_.delay_ms());
      }
    }

    if (this->dying_.load(std::memory_order_acquire)) {
      this->emergency_close_();
      this->writer_task_ = nullptr;
      vTaskDelete(nullptr);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(WRITER_POLL_MS));
  }
}

void SdLogger::on_shutdown() {
  // The third exit path, and the reason a missing `#close` means power loss
  // rather than "the firmware restarted". OTA and a requested reboot both come
  // through here; the writer task is still running, so hand it the same
  // dying_ flag the VCC monitor uses and give it a moment to finish.
  // The writer task now outlives a card failure, so its existence — not the file's — is what says
  // there is something to wind down. Skipping this when a recovery happens to be pending would
  // leave the task running through the reboot.
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // First, and before the writer is told to wind down: httpd_stop() blocks until the server task
  // has finished whatever handler is in flight, so after this no serving flag is held, no read fd
  // is open on the card, and nothing can post a confirm the writer will never drain.
  this->collection_server_.stop();
#endif
  if (this->writer_task_ == nullptr && this->fd_ < 0)
    return;
  this->request_emergency_close("clean");
  for (uint8_t i = 0; i < 20 && this->writer_task_ != nullptr; i++)
    vTaskDelay(pdMS_TO_TICKS(10));
}

void SdLogger::emergency_close_() {
  // Best-effort clean close on a sagging supply — or an orderly one on
  // shutdown, which takes the same path with a different `#close` reason.
  // Every step is bounded by the VFS timeouts; the hold-up cap (spec §7 H6)
  // must cover this window.
  const char *reason = this->close_reason_;
  ESP_LOGW(TAG, "closing log (%s): %" PRIu32 " records, %" PRIu32 " dropped", reason, this->records_written_,
           this->dropped_records_.load());
  this->mounted_ = false;  // stop accepting new records
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // **Fence the server before anything else.** `on_shutdown()` stops it up front, but that is only
  // one of the three ways this function is reached: the VCC monitor's `dying_` store and the public
  // `request_emergency_close()` both arrive here with the httpd task still running and still willing
  // to grant transfers — against a card this function is about to unmount and then power off.
  //
  // Closing the gate rather than calling `stop()` here, and `stop()` only at the end: `httpd_stop()`
  // blocks until the handler in flight returns, which on a stalled socket is `send_wait_timeout`
  // (2 s) — spent from the hold-up budget (spec §7 H6), which is sized to finish one write, not to
  // wait out a TCP peer. So the flag fences first (no new transfer, no confirm answered 200 into a
  // queue this task is about to stop draining), the bounded drain inside `unmount_card_()` collects
  // whatever was already in flight, and the blocking part happens once the card is safe.
  {
    IndexLock lock(this->index_mux_);
    this->serving_open_ = false;
  }
#endif
  // The rail is going, or the firmware is: there is no "later" to retry into, and a remount here
  // would spend the hold-up budget (spec §7 H6) on a card it is about to power down anyway.
  this->recovery_.disable();

  // Drain every tail into the file — but only if there is one. Reaching here with no file is
  // normal now (a shutdown during a recovery window), and draining then would push the whole ring
  // through the card-drop counter for nothing. write_record_() will not rotate while dying_ is
  // set, so this cannot end up opening a file it has no time to close.
  const uint64_t now64 = static_cast<uint64_t>(esp_timer_get_time());
  if (this->file_ok_()) {
    // The record ring drains below; the tap rings do not. Their consumer (tap_drain_loop_) stopped
    // feeding the ring when mounted_ went false above, so what reaches the file is everything the
    // drain task got there in time — the few ms of tail still sitting in the tap rings is spent
    // hold-up budget nobody has, and the RX ISR keeps counting it into tap_dropped.
    LogRecord rec;
    while (this->file_ok_() && this->pop_record_(rec))
      this->write_record_(rec, now64);
    TextRecord text;
    while (this->file_ok_() && this->pop_text_(text))
      this->write_text_(text, now64);
    // Markers before the close line: a run that ended while shedding should say
    // so in the file it ended in, not in the next one.
    this->write_drop_markers_(now64);
    this->close_file_(reason);
  }
  this->unmount_card_();
  // Cut card power last, so the card is idle before the rail collapses.
  if (this->card_power_pin_ >= 0)
    gpio_set_level(static_cast<gpio_num_t>(this->card_power_pin_), 0);
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // And now the blocking half, after the card is down and before this task deletes itself. Leaving
  // the server up would leave a device that answers every route from a dead writer: `/sdlog/index`
  // listing chunks off an unmounted card, `/sdlog/f/<name>` reading over a powered-down one, and
  // `POST /sdlog/done` queueing intents nobody will ever drain. The gate above already makes each of
  // those refuse rather than lie; this is what stops them being asked at all, and it frees the
  // server's task, sockets and 4 KB block on the way out.
  //
  // Idempotent and safe against `on_shutdown()` having called it first from the main loop — see
  // `CollectionServer::stop()`, which claims the handle atomically so only one caller tears down.
  this->collection_server_.stop();
#endif
}

void SdLogger::monitor_loop_() {
  uint8_t under = 0;
  for (;;) {
    int raw = 0;
    if (adc_oneshot_read(this->adc_handle_, static_cast<adc_channel_t>(this->vcc_adc_gpio_), &raw) == ESP_OK) {
      const float v_rail = this->vcc_rail_volts_(raw);
      if (v_rail < this->vcc_threshold_v_) {
        if (++under >= VCC_TRIP_SAMPLES && !this->dying_.load(std::memory_order_acquire)) {
          // Say what was measured. The trip is one-way by design, so this line is the only
          // evidence of *why* logging stopped — and when the conversion was wrong, its absence is
          // what let a mis-scaled reading masquerade as a dying supply for two sessions.
          ESP_LOGW(TAG, "VCC sag: rail %.2f V < threshold %.2f V (raw %d, %s) — closing the log", v_rail,
                   this->vcc_threshold_v_, raw, this->adc_cali_ != nullptr ? "calibrated" : "UNCALIBRATED estimate");
          this->dying_.store(true, std::memory_order_release);
          this->monitor_task_ = nullptr;
          vTaskDelete(nullptr);
          return;
        }
      } else {
        under = 0;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(MONITOR_POLL_MS));
  }
}

void SdLogger::loop() {
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  // Before the stats early-return, because `statistics:` is optional and a config without it must
  // still get a server. One branch on a bool per main-loop pass for the life of the run, which is
  // the price of not having a second "has it started yet" state to keep consistent.
  if (this->collection_server_pending_) {
    this->collection_server_pending_ = false;
    this->collection_server_.start(this->collection_port_);
  }
#endif
  if (this->stats_log_interval_ms_ == 0)
    return;
  const uint32_t now = millis();
  if (now - this->last_stats_log_ms_ < this->stats_log_interval_ms_)
    return;
  this->last_stats_log_ms_ = now;
  // Three drop counters, never summed: `dropped` is a producer that found the
  // logger's own ring full, `tap_dropped` is the gateway's RX ISR that found a
  // tap ring full, `text_dropped` is a log line that found the text ring full.
  // They name different bottlenecks and the fixes differ (buffer_depth / SD
  // throughput vs log_tap_queue_depth / drain cadence vs esphome_logs depth).
  // `card_dropped` is the fourth: everything lost while there was no writable file at all. It is
  // the counter that distinguishes "the card is gone" from "the card is too slow", which the
  // other three cannot say — and while it is climbing, `mounted=0` explains why.
  //
  // `index_refused` is not a drop at all and sits next to `file=` for that reason: no record was
  // lost, but that many chunks are on the card outside the index, so retention will never reclaim
  // them and the card fills with the writer reporting a clean run. It is the only counter here that
  // a healthy build fixes from the *config* (`collection: max_chunks:`) rather than from throughput.
#ifdef USE_SD_LOGGER_CAN_TAP
  ESP_LOGI(TAG,
           "records=%" PRIu32 " dropped=%" PRIu32 " tap_dropped=%" PRIu32 " text_dropped=%" PRIu32
           " card_dropped=%" PRIu32 " bytes=%" PRIu32 " file=L%07" PRIu32 " index_refused=%" PRIu32 " mounted=%d",
           this->records_written_, this->dropped_records_.load(), this->get_tap_dropped_records(),
           this->text_dropped_.load(), this->card_dropped_.load(), this->bytes_written_, this->seq_,
           this->index_refused_.load(std::memory_order_relaxed), this->mounted_ ? 1 : 0);
#else
  ESP_LOGI(TAG,
           "records=%" PRIu32 " dropped=%" PRIu32 " text_dropped=%" PRIu32 " card_dropped=%" PRIu32 " bytes=%" PRIu32
           " file=L%07" PRIu32 " index_refused=%" PRIu32 " mounted=%d",
           this->records_written_, this->dropped_records_.load(), this->text_dropped_.load(),
           this->card_dropped_.load(), this->bytes_written_, this->seq_,
           this->index_refused_.load(std::memory_order_relaxed), this->mounted_ ? 1 : 0);
#endif
#ifdef USE_SD_LOGGER_COLLECTION
  // Retention's lifetime loss, on its own line and only once it exists. `#gap` states deltas, so
  // unlike `#drop` it has no lifetime column a reader could fall back on — this is the running total
  // the bench reads while a soak is in flight, and the number the `#gap` lines on the card have to
  // add up to afterwards. It disagreeing with them is a finding.
  const uint32_t discarded = this->discarded_chunks_.load(std::memory_order_relaxed);
  if (discarded > 0) {
    ESP_LOGW(TAG,
             "  retention: %" PRIu32 " never-collected chunk(s) discarded, %" PRIu32
             " MiB (lifetime; the files' `#gap` lines are the in-band record)",
             discarded, this->discarded_kib_.load(std::memory_order_relaxed) / 1024);
  }
#endif
  if (!this->mounted_ && this->recovery_.armed()) {
    ESP_LOGW(TAG, "  no card: recovery attempt %" PRIu32 " pending, next in <=%" PRIu32 " ms",
             this->recovery_.attempts() + 1, this->recovery_.delay_ms());
  } else if (!this->mounted_ && this->recovery_.exhausted()) {
    ESP_LOGE(TAG, "  no card: recovery exhausted after %" PRIu32 " attempts", this->recovery_.attempts());
  }
}

void SdLogger::dump_config() {
  ESP_LOGCONFIG(TAG, "SD logger:");
  ESP_LOGCONFIG(TAG, "  SPI: clk=%d mosi=%d miso=%d cs=%d @ %" PRIu32 " Hz", this->clk_pin_, this->mosi_pin_,
                this->miso_pin_, this->cs_pin_, this->clock_hz_);
  ESP_LOGCONFIG(TAG, "  mount=%s buffer_depth=%" PRIu32 " sync_interval=%" PRIu32 "ms", this->mount_point_.c_str(),
                this->buffer_depth_, this->sync_interval_ms_);
  ESP_LOGCONFIG(TAG, "  max_file_size=%" PRIu32 " B  mounted=%s", this->max_file_size_,
                this->mounted_ ? "yes" : "NO (logging disabled)");
  // Configured values only, never index state: the writer task owns the chunk index and this runs
  // on the main loop.
  if (this->max_file_seconds_ > 0) {
    ESP_LOGCONFIG(TAG, "  max_file_seconds=%" PRIu32 " s (rotation takes whichever bound fires first)",
                  this->max_file_seconds_);
  } else {
    ESP_LOGCONFIG(TAG, "  max_file_seconds: unset (rotation is size-only; a quiet bus never seals)");
  }
#ifdef USE_SD_LOGGER_COLLECTION
  if (this->collection_.enabled()) {
    // The capacity is compile-time (`max_chunks:` emits the define), so this is the only place a
    // running board can be asked what it was built with — and it is the number `index_refused=` in
    // the stats line has to be read against.
    ESP_LOGCONFIG(TAG, "  collection: retention arms at %u%% card fill, index holds %u chunk(s)",
                  this->collection_.retention_percent(), static_cast<unsigned>(SD_LOG_MAX_CHUNKS));
  } else {
    ESP_LOGCONFIG(TAG, "  collection: declared but disabled (the card fills and the writer stops)");
  }
  // Said separately from retention because it is the other half of the block and it fails
  // separately: the card half can be perfectly healthy while nothing can be collected off it.
#ifdef USE_SD_LOGGER_COLLECTION_SERVER
  if (this->collection_server_.running()) {
    ESP_LOGCONFIG(TAG, "  collection: serving on port %u (GET /sdlog/index, /sdlog/f/<name>, /sdlog/status)",
                  this->collection_server_.port());
  } else {
    ESP_LOGCONFIG(TAG, "  collection: port %u was configured but the server is NOT running", this->collection_port_);
  }
#else
  if (this->collection_serve_) {
    ESP_LOGCONFIG(TAG, "  collection: port %u was configured but this build has no server", this->collection_port_);
  } else {
    ESP_LOGCONFIG(TAG, "  collection: serve: false — chunks are collected by taking the card out");
  }
#endif
#endif
  if (this->card_power_pin_ >= 0)
    ESP_LOGCONFIG(TAG, "  card_power_pin=%d", this->card_power_pin_);
  if (this->recovery_.enabled()) {
    ESP_LOGCONFIG(TAG, "  recovery: retry in %" PRIu32 " ms, max_attempts=%" PRIu32 ", power_cycle=%s",
                  this->recovery_.delay_ms(), this->recovery_.attempts_limit(),
                  this->recovery_power_cycle_ ? "yes" : "no");
    if (this->recovery_.in_band_reset()) {
      ESP_LOGCONFIG(TAG,
                    "  recovery: in-band reset from attempt 2, busy budget %" PRIu32 " ms doubling to %" PRIu32 " ms",
                    this->recovery_.busy_timeout_ms(), this->recovery_.max_busy_timeout_ms());
    } else {
      ESP_LOGCONFIG(TAG, "  recovery: in-band reset off (a card wedged mid-write will not come back)");
    }
  } else {
    ESP_LOGCONFIG(TAG, "  recovery: off (a card failure disables logging for the run)");
  }
  if (this->vcc_adc_gpio_ >= 0)
    ESP_LOGCONFIG(TAG, "  vcc_monitor: adc_gpio=%d threshold=%.2fV divider=%.2f (%s)", this->vcc_adc_gpio_,
                  this->vcc_threshold_v_, this->vcc_divider_,
                  this->adc_cali_ != nullptr ? "eFuse-calibrated" : "UNCALIBRATED estimate");
  ESP_LOGCONFIG(TAG, "  format: v%u (L#######.LOG)", SD_LOG_FORMAT_VERSION);
  // The same table each file re-emits as `#src` lines, so the console and the
  // card agree on what a label means.
  for (uint8_t i = 0; i < this->sources_.size(); i++)
    ESP_LOGCONFIG(TAG, "  source %u -> %c,%s", this->sources_.at(i).tag, this->sources_.at(i).kind,
                  this->sources_.at(i).label);
  if (this->text_depth_ > 0)
    ESP_LOGCONFIG(TAG, "  esphome_logs: level<=%u depth=%u", this->log_level_, this->text_depth_);
#ifdef USE_SD_LOGGER_CAN_TAP
  for (uint8_t i = 0; i < this->can_tap_count_; i++)
    ESP_LOGCONFIG(TAG, "  can tap %u -> source %u '%s' (ring %s)", i, this->can_taps_[i].source,
                  this->can_taps_[i].label, this->can_taps_[i].port->log_tap_ring() != nullptr ? "armed" : "NOT ARMED");
#endif
}

}  // namespace sd_logger
}  // namespace esphome
