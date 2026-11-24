// Execute the production communication + protocol sources with recorded UART writes.
// Private access only inspects exact table restoration and drives the event-task sender;
// initialization, actions, installation and verdicts use the production public API.
#include "harness.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include "esphome/core/log.h"
#include "freertos/semphr.h"
#define private public
#include "lin_communication.h"
#undef private
#include "lin_serial.h"

using namespace esphome::linbus;

namespace {
struct Recorder {
  unsigned breaks = 0;
  unsigned writes = 0;
  std::mutex mutex;
  std::condition_variable cv;
  bool block_break = false;
  bool entered_break = false;
  bool release_break = false;
} tx;
constexpr uint8_t CASE_IDS[] = {0x00, 0x01, 0x15, 0x1F, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};

void reset_recorder() {
  tx.breaks = tx.writes = 0;
  tx.block_break = tx.entered_break = tx.release_break = false;
  linbus_test::now_us = 1'000'000;
  linbus_test::logs.clear();
}

struct Tables {
  LinScheduleItem schedule[LIN_MAX_ID_RQ_COUNT];
  LinRespondData responses[LIN_MAX_RESPONSES];
  explicit Tables(const LINCommunication &comm) {
    memcpy(schedule, comm.schedule_, sizeof(schedule));
    memcpy(responses, comm.responses_, sizeof(responses));
  }
  void check(const LINCommunication &comm) const {
    CHECK_BYTES(reinterpret_cast<const uint8_t *>(comm.schedule_), reinterpret_cast<const uint8_t *>(schedule),
                sizeof(schedule));
    CHECK_BYTES(reinterpret_cast<const uint8_t *>(comm.responses_), reinterpret_cast<const uint8_t *>(responses),
                sizeof(responses));
  }
};

void start(LINCommunication &comm) {
  comm.enable_self_test(true);
  comm.lin_process();
}

void drive_all_tx(LINCommunication &comm) {
  // A restored empty schedule must never emit a break, sync or PID, including ID 0.
  for (unsigned i = 0; i < 32; i++) {
    linbus_test::now_us += 1'000'000;
    comm.lin_protocol_handler_->commander_send();
  }
  for (auto id : CASE_IDS)
    comm.lin_protocol_handler_->on_id(id);
}

void complete_installed_cases(LINCommunication &comm) {
  // Feed valid observations twice for whatever actually installed, reproducing the
  // old subset-only PASS even when most required cases were skipped.
  for (unsigned lap = 0; lap < 2; lap++) {
    linbus_test::now_us += 3'000'000;
    for (const auto &response : comm.responses_) {
      if (response.dataLength != 0)
        comm.lin_protocol_handler_->on_data(response.id, response.responseData, response.dataLength);
    }
    comm.lin_process();
  }
}

void seed_user_tables(LINCommunication &comm) {
  CHECK(comm.commander_add_id_schedule(0x00, 777));
  CHECK(comm.commander_add_id_schedule(0x12, 900));
  uint8_t payload[] = {0x12, 0x34, 0x56, 0x78};
  CHECK(comm.set_response_data(0x00, payload, sizeof(payload), false));
  CHECK(comm.set_response_data(0x12, payload, 2));
  // Non-default cadence and metadata must survive, not just IDs/payload/interval.
  comm.schedule_[0].sent_once = true;
  comm.schedule_[0].last_send_time_ms = 123;
  comm.responses_[0].lastResponseTime = 456;
}

bool logged(const char *needle) {
  for (const auto &line : linbus_test::logs)
    if (line.find(needle) != std::string::npos)
      return true;
  return false;
}
}  // namespace

// Replace only physical serial I/O; every schedule lookup and send decision is real.
namespace esphome::linbus {
LinSerial::LinSerial(uart_port_t port, int tx_pin, int rx_pin, int baud)
    : uart_num_(port), tx_pin_(tx_pin), rx_pin_(rx_pin), baud_rate_(baud) {}
LinSerial::~LinSerial() = default;
esp_err_t LinSerial::init() { return ESP_OK; }
bool LinSerial::write_bytes(const uint8_t *, size_t) {
  tx.writes++;
  return true;
}
bool LinSerial::send_break() {
  std::unique_lock<std::mutex> lock(tx.mutex);
  if (tx.block_break) {
    tx.entered_break = true;
    tx.cv.notify_all();
    tx.cv.wait(lock, [] { return tx.release_break; });
  }
  tx.breaks++;
  return true;
}
}  // namespace esphome::linbus

TEST(linbus_stop_self_test_stops_headers_and_responses) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  const Tables before(comm);
  start(comm);
  drive_all_tx(comm);
  CHECK(tx.breaks > 0);
  CHECK(tx.writes > 0);
  comm.enable_self_test(false);
  const auto breaks = tx.breaks;
  const auto writes = tx.writes;
  drive_all_tx(comm);
  CHECK_EQ(tx.breaks, breaks);
  CHECK_EQ(tx.writes, writes);
  before.check(comm);
  CHECK(!comm.self_test_passed());
}

TEST(linbus_stop_self_test_restores_exact_user_tables) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  seed_user_tables(comm);
  const Tables before(comm);
  start(comm);
  drive_all_tx(comm);
  comm.enable_self_test(false);
  before.check(comm);
  // Restored user traffic remains available.
  const auto writes = tx.writes;
  comm.lin_protocol_handler_->on_id(0x00);
  CHECK_EQ(tx.writes, writes + 1);
}

TEST(linbus_passed_self_test_restores_tables_on_stop_and_can_rearm) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  seed_user_tables(comm);
  const Tables before(comm);
  for (unsigned run = 0; run < 2; run++) {
    start(comm);
    CHECK(!comm.self_test_passed());
    complete_installed_cases(comm);
    CHECK(comm.self_test_passed());
    CHECK(comm.self_test_enabled_);
    // Re-enabling an already running, passed test must preserve its latch/snapshot.
    start(comm);
    CHECK(comm.self_test_passed());
    comm.enable_self_test(false);
    CHECK(!comm.self_test_passed());
    before.check(comm);
  }
  comm.enable_self_test(false);
  CHECK(!comm.self_test_passed());
  before.check(comm);
}

TEST(linbus_full_schedule_self_test_never_passes_on_subset) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  CHECK(comm.commander_add_id_schedule(0x00, 777));
  for (uint8_t id = 2; id < 17; id++)
    CHECK(comm.commander_add_id_schedule(id, 888));
  const Tables before(comm);
  start(comm);
  before.check(comm);  // Installation must be all-or-nothing before the worker can TX.
  complete_installed_cases(comm);
  CHECK(!comm.self_test_passed());
  CHECK(logged("INCOMPLETE"));
  CHECK(comm.self_test_incomplete());
  comm.enable_self_test(false);
  before.check(comm);
}

TEST(linbus_full_responses_self_test_is_incomplete_and_restores) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  uint8_t payload[] = {0x42};
  for (uint8_t id = 2; id < 18; id++)
    CHECK(comm.set_response_data(id, payload, sizeof(payload)));
  const Tables before(comm);
  start(comm);
  before.check(comm);
  complete_installed_cases(comm);
  CHECK(!comm.self_test_passed());
  CHECK(logged("INCOMPLETE"));
  CHECK(comm.self_test_incomplete());
  comm.enable_self_test(false);
  before.check(comm);
}

TEST(linbus_stop_waits_for_inflight_send_then_prevents_more_tx) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  const Tables before(comm);
  start(comm);
  tx.block_break = true;
  std::thread worker([&] { comm.lin_protocol_handler_->commander_send(); });
  {
    std::unique_lock<std::mutex> lock(tx.mutex);
    tx.cv.wait(lock, [] { return tx.entered_break; });
  }
  std::promise<void> stopping;
  auto stop = std::async(std::launch::async, [&] {
    stopping.set_value();
    comm.enable_self_test(false);
  });
  stopping.get_future().wait();
  CHECK(stop.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
  {
    std::lock_guard<std::mutex> lock(tx.mutex);
    tx.release_break = true;
    tx.cv.notify_all();
  }
  worker.join();
  stop.get();
  const auto breaks = tx.breaks;
  const auto writes = tx.writes;
  drive_all_tx(comm);
  CHECK_EQ(tx.breaks, breaks);
  CHECK_EQ(tx.writes, writes);
  before.check(comm);
}

TEST(linbus_pass_latches_and_keeps_transmitting_until_stopped) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  const Tables before(comm);
  start(comm);
  complete_installed_cases(comm);
  CHECK(comm.self_test_passed());
  CHECK(comm.self_test_enabled_);
  CHECK(comm.self_test_initialized_);
  // Keep sending throughout the bench's 40-second observation window after PASS.
  // Observe both header TX and response TX, including the diagnostic test IDs.
  for (unsigned second = 0; second < 40; ++second) {
    linbus_test::now_us += 1'000'000;
    const auto breaks = tx.breaks;
    comm.lin_protocol_handler_->commander_send();
    CHECK_EQ(tx.breaks, breaks + 1);
    for (auto id : CASE_IDS) {
      const auto writes = tx.writes;
      comm.lin_protocol_handler_->on_id(id);
      CHECK_EQ(tx.writes, writes + 1);
    }
    comm.lin_process();
    CHECK(comm.self_test_passed());
  }
  comm.enable_self_test(false);
  CHECK(!comm.self_test_passed());
  before.check(comm);
  const auto breaks = tx.breaks;
  const auto writes = tx.writes;
  drive_all_tx(comm);
  CHECK_EQ(tx.breaks, breaks);
  CHECK_EQ(tx.writes, writes);
}

TEST(linbus_repeated_start_preserves_original_snapshot) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  seed_user_tables(comm);
  const Tables before(comm);
  start(comm);
  start(comm);
  comm.enable_self_test(false);
  before.check(comm);
}

TEST(linbus_full_tables_with_all_case_ids_can_run_and_restore) {
  reset_recorder();
  LINCommunication comm(UART_NUM_1, 1, 2);
  CHECK_EQ(comm.init(LIN_MODE_COMMANDER, 19200), ESP_OK);
  uint8_t payload[] = {0x42};
  for (auto id : CASE_IDS) {
    CHECK(comm.commander_add_id_schedule(id, 777));
    CHECK(comm.set_response_data(id, payload, sizeof(payload), false));
  }
  for (uint8_t id = 2; id < 9; id++) {
    CHECK(comm.commander_add_id_schedule(id, 888));
    CHECK(comm.set_response_data(id, payload, sizeof(payload)));
  }
  const Tables before(comm);
  start(comm);
  CHECK(!comm.self_test_incomplete());
  complete_installed_cases(comm);
  CHECK(comm.self_test_passed());
  comm.enable_self_test(false);
  before.check(comm);
}
