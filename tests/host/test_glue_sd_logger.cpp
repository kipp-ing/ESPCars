// Isolated boundary fakes; the generated includes are production method bodies.
#include "harness.h"
#include "collection_policy.h"
#include "log_record.h"
#include "sd_diagnostics.h"
#include "recovery_policy.h"
#include "tap_translate.h"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cinttypes>
#include <cerrno>

using namespace esphome::sd_logger;
#define USE_SWITCH
#define USE_SD_LOGGER_CAN_TAP
#define USE_SD_LOGGER_COLLECTION_SERVER
static constexpr const char *TAG = "sd_logger";
static std::vector<std::string> logs;
template<typename... Ts> static void fake_log(const char *, const char *fmt, const Ts &...) { logs.emplace_back(fmt); }
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define LOG_STR(s) s
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
#define taskENTER_CRITICAL(m) (m)->lock()
#define taskEXIT_CRITICAL(m) (m)->unlock()
constexpr int ESP_OK = 0, pdTRUE = 1, pdPASS = 1, portMAX_DELAY = -1, tskNO_AFFINITY = 0;
constexpr uint32_t UNMOUNT_DRAIN_MS = 2500;
constexpr size_t FAT_PARTITION_COUNT = 4;
constexpr size_t FAT_PARTITION_TABLE_OFFSET = 446, FAT_PARTITION_ENTRY_BYTES = 16;
using esp_err_t = int;
using spi_host_device_t = int;
constexpr int SDSPI_DEFAULT_HOST = 0, MALLOC_CAP_DEFAULT = 0, MALLOC_CAP_DMA = 1;
using TaskHandle_t = void *;
struct sdmmc_card_t {
  struct {
    uint32_t sector_size = 512, capacity = 10000;
  } csd;
};
struct RawFat32Geometry {
  uint32_t volume_lba;
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t fat_count;
  uint32_t fat_sectors, root_cluster, fat_lba, data_lba;
};
struct TextRecord {
  uint32_t t_us = 0;
};
struct ConfirmIntent {
  uint32_t seq;
  uint32_t mount_generation;
};
struct Queue {
  std::deque<ConfirmIntent> values;
};
using QueueHandle_t = Queue *;
struct IndexLock {
  explicit IndexLock(std::mutex *m) : m(m) { m->lock(); }
  ~IndexLock() { m->unlock(); }
  std::mutex *m;
};
static std::atomic<uint64_t> timer_us{0};
static uint64_t esp_timer_get_time() { return timer_us.load(); }
static uint32_t millis() { return timer_us / 1000; }
static int pdMS_TO_TICKS(int ms) { return ms; }
static std::function<void()> delay_hook, wait_hook, trace_hook, notify_hook, create_hook;
static int create_result = 0;
static std::atomic<int> notifications{0};
static void xTaskNotifyGive(TaskHandle_t) {
  notifications++;
  if (notify_hook)
    notify_hook();
}
static void vTaskDelay(int) {
  if (delay_hook)
    delay_hook();
}
static void ulTaskNotifyTake(int, int) {
  if (wait_hook)
    wait_hook();
}
static int xQueueSend(Queue *q, const ConfirmIntent *v, int) {
  q->values.push_back(*v);
  return pdTRUE;
}
static int xQueuePeek(Queue *q, ConfirmIntent *v, int) {
  if (q->values.empty())
    return 0;
  *v = q->values.front();
  return pdTRUE;
}
static int xQueueReceive(Queue *q, ConfirmIntent *v, int n) {
  if (!xQueuePeek(q, v, n))
    return 0;
  q->values.pop_front();
  return pdTRUE;
}
static int xQueueReset(Queue *q) {
  q->values.clear();
  return pdTRUE;
}
static int xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *out, int) {
  if (create_result == pdPASS)
    *out = reinterpret_cast<void *>(2);
  if (create_hook)
    create_hook();
  return create_result;
}
static int closed = 0, unmounted = 0;
static int fake_close(int) {
  closed++;
  return 0;
}
#define close fake_close
static int esp_vfs_fat_sdcard_unmount(const char *, sdmmc_card_t *) {
  unmounted++;
  return ESP_OK;
}
static void spi_bus_free(int) {}
static void vTaskDelete(void *) {}
constexpr uint32_t TAP_DRAIN_POLL_MS = 5;
static const char *esp_err_to_name(int) { return "fake"; }
static size_t heap_caps_get_free_size(int) { return 100; }
static size_t heap_caps_get_largest_free_block(int) { return 100; }
struct {
  SdSpiTraceSummary summarize(uint32_t, uint32_t) {
    if (trace_hook)
      trace_hook();
    return {};
  }
} g_sdspi_trace;
struct SdSpiState {
  static std::atomic<bool> idle_bit_override;
};
std::atomic<bool> SdSpiState::idle_bit_override{false};
static uint8_t sectors[3][512];
static std::vector<uint32_t> sector_reads;
static int sdmmc_read_sectors(sdmmc_card_t *, void *out, uint32_t lba, int) {
  sector_reads.push_back(lba);
  if (lba >= 3)
    return -1;
  std::memcpy(out, sectors[lba], 512);
  return ESP_OK;
}
struct FreezeSwitch {
  int calls = 0;
  void publish_frozen(bool) { calls++; }
};
struct Resettable {
  void reset() {}
  uint32_t count() { return 0; }
};
namespace can_gateway = esphome::can_gateway;
struct TapPort {
  can_gateway::ObserveRing<8, can_gateway::TapRecord> rx, tx;
  auto *log_tap_ring() { return &rx; }
  auto *log_tap_tx_ring() { return &tx; }
};
class SdLogger {
 public:
#include "sd_logger_getters.inc"
  void unmount_card_();
  void publish_debug_writer_frozen_(bool);
  void debug_freeze_writer_();
  void notify_writer_();
  void finish_writer_();
  void on_shutdown();
  bool teardown();
  void request_emergency_close(const char *reason = "emergency");
  void drain_confirms_();
  void loop();
  void setup_writer();
  void restart_writer();
  void start_monitor();
  void tap_drain_loop_();
  bool collection_raw_read_sector(uint32_t, uint8_t *, char *, size_t);
  bool collection_transfer_cancelled() const { return transfers_cancelled_.load(); }
  struct Tap {
    TapPort *port;
    uint8_t source = 1;
    uint64_t rx_observed_us = 0, tx_observed_us = 0;
    std::atomic<uint32_t> drained{0}, record_ring_accepted{0};
  };
  TapPort port;
  Tap can_taps_[1]{{&port}};
  uint8_t can_tap_count_ = 1;
  TaskHandle_t tap_drain_task_ = nullptr;
  uint32_t mount_generation_ = 0;
  void header_baselines();
  void publish_runtime_status_();
  bool push_record(const LogRecord &);
  bool pop_record_(LogRecord &);
  bool pop_text_(TextRecord &);
  void capture_write_failure_(int32_t, int, uint32_t, uint32_t, uint32_t, int, uint32_t, int64_t, uint32_t, uint32_t);
  enum class ConfirmResult { BUSY, NOT_TRACKED, IS_OPEN, CONFIRMED };
  ConfirmResult collection_confirm(uint32_t);
  bool file_ok_() { return fd_ >= 0; }
  bool close_file_(const char *) {
    fd_ = -1;
    return true;
  }
  bool open_next_file_() {
    reopens++;
    fd_ = 10;
    return true;
  }
  void enter_failed_(const char *, WriteFailureAction = WriteFailureAction::RETRY_RECOVERY);
  void drop_unflushed_gap_() {}
  static void writer_trampoline(void *) {}
  static void monitor_trampoline(void *) {}
  void mark_failed(const char *) { failed = true; }
  void status_set_warning() { warning = true; }
  void status_clear_warning() { warning = false; }
  void enable_loop_soon_any_context() {}
  std::mutex index_mutex, ring_mux_, text_mux_, writer_mux_;
  mutable std::mutex status_mux_;
  std::mutex *index_mux_ = &index_mutex;
  CollectionPolicy collection_;
  Queue queue;
  Queue *confirm_q_ = &queue;
  std::atomic<uint32_t> transfers_in_flight_{0};
  std::atomic<bool> transfers_cancelled_{false};
  bool serving_open_ = true;
  int fd_ = 10;
  sdmmc_card_t card;
  sdmmc_card_t *card_ = &card;
  std::string mount_point_{"sdlog-host-missing"};
  Resettable block_;
  bool last_unmount_skipped_ = false, spi_bus_ok_ = true, degraded_identification_ = false;
  int32_t last_unmount_err_ = 0;
  uint32_t unmounts_ok_ = 0, last_sync_ms_ = 0;
  std::atomic<uint32_t> seq_{0};
  std::atomic<TaskHandle_t> writer_task_{reinterpret_cast<void *>(1)};
  TaskHandle_t monitor_task_ = nullptr;
  std::atomic<bool> dying_{false}, confine_format_stop_requested_{false}, confine_format_in_progress_{false};
  std::atomic<bool> debug_writer_freeze_requested_{false}, debug_writer_frozen_{false},
      debug_writer_frozen_pending_{false};
  std::atomic<bool> writer_stopped_{false};
  bool debug_reopen_after_freeze_ = false;
  FreezeSwitch sw;
  FreezeSwitch *debug_writer_freeze_switch_ = &sw;
  std::atomic<const char *> close_reason_{"emergency"};
  std::atomic<bool> mounted_{true};
  bool failed = false, warning = false;
  int reopens = 0;
  struct {
    void stop() {}
    void start(int) {}
  } collection_server_;
  bool collection_server_pending_ = false;
  uint16_t collection_port_ = 0;
  LogRecord records[4];
  LogRecord *ring_ = records;
  std::atomic<uint32_t> head_{0}, tail_{0};
  uint32_t ring_mask_ = 3, buffer_depth_ = 4;
  TextRecord texts[4];
  TextRecord *text_ring_ = texts;
  uint16_t text_head_ = 0, text_tail_ = 0, text_mask_ = 3;
  static constexpr uint64_t QUEUE_MAX_AGE_US = 600000000ULL;
  uint64_t ring_observed_us_ = 0, text_observed_us_ = 0, record_sample_us_ = 0, text_sample_us_ = 0;
  bool is_debug_writer_frozen() const { return debug_writer_frozen_.load(); }
  bool vcc_monitor_unavailable_ = false;
  RecoveryPolicy recovery_;
  bool write_recovery_stopped_ = false;
  std::atomic<uint32_t> recovery_attempts_pub_{0}, recovery_delay_pub_{0}, recovery_busy_timeout_pub_{0};
  std::atomic<bool> recovery_enabled_pub_{false};
  std::atomic<const char *> recovery_state_pub_{"off"};
  Resettable unflushed_records_;
  std::atomic<uint32_t> write_lost_{0};
  std::atomic<uint32_t> dropped_records_{0}, card_dropped_{0}, text_dropped_{0};
  uint32_t marked_text_dropped_ = 0;
  uint32_t mounted_card_sectors_ = 0, mounted_volume_first_lba_ = 0, mounted_volume_sectors_ = 0;
  LastWriteFailure last_write_failure_{};
  WriteFailureSnapshot write_failure_pub_{};
};
#include "sd_logger_runtime.inc"
#undef close

static void reset_fakes() {
  delay_hook = {};
  wait_hook = {};
  trace_hook = {};
  notify_hook = {};
  create_hook = {};
  create_result = 0;
  timer_us = 0;
  closed = unmounted = 0;
  notifications = 0;
  logs.clear();
  sector_reads.clear();
}

TEST(sdc1_reader_outlives_drain_deadline) {
  reset_fakes();
  SdLogger log;
  log.transfers_in_flight_ = 1;
  std::atomic<int> ticks{0};
  std::atomic<bool> released{false};
  std::thread reader([&] {
    while (ticks < 300)
      std::this_thread::yield();
    released = true;
    log.transfers_in_flight_ = 0;
  });
  delay_hook = [&] {
    ticks++;
    std::this_thread::yield();
  };
  log.unmount_card_();
  bool safe = released.load();
  ticks = 300;
  reader.join();
  CHECK(safe);
  CHECK(unmounted == 1);
}
TEST(sdc2_remount_drops_overlapping_confirm) {
  reset_fakes();
  SdLogger log;
  log.queue.values.push_back({7, 0});
  log.unmount_card_();
  CHECK(log.queue.values.empty());
}
TEST(sdc3_teardown_waits_for_close) {
  reset_fakes();
  SdLogger log;
  delay_hook = [] {};
  log.on_shutdown();
  CHECK(notifications > 0);
  CHECK(!log.teardown());
  log.writer_task_ = nullptr;
  log.writer_stopped_ = true;
  CHECK(log.teardown());
}
TEST(sdc4_freeze_callbacks_on_main_loop) {
  reset_fakes();
  SdLogger log;
  std::thread writer([&] { log.publish_debug_writer_frozen_(true); });
  writer.join();
  CHECK(log.sw.calls == 0);
  log.loop();
  CHECK(log.sw.calls == 1);
}
TEST(sdc5_format_exits_frozen_wait_without_reopen) {
  reset_fakes();
  SdLogger log;
  log.debug_writer_freeze_requested_ = true;
  int waits = 0;
  wait_hook = [&] {
    log.confine_format_stop_requested_ = true;
    if (++waits > 1)
      log.dying_ = true;
  };
  log.debug_freeze_writer_();
  CHECK(waits == 1);
  CHECK(log.reopens == 0);
}
TEST(sdc5_emergency_notifies_frozen_writer) {
  reset_fakes();
  SdLogger log;
  log.request_emergency_close();
  CHECK(notifications == 1);
}
static void partition_fixture() {
  std::memset(sectors, 0, sizeof(sectors));
  for (int i = 0; i < 2; i++) {
    sectors[0][446 + i * 16 + 4] = 0x0b;
    sectors[0][446 + i * 16 + 8] = i + 1;
  }
  auto *b = sectors[2];
  b[510] = 0x55;
  b[511] = 0xaa;
  b[12] = 2;
  b[13] = 8;
  b[14] = 32;
  b[16] = 2;
  b[36] = 10;
  b[44] = 2;
  b[32] = 100;
}
TEST(sdc6_partition_probe_preserves_mbr) {
  reset_fakes();
  partition_fixture();
  sdmmc_card_t card;
  uint32_t lba = 0, count = 0;
  CHECK(mounted_volume_bounds(&card, &lba, &count));
  CHECK(lba == 2);
  RawFat32Geometry g{};
  char error[128];
  CHECK(load_raw_fat32_geometry(&card, &g, error, sizeof(error)));
  CHECK(g.volume_lba == 2);
  // Geometry logger must also reach the second partition (the read fake is observed below).
  sector_reads.clear();
  log_fat_geometry(&card);
  CHECK(sector_reads == std::vector<uint32_t>({0, 1, 2}));
}
TEST(sdc7_writer_creation_failure_releases_card) {
  reset_fakes();
  SdLogger log;
  log.setup_writer();
  CHECK(!log.mounted_);
  CHECK(log.fd_ < 0);
  CHECK(log.failed);
  SdLogger restart;
  restart.confine_format_in_progress_ = true;
  restart.restart_writer();
  CHECK(restart.warning);
  CHECK(!restart.confine_format_in_progress_);
  CHECK(!restart.push_record(LogRecord{}));
  CHECK(restart.head_ == 0);
  CHECK(!restart.mounted_);
  CHECK(restart.fd_ < 0);
}
TEST(sdc10_recovery_and_freeze_expire_ambiguous_epoch) {
  reset_fakes();
  SdLogger log;
  LogRecord rec{};
  rec.t_us = 100;
  log.push_record(rec);
  timer_us = (1ULL << 32) + 200;
  LogRecord out{};
  CHECK(!log.pop_record_(out));
  CHECK(log.card_dropped_ == 1);
  log.text_head_ = 1;
  TextRecord text;
  CHECK(!log.pop_text_(text));
  CHECK(log.card_dropped_ == 2);
}
TEST(sdc11_rotation_preserves_text_loss) {
  reset_fakes();
  SdLogger log;
  log.text_dropped_ = 3;
  log.header_baselines();
  CHECK(log.marked_text_dropped_ == 0);
}
TEST(sdc12_monitor_task_creation_failure_visible) {
  reset_fakes();
  SdLogger log;
  log.start_monitor();
  CHECK(log.warning);
  CHECK(log.monitor_task_ == nullptr);
}
TEST(sdc13_failure_count_and_facts_publish_together) {
  reset_fakes();
  SdLogger log;
  bool consistent = false;
  // This executes after fact collection starts, before publication; a real reader thread checks it.
  trace_hook = [&] {
    std::thread http([&] { consistent = log.get_write_failures() == 0; });
    http.join();
  };
  log.capture_write_failure_(-1, EIO, 512, 123, 123, 0, 0, 100, 0, 1);
  CHECK(consistent);
  CHECK(log.get_write_failures() == 1);
  CHECK(log.get_last_write_offset() == 123);
}
TEST(sdc14_card_warning_on_loop_and_recovery) {
  reset_fakes();
  SdLogger log;
  log.enter_failed_("injected");
  log.loop();
  CHECK(log.warning);
  log.mounted_ = true;
  log.loop();
  CHECK(!log.warning);
}

TEST(sdc1_raw_transfer_cancels_before_next_card_read) {
  reset_fakes();
  SdLogger log;
  log.transfers_cancelled_ = true;
  uint8_t out[512];
  char error[128];
  CHECK(!log.collection_raw_read_sector(0, out, error, sizeof(error)));
  CHECK(sector_reads.empty());
}
TEST(sdc2_old_generation_cannot_confirm_replacement_card) {
  reset_fakes();
  SdLogger log;
  log.collection_.add(7, 512, ChunkState::SEALED);
  log.queue.values.push_back({7, 0});
  log.mount_generation_ = 1;
  log.drain_confirms_();
  CHECK(log.collection_.find(7)->state == ChunkState::SEALED);
  CHECK(log.queue.values.empty());
}
TEST(sdc10_tap_queues_expire_while_unmounted) {
  reset_fakes();
  SdLogger log;
  log.mounted_ = false;
  can_gateway::TapRecord tap{};
  log.port.rx.push(tap);
  log.port.tx.push(tap);
  timer_us = (1ULL << 32) + 10;
  delay_hook = [&] { log.dying_ = true; };
  std::thread drain([&] { log.tap_drain_loop_(); });
  drain.join();
  CHECK(log.port.rx.empty());
  CHECK(log.port.tx.empty());
  CHECK(log.card_dropped_ == 2);
}
TEST(sdc10_recent_queue_keeps_wrap_epoch) {
  reset_fakes();
  SdLogger log;
  timer_us = (1ULL << 32) - 10;
  LogRecord rec{};
  rec.t_us = static_cast<uint32_t>(timer_us.load());
  CHECK(log.push_record(rec));
  timer_us = (1ULL << 32) + 10;
  LogRecord out{};
  CHECK(log.pop_record_(out));
  CHECK(reconstruct_us(log.record_sample_us_, out.t_us) == (1ULL << 32) - 10);
}
TEST(sdc12_monitor_warning_survives_healthy_card) {
  reset_fakes();
  SdLogger log;
  log.start_monitor();
  log.loop();
  CHECK(log.warning);
}
TEST(sdc13_http_snapshots_remain_coherent_during_writes) {
  reset_fakes();
  SdLogger log;
  std::atomic<bool> done{false}, coherent{true};
  std::thread writer([&] {
    for (uint32_t i = 1; i <= 10000; i++)
      log.capture_write_failure_(-1, EIO, 512, i, i, 0, 0, 1, 0, 0);
    done = true;
  });
  do {
    const auto snapshot = log.get_write_failure_snapshot();
    if (snapshot.count != snapshot.facts.offset)
      coherent = false;
  } while (!done);
  writer.join();
  CHECK(coherent);
  CHECK(log.get_write_failures() == 10000);
}

TEST(sdc3_task_retirement_cannot_overlap_notification) {
  reset_fakes();
  SdLogger log;
  std::mutex mutex;
  std::condition_variable cv;
  bool notifying = false, release = false;
  notify_hook = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    notifying = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
  };
  std::thread requester([&] { log.request_emergency_close(); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return notifying; });
  }
  const bool available = log.writer_mux_.try_lock();
  CHECK(!available);
  if (available)
    log.writer_mux_.unlock();
  std::thread retiring([&] { log.finish_writer_(); });
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  requester.join();
  retiring.join();
  log.request_emergency_close();
  CHECK(notifications == 1);
  CHECK(log.teardown());
}

TEST(sdc3_completed_task_handle_is_not_republished_at_creation) {
  reset_fakes();
  SdLogger log;
  create_result = pdPASS;
  create_hook = [&] { log.finish_writer_(); };
  log.setup_writer();
  CHECK(log.writer_task_ == nullptr);
  CHECK(log.teardown());
  log.request_emergency_close();
  CHECK(notifications == 0);
}

TEST(sdc13_ring_occupancy_reads_during_production) {
  reset_fakes();
  SdLogger log;
  std::atomic<bool> done{false}, valid{true};
  std::thread producer([&] {
    for (int i = 0; i < 10000; i++)
      log.push_record(LogRecord{});
    done = true;
  });
  LogRecord rec{};
  do {
    if (log.ring_fill_percent() > 100)
      valid = false;
    log.pop_record_(rec);
  } while (!done);
  producer.join();
  CHECK(valid);
}

TEST(sdc10_record_stream_stays_nonempty_without_expiry) {
  reset_fakes();
  SdLogger log;
  CHECK(log.push_record(LogRecord{}));
  CHECK(log.push_record(LogRecord{}));
  for (uint32_t step = 1; step <= 12; step++) {
    timer_us = step * (SdLogger::QUEUE_MAX_AGE_US / 4);
    LogRecord record{};
    CHECK(log.pop_record_(record));
    CHECK(log.head_ != log.tail_);
    record.t_us = static_cast<uint32_t>(timer_us.load());
    CHECK(log.push_record(record));
  }
  CHECK(log.card_dropped_ == 0);
  CHECK(log.dropped_records_ == 0);
  CHECK(logs.empty());
}

TEST(sdc10_text_stream_stays_nonempty_without_expiry) {
  reset_fakes();
  SdLogger log;
  // Controlled text producer: keep two slots queued, so every real pop leaves one pending.
  log.text_head_ = 2;
  for (uint32_t step = 1; step <= 12; step++) {
    timer_us = step * (SdLogger::QUEUE_MAX_AGE_US / 4);
    TextRecord text{};
    CHECK(log.pop_text_(text));
    CHECK(log.text_head_ != log.text_tail_);
    log.texts[log.text_head_].t_us = static_cast<uint32_t>(timer_us.load());
    log.text_head_ = (log.text_head_ + 1) & log.text_mask_;
  }
  CHECK(log.card_dropped_ == 0);
  CHECK(logs.empty());
}

static void check_continuous_tap_stream(bool tx) {
  reset_fakes();
  SdLogger log;
  auto *ring = tx ? log.port.log_tap_tx_ring() : log.port.log_tap_ring();
  can_gateway::TapRecord tap{};
  while (ring->size() < ring->capacity())
    CHECK(ring->push(tap));
  uint32_t passes = 0;
  delay_hook = [&] {
    // Backpressure leaves a nonempty tap backlog after each drain pass; the writer makes room
    // between passes and the synthetic producer refills it before the next expiry check.
    CHECK(!ring->empty());
    LogRecord record{};
    while (log.pop_record_(record)) {
    }
    tap.t_us = static_cast<uint32_t>(timer_us.load());
    while (ring->size() < ring->capacity())
      CHECK(ring->push(tap));
    timer_us += SdLogger::QUEUE_MAX_AGE_US / 4;
    if (++passes == 12)
      log.dying_ = true;
  };
  std::thread drain([&] { log.tap_drain_loop_(); });
  drain.join();
  CHECK(passes == 12);
  CHECK(log.can_taps_[0].drained > 0);
  CHECK(log.card_dropped_ == 0);
  CHECK(logs.empty());
}

TEST(sdc10_tap_rx_stream_stays_nonempty_without_expiry) { check_continuous_tap_stream(false); }
TEST(sdc10_tap_tx_stream_stays_nonempty_without_expiry) { check_continuous_tap_stream(true); }
