#include "lin_communication.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

// Project Headers
#include "lin_serial.h"
#include "lin_protocol_handler.h"

#include <cinttypes>

namespace esphome::linbus {

ESPHOME_LOG_TAG(TAG, "LIN_COMM");

// The former IDF logger compiled non-error logs out (CONFIG_LOG_MAXIMUM_LEVEL=1).
// Keep them at VERBOSE so DEBUG adds no logging work on LIN timing paths.

LINCommunication::LINCommunication(uart_port_t uart_num, int tx_pin, int rx_pin, int cs_pin)
    : uart_num_(uart_num), tx_pin_(tx_pin), rx_pin_(rx_pin), cs_pin_(cs_pin), mode_(LIN_MODE_LISTENER), baud_rate_(0) {
  // Initialize LinSerial (can be nullptr until init)
  lin_serial_ = nullptr;

  // Initialize arrays to zero
  memset(schedule_, 0, sizeof(schedule_));
  memset(responses_, 0, sizeof(responses_));
  memset(discovered_ids_, 0, sizeof(discovered_ids_));
  data_mutex_ = xSemaphoreCreateMutex();
}

LINCommunication::~LINCommunication() {
  // Stop the UART event task BEFORE deleting the objects it dereferences, to avoid a
  // use-after-free on the serial/handler pointers. See UPGRADES.md H8.
  if (uart_event_task_handle_ != nullptr) {
    vTaskDelete(uart_event_task_handle_);
    uart_event_task_handle_ = nullptr;
  }
  delete lin_protocol_handler_;
  lin_protocol_handler_ = nullptr;
  delete lin_serial_;
  lin_serial_ = nullptr;
  if (data_mutex_ != nullptr) {
    vSemaphoreDelete(data_mutex_);
    data_mutex_ = nullptr;
  }
}

esp_err_t LINCommunication::init(LinMode mode, int baud_rate) {
  if (is_initialized_) {
    ESP_LOGV(TAG, "LINCommunication is already initialized. Skipping reinitialization.");
    return ESP_ERR_INVALID_STATE;
  }

  if (baud_rate <= 0) {
    ESP_LOGE(TAG, "Invalid baud rate %d", baud_rate);
    return ESP_ERR_INVALID_ARG;
  }

  mode_ = mode;
  baud_rate_ = baud_rate;

  // Compute the inter-byte timeout BEFORE the event task can run, so the task never
  // latches a 0 -> forced-1-tick queue timeout that chops frames. See UPGRADES.md C1.
  // Approx 3 byte times (10 bits/byte). 3 * 10 * 1e6 / baud
  inter_byte_timeout_us_ = (uint32_t) ((3ULL * 10ULL * 1000000ULL) / (uint32_t) baud_rate_);

  // Initialize LinSerial. A driver-install failure returns an error to mark_failed()
  // instead of aborting into a bootloop. See UPGRADES.md H5.
  lin_serial_ = new LinSerial(uart_num_, tx_pin_, rx_pin_, baud_rate_);
  esp_err_t serr = lin_serial_->init();
  if (serr != ESP_OK) {
    ESP_LOGE(TAG, "LinSerial init failed: %s", esp_err_to_name(serr));
    delete lin_serial_;
    lin_serial_ = nullptr;
    return serr;
  }

  // Now create LinProtocolHandler with valid pointers
  lin_protocol_handler_ =
      new LinProtocolHandler(lin_serial_, baud_rate_, schedule_, discovered_ids_, responses_, mode_, data_mutex_);

  if (cs_pin_ != -1) {
    // Configure CS  pin
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << (gpio_num_t) cs_pin_);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level((gpio_num_t) cs_pin_, 1);
  }

  // LIN at <=20 kbit/s needs low latency, not maximum priority. Running above esp_timer /
  // Wi-Fi / BT (configMAX_PRIORITIES - 1) can starve the system. Use a high-but-bounded
  // priority instead. See UPGRADES.md H9.
  UBaseType_t task_prio = (configMAX_PRIORITIES > 19) ? 18 : (UBaseType_t) (configMAX_PRIORITIES - 2);
  BaseType_t ok = xTaskCreatePinnedToCore(uart_event_task_trampoline, "lin_uart_evt", 4096, this, task_prio,
                                          &uart_event_task_handle_, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create UART event task");
    return ESP_FAIL;
  }

  ESP_LOGV(TAG, "LINCommunication initialized in mode %d at %d baud", mode_, baud_rate_);
  is_initialized_ = true;  // Mark as initialized

  return ESP_OK;
}

void LINCommunication::uart_event_task_trampoline(void *arg) {
  auto *self = reinterpret_cast<LINCommunication *>(arg);
  if (!self || !self->lin_serial_ || !self->lin_protocol_handler_) {
    vTaskDelete(nullptr);
    return;
  }
  QueueHandle_t q = self->lin_serial_->get_event_queue();
  uart_event_t evt;
  uint8_t buf[32];

  // Convert desired microsecond inter-byte timeout to FreeRTOS ticks. An N-tick
  // xQueueReceive guarantees only N-1 *full* tick periods (it can expire at the very next
  // tick interrupt), so floor at 2 ticks. See UPGRADES.md C1.
  TickType_t wait_ticks = pdMS_TO_TICKS((self->inter_byte_timeout_us_ + 999) / 1000);  // round up to ms
  if (wait_ticks < 2)
    wait_ticks = 2;

  // Feed every byte already sitting in the RX ring buffer through the decoder. The UART
  // driver posts BREAK / error events when the condition is detected on the WIRE, while the
  // bytes that precede it may still sit unread in the ring (evt.size batching, boot
  // backlog). Resyncing the decoder on such an event before draining those bytes latches a
  // permanent skew between the event stream and the byte stream — measured on the bench as
  // a listener that reboots onto a busy bus and then decodes ~nothing while uart_breaks
  // keeps climbing (HW-6b). Draining first keeps the decoder aligned; the in-band 0x00
  // break byte then does the actual framing (A2).
  auto drain_rx = [self, &buf]() {
    for (;;) {
      int r = uart_read_bytes(self->uart_num_, buf, sizeof(buf), 0);
      if (r <= 0)
        break;
      for (int i = 0; i < r; ++i)
        self->lin_protocol_handler_->receive_decode(buf[i], 0);
    }
  };

  // Start from a clean slate: bytes that arrived between driver install and this task
  // starting are a mid-stream fragment at best — discard them so the first event the task
  // acts on refers to data it has actually seen (same HW-6b boot-alignment guard).
  uart_flush_input(self->uart_num_);
  xQueueReset(q);

  for (;;) {
    // Wait for UART event OR timeout (idle gap)
    if (xQueueReceive(q, &evt, wait_ticks) == pdTRUE) {
      switch (evt.type) {
        case UART_DATA: {
          size_t remaining = evt.size;
          while (remaining > 0) {
            size_t to_read = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            int r = uart_read_bytes(self->uart_num_, buf, to_read, 0);
            if (r <= 0)
              break;
            remaining -= (size_t) r;
            for (int i = 0; i < r; ++i) {
              self->lin_protocol_handler_->receive_decode(buf[i], 0);
            }
          }
          // (No manual finalize here; rely on timeout path)
          break;
        }
        case UART_BREAK:
          // Resync on a hardware BREAK — but only after draining the bytes that preceded it
          // (HW-6b): the event refers to the wire NOW, the decoder to the ring buffer THEN.
          drain_rx();
          self->lin_protocol_handler_->on_uart_break();
          break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
          // Corrupt byte on the wire: drop the in-flight frame and count it, rather than
          // parsing garbage into header states (ghost-header risk). See UPGRADES.md C3.
          // Drain first for the same stream-alignment reason as UART_BREAK (HW-6b).
          drain_rx();
          self->lin_protocol_handler_->on_frame_error();
          break;
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
          uart_flush_input(self->uart_num_);
          self->lin_protocol_handler_->on_frame_error();
          break;
        default:
          break;
      }
    } else {
      // Queue wait timed out: inter-byte gap -> finalize frame if in DATA
      self->lin_protocol_handler_->on_inter_byte_timeout();
    }

    // Off-loop schedule tick (A5/N8): the commander header cadence lives HERE, not in the main
    // loop, so the bus keeps being polled through OTA writes and heavy-lambda stalls (N2 for
    // the commander side). This task wakes on every UART event and at least every wait_ticks
    // (~2 ms), which bounds schedule jitter to ~2 ms + any in-flight frame time. Header TX
    // and response TX (on_id) now share this one task context, so they can never interleave
    // on the wire; data_mutex_ + the is_bus_idle() gate inside commander_send() still guard
    // against main-loop table mutation and mid-frame transmission.
    if (self->mode_ == LIN_MODE_COMMANDER) {
      self->lin_protocol_handler_->commander_send();
    }
  }
}

bool LINCommunication::lin_process() {
  if (lin_protocol_handler_ == nullptr)
    return false;

  // Note: the commander schedule is NOT ticked here. Header cadence runs in the UART event
  // task (A5 off-loop tick, see uart_event_task_trampoline), so a blocked main loop never
  // stalls the bus. This call only carries the loop-context housekeeping below.

  // Built-in self test progression. The self-test's scheduled IDs transmit under the
  // off-loop engine automatically (self_test_init_if_needed_() adds them via the mutex-protected
  // schedule API); this tick is observation/evaluation only, so a loop stall can delay the
  // verdict but never the exercised traffic.
  if (self_test_enabled_) {
    self_test_tick_();
  }

  update_statistics();
  return true;
}

// commander scheduler functions

bool LINCommunication::commander_add_id_schedule(uint8_t id, uint32_t interval_ms) {
  if (data_mutex_ != nullptr && xSemaphoreTake(data_mutex_, portMAX_DELAY) != pdTRUE)
    return false;
  // Update existing
  for (auto &schedule : schedule_) {
    if (schedule.updateInterval == 0)
      continue;  // skip free
    if (schedule.id == id) {
      schedule.updateInterval = interval_ms;
      schedule.last_send_time_ms = 0;
      schedule.sent_once = false;
      ESP_LOGV(TAG, "Updated schedule ID 0x%02X interval %" PRIu32 " ms", id, interval_ms);
      if (data_mutex_ != nullptr)
        xSemaphoreGive(data_mutex_);
      return true;
    }
  }
  // Find free slot
  for (auto &schedule : schedule_) {
    if (schedule.updateInterval == 0) {
      schedule.id = id;
      schedule.updateInterval = interval_ms;
      schedule.last_send_time_ms = 0;
      schedule.sent_once = false;
      ESP_LOGV(TAG, "Added schedule ID 0x%02X interval %" PRIu32 " ms", id, interval_ms);
      if (data_mutex_ != nullptr)
        xSemaphoreGive(data_mutex_);
      return true;
    }
  }
  ESP_LOGV(TAG, "Schedule full, cannot add ID 0x%02X", id);
  if (data_mutex_ != nullptr)
    xSemaphoreGive(data_mutex_);
  return false;
}

bool LINCommunication::commander_remove_id_schedule(uint8_t id) {
  if (data_mutex_ != nullptr && xSemaphoreTake(data_mutex_, portMAX_DELAY) != pdTRUE)
    return false;
  for (auto &schedule : schedule_) {
    if (schedule.updateInterval == 0)
      continue;
    if (schedule.id == id) {
      schedule.updateInterval = 0;  // mark free
      schedule.last_send_time_ms = 0;
      schedule.sent_once = false;
      ESP_LOGV(TAG, "Removed schedule ID 0x%02X", id);
      if (data_mutex_ != nullptr)
        xSemaphoreGive(data_mutex_);
      return true;
    }
  }
  ESP_LOGV(TAG, "ID 0x%02X not found in schedule", id);
  if (data_mutex_ != nullptr)
    xSemaphoreGive(data_mutex_);
  return false;
}

// response handling functions
bool LINCommunication::set_response_data(uint8_t id, uint8_t *data, uint8_t data_length, bool enhanced,
                                         bool preserve_existing_checksum) {
  if (id > 0x3F)
    return false;
  if (data_length > LIN_MAX_DATA_SIZE)
    data_length = LIN_MAX_DATA_SIZE;
  if (data_mutex_ != nullptr && xSemaphoreTake(data_mutex_, portMAX_DELAY) != pdTRUE)
    return false;

  // If caller wants to clear: data_length == 0
  if (data_length == 0) {
    for (auto &response : responses_) {
      if (response.dataLength == 0)
        continue;
      if (response.id == id) {
        response.dataLength = 0;  // mark free
        // (optional) zero buffer
        memset(response.responseData, 0, sizeof(response.responseData));
        response.enhancedChecksum = true;
        if (data_mutex_ != nullptr)
          xSemaphoreGive(data_mutex_);
        return true;
      }
    }
    if (data_mutex_ != nullptr)
      xSemaphoreGive(data_mutex_);
    return true;  // nothing to clear
  }

  // Update existing
  for (auto &response : responses_) {
    if (response.dataLength == 0)
      continue;
    if (response.id == id) {
      bool checksum_enhanced = preserve_existing_checksum ? response.enhancedChecksum : enhanced;
      memcpy(response.responseData, data, data_length);
      uint8_t cs = lin_protocol_handler_->calculate_checksum(id, data, data_length, checksum_enhanced);
      response.responseData[data_length] = cs;
      response.enhancedChecksum = checksum_enhanced;
      response.dataLength = data_length + 1;
      if (data_mutex_ != nullptr)
        xSemaphoreGive(data_mutex_);
      return true;
    }
  }
  // Find free (dataLength == 0)
  for (auto &response : responses_) {
    if (response.dataLength != 0)
      continue;
    response.id = id;  // keep ID
    memcpy(response.responseData, data, data_length);
    uint8_t cs = lin_protocol_handler_->calculate_checksum(id, data, data_length, enhanced);
    response.responseData[data_length] = cs;
    response.enhancedChecksum = enhanced;
    response.dataLength = data_length + 1;
    response.lastResponseTime = 0;
    if (data_mutex_ != nullptr)
      xSemaphoreGive(data_mutex_);
    return true;
  }
  if (data_mutex_ != nullptr)
    xSemaphoreGive(data_mutex_);
  return false;  // no free slot
}

void LINCommunication::configure_rx_hint(uint8_t id, uint8_t length, int8_t checksum_mode) {
  if (lin_protocol_handler_ == nullptr)
    return;
  if (length > 0)
    lin_protocol_handler_->set_rx_expected_length(id, length);
  if (checksum_mode >= 0)
    lin_protocol_handler_->set_rx_checksum_mode(id, checksum_mode != 0);
}

void LINCommunication::note_frames_coalesced(uint32_t n) {
  if (lin_protocol_handler_ != nullptr)
    lin_protocol_handler_->note_coalesced(n);
}

bool LINCommunication::get_response_4id(uint8_t id, LinDiscoveredId *lindata) {
  if (!lindata) {
    return false;
  }
  if (data_mutex_ != nullptr && xSemaphoreTake(data_mutex_, portMAX_DELAY) != pdTRUE)
    return false;

  // Search for the ID in discovered_ids_ array
  for (const auto &discovered_id : discovered_ids_) {
    if (discovered_id.dataLength > 0 && discovered_id.id == id) {
      // Found the ID, copy data
      *lindata = discovered_id;
      if (data_mutex_ != nullptr)
        xSemaphoreGive(data_mutex_);
      return true;
    }
  }

  if (data_mutex_ != nullptr)
    xSemaphoreGive(data_mutex_);
  return false;
}

void LINCommunication::dump_received_pids_to_log() const {
  if (data_mutex_ != nullptr && xSemaphoreTake(data_mutex_, portMAX_DELAY) != pdTRUE)
    return;
  ESP_LOGV(TAG, "| %-5s | %-4s | %-3s | %-7s | %-8s | %-24s |", "Idx", "ID", "Len", "Count", "Last(ms)", "Data(hex)");
  ESP_LOGV(TAG, "|-------|------|-----|---------|----------|--------------------------|");
  for (const auto &discovered_id : discovered_ids_) {
    if (discovered_id.dataLength == 0)
      continue;  // skip free slot (ID may be 0 validly)

    char data_str[3 * LIN_MAX_DATA_SIZE + 1] = {0};
    char *p = data_str;
    for (uint8_t j = 0; j < discovered_id.dataLength; ++j) {
      snprintf(p, sizeof(data_str) - (p - data_str), "%02X ", discovered_id.data[j]);
      p += 3;
    }

    ESP_LOGV(TAG, "| %-5zu | 0x%02X | %-3u | %-7" PRIu32 " | %-8" PRId64 " | %-24s |",
             static_cast<size_t>(&discovered_id - discovered_ids_), discovered_id.id, discovered_id.dataLength,
             discovered_id.updateCount, (esp_timer_get_time() - discovered_id.lastUpdateTime) / 1000, data_str);
  }
  if (data_mutex_ != nullptr)
    xSemaphoreGive(data_mutex_);
}

// TEST stuff

void LINCommunication::enable_self_test(bool enable) {
  self_test_enabled_ = enable;
  if (!enable) {
    self_test_initialized_ = false;
    // Clear the slot count too, or a re-arm evaluates stale stats from the
    // previous run before self_test_init_if_needed_() has refilled them.
    self_test_count_ = 0;
    self_test_passed_ = false;
  }
}

void LINCommunication::self_test_tick_() {
  if (mode_ != LIN_MODE_COMMANDER)
    return;
  if (!lin_protocol_handler_)
    return;
  if (!self_test_initialized_) {
    self_test_init_if_needed_();
    return;
  }
  if (data_mutex_ != nullptr && xSemaphoreTake(data_mutex_, portMAX_DELAY) != pdTRUE)
    return;
  for (size_t i = 0; i < self_test_count_; ++i) {
    auto &st = self_test_stats_[i];
    for (const auto &discovered_id : discovered_ids_) {
      if (discovered_id.dataLength == 0)
        continue;
      if (discovered_id.id == st.id) {
        const int64_t t = discovered_id.lastUpdateTime;
        // A new update is one whose timestamp differs from the last one RECORDED.
        // Anchoring this on first_update_time_us (as it did until 2026-07-29)
        // re-counted on every loop pass as soon as an id had updated twice: the
        // condition `t != first` stays true forever once lastUpdateTime moves off
        // the first timestamp, so seen_updates counted main-loop iterations rather
        // than LIN traffic, and an id that answered twice and then went silent
        // still climbed. min/max interval were computed off the same bad anchor
        // and so reported ~0 ms.
        if (t != 0 && t != st.last_update_time_us) {
          if (st.first_update_time_us == 0)
            st.first_update_time_us = t;
          if (st.last_update_time_us != 0) {
            const int64_t interval = t - st.last_update_time_us;
            if (st.min_interval_us == 0 || interval < st.min_interval_us)
              st.min_interval_us = interval;
            if (interval > st.max_interval_us)
              st.max_interval_us = interval;
            const int64_t expected_us = (int64_t) st.scheduled_interval_ms * 1000;
            if (interval > (expected_us + expected_us / 2)) {
              st.missed_windows++;
            }
          }
          st.last_update_time_us = t;
          st.seen_updates++;
        }
        break;
      }
    }
  }
  if (data_mutex_ != nullptr)
    xSemaphoreGive(data_mutex_);
  self_test_evaluate_();
}

void LINCommunication::self_test_init_if_needed_() {
  struct CaseDef {
    uint32_t interval_ms;
    uint8_t id;
    uint8_t payload_len;
  };
  static const CaseDef CASES[] = {{120, 0x00, 3}, {150, 0x01, 2}, {180, 0x15, 4}, {200, 0x1F, 8}, {220, 0x3B, 3},
                                  {250, 0x3C, 2}, {280, 0x3D, 2}, {300, 0x3E, 6}, {330, 0x3F, 1}};
  const size_t case_count = sizeof(CASES) / sizeof(CASES[0]);
  if (case_count > SELF_TEST_MAX_IDS) {
    ESP_LOGV(TAG, "SelfTest truncated: too many cases (%zu)", case_count);
  }
  size_t added = 0;
  for (size_t i = 0; i < case_count && added < SELF_TEST_MAX_IDS; ++i) {
    const auto &c = CASES[i];
    if (!commander_add_id_schedule(c.id, c.interval_ms)) {
      ESP_LOGV(TAG, "Schedule full while adding self-test ID 0x%02X", c.id);
      continue;
    }
    uint8_t payload[LIN_MAX_DATA_SIZE]{};
    for (uint8_t k = 0; k < c.payload_len && k < LIN_MAX_DATA_SIZE; ++k) {
      payload[k] = (uint8_t) (0xA0 + c.id + k);
    }
    set_response_data(c.id, payload, c.payload_len);
    self_test_stats_[added] = {.id = c.id,
                               .scheduled_interval_ms = c.interval_ms,
                               .seen_updates = 0,
                               .last_update_time_us = 0,
                               .first_update_time_us = 0,
                               .missed_windows = 0,
                               .min_interval_us = 0,
                               .max_interval_us = 0};
    added++;
  }
  self_test_count_ = added;
  self_test_start_time_us_ = esp_timer_get_time();
  self_test_initialized_ = true;
  ESP_LOGV(TAG, "LIN self-test initialized with %zu IDs", added);
}

void LINCommunication::self_test_evaluate_() {
  int64_t elapsed_us = esp_timer_get_time() - self_test_start_time_us_;
  if (elapsed_us - self_test_last_eval_us_ < 2'000'000)
    return;
  self_test_last_eval_us_ = elapsed_us;

  ESP_LOGV(TAG, "---- LIN Self-Test Status (%.2fs) ----", elapsed_us / 1e6f);
  // Iterate the slots that were actually filled. This used to run to
  // SELF_TEST_MAX_IDS and skip `st.id == 0` as an empty-slot sentinel, which
  // also skipped the scheduled id 0x00 — the first case the exerciser adds — so
  // a 0x00 that never answered could not fail the verdict.
  bool all_have_updates = self_test_count_ > 0;
  for (size_t i = 0; i < self_test_count_; ++i) {
    const auto &st = self_test_stats_[i];
    bool pass = st.seen_updates >= 2;
    all_have_updates &= pass;
    ESP_LOGV(TAG, "ID 0x%02X upd=%" PRIu32 " miss=%" PRIu32 " min=%.1fms max=%.1fms exp=%.1fms %s", st.id,
             st.seen_updates, st.missed_windows, st.min_interval_us / 1000.0, st.max_interval_us / 1000.0,
             ((int64_t) st.scheduled_interval_ms * 1000) / 1000.0,
             pass ? LOG_STR_LITERAL("OK") : LOG_STR_LITERAL("WAIT"));
  }
  if (all_have_updates && elapsed_us > 5'000'000) {
    self_test_passed_ = true;
    ESP_LOGV(TAG, "LIN self-test: BASIC PASS (all IDs active).");
  }
}

// Statistics methods (low overhead wrappers)
const LinStatistics &LINCommunication::get_statistics() const {
  if (lin_protocol_handler_) {
    return lin_protocol_handler_->get_statistics();
  }
  // Return empty statistics if not initialized
  static LinStatistics empty_stats = {};
  return empty_stats;
}

void LINCommunication::log_statistics() const {
  if (lin_protocol_handler_) {
    lin_protocol_handler_->log_statistics();
  }
}

void LINCommunication::reset_statistics() {
  if (lin_protocol_handler_) {
    lin_protocol_handler_->reset_statistics();
  }
}

void LINCommunication::update_statistics() {
  if (lin_protocol_handler_) {
    lin_protocol_handler_->update_statistics();
  }
}

}  // namespace esphome::linbus
