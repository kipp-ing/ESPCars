#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "esp_timer.h"  // ensure own direct dependency
#include "lin_common.h"

#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esphome::linbus {

// Forward Declarations (to avoid circular dependencies)
class LinSerial;

// Constants for LIN protocol
static constexpr uint8_t LIN_BREAK = 0x00;
static constexpr uint8_t LIN_SYNC = 0x55;

// LIN frame structure
struct LinFrame {
  uint8_t id;                       // ID
  uint8_t dataLength;               // Length of data bytes
  uint8_t data[LIN_MAX_DATA_SIZE];  // Data bytes
  uint8_t checksum;                 // Checksum byte
};

// Structure to track discovered PIDs
struct LinDiscoveredId {
  uint8_t id;                       // ID
  uint8_t dataLength;               // Length of data observed
  uint8_t data[LIN_MAX_DATA_SIZE];  // Data bytes
  int64_t lastUpdateTime;           // Last time this PID was seen (microseconds)
  uint32_t updateCount;             // How many times this PID was observed
};

struct LinScheduleItem {
  uint8_t id;                  // ID
  uint32_t updateInterval;     // Send interval (ms); 0 marks a free slot
  uint32_t last_send_time_ms;  // esp_timer_get_time() / 1000 at last send (unsigned; wrap-safe subtraction)
  bool sent_once;              // false until first send (replaces the ambiguous 0 sentinel)
};

struct LinRespondData {
  uint8_t id;
  uint8_t dataLength;                           // Payload plus checksum; 0 marks a free slot
  uint8_t responseData[LIN_MAX_DATA_SIZE + 1];  // last byte = checksum
  bool enhancedChecksum;
  uint32_t lastResponseTime;
};
// NOTE: Free / unused response slot is now indicated by dataLength == 0 (ID may be any valid value, including 0).

// LIN Statistics structure (10-second rolling window)
struct LinStatistics {
  // Windowed counters (reset every 10 s; feed the per-second rate fields below)
  uint32_t commander_requests_sent;
  uint32_t id_requests_answered;
  uint32_t data_bytes_received;
  uint32_t frames_received;  // accepted (checksum-valid) frames this window (N6)
  uint32_t unique_ids_seen;

  // Windowed error counters — one per distinct fault class (N6 root-cause taxonomy)
  uint32_t commander_send_failures;
  uint32_t pid_errors;
  uint32_t checksum_errors;
  uint32_t checksum_fallback;  // frame accepted with the *other* checksum mode (see UPGRADES.md C2)
  uint32_t frame_errors;       // UART framing/parity/OVF resyncs (spec "framing_errors", UPGRADES.md C3)
  uint32_t collisions;         // TX readback mismatch — competing publisher on a shared ID (N5)
  uint32_t frames_coalesced;   // on_frame drops when frames arrive faster than the loop drains (H3)
  uint32_t uart_breaks;        // UART_BREAK events actually delivered by the driver (HW-0b/HW-0c)

  // Lifetime totals (never reset -> safe for STATE_CLASS_TOTAL_INCREASING sensors, UPGRADES.md H4)
  uint32_t commander_requests_total;
  uint32_t id_requests_answered_total;
  uint32_t data_bytes_received_total;
  uint32_t frames_received_total;
  uint32_t checksum_errors_total;
  uint32_t commander_send_failures_total;
  uint32_t pid_errors_total;
  uint32_t frame_errors_total;
  uint32_t collisions_total;
  uint32_t checksum_fallback_total;
  uint32_t frames_coalesced_total;
  uint32_t uart_breaks_total;

  // Bus activity
  uint32_t total_bus_bytes;
  int64_t stats_start_time;

  // Per-second rates (calculated values)
  float commander_reqs_per_sec;
  float id_answers_per_sec;
  float data_bytes_per_sec;
  float estimated_bus_load_percent;
  float error_rate_percent;
};

enum LinReadState {
  READ_STATE_BREAK,
  READ_STATE_SYNC,
  READ_STATE_SID,
  READ_STATE_DATA,
};

// Example: Handler for PID requests and responses
class LinProtocolHandler {
 public:
  // Pass schedule pointer to constructor
  LinProtocolHandler(LinSerial *serial, int baud_rate, LinScheduleItem *schedule, LinDiscoveredId *discovered_ids,
                     LinRespondData *responses, LinMode mode, SemaphoreHandle_t data_mutex);
  virtual ~LinProtocolHandler();

  // Called when a PID is received, to check for registered responses
  void on_id(uint8_t id);

  // Called when a data frame is received
  void on_data(uint8_t pid, const uint8_t *data, size_t len);

  // Send the next due scheduled header (break + sync + PID). Called from the UART event
  // task on every wake (A5 off-loop tick), so the cadence is main-loop-independent (N8).
  // At most one header per call; returns the ID sent, or 0 if none was due / bus busy.
  uint8_t commander_send();

  // Get data for a specific PID, returns length or 0 if not found
  size_t get_data_for_id(uint8_t id, uint8_t *data_buf);

  // Statistics methods
  void update_statistics();
  void reset_statistics();
  const LinStatistics &get_statistics() const;
  void log_statistics() const;

  // Called from the UART event task with each received byte (command = 0). Not an ISR —
  // but still the per-byte hot path: no logging, no heap, no blocking (N7).
  LinReadState receive_decode(uint8_t readbyte, uint8_t command);

  uint8_t calculate_checksum(uint8_t id, const uint8_t *data, size_t len, bool enhanced = true);

  // Per-received-ID static hints from config (set once at setup, before the event task runs).
  // A LIN frame's length and checksum type are static per ID (the LDF), so these let a
  // listener/sniffer frame and checksum IDs it never transmits. See spec A3 (length-aware
  // finalize) and C2/B6 (RX checksum control).
  void set_rx_expected_length(uint8_t id, uint8_t len);  // len 0..8; 0 = unknown (timeout fallback)
  void set_rx_checksum_mode(uint8_t id, bool enhanced);  // declare classic/enhanced for this ID

  // Called under the TX table mutex. Derived lengths may change with actions;
  // the event task snapshots one at the header, never in the middle of a frame.
  bool rx_length_conflicts(uint8_t id, uint8_t length) const {
    return this->rx_expected_len_[id] != 0 && this->rx_expected_len_[id] != length;
  }
  void set_response_length(uint8_t id, uint8_t length) {
    this->response_lengths_[id].store(length, std::memory_order_relaxed);
  }
  uint8_t get_response_length(uint8_t id) const { return this->response_lengths_[id].load(std::memory_order_relaxed); }

  void on_inter_byte_timeout();

  // Count frames dropped by main-loop-rate coalescing on the on_frame trigger (H3/N6). Called
  // from the control loop; folds into the frames_coalesced windowed + lifetime counters.
  void note_coalesced(uint32_t n);

  // UART-event hooks (called from the UART event task, see UPGRADES.md C3)
  void on_uart_break();   // hardware BREAK: finalize any pending frame, resync to the sync byte
  void on_frame_error();  // UART framing/parity error: reset the decoder and count it

 private:
  LinSerial *serial_;
  int baud_rate_;
  LinMode mode_;
  SemaphoreHandle_t data_mutex_;

  int64_t byte_time_us_;
  void reset_decode_state_();

  LinReadState state_;
  uint8_t current_id_;
  uint8_t current_pid_with_parity_;
  uint8_t current_data_[LIN_MAX_DATA_SIZE + 2];
  uint8_t current_data_count_;
  int current_frame_bytes_ = -1;

  // Explicit hints are immutable after task start. Only response-derived lengths
  // change at runtime, published atomically and latched once per header.
  std::atomic<uint8_t> response_lengths_[64]{};
  uint8_t rx_expected_len_[64] = {0};  // 0 = unknown; else payload length -> finalize at len+1
  int8_t rx_checksum_mode_[64];        // -1 unknown, 0 classic, 1 enhanced (init in ctor)
  // Expected total DATA-phase byte count (payload + checksum) for an ID, or -1 if unknown.
  int expected_frame_bytes_(uint8_t id) const;

  // Pointer to schedule array and count (owned by LINCommunication)
  LinScheduleItem *schedule_;

  // Rotating scan start for commander_send() fairness: the off-loop tick calls it every ~2 ms,
  // so a pathologically short interval in a low slot would otherwise win the from-zero scan
  // every time and starve later slots. Consecutive calls resume after the last-sent slot.
  // Touched only under lock_data_().
  size_t schedule_scan_next_ = 0;

  // --- TX-readback collision detection (A6/N5/B9) ---
  // Every byte this node drives onto the one-wire bus echoes back into its own decoder. The
  // expected-echo FIFO holds what we just transmitted (ECHO_BREAK marks the break, whose echo
  // presentation varies — HW-0b vs HW-0c); receive_decode() compares each incoming byte against
  // the front and counts a mismatch as a collision: a competing publisher overdrove us.
  // Single-context by design: since the A5 event-task fold, ALL TX (commander_send headers, on_id
  // responses) and ALL consumption (receive_decode / on_uart_break / on_inter_byte_timeout /
  // on_frame_error) run in the UART event task, so the FIFO itself needs no locking.
  // Fault deltas are atomic and merged by statistics readers under the table mutex. Static storage, no
  // heap on the hot path (N7); sized for a header (3) + a max response (9) with slack.
  static constexpr size_t ECHO_FIFO_SIZE = 16;
  static constexpr uint16_t ECHO_BREAK = 0x100;  // out-of-band break marker (> any data byte)
  uint16_t echo_fifo_[ECHO_FIFO_SIZE] = {};
  uint8_t echo_head_ = 0;
  uint8_t echo_count_ = 0;

  void echo_count_collision_();  // retained atomic delta; merged by statistics readers
  void echo_clear_() {
    echo_head_ = 0;
    echo_count_ = 0;
  }
  void echo_begin_tx_();  // stale leftovers (previous echo never returned) -> one collision + clear
  void echo_push_(uint16_t v);
  uint16_t echo_front_() const { return echo_fifo_[echo_head_]; }
  void echo_pop_() {
    echo_head_ = (uint8_t) ((echo_head_ + 1) % ECHO_FIFO_SIZE);
    echo_count_--;
  }
  void echo_compare_(uint8_t readbyte);  // consume/compare the front against an incoming byte

  LinRespondData *responses_;

  LinDiscoveredId *discovered_ids_;

  // Statistics tracking
  // Faults cannot wait for the table mutex or disappear when the control loop
  // owns it. Atomic deltas retain them until a statistics reader merges them
  // under that mutex. Fault paths allocate nothing and never wait for the
  // table mutex; ESP-IDF supplies atomic helpers on targets without HW atomics.
  mutable std::atomic<uint32_t> pending_collisions_{0};
  mutable std::atomic<uint32_t> pending_pid_errors_{0};
  mutable std::atomic<uint32_t> pending_frame_errors_{0};
  mutable std::atomic<uint32_t> pending_uart_breaks_{0};
  mutable std::atomic<uint32_t> pending_coalesced_{0};
  mutable std::atomic<uint32_t> pending_header_bytes_{0};
  void merge_pending_statistics_() const;  // caller holds data_mutex_
  mutable LinStatistics statistics_{};
  mutable LinStatistics statistics_snapshot_{};
  uint8_t unique_id_tracker_[256];  // Bitmap to track unique IDs seen in current window

  bool lock_data_(TickType_t timeout = portMAX_DELAY) const;
  void unlock_data_() const;

  // Utility
  // Utility functions
  uint8_t calculate_pid_(uint8_t id);

  uint8_t get_id_from_pid_(uint8_t pid);

  int64_t last_bus_activity_us_ = 0;  // timestamp of last seen bus byte
 public:
  bool is_bus_idle(uint32_t required_idle_bits = 3);
};

}  // namespace esphome::linbus
