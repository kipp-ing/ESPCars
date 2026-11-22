#include "lin_protocol_handler.h"
// Third-Party Library Headers
#include "lin_serial.h"        // Explicit include for LinSerial
#include "lin_common.h"        // Explicit include for shared constants
#include "esphome/core/log.h"  // Explicit include for ESP_LOG* macros
#include "esp_timer.h"         // Explicit include for esp_timer_get_time()
#include <cstring>             // For memset

#include "driver/uart.h"

#include <cinttypes>

namespace esphome::linbus {

ESPHOME_LOG_TAG(TAG, "LinProt");

// The former IDF logger compiled non-error logs out (CONFIG_LOG_MAXIMUM_LEVEL=1).
// Keep them at VERBOSE so DEBUG adds no logging work on LIN timing paths.

LinProtocolHandler::LinProtocolHandler(LinSerial *serial, int baud_rate, LinScheduleItem *schedule,
                                       LinDiscoveredId *discovered_ids, LinRespondData *responses, LinMode mode,
                                       SemaphoreHandle_t data_mutex)
    : serial_(serial),
      baud_rate_(baud_rate),
      mode_(mode),
      data_mutex_(data_mutex),
      state_(READ_STATE_BREAK),
      schedule_(schedule),
      responses_(responses),
      discovered_ids_(discovered_ids) {
  byte_time_us_ = 1000000 / baud_rate_ * 10;  // 10 bits per byte at 19200 baud ≈ 520us

  // Per-ID RX hints: no length known and no checksum mode declared until config sets them.
  memset(rx_expected_len_, 0, sizeof(rx_expected_len_));
  memset(rx_checksum_mode_, -1, sizeof(rx_checksum_mode_));  // 0xFF bytes -> int8_t -1

  // Initialize statistics
  reset_statistics();
}

// Destructor to free buffer
LinProtocolHandler::~LinProtocolHandler() { ; }

bool LinProtocolHandler::lock_data_(TickType_t timeout) const {
  return data_mutex_ == nullptr || xSemaphoreTake(data_mutex_, timeout) == pdTRUE;
}

void LinProtocolHandler::unlock_data_() const {
  if (data_mutex_ != nullptr) {
    xSemaphoreGive(data_mutex_);
  }
}

void LinProtocolHandler::on_id(uint8_t id) {
  if (!serial_ || mode_ == LIN_MODE_LISTENER)
    return;
  if (!lock_data_())
    return;
  for (size_t i = 0; i < LIN_MAX_RESPONSES; ++i) {
    if (responses_[i].id == id && responses_[i].dataLength) {
      serial_->write_bytes(responses_[i].responseData, responses_[i].dataLength);

      // Update statistics - ID request answered
      statistics_.id_requests_answered++;
      statistics_.id_requests_answered_total++;
      statistics_.total_bus_bytes += responses_[i].dataLength;

      // Copy the just-written bytes while still under the mutex (the main loop may mutate
      // responses_ after we release), then register the echo expectation OUTSIDE the lock:
      // echo_begin_tx_()/echo_push_() can count a collision, which takes lock_data_(0) itself —
      // the mutex is non-recursive, so pushing while locked would silently drop the count.
      uint8_t echo_len = responses_[i].dataLength;
      uint8_t echo_bytes[LIN_MAX_DATA_SIZE + 1];
      memcpy(echo_bytes, responses_[i].responseData, echo_len);
      unlock_data_();

      echo_begin_tx_();
      for (uint8_t k = 0; k < echo_len; ++k)
        echo_push_(echo_bytes[k]);
      return;
    }
  }
  unlock_data_();
}

void LinProtocolHandler::on_data(uint8_t pid, const uint8_t *data, size_t len) {
  // Per-frame hot path (event task): no logging here (N7); counters carry the observability.
  if (len == 0 || len > 16)
    return;

  if (!lock_data_())
    return;

  // Update statistics - data received
  statistics_.data_bytes_received += len;
  statistics_.data_bytes_received_total += len;
  statistics_.total_bus_bytes += len;

  // Track unique IDs
  uint8_t id = pid & 0x3F;
  if (!(unique_id_tracker_[id / 8] & (1 << (id % 8)))) {
    unique_id_tracker_[id / 8] |= (1 << (id % 8));
    statistics_.unique_ids_seen++;
  }

  // Preferred checksum mode: the checksum type is a *static per-ID property* (defined in the
  // LDF), not a function of who transmits it. Prefer an explicitly declared RX mode (rx_ids /
  // on_frame checksum, spec C2/B6); otherwise infer it from the response this node publishes
  // for the ID; otherwise default enhanced. If the preferred mode fails we retry the other
  // mode instead of dropping the frame, so a listener/sniffer still receives classic (LIN 1.x)
  // traffic it never transmits — counted as checksum_fallback. See UPGRADES.md C2.
  bool enhanced_checksum = true;
  int8_t declared_mode = rx_checksum_mode_[id & 0x3F];
  if (declared_mode >= 0) {
    enhanced_checksum = (declared_mode != 0);
  } else {
    for (size_t i = 0; i < LIN_MAX_RESPONSES; ++i) {
      if (responses_[i].id == id && responses_[i].dataLength) {
        enhanced_checksum = responses_[i].enhancedChecksum;
        break;
      }
    }
  }

  uint8_t expected_checksum = calculate_checksum(pid, data, len - 1, enhanced_checksum);
  if (data[len - 1] != expected_checksum) {
    uint8_t alt_checksum = calculate_checksum(pid, data, len - 1, !enhanced_checksum);
    if (data[len - 1] == alt_checksum) {
      // Accept, but record that we needed the other checksum mode.
      statistics_.checksum_fallback++;
      statistics_.checksum_fallback_total++;
    } else {
      ESP_LOGV(TAG, "Checksum error for PID 0x%02X: got 0x%02X, expected 0x%02X", pid, data[len - 1],
               expected_checksum);
      statistics_.checksum_errors++;
      statistics_.checksum_errors_total++;
      unlock_data_();
      return;
    }
  }

  // Frame accepted (checksum valid under the preferred or fallback mode).
  statistics_.frames_received++;
  statistics_.frames_received_total++;

  // id already declared above - reuse it
  int oldest_idx = -1;
  int64_t oldest_time = INT64_MAX;

  // Scan for existing entry (match by ID) or first free (dataLength == 0)
  for (size_t i = 0; i < LIN_MAX_DISCOVERED_PIDS; ++i) {
    if (discovered_ids_[i].dataLength > 0) {
      // Used slot
      if (discovered_ids_[i].id == id) {
        size_t copy_len = (len - 1 > LIN_MAX_DATA_SIZE) ? LIN_MAX_DATA_SIZE : (len - 1);
        memcpy(discovered_ids_[i].data, data, copy_len);
        discovered_ids_[i].dataLength = copy_len;
        discovered_ids_[i].updateCount++;
        discovered_ids_[i].lastUpdateTime = esp_timer_get_time();
        unlock_data_();
        return;
      }
      if (discovered_ids_[i].lastUpdateTime < oldest_time) {
        oldest_time = discovered_ids_[i].lastUpdateTime;
        oldest_idx = (int) i;
      }
    } else {
      // Free slot (dataLength == 0). Use it.
      discovered_ids_[i].id = id;
      size_t copy_len = (len - 1 > LIN_MAX_DATA_SIZE) ? LIN_MAX_DATA_SIZE : (len - 1);
      memcpy(discovered_ids_[i].data, data, copy_len);
      discovered_ids_[i].dataLength = copy_len;
      discovered_ids_[i].updateCount = 1;
      discovered_ids_[i].lastUpdateTime = esp_timer_get_time();
      ESP_LOGV(TAG, "Discovered new ID 0x%02X", id);
      unlock_data_();
      return;
    }
  }

  // All slots used – overwrite oldest
  if (oldest_idx >= 0) {
    size_t copy_len = (len - 1 > LIN_MAX_DATA_SIZE) ? LIN_MAX_DATA_SIZE : (len - 1);
    discovered_ids_[oldest_idx].id = id;
    memcpy(discovered_ids_[oldest_idx].data, data, copy_len);
    discovered_ids_[oldest_idx].dataLength = copy_len;
    discovered_ids_[oldest_idx].updateCount = 1;
    discovered_ids_[oldest_idx].lastUpdateTime = esp_timer_get_time();
    ESP_LOGV(TAG, "Overwrote oldest slot with ID 0x%02X", id);
  }
  unlock_data_();
}

// Runs in the UART event task (A5 off-loop tick) — the same context as the response TX in
// on_id(), so header and response bytes can never interleave on the wire. lock_data_(0) is
// non-blocking: if the main loop holds the mutex (schedule/response mutation) this wake is
// skipped and the next one (≤ ~2 ms later) retries; due-ness is not consumed by a skip.
uint8_t LinProtocolHandler::commander_send() {
  if (mode_ != LIN_MODE_COMMANDER)
    return 0;
  if (!lock_data_(0))
    return 0;
  // Unsigned millis with wrap-safe subtraction: no int32 overflow at ~24.8 days. See UPGRADES.md C4.
  uint32_t current_time_ms = (uint32_t) (esp_timer_get_time() / 1000);
  for (size_t k = 0; k < LIN_MAX_ID_RQ_COUNT; k++) {
    // Rotating scan start: resume after the last-sent slot so an always-due short interval
    // in a low slot cannot starve later slots at the ~2 ms tick rate.
    size_t i = (schedule_scan_next_ + k) % LIN_MAX_ID_RQ_COUNT;
    if (schedule_[i].updateInterval == 0)
      continue;  // free slot sentinel

    uint32_t elapsed_ms = current_time_ms - schedule_[i].last_send_time_ms;
    if (!schedule_[i].sent_once || elapsed_ms >= schedule_[i].updateInterval) {
      if (!is_bus_idle(3)) {
        unlock_data_();
        return 0;
      }

      uint8_t id = schedule_[i].id;
      bool sent = false;
      if (serial_) {
        sent = serial_->send_break();
        if (sent) {
          uint8_t sync = 0x55;
          sent = serial_->write_bytes(&sync, 1);
          uint8_t pid = calculate_pid_(id);
          sent = serial_->write_bytes(&pid, 1) && sent;
        }
      }
      if (sent) {
        // Update statistics - commander request sent
        statistics_.commander_requests_sent++;
        statistics_.commander_requests_total++;
        statistics_.total_bus_bytes += 3;  // break + sync + pid
      } else {
        // Update statistics - real commander send failure (TX error/timeout), see UPGRADES.md M2
        statistics_.commander_send_failures++;
        statistics_.commander_send_failures_total++;
      }
      schedule_[i].last_send_time_ms = current_time_ms;
      schedule_[i].sent_once = true;
      schedule_scan_next_ = (i + 1) % LIN_MAX_ID_RQ_COUNT;
      unlock_data_();
      // Register the header's echo expectation (A6) — outside the lock, see on_id(). Only on a
      // successful send: a failed TX is already counted as send_failure, and whatever fragment
      // reached the wire passes as foreign traffic rather than faking a collision.
      if (sent) {
        echo_begin_tx_();
        echo_push_(ECHO_BREAK);
        echo_push_(0x55);
        echo_push_(calculate_pid_(id));
      }
      return id;
    }
  }
  unlock_data_();
  return 0;
}

// FIX: Treat argument as PID (with parity) OR raw ID. Always mask to 6-bit ID
// and only include PID in checksum for enhanced frames (non-diagnostic).
uint8_t LinProtocolHandler::calculate_checksum(uint8_t pid_or_id, const uint8_t *data, size_t len, bool enhanced) {
  uint8_t id = pid_or_id & 0x3F;  // 6-bit ID
  uint16_t sum = 0;

  // Enhanced checksum (LIN 2.x): include protected identifier (with parity)
  // Classic checksum: data bytes only.
  // Diagnostic frames (0x3C, 0x3D) ALWAYS use classic checksum.
  if (enhanced && id != 0x3C && id != 0x3D) {
    // Use canonical PID value (rebuild from ID) to avoid parity inconsistencies
    uint8_t pid = calculate_pid_(id);
    sum += pid;
  }

  for (size_t i = 0; i < len; ++i) {
    sum += data[i];
  }

  // Fold carries
  while (sum > 0xFF) {
    sum = (sum & 0xFF) + (sum >> 8);
  }
  return (uint8_t) (~sum);
}

// Get data for a specific PID, returns length or 0 if not found
size_t LinProtocolHandler::get_data_for_id(uint8_t id, uint8_t *data_buf) {
  if (!lock_data_())
    return 0;
  for (size_t i = 0; i < LIN_MAX_DISCOVERED_PIDS; ++i) {
    if (discovered_ids_[i].dataLength == 0)
      continue;  // skip free
    if (discovered_ids_[i].id == id) {
      memcpy(data_buf, discovered_ids_[i].data, discovered_ids_[i].dataLength);
      size_t data_length = discovered_ids_[i].dataLength;
      unlock_data_();
      return data_length;
    }
  }
  unlock_data_();
  return 0;
}

void inline LinProtocolHandler::reset_decode_state_() { state_ = READ_STATE_BREAK; }

void LinProtocolHandler::set_rx_expected_length(uint8_t id, uint8_t len) {
  if ((id & 0x3F) != id)
    return;
  if (len > LIN_MAX_DATA_SIZE)
    len = LIN_MAX_DATA_SIZE;
  rx_expected_len_[id] = len;
}

void LinProtocolHandler::set_rx_checksum_mode(uint8_t id, bool enhanced) {
  if ((id & 0x3F) != id)
    return;
  rx_checksum_mode_[id] = enhanced ? 1 : 0;
}

int LinProtocolHandler::expected_frame_bytes_(uint8_t id) const {
  uint8_t len = rx_expected_len_[id & 0x3F];
  if (len == 0)
    return -1;                       // unknown -> caller uses the inter-byte-timeout fallback
  return static_cast<int>(len) + 1;  // payload + checksum byte
}

void LinProtocolHandler::note_coalesced(uint32_t n) {
  if (n == 0)
    return;
  if (!lock_data_(0))
    return;
  statistics_.frames_coalesced += n;
  statistics_.frames_coalesced_total += n;
  unlock_data_();
}

// --- TX-readback collision detection (A6/N5/B9) — see the header for the context/locking
// contract. The FIFO is only ever touched from the UART event task. ---

void LinProtocolHandler::echo_count_collision_() {
  if (lock_data_(0)) {
    statistics_.collisions++;
    statistics_.collisions_total++;
    unlock_data_();
  }
}

void LinProtocolHandler::echo_begin_tx_() {
  // Leftover expectations at the start of a new TX mean the previous echo never fully
  // returned although the write succeeded — count ONE collision and start clean (N6: one
  // cause, one counter; the inter-byte timeout usually catches this first).
  if (echo_count_ > 0) {
    echo_count_collision_();
    echo_clear_();
  }
}

void LinProtocolHandler::echo_push_(uint16_t v) {
  if (echo_count_ >= ECHO_FIFO_SIZE) {
    // Cannot happen (header 3 + max response 9 ≤ 16) — defensive guard, never wedge (N3).
    echo_count_collision_();
    echo_clear_();
  }
  echo_fifo_[(echo_head_ + echo_count_) % ECHO_FIFO_SIZE] = v;
  echo_count_++;
}

void LinProtocolHandler::echo_compare_(uint8_t readbyte) {
  uint16_t front = echo_front_();
  if (front == ECHO_BREAK) {
    // Our own break echoes as a bare 0x00 data byte on this silicon (HW-0b). Anything else
    // while we drove the bus dominant means another node overdrove us.
    echo_pop_();
    if (readbyte != 0x00)
      echo_count_collision_();
    return;
  }
  if (front == 0x55 && readbyte == 0x00) {
    // A UART_BREAK event may already have consumed the break marker (HW-0c) while the
    // break's 0x00 data byte still trails in. Tolerate without popping — mirrors the
    // decoder's stray-0x00-in-SYNC tolerance (B3).
    return;
  }
  echo_pop_();
  if (readbyte != (uint8_t) front)
    echo_count_collision_();
}

void LinProtocolHandler::on_inter_byte_timeout() {
  if (state_ == READ_STATE_DATA) {
    // If we already have at least 2 bytes (payload+checksum min) finalize
    if (current_data_count_ >= 2) {
      on_data(current_pid_with_parity_, current_data_, current_data_count_);
    }
    reset_decode_state_();
  }
  // A6 desync recovery: expectations still queued after an idle gap mean our echo never
  // arrived (bus held dominant / overdriven end-to-end) -> one collision, start clean.
  if (echo_count_ > 0) {
    echo_count_collision_();
    echo_clear_();
  }
}

void LinProtocolHandler::on_uart_break() {
  // A6: a genuine external-style break can surface as the UART_BREAK event instead of a
  // 0x00 data byte (HW-0c) — if we are expecting our own break's echo, this event is it.
  if (echo_count_ > 0 && echo_front_() == ECHO_BREAK)
    echo_pop_();
  // A hardware BREAK deterministically ends the previous frame slot and starts a new
  // header. Finalize any complete pending frame, then wait for the sync byte. This is the
  // authoritative resync and removes the "ghost header" collision risk. See UPGRADES.md C3.
  if (state_ == READ_STATE_DATA && current_data_count_ >= 2) {
    on_data(current_pid_with_parity_, current_data_, current_data_count_);
  }
  current_data_count_ = 0;
  state_ = READ_STATE_SYNC;  // expect 0x55 next; a stray 0x00 break byte is tolerated in SYNC
  last_bus_activity_us_ = esp_timer_get_time();
  // Count that the driver actually delivered a UART_BREAK event. On the C6 self-loopback this
  // stays 0 (the break arrives as a 0x00 data byte, HW-0b); a climbing count on a two-node bus
  // answers HW-0c — a genuine external break does raise the event. Locked separately, after the
  // on_data() above has already released the mutex (no re-entrancy).
  if (lock_data_(0)) {
    statistics_.uart_breaks++;
    statistics_.uart_breaks_total++;
    unlock_data_();
  }
}

void LinProtocolHandler::on_frame_error() {
  // A6: corrupted wire state — drop any echo expectations silently; frame_errors (below)
  // already counts this fault, and double-counting it as a collision would blur the N6
  // taxonomy.
  echo_clear_();
  if (lock_data_(0)) {
    statistics_.frame_errors++;
    statistics_.frame_errors_total++;
    unlock_data_();
  }
  // Event-order robustness (N3/B3): a UART_FRAME_ERR can *trail* the very break that
  // on_uart_break() just resynced on (a real 13-bit break violates framing). If we are already
  // waiting for the sync byte, keep waiting — do not bounce back to BREAK and swallow the
  // following 0x55. In any other state, drop the in-flight frame and resync from scratch;
  // a corrupt byte must never be finalized into a frame.
  if (state_ != READ_STATE_SYNC) {
    reset_decode_state_();  // -> BREAK; partial data is discarded on the next break byte
  }
}

LinReadState LinProtocolHandler::receive_decode(uint8_t readbyte, uint8_t command) {
  // Called from the UART event task with each received byte (command = 0). Not an ISR, but
  // still the per-byte hot path: straight fall-through state machine, no logging, no heap,
  // no blocking, so response TX timing (on_id) stays tight (N7).

  if (command == 0) {
    last_bus_activity_us_ = esp_timer_get_time();
    // TX readback (A6/N5): if we are expecting our own echo, compare-and-consume first. The
    // byte then ALWAYS continues through the state machine below — a mismatch counts a
    // collision but never corrupts decode state (B9). Empty FIFO (foreign traffic, the
    // steady listener/responder-idle path) costs one comparison.
    if (echo_count_ > 0)
      echo_compare_(readbyte);
  }

  switch (state_) {
    case READ_STATE_BREAK:
      current_data_count_ = 0;  // init for later data receive
      if (readbyte == 0x00) {   // LIN_BREAK
        state_ = READ_STATE_SYNC;
      }
      break;
    case READ_STATE_SYNC:
      if (readbyte == 0x55) {  // LIN_SYNC
        state_ = READ_STATE_SID;
      } else if (readbyte == 0x00) {
        // Stray break byte before sync (e.g. after on_uart_break() resync): keep waiting for 0x55.
      } else {
        state_ = READ_STATE_BREAK;
      }
      break;
    case READ_STATE_SID: {
      // Accept any PID (0..63 with parity)
      uint8_t pid = readbyte;
      uint8_t id = pid & 0x3F;
      if (calculate_pid_(id) == pid) {
        current_pid_with_parity_ = pid;
        current_id_ = id;

        // Immediately transmit response (first data byte timing critical)
        on_id(id);

        // Enter data phase to capture response (including our own echo HW loops back)
        current_data_count_ = 0;
        state_ = READ_STATE_DATA;
      } else {
        // Parity error -> reset
        if (lock_data_(0)) {
          statistics_.pid_errors++;
          statistics_.pid_errors_total++;
          unlock_data_();
        }
        state_ = READ_STATE_BREAK;
      }
      break;
    }

    case READ_STATE_DATA:
      if (command == 0) {  // only real bytes
        if (current_data_count_ < (LIN_MAX_DATA_SIZE + 1)) {
          current_data_[current_data_count_++] = readbyte;
          // Length-aware finalize (A3/H1): when the ID's frame length is known, end the frame
          // deterministically after exactly length+1 bytes (payload + checksum) instead of
          // waiting for the inter-byte silence. This makes back-to-back frames and
          // within-budget mid-response pauses safe, and is the primary end-of-frame boundary
          // on silicon that never signals a hardware BREAK (HW-0b). Silence stays the fallback.
          int expect = expected_frame_bytes_(current_id_);
          if (expect > 0 && (int) current_data_count_ >= expect) {
            on_data(current_pid_with_parity_, current_data_, current_data_count_);
            reset_decode_state_();
          }
        } else {
          // Overflow guard (unknown length): finalize what we have and resync.
          if (current_data_count_ >= 2) {
            on_data(current_pid_with_parity_, current_data_, current_data_count_);
          }
          reset_decode_state_();
        }
      }
      break;

    default:
      reset_decode_state_();
      break;
  }
  return state_;
}

// Helper: check bus idle (driver-only)
bool LinProtocolHandler::is_bus_idle(uint32_t required_idle_bits) {
  if (required_idle_bits == 0)
    required_idle_bits = 3;
  int64_t now = esp_timer_get_time();
  int64_t bit_time_us = (baud_rate_ > 0) ? (1000000LL / baud_rate_) : 52;
  int64_t needed_us = (int64_t) required_idle_bits * bit_time_us;

  if (state_ == READ_STATE_DATA)
    return false;
  if ((now - last_bus_activity_us_) < needed_us)
    return false;

  if (serial_ == nullptr)
    return false;
  if (uart_wait_tx_done(serial_->get_port(), 0) != ESP_OK)
    return false;

  return true;
}

// Utility functions remain unchanged
uint8_t LinProtocolHandler::calculate_pid_(uint8_t id) {
  uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 0x01;
  uint8_t p1 = ~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 0x01;
  uint8_t pid = id | (p0 << 6) | (p1 << 7);
  return pid;
}

uint8_t LinProtocolHandler::get_id_from_pid_(uint8_t pid) {
  uint8_t id = pid & 0x3F;
  if (calculate_pid_(id) == pid) {
    return id;
  } else {
    return 0x3F;  // Invalid PID
  }
}

// Statistics implementation
void LinProtocolHandler::reset_statistics() {
  if (!lock_data_())
    return;
  memset(&statistics_, 0, sizeof(statistics_));
  memset(&statistics_snapshot_, 0, sizeof(statistics_snapshot_));
  memset(unique_id_tracker_, 0, sizeof(unique_id_tracker_));
  statistics_.stats_start_time = esp_timer_get_time();
  unlock_data_();
}

void LinProtocolHandler::update_statistics() {
  if (!lock_data_())
    return;
  int64_t now = esp_timer_get_time();
  int64_t window_duration_us = now - statistics_.stats_start_time;

  // Update every 10 seconds (10,000,000 microseconds)
  if (window_duration_us >= 10000000) {
    // Debug: Show raw counters before calculation
    ESP_LOGV(TAG,
             "Stats window: commander_reqs=%" PRIu32 ", id_answers=%" PRIu32 ", data_bytes=%" PRIu32 ", errors=%" PRIu32
             "+%" PRIu32 "+%" PRIu32,
             statistics_.commander_requests_sent, statistics_.id_requests_answered, statistics_.data_bytes_received,
             statistics_.commander_send_failures, statistics_.pid_errors, statistics_.checksum_errors);

    // Calculate rates (per second)
    double window_duration_s = window_duration_us / 1000000.0;

    statistics_.commander_reqs_per_sec = statistics_.commander_requests_sent / window_duration_s;
    statistics_.id_answers_per_sec = statistics_.id_requests_answered / window_duration_s;
    statistics_.data_bytes_per_sec = statistics_.data_bytes_received / window_duration_s;

    // Calculate bus load percentage
    // Assuming 8N1 format: 1 start + 8 data + 1 stop = 10 bits per byte
    int64_t theoretical_max_bits = (int64_t) baud_rate_ * window_duration_s;
    int64_t actual_bits = statistics_.total_bus_bytes * 10;
    statistics_.estimated_bus_load_percent =
        (theoretical_max_bits > 0) ? (double) actual_bits / theoretical_max_bits * 100.0 : 0.0;

    // Calculate error rates
    int64_t total_transactions = statistics_.commander_requests_sent + statistics_.id_requests_answered;
    int64_t total_errors = statistics_.commander_send_failures + statistics_.pid_errors + statistics_.checksum_errors;
    statistics_.error_rate_percent =
        (total_transactions > 0) ? (double) total_errors / total_transactions * 100.0 : 0.0;

    // Reset for next window (windowed counters only; lifetime *_total fields never reset)
    statistics_.stats_start_time = now;
    statistics_.commander_requests_sent = 0;
    statistics_.id_requests_answered = 0;
    statistics_.data_bytes_received = 0;
    statistics_.frames_received = 0;
    statistics_.commander_send_failures = 0;
    statistics_.pid_errors = 0;
    statistics_.checksum_errors = 0;
    statistics_.checksum_fallback = 0;
    statistics_.frame_errors = 0;
    statistics_.collisions = 0;
    statistics_.frames_coalesced = 0;
    statistics_.uart_breaks = 0;
    statistics_.total_bus_bytes = 0;
  }
  unlock_data_();
}

const LinStatistics &LinProtocolHandler::get_statistics() const {
  if (lock_data_()) {
    statistics_snapshot_ = statistics_;
    unlock_data_();
  }
  return statistics_snapshot_;
}

void LinProtocolHandler::log_statistics() const {
  const LinStatistics &stats = get_statistics();
  ESP_LOGV(TAG, "LIN Statistics (10s window):");
  ESP_LOGV(TAG, "  Commander requests: %.1f/s", stats.commander_reqs_per_sec);
  ESP_LOGV(TAG, "  ID responses: %.1f/s", stats.id_answers_per_sec);
  ESP_LOGV(TAG, "  Data bytes: %.1f/s", stats.data_bytes_per_sec);
  ESP_LOGV(TAG, "  Unique IDs seen: %" PRIu32, stats.unique_ids_seen);
  ESP_LOGV(TAG, "  Bus load: %.1f%%", stats.estimated_bus_load_percent);
  ESP_LOGV(TAG, "  Error rate: %.1f%%", stats.error_rate_percent);
  ESP_LOGV(TAG, "  Stats window: %" PRId64 " ms", (esp_timer_get_time() - stats.stats_start_time) / 1000);
}

}  // namespace esphome::linbus
