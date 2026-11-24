// Compile the actual component, protocol, lifecycle and serial code against host boundaries.
#include "harness.h"
#include "esphome/core/log.h"
#include <atomic>
#include <future>
#include <thread>
#include <unordered_map>
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define private public
#define protected public
#include "linbus.h"
#undef protected
#undef private
#include "lin_serial.h"
using namespace esphome::linbus;

namespace {
void reset() {
  linbus_test::installed = linbus_test::fail_write = linbus_test::fail_task = linbus_test::fail_mutex = false;
  linbus_test::param_calls = linbus_test::pin_calls = linbus_test::deletes = 0;
  linbus_test::raw_gpio_writes = 0;
  linbus_test::task_created = {};
  linbus_test::now_us = 1'000'000;
  linbus_test::logs.clear();
}
struct Bus : LinBusComponent {
  ~Bus() { delete this->lin_comm_; }
};
void header(LinProtocolHandler &h, uint8_t id) {
  h.receive_decode(0, 0);
  h.receive_decode(0x55, 0);
  h.receive_decode(h.calculate_pid_(id), 0);
}
void frame(LinProtocolHandler &h, uint8_t id, std::initializer_list<uint8_t> data) {
  header(h, id);
  for (auto b : data)
    h.receive_decode(b, 0);
  std::vector<uint8_t> payload(data);
  h.receive_decode(h.calculate_checksum(id, payload.data(), payload.size()), 0);
  h.on_inter_byte_timeout();
}
}  // namespace

TEST(linbus_uart_owner_refused_before_reconfiguration) {
  reset();
  linbus_test::installed = true;
  {
    LinSerial serial(UART_NUM_1, 4, 5, 19200);
    CHECK_EQ(serial.init(), ESP_ERR_INVALID_STATE);
    CHECK_EQ(linbus_test::param_calls, 0);
    CHECK_EQ(linbus_test::pin_calls, 0);
  }
  CHECK(linbus_test::installed);
  CHECK_EQ(linbus_test::deletes, 0);
}

TEST(linbus_all_matching_frame_triggers_fire_once_and_coalescing_is_per_id) {
  reset();
  Bus bus;
  bus.setup();
  LinFrameTrigger a(&bus, 0x12), b(&bus, 0x12);
  unsigned ac = 0, bc = 0;
  a.callback = [&](auto data, auto len) {
    CHECK_EQ(len, 2);
    CHECK_EQ(data[1], 0x34);
    ++ac;
  };
  b.callback = [&](auto, auto) { ++bc; };
  bus.register_frame_trigger(&a);
  bus.register_frame_trigger(&b);
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  for (unsigned i = 0; i < 3; ++i)
    frame(h, 0x12, {0x12, 0x34});
  bus.loop();
  bus.loop();
  CHECK_EQ(ac, 1);
  CHECK_EQ(bc, 1);
  CHECK_EQ(h.get_statistics().frames_coalesced_total, 2);
  frame(h, 0x12, {0x12, 0x34});
  bus.loop();
  CHECK_EQ(ac, 2);
  CHECK_EQ(bc, 2);
}

TEST(linbus_updated_response_length_decodes_complete_echo) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, true);
  bus.setup();
  bus.update_response(0x12, {1, 2, 3, 4});
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  frame(h, 0x12, {1, 2, 3, 4});
  LinDiscoveredId got{};
  CHECK(bus.lin_comm_->get_response_4id(0x12, &got));
  CHECK_EQ(got.dataLength, 4);
}

TEST(linbus_explicit_length_refuses_incompatible_response_update) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, true);
  bus.add_rx_hint(0x12, 2, -1);
  bus.setup();
  bus.update_response(0x12, {1, 2, 3, 4});
  CHECK_EQ(bus.lin_comm_->responses_[0].dataLength, 3);
  CHECK(bus.warning);
}

TEST(linbus_hints_present_at_worker_start) {
  reset();
  Bus bus;
  bus.add_rx_hint(0x12, 4, 0);
  linbus_test::task_created = [&](void *arg) {
    auto &h = *static_cast<LINCommunication *>(arg)->lin_protocol_handler_;
    CHECK_EQ(h.expected_frame_bytes_(0x12), 5);
    CHECK_EQ(h.rx_checksum_mode_[0x12], 0);
  };
  bus.setup();
  linbus_test::task_created = {};
}

TEST(linbus_mutex_failure_marks_setup_failed_without_uart) {
  reset();
  linbus_test::fail_mutex = true;
  Bus bus;
  bus.setup();
  CHECK(bus.is_failed());
  CHECK(!bus.failure_reason.empty());
  CHECK(!linbus_test::installed);
  linbus_test::fail_mutex = false;
}

TEST(linbus_failed_response_has_no_answer_or_echo_and_warns_in_loop) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, true);
  bus.setup();
  linbus_test::fail_write = true;
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  h.on_id(0x12);
  CHECK_EQ(h.get_statistics().id_requests_answered_total, 0);
  CHECK_EQ(h.get_statistics().commander_send_failures_total, 1);
  CHECK_EQ(h.echo_count_, 0);
  bus.loop();
  CHECK(bus.warning);
}

TEST(linbus_cs_inversion_survives_setup) {
  reset();
  esphome::InternalGPIOPin cs;
  cs.inverted = true;
  Bus bus;
  bus.set_cs_pin(&cs);
  bus.setup();
  CHECK(!cs.physical);
  // Raw GPIO writes would bypass the GPIOPin inversion even though its cached value looks correct.
  CHECK_EQ(linbus_test::raw_gpio_writes, 0);
}

TEST(linbus_listener_error_rate_uses_observed_frames) {
  reset();
  LINCommunication comm(UART_NUM_1, 4, 5);
  CHECK_EQ(comm.init(LIN_MODE_LISTENER, 19200), ESP_OK);
  auto &h = *comm.lin_protocol_handler_;
  frame(h, 0x12, {1, 2});
  uint8_t bad[] = {1, 2, 0};
  h.on_data(0x12, bad, 3);
  linbus_test::now_us += 10'000'000;
  h.update_statistics();
  CHECK_EQ(h.get_statistics().error_rate_percent, 50.0f);
}

TEST(linbus_bus_load_counts_foreign_header_and_own_echo_once) {
  reset();
  for (auto mode : {LIN_MODE_LISTENER, LIN_MODE_RESPONDER, LIN_MODE_COMMANDER}) {
    LINCommunication comm(UART_NUM_1, 4, 5);
    CHECK_EQ(comm.init(mode, 19200), ESP_OK);
    uint8_t data[] = {1, 2};
    if (mode != LIN_MODE_LISTENER)
      CHECK(comm.set_response_data(0x12, data, 2));
    auto &h = *comm.lin_protocol_handler_;
    if (mode == LIN_MODE_COMMANDER) {
      CHECK(comm.commander_add_id_schedule(0x12, 100));
      h.commander_send();
    }
    frame(h, 0x12, {1, 2});
    CHECK_EQ(h.get_statistics().total_bus_bytes, 6);
  }
}

TEST(linbus_contended_worker_faults_are_retained_exactly_once) {
  reset();
  LINCommunication comm(UART_NUM_1, 4, 5);
  CHECK_EQ(comm.init(LIN_MODE_LISTENER, 19200), ESP_OK);
  auto &h = *comm.lin_protocol_handler_;
  std::promise<void> locked, release;
  auto ready = release.get_future();
  std::thread holder([&] {
    xSemaphoreTake(comm.data_mutex_, portMAX_DELAY);
    locked.set_value();
    ready.wait();
    xSemaphoreGive(comm.data_mutex_);
  });
  locked.get_future().wait();
  h.echo_count_collision_();
  h.on_frame_error();
  h.on_uart_break();
  h.note_coalesced(7);
  h.receive_decode(0x55, 0);
  h.receive_decode(0x12, 0);  // bad PID parity
  release.set_value();
  holder.join();
  const auto stats = h.get_statistics();
  CHECK_EQ(stats.collisions_total, 1);
  CHECK_EQ(stats.frame_errors_total, 1);
  CHECK_EQ(stats.uart_breaks_total, 1);
  CHECK_EQ(stats.pid_errors_total, 1);
  CHECK_EQ(stats.frames_coalesced_total, 7);
  CHECK_EQ(h.get_statistics().pid_errors_total, 1);
}

TEST(linbus_task_failure_unwinds_resources_and_retains_reason) {
  reset();
  linbus_test::fail_task = true;
  Bus bus;
  bus.setup();
  CHECK(bus.is_failed());
  CHECK(!bus.failure_reason.empty());
  CHECK(!linbus_test::installed);
  CHECK_EQ(linbus_test::deletes, 1);
  CHECK(bus.lin_comm_ == nullptr);
}

TEST(linbus_response_update_and_header_snapshot_share_tx_lock) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, true);
  bus.setup();
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  std::promise<void> reached_lock, allow_lock;
  auto allowed = allow_lock.get_future();
  const auto main_thread = std::this_thread::get_id();
  linbus_test::before_take = [&] {
    if (std::this_thread::get_id() != main_thread) {
      reached_lock.set_value();
      allowed.wait();
    }
  };
  std::thread worker([&] { header(h, 0x12); });
  reached_lock.get_future().wait();
  bus.update_response(0x12, {1, 2, 3, 4});
  allow_lock.set_value();
  worker.join();
  linbus_test::before_take = {};
  // The response actually sent was the new four-byte one, so its checksum
  // must be the fifth DATA byte, irrespective of when the header arrived.
  CHECK_EQ(h.current_frame_bytes_, 5);
}

TEST(linbus_response_length_change_keeps_inflight_frame_boundary) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, true);
  bus.setup();
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  header(h, 0x12);
  // The two-byte response has been sent. Updating the next response cannot
  // change how the queued echo of this one is finalized.
  bus.update_response(0x12, {1, 2, 3, 4});
  uint8_t old[] = {1, 2};
  h.receive_decode(1, 0);
  h.receive_decode(2, 0);
  h.receive_decode(h.calculate_checksum(0x12, old, 2), 0);
  CHECK_EQ(h.state_, READ_STATE_BREAK);
  CHECK_EQ(h.get_statistics().frames_received_total, 1);
  frame(h, 0x12, {1, 2, 3, 4});
  CHECK_EQ(h.get_statistics().frames_received_total, 2);
}

TEST(linbus_response_clear_removes_derived_length_but_keeps_explicit_hint) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, false);
  bus.add_response(0x13, {1, 2}, true);
  bus.add_rx_hint(0x13, 2, -1);
  bus.setup();
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  bus.update_response(0x12, {3, 4});
  CHECK(!bus.lin_comm_->responses_[0].enhancedChecksum);
  bus.update_response(0x12, {});
  bus.update_response(0x13, {});
  CHECK_EQ(h.expected_frame_bytes_(0x12), -1);
  CHECK_EQ(h.expected_frame_bytes_(0x13), 3);
  CHECK(!bus.warning);
}

TEST(linbus_late_hint_is_refused_and_initial_tables_precede_worker) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, true);
  bus.add_schedule_item(0x12, 777);
  linbus_test::task_created = [&](void *arg) {
    const auto &comm = *static_cast<LINCommunication *>(arg);
    CHECK_EQ(comm.responses_[0].dataLength, 3);
    CHECK_EQ(comm.schedule_[0].updateInterval, 777);
  };
  bus.setup();
  linbus_test::task_created = {};
  bus.lin_comm_->configure_rx_hint(0x12, 4, 0);
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  CHECK_EQ(h.expected_frame_bytes_(0x12), 3);
  CHECK_EQ(h.rx_checksum_mode_[0x12], 1);
}

TEST(linbus_fault_event_rate_includes_all_wire_faults_but_not_fallback_or_coalescing) {
  reset();
  LINCommunication comm(UART_NUM_1, 4, 5);
  CHECK_EQ(comm.init(LIN_MODE_LISTENER, 19200), ESP_OK);
  auto &h = *comm.lin_protocol_handler_;
  frame(h, 0x12, {1, 2});
  h.echo_count_collision_();
  h.on_frame_error();
  header(h, 0x12);
  h.on_frame_error();  // discard partial frame
  uint8_t bad[] = {1, 2, 0};
  h.on_data(0x12, bad, 3);
  h.note_coalesced(100);
  linbus_test::now_us += 10'000'000;
  h.update_statistics();
  CHECK_EQ(h.get_statistics().error_rate_percent, 80.0f);
}

TEST(linbus_contended_fault_deltas_survive_repeated_statistics_windows) {
  reset();
  LINCommunication comm(UART_NUM_1, 4, 5);
  CHECK_EQ(comm.init(LIN_MODE_LISTENER, 19200), ESP_OK);
  auto &h = *comm.lin_protocol_handler_;
  std::atomic<bool> finished{false};
  std::thread worker([&] {
    for (unsigned i = 0; i < 10000; ++i) {
      h.on_frame_error();
      h.echo_count_collision_();
    }
    finished.store(true);
  });
  while (!finished.load()) {
    const auto snapshot = h.get_statistics();
    CHECK(snapshot.frame_errors_total <= 10000);
    CHECK(snapshot.collisions_total <= 10000);
  }
  worker.join();
  linbus_test::now_us += 10'000'000;
  h.update_statistics();
  CHECK_EQ(h.get_statistics().frame_errors_total, 10000);
  CHECK_EQ(h.get_statistics().collisions_total, 10000);
  CHECK_EQ(h.get_statistics().frame_errors, 0);
}

TEST(linbus_self_test_restores_derived_lengths_after_temporary_action) {
  reset();
  Bus bus;
  bus.add_response(0x12, {1, 2}, false);
  bus.setup();
  bus.run_self_test(true);
  bus.loop();
  bus.update_response(0x12, {1, 2, 3, 4});
  bus.run_self_test(false);
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  CHECK_EQ(h.expected_frame_bytes_(0x12), 3);
  CHECK_EQ(bus.lin_comm_->responses_[0].dataLength, 3);
  CHECK(!bus.lin_comm_->responses_[0].enhancedChecksum);
  // Self-test-only IDs retain their original unknown-length timeout behavior.
  CHECK_EQ(h.expected_frame_bytes_(0x01), -1);
}

TEST(linbus_self_test_preserves_initial_classic_rx_preference) {
  reset();
  Bus bus;
  bus.add_response(0x00, {1, 2}, false);
  bus.setup();
  bus.run_self_test(true);
  bus.loop();
  auto &h = *bus.lin_comm_->lin_protocol_handler_;
  CHECK_EQ(h.rx_checksum_mode_[0], 0);
  const auto &response = bus.lin_comm_->responses_[0];
  h.on_data(response.id, response.responseData, response.dataLength);
  CHECK_EQ(h.get_statistics().checksum_fallback_total, 1);
}
