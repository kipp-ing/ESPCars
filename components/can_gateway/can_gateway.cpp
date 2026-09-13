#include "can_gateway.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_attr.h>
#include <driver/gpio.h>

#include <esp_idf_version.h>
#include <soc/soc_caps.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>

// The bus-off orphan reclamation (OutstandingTracker, gateway_core.h) and the
// TX-queue occupancy gates encode driver-internal behavior of esp_driver_twai
// that was established by reading and hardware-testing IDF 5.5.x. A newer IDF
// may change any of it — e.g. delivering on_tx_done for the frame halted by
// bus-off would turn the eager head reclaim into a double release. Re-verify
// the contract points listed on OutstandingTracker before raising this bound.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 6, 0)
#error "can_gateway: revalidate the esp_driver_twai driver contract before building with ESP-IDF > 5.5.x"
#endif

// ...and the same contract is HAL-VARIANT specific, which the version bound
// above does not cover. esp_driver_twai sits on two different HALs (IDF's
// components/hal/CMakeLists.txt selects by SOC_TWAI_SUPPORT_FD):
//   twai_hal_v1.c — every TWAI target except the C5. Bus-off clears the
//     TX-occupied state flag WITHOUT raising TWAI_HAL_EVENT_TX_BUFF_FREE
//     ("Any TX would have been halted by entering bus off"), so the halted
//     frame never reaches on_tx_done. That is the contract OutstandingTracker
//     is written against, and reclaim_head() is safe.
//   twai_hal_v2.c — SOC_TWAI_SUPPORT_FD targets (only the ESP32-C5 in IDF
//     5.5.4). twai_hal_get_events() derives TX_BUFF_FREE from
//     TWAIFD_LL_INTR_TX_DONE, the TXT-buffer hardware-command interrupt
//     documented as "Transmit finish (ok or error)" — it is not suppressed by
//     bus-off, so the halted frame DOES complete and pops itself out of the
//     tracker. loop_()'s eager reclaim_head() then frees the NEXT entry, a
//     frame the driver still holds a pointer to and replays after recovery:
//     the slot gets re-acquired and overwritten, putting a wrong ID/payload on
//     a vehicle bus, and its later completion is a double release.
// Rejecting the build is the only safe answer until the tracker is taught the
// v2 event model and it is verified on C5 silicon. See docs/HANDOVER.md Todo 4.
#if defined(SOC_TWAI_SUPPORT_FD) && SOC_TWAI_SUPPORT_FD
#error \
    "can_gateway: unsupported TWAI HAL variant. This target has SOC_TWAI_SUPPORT_FD and builds twai_hal_v2.c, where a frame halted by bus-off still raises TX_BUFF_FREE and completes; the OutstandingTracker bus-off contract assumes it never completes, so the eager head reclaim would release a slot the driver still owns (wrong frame on the wire, then a double release). Only the ESP32-C5 is affected in ESP-IDF 5.5.4. See docs/HANDOVER.md Todo 4."
#endif

namespace esphome::can_gateway {

static const char *const TAG = "can_gateway";

// on_tx_done recovers the owning TxSlot from the driver's frame pointer.
static_assert(offsetof(TxSlot, frame) == 0, "twai_frame_t must be the first TxSlot member");
static_assert(std::is_standard_layout<TxSlot>::value, "TxSlot must be standard layout");

// ---------------------------------------------------------------------------
// ISR callback shims (IRAM: they can run while flash cache is disabled, and
// CONFIG_TWAI_ISR_CACHE_SAFE requires it)
// ---------------------------------------------------------------------------

static bool IRAM_ATTR on_rx_done_cb(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx) {
  static_cast<GatewayPort *>(user_ctx)->handle_rx_isr();
  return false;
}

static bool IRAM_ATTR on_tx_done_cb(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user_ctx) {
  static_cast<GatewayPort *>(user_ctx)->handle_tx_done_isr(edata);
  return false;
}

static bool IRAM_ATTR on_state_change_cb(twai_node_handle_t handle, const twai_state_change_event_data_t *edata,
                                         void *user_ctx) {
  static_cast<GatewayPort *>(user_ctx)->handle_state_change_isr(edata);
  return false;
}

static bool IRAM_ATTR on_error_cb(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *user_ctx) {
  static_cast<GatewayPort *>(user_ctx)->handle_error_isr(edata->err_flags);
  return false;
}

// ---------------------------------------------------------------------------
// GatewayRoute
// ---------------------------------------------------------------------------

GatewayRoute::GatewayRoute(GatewayPort *from, GatewayPort *to, uint8_t rule_count, bool default_accept)
    : from_(from), to_(to) {
  this->rules_.init(rule_count);
  this->table_.default_drop = !default_accept;
  for (uint8_t i = 0; i < CAN_GATEWAY_TX_SLOTS; i++) {
    this->slots_[i].route = this;
    this->slots_[i].index = i;
  }
}

void GatewayRoute::add_rule(uint32_t match_id, uint32_t match_mask, uint8_t flags, uint32_t new_id, uint64_t and_mask,
                            uint64_t or_value, RulePatch *patch) {
  RuleEntry entry{};
  entry.match_id = match_id;
  entry.match_mask = match_mask;
  // Bits 0-4 are shared with the core; bits 5-6 are consumed here.
  entry.flags =
      flags & (RULE_FLAG_EXTENDED | RULE_FLAG_CHECK_RTR | RULE_FLAG_RTR_VALUE | RULE_FLAG_DROP | RULE_FLAG_HAS_PATCH);

  PatchData data{};
  data.replace_id = (flags & RULE_FLAG_REPLACE_ID) != 0;
  data.new_can_id = new_id;
  data.new_extended = (flags & RULE_FLAG_OUT_EXT) != 0;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    data.and_mask[i] = static_cast<uint8_t>(and_mask >> (8 * i));
    data.or_value[i] = static_cast<uint8_t>(or_value >> (8 * i));
  }

  if (patch != nullptr) {
    patch->init_(data);
    entry.banks = patch->banks_ptr_();
  } else {
    entry.static_patch = data;
  }

  this->rules_.push_back(entry);
  this->table_.rules = &this->rules_[0];
  this->table_.rule_count = static_cast<uint8_t>(this->rules_.size());
}

// ---------------------------------------------------------------------------
// GatewayPort: bring-up and control plane (loop context)
// ---------------------------------------------------------------------------

bool GatewayPort::start_(GatewayRoute *route_out, const std::atomic<bool> *gateway_enabled,
                         uint8_t interrupt_priority) {
  this->gateway_enabled_ = gateway_enabled;
  for (uint8_t i = 0; i < CAN_GATEWAY_INJECT_SLOTS; i++) {
    this->inject_slots_[i].port = this;
    this->inject_slots_[i].index = i;
  }

#ifdef USE_CAN_GATEWAY_OBSERVE
  // Build the observation set/ring from the codegen-registered subscribers and
  // mark the route observed if forwarding — before try_hw_filter_offload_ below
  // reads route->observed_ (B25).
  this->build_observation_(route_out);
#endif

#ifdef USE_CAN_GATEWAY_LOG_TAP
  // Datalogger tap (sd_logger S2). Allocated only on an armed port, so an
  // unarmed one keeps the null test and nothing else. Like the observation
  // ring this must exist before the node is enabled, or the first frames off
  // the wire would find a null ring.
  if (this->log_tap_enabled_) {
    this->log_tap_ring_ =
        new ObserveRing<CAN_GATEWAY_LOG_TAP_DEPTH, TapRecord>();  // NOLINT(cppcoreguidelines-owning-memory)
    // A tapped port must not let the hardware filter drop frames the log wants,
    // for the same reason observation must not (B25).
    if (route_out != nullptr)
      route_out->mark_observed();
  }
#endif

  twai_onchip_node_config_t config{};
  config.io_cfg.tx = static_cast<gpio_num_t>(this->tx_pin_);
  config.io_cfg.rx = static_cast<gpio_num_t>(this->rx_pin_);
  config.io_cfg.quanta_clk_out = GPIO_NUM_NC;
  config.io_cfg.bus_off_indicator = GPIO_NUM_NC;
  config.bit_timing.bitrate = this->bit_rate_;
  config.fail_retry_cnt = -1;  // standard CAN behavior: hardware retransmits
  config.tx_queue_depth = this->tx_queue_depth_;
  config.intr_priority = interrupt_priority;
  config.flags.enable_listen_only = this->listen_only_;
  config.flags.enable_self_test = this->self_test_;
  // Note: flags.enable_loopback deliberately NOT exposed — the ESP32-C6
  // TWAI core has no loopback mode bit (twai_ll_set_mode ignores it).

  if (twai_new_node_onchip(&config, &this->node_) != ESP_OK) {
    ESP_LOGE(TAG, "Port %u: controller allocation failed", this->index_);
    return false;
  }

  // Filter offload must happen while the node is still disabled. The route is
  // passed in because route_out_ is only wired after all ports are up.
  this->try_hw_filter_offload_(route_out);

  twai_event_callbacks_t callbacks{};
  callbacks.on_rx_done = on_rx_done_cb;
  callbacks.on_tx_done = on_tx_done_cb;
  callbacks.on_state_change = on_state_change_cb;
  callbacks.on_error = on_error_cb;
  if (twai_node_register_event_callbacks(this->node_, &callbacks, this) != ESP_OK) {
    ESP_LOGE(TAG, "Port %u: callback registration failed", this->index_);
    return false;
  }

  if (twai_node_enable(this->node_) != ESP_OK) {
    ESP_LOGE(TAG, "Port %u: enable failed", this->index_);
    return false;
  }

  if (this->open_drain_tx_) {
    // Bench aid: both controllers on one wire without
    // transceivers. Requires an external pull-up; never with a transceiver.
    gpio_set_direction(static_cast<gpio_num_t>(this->tx_pin_), GPIO_MODE_INPUT_OUTPUT_OD);
  }
  return true;
}

void GatewayPort::try_hw_filter_offload_(const GatewayRoute *route) {
  // A single accept rule + default drop + no RTR constraint collapses onto
  // the C6's one hardware mask filter; everything else keeps accept-all and
  // the software table decides. Forwarding behavior is identical either way
  // (the rule still runs in software for patching), but diagnostics are not:
  // frames the hardware rejects never reach the RX ISR, so they would vanish
  // from the filtered counter, the bus statistics, and the last_frame
  // snapshot. The offload therefore only engages when no such consumer
  // exists for this port.
#ifndef USE_CAN_GATEWAY_STATS
#ifdef USE_CAN_GATEWAY_SNAPSHOT
  if (this->snapshot_ring_ != nullptr)
    return;
#endif
  if (route == nullptr || !route->table_.default_drop || route->rules_.size() != 1 || route->observed_)
    return;
  const RuleEntry &rule = route->rules_[0];
  if (rule.is_drop() || (rule.flags & RULE_FLAG_CHECK_RTR) != 0)
    return;

  twai_mask_filter_config_t filter{};
  filter.id = rule.match_id;
  filter.mask = rule.match_mask;
  filter.is_ext = (rule.flags & RULE_FLAG_EXTENDED) != 0;
  this->hw_filter_engaged_ = twai_node_config_mask_filter(this->node_, 0, &filter) == ESP_OK;
#else
  (void) route;
#endif
}

bool GatewayPort::read_error_counters(uint16_t &tec, uint16_t &rec) const {
  if (this->node_ == nullptr)
    return false;
  twai_node_status_t status{};
  if (twai_node_get_info(this->node_, &status, nullptr) != ESP_OK)
    return false;
  tec = status.tx_error_count;
  rec = status.rx_error_count;
  return true;
}

bool GatewayPort::inject(uint32_t can_id, bool extended, bool rtr, const uint8_t *data, uint8_t len) {
  const char *fail_reason = nullptr;
  if (this->node_ == nullptr || this->listen_only_) {
    fail_reason = "port cannot transmit";
  } else if (this->is_bus_off()) {
    // Cheap early-out for a port that is already known bus-off: skip the whole
    // slot setup rather than acquire, fill and release again. NOT the binding
    // check — a state-change ISR can flip the port to bus-off between here and
    // the hand-off (~100-200 cycles of pool acquire, header init and memcpy).
    // The check that decides is the one inside the critical section below.
    fail_reason = "bus off";
  } else {
    uint8_t slot_index = this->inject_pool_.acquire();
    if (slot_index == SLOT_NONE) {
      fail_reason = "no inject slot free";
    } else {
      TxSlot &slot = this->inject_slots_[slot_index];
      if (len > MAX_FRAME_DATA_LEN)
        len = MAX_FRAME_DATA_LEN;
      slot.frame.header = {};
      slot.frame.header.id = can_id & (extended ? MAX_EXTENDED_ID : MAX_STANDARD_ID);
      slot.frame.header.ide = extended;
      slot.frame.header.rtr = rtr;
      slot.frame.header.dlc = rtr ? 0 : len;
      if (!rtr && len > 0 && data != nullptr)
        std::memcpy(slot.payload, data, len);
      slot.frame.buffer = slot.payload;
      // TX contract: the driver rejects the frame unless buffer_len agrees
      // with header.dlc (esp_twai_onchip.c _node_queue_tx).
      slot.frame.buffer_len = rtr ? 0 : len;
      // Critical section: the bus-off check, the occupancy gate, the transmit
      // and the tracker push are all one atomic step against the TWAI ISRs.
      // Interrupt masking on the single-core C6 is sufficient; the ISRs
      // themselves never lock. Both driver calls are non-blocking (timeout 0).
      //
      // Ordering: transmit + push must not be split, or an interleaved forward
      // from the peer's RX ISR would break the tracker's submission-order
      // mirror of the driver queue.
      //
      // Bus-off: the state mirror is re-read HERE, with interrupts already
      // masked, because that is the only point at which it stays true until the
      // hand-off. Reaching twai_node_transmit while bus-off hits
      // _node_queue_tx's ESP_RETURN_ON_FALSE_ISR guard ("node is bus off",
      // esp_twai_onchip.c), which expands to ESP_EARLY_LOGE -> esp_rom_printf:
      // a busy-wait UART write of ~48 characters, ~4 ms at 115200 baud, with
      // every interrupt masked. It takes no lock and so does not abort -- it
      // simply stops the machine long enough to overrun both TWAI RX FIFOs
      // (~20 frames on a 500 kbit/s bus), silently losing traffic on both
      // ports. Cost of the re-read: one relaxed atomic load.
      //
      // Occupancy: when the driver's TX queue is full, the task-context branch
      // of _node_queue_tx reports through ESP_RETURN_ON_FALSE -> ESP_LOGE, and
      // that path does take a lock -- newlib's lock_acquire_generic sees
      // xPortCanYield() == false inside a critical section, treats it as ISR
      // context and abort()s (a device reset). outstanding_ mirrors the driver
      // FIFO, so size() >= tx_queue_depth_ means the driver would reject and
      // log -- skip the call in that case and shed the frame ourselves. May
      // conservatively shed one frame the driver would still accept (dequeued
      // but not yet tx-done); shedding under saturation beats an abort.
      portENTER_CRITICAL(&this->mux_);
      bool bus_off = this->is_bus_off();
      bool accepted = false;
      if (!bus_off && this->outstanding_.size() < this->tx_queue_depth_) {
        accepted = twai_node_transmit(this->node_, &slot.frame, 0) == ESP_OK;
        if (accepted)
          this->outstanding_.push(&slot);
      }
      portEXIT_CRITICAL(&this->mux_);
      if (!accepted) {
        this->inject_pool_.release(slot_index);
        fail_reason = bus_off ? "bus off" : "tx queue full";
      } else {
        count(this->counters_.injected);
        return true;
      }
    }
  }
  // A failed hand-off logs a throttled warning and counts nothing.
  uint32_t now = millis();
  if (now - this->last_inject_warn_ms_ >= 1000) {
    this->last_inject_warn_ms_ = now;
    ESP_LOGW(TAG, "Port %u: inject failed (%s)", this->index_, fail_reason);
  }
  return false;
}

void GatewayPort::loop_(uint32_t now_ms) {
  uint8_t state = this->state_.load(std::memory_order_relaxed);
  if (state != this->last_loop_state_) {
    bool was_bus_off = this->last_loop_state_ == static_cast<uint8_t>(TWAI_ERROR_BUS_OFF);
    this->last_loop_state_ = state;
    if (state == static_cast<uint8_t>(TWAI_ERROR_BUS_OFF)) {
      // Schedule recovery, tell the application. Everything here is loop
      // context; the ISR only stored the state atomic.
      ESP_LOGW(TAG, "Port %u: bus-off, starting recovery", this->index_);
      this->bus_off_head_reclaimed_ = false;
      this->backoff_.on_bus_off(now_ms);
      this->bus_off_callback_.call();
#ifdef USE_BINARY_SENSOR
      if (this->bus_off_sensor_ != nullptr)
        this->bus_off_sensor_->publish_state(true);
#endif
    } else if (was_bus_off && state == static_cast<uint8_t>(TWAI_ERROR_ACTIVE)) {
      ESP_LOGI(TAG, "Port %u: recovered", this->index_);
      count(this->counters_.recoveries);
      this->backoff_.on_recovered(now_ms);
      this->recovered_callback_.call();
#ifdef USE_BINARY_SENSOR
      if (this->bus_off_sensor_ != nullptr)
        this->bus_off_sensor_->publish_state(false);
#endif
    }
  }

  if (state == static_cast<uint8_t>(TWAI_ERROR_BUS_OFF) && !this->bus_off_head_reclaimed_) {
    // The frame mounted on hardware when bus-off struck will never get
    // on_tx_done (see OutstandingTracker's driver contract) — free its slot
    // now so a fully-orphaned pool cannot leave the port mute. Queued frames
    // stay tracked: the driver retains and replays them after recovery.
    // Must run under the state re-check with interrupts masked: if recovery
    // already completed (loop was blocked), replay completions own the
    // reclaim via complete()'s self-healing and the head may be live again.
    portENTER_CRITICAL(&this->mux_);
    if (this->state_.load(std::memory_order_relaxed) == static_cast<uint8_t>(TWAI_ERROR_BUS_OFF)) {
      TxSlot *orphan = nullptr;
      if (this->outstanding_.reclaim_head(orphan))
        this->reclaim_orphan_(orphan);
    }
    this->bus_off_head_reclaimed_ = true;
    portEXIT_CRITICAL(&this->mux_);
  }

  if (state == static_cast<uint8_t>(TWAI_ERROR_BUS_OFF) && this->backoff_.should_attempt(now_ms)) {
    twai_node_recover(this->node_);
    // Re-arm at the doubled delay in case the bus stays dead; the schedule only
    // resets after a stable-on-bus period.
    this->backoff_.on_bus_off(now_ms);
  } else if (state == static_cast<uint8_t>(TWAI_ERROR_ACTIVE)) {
    this->backoff_.on_stable_tick(now_ms);
  }

#if defined(USE_CAN_GATEWAY_SNAPSHOT) && defined(USE_TEXT_SENSOR)
  if (this->snapshot_ring_ != nullptr && this->last_frame_sensor_ != nullptr &&
      now_ms - this->last_snapshot_publish_ms_ >= this->snapshot_throttle_ms_) {
    FrameSnapshot snapshot;
    if (this->snapshot_ring_->read_latest(snapshot)) {
      this->last_snapshot_publish_ms_ = now_ms;
      // e.g. "0x2A0 [8] 01 02 03 04 05 06 07 08" / "0x18DAF110 X [0] RTR"
      char buffer[48];
      int written = snprintf(buffer, sizeof(buffer), "0x%" PRIX32 "%s [%u]%s", snapshot.can_id,
                             snapshot.extended ? " X" : "", snapshot.dlc, snapshot.rtr ? " RTR" : "");
      if (!snapshot.rtr && snapshot.dlc > 0 && written < static_cast<int>(sizeof(buffer)) - 1) {
        buffer[written++] = ' ';
        format_hex_pretty_to(buffer + written, sizeof(buffer) - written, snapshot.data, snapshot.dlc, ' ');
      }
      this->last_frame_sensor_->publish_state(buffer);
    }
  }
#endif

  // bus_err = genuine bus faults, derived from upward TEC/REC movement. A benign
  // arbitration loss leaves TEC/REC untouched, and its error flag is
  // indistinguishable from a real one on the C6 TWAIFD (see handle_error_isr),
  // so counting error events would inflate bus_err on any contended bus. TEC/REC
  // are read here in loop context because they are not cache-safe to read in the
  // ISR, and only on an interval — a driver status call per loop pass would be
  // wasted work on a healthy bus. Genuine errors add +8 (TEC) or +1 (REC) each;
  // recovery resets them downward, which is ignored.
  if (now_ms - this->last_err_poll_ms_ >= ERR_POLL_INTERVAL_MS) {
    this->last_err_poll_ms_ = now_ms;
    uint16_t tec = 0;
    uint16_t rec = 0;
    if (this->read_error_counters(tec, rec)) {
      if (this->err_counters_seeded_) {
        uint32_t delta = 0;
        if (tec > this->last_tec_)
          delta += static_cast<uint32_t>(tec - this->last_tec_);
        if (rec > this->last_rec_)
          delta += static_cast<uint32_t>(rec - this->last_rec_);
        if (delta > 0)
          this->counters_.bus_err.fetch_add(delta, std::memory_order_relaxed);
      }
      this->last_tec_ = tec;
      this->last_rec_ = rec;
      this->err_counters_seeded_ = true;
    }
  }

#ifdef USE_CAN_GATEWAY_OBSERVE
  // Drain the observation ring and dispatch to the decode / subscription
  // consumers (A14). Bounded per pass (B21); allocation-free (N8).
  this->drain_observations_();
#endif
}

#ifdef USE_CAN_GATEWAY_OBSERVE
void GatewayPort::build_observation_(GatewayRoute *route_out) {
  if (!this->observed_)
    return;  // no consumer: no set, no ring, byte-identical fast path (B26)
  // Static sorted membership set for the ISR (A14). Built once from the
  // subscribers registered at codegen; const afterward.
  this->subscribed_set_ = new SubscribedSet<CAN_GATEWAY_MAX_OBSERVE_IDS>();  // NOLINT(cppcoreguidelines-owning-memory)
  for (const auto &sub : this->subscribers_) {
    if (!sub.all && !this->subscribed_set_->insert(sub.can_id, sub.extended)) {
      ESP_LOGW(TAG, "Port %u: over %u subscribed IDs; extra IDs will not be observed", this->index_,
               CAN_GATEWAY_MAX_OBSERVE_IDS);
    }
  }
  this->observe_ring_ = new ObserveRing<CAN_GATEWAY_OBSERVE_QUEUE_DEPTH>();  // NOLINT(cppcoreguidelines-owning-memory)
  this->rx_staging_frame_.buffer = this->rx_staging_payload_;
  // A forwarding port that also observes must keep the hardware filter offload
  // off, or hardware-rejected frames would never reach the ring (B25).
  if (route_out != nullptr)
    route_out->mark_observed();
}

void GatewayPort::drain_observations_() {
  if (this->observe_ring_ == nullptr)
    return;
  // Bounded drain (B21): at most max_frames_per_loop records this pass, the rest
  // wait for the next loop, so a busy bus cannot stretch one loop iteration.
  FrameRecord record;
  uint8_t drained = 0;
  while (drained < this->max_frames_per_loop_ && this->observe_ring_->pop(record)) {
    dispatch_record(this->subscribers_.data(), static_cast<uint16_t>(this->subscribers_.size()), record);
    drained++;
  }
}
#endif

#ifdef USE_CAN_GATEWAY_STATS
void GatewayPort::log_statistics_(uint32_t now_ms) {
  uint32_t rx = this->rx_bits_.load(std::memory_order_relaxed);
  uint32_t tx = this->tx_bits_.load(std::memory_order_relaxed);
  // The first call only seeds the window; loads are printed from the second on
  // so idle boot time is never folded into the first percentage.
  if (this->log_seeded_) {
    uint32_t elapsed_ms = now_ms - this->last_log_ms_;
    if (elapsed_ms > 0) {
      uint32_t rx_delta = rx - this->last_log_rx_bits_;
      uint32_t tx_delta = tx - this->last_log_tx_bits_;
      float capacity = static_cast<float>(this->bit_rate_) * static_cast<float>(elapsed_ms) / 1000.0f;
      float rx_load = capacity > 0.0f ? 100.0f * static_cast<float>(rx_delta) / capacity : 0.0f;
      float tx_load = capacity > 0.0f ? 100.0f * static_cast<float>(tx_delta) / capacity : 0.0f;
      uint16_t tec = 0;
      uint16_t rec = 0;
      this->read_error_counters(tec, rec);
      ESP_LOGI(TAG,
               "Port %u stats: bus load %.1f%% (rx %.1f%% / tx %.1f%%), bus_err %" PRIu32 ", err_events %" PRIu32
               ", tx_fail %" PRIu32 ", TEC %u, REC %u",
               this->index_, rx_load + tx_load, rx_load, tx_load,
               this->counters_.bus_err.load(std::memory_order_relaxed),
               this->counters_.err_events.load(std::memory_order_relaxed),
               this->counters_.tx_fail.load(std::memory_order_relaxed), tec, rec);
    }
  }
  this->log_seeded_ = true;
  this->last_log_rx_bits_ = rx;
  this->last_log_tx_bits_ = tx;
  this->last_log_ms_ = now_ms;

#ifdef USE_CAN_GATEWAY_ID_STATS
  // Only ARM the per-ID dump here; the lines are emitted a bounded few per loop
  // pass by log_id_timings_step_(). Writing the whole table from this one call
  // is what made a 128-entry table (id_timings_max) cost up to 128 lines per
  // port in a single pass — ~1.5 s of blocking UART on both ports against a
  // ~16 ms loop period, which trips ESPHome's "took a long time" warning,
  // resyncs every CyclicSend schedule and overflows the observation ring.
  // Nothing is dropped: the dump resumes where it left off, and the one case
  // that does lose lines (a dump still running when the next interval arrives)
  // says so in the log below.
  if (this->id_timing_ != nullptr) {
    if (this->id_log_active_) {
      // The previous dump had not finished when this interval came round. Say
      // so rather than restart silently: the entries from id_log_cursor_ on
      // were never printed for that interval.
      ESP_LOGW(TAG, "  Port %u ID timings: previous dump cut short after %u of %u entries", this->index_,
               this->id_log_cursor_, this->id_log_total_);
    }
    // Snapshot the size: the RX ISR appends while we walk, and entries are
    // append-only (an ID keeps its index for the table's lifetime), so walking
    // [0, total) stays consistent. IDs first seen mid-dump are printed by the
    // next interval, not skipped.
    this->id_log_cursor_ = 0;
    this->id_log_total_ = this->id_timing_->size();
    this->id_log_active_ = this->id_log_total_ > 0;
  }
#endif
}

#ifdef USE_CAN_GATEWAY_ID_STATS
void GatewayPort::log_id_timings_step_() {
  if (!this->id_log_active_)
    return;
  uint16_t emitted = 0;
  while (this->id_log_cursor_ < this->id_log_total_ && emitted < ID_LOG_LINES_PER_PASS) {
    // Copy one entry with this port's interrupts masked so its 64-bit period
    // sum is not torn by a concurrent record() from the RX ISR (single-core).
    portENTER_CRITICAL(&this->mux_);
    IdTimingEntry entry = this->id_timing_->entry(this->id_log_cursor_);
    portEXIT_CRITICAL(&this->mux_);
    this->id_log_cursor_++;
    emitted++;
    if (entry.count > 1) {
      uint32_t avg_us = static_cast<uint32_t>(entry.sum_period_us / (entry.count - 1));
      ESP_LOGI(TAG, "  Port %u ID 0x%" PRIX32 "%s: avg %" PRIu32 " us, n %" PRIu32, this->index_, entry.can_id,
               entry.extended ? " X" : "", avg_us, entry.count);
    } else {
      ESP_LOGI(TAG, "  Port %u ID 0x%" PRIX32 "%s: n %" PRIu32, this->index_, entry.can_id, entry.extended ? " X" : "",
               entry.count);
    }
  }
  if (this->id_log_cursor_ < this->id_log_total_)
    return;  // the rest continues on the next loop pass
  this->id_log_active_ = false;
  uint32_t overflow = this->id_timing_->overflow();
  if (overflow > 0) {
    ESP_LOGW(TAG, "  Port %u ID table full: %" PRIu32 " frames untracked (raise id_timings_max)", this->index_,
             overflow);
  }
}
#endif
#endif

// ---------------------------------------------------------------------------
// GatewayPort: data plane (ISR context, IRAM)
// ---------------------------------------------------------------------------

void IRAM_ATTR GatewayPort::handle_rx_isr() {
  GatewayRoute *route = this->route_out_;
  if (route == nullptr) {
    // No outbound route. A port with an observation consumer receives into its
    // static staging frame and delivers it (A12/B20), and a tapped port is a
    // consumer the same way: the datalogger drains the ring, so the frame must
    // be retrieved even though nothing routes, decodes or triggers on this
    // port. Found on the bench as a port with only `log_tap: true` that never
    // received a frame — bus load 0.0 % against a live, several-hundred-
    // frames/s sender, because leaving the frame to the driver also skips
    // run_rx_taps_() (private/notes/HANDOVER-guest-bms.md §8). A true
    // destination-only port has neither ring and lets the driver discard the
    // frame (v0.5 behavior).
#if defined(USE_CAN_GATEWAY_OBSERVE) || defined(USE_CAN_GATEWAY_LOG_TAP)
    bool armed = false;
#ifdef USE_CAN_GATEWAY_OBSERVE
    armed = this->observe_ring_ != nullptr;
#endif
#ifdef USE_CAN_GATEWAY_LOG_TAP
    armed = armed || this->log_tap_ring_ != nullptr;
#endif
    if (armed)
      this->receive_into_staging_();
#endif
    return;
  }

  // These three gates decide whether the frame is FORWARDED, not whether it is
  // received. Order is unchanged — enable -> destination alive -> TX slot — and
  // each still counts its shed exactly once, before anything else. What follows
  // a shed is tap_shed_frame_(): the frame was on this port's wire regardless of
  // where it was headed, so it is still retrieved (into the staging frame) and
  // still tapped, keeping observation, snapshot and bus load alive on a port
  // whose forwarding is shed (Issue #1). Only a port that consumes nothing
  // leaves the frame to the driver, which discards it.
  if (!this->gateway_enabled_->load(std::memory_order_relaxed)) {
    count(route->counters_.disabled);
    this->tap_shed_frame_();
    return;
  }
  GatewayPort *dest = route->to_;
  if (dest->state_.load(std::memory_order_relaxed) == static_cast<uint8_t>(TWAI_ERROR_BUS_OFF)) {
    count(route->counters_.bus_off);
    this->tap_shed_frame_();
    return;
  }
  uint8_t slot_index = route->pool_.acquire();
  if (slot_index == SLOT_NONE) {
    count(route->counters_.tx_full);
    this->tap_shed_frame_();
    return;
  }

  TxSlot &slot = route->slots_[slot_index];
  slot.frame.header = {};
  slot.frame.buffer = slot.payload;
  slot.frame.buffer_len = MAX_FRAME_DATA_LEN;
  if (twai_node_receive_from_isr(this->node_, &slot.frame) != ESP_OK) {
    route->pool_.release(slot_index);
    return;  // no frame retrieved; nothing to count
  }

  uint32_t can_id = slot.frame.header.id;
  bool extended = slot.frame.header.ide != 0;
  bool rtr = slot.frame.header.rtr != 0;
  uint8_t dlc =
      slot.frame.header.dlc > MAX_FRAME_DATA_LEN ? MAX_FRAME_DATA_LEN : static_cast<uint8_t>(slot.frame.header.dlc);

  // Every non-forwarding consumer of this frame, in one inlined block: bus-load
  // bits and per-ID timing (the frame occupied this port's bus whatever becomes
  // of it), the snapshot ring, the observation ring. Runs from the freshly
  // received slot, before process_frame rewrites it, so observation sees the
  // wire pre-patch. Identical to what a shed frame gets from tap_shed_frame_();
  // a filtered frame reaches it too, by falling through to process_frame below.
  this->run_rx_taps_(can_id, extended, rtr, dlc, slot.payload);

  // The frame's single copy already happened: receive_from_isr wrote straight
  // into the TX slot. Match and patch in place, then hand the slot over.
  FrameAction action = process_frame(route->table_, can_id, extended, rtr, slot.payload, dlc);
  if (action == FrameAction::DROP_FILTERED) {
    route->pool_.release(slot_index);
    count(route->counters_.filtered);
    return;
  }

  slot.frame.header.id = can_id;
  slot.frame.header.ide = extended;
  slot.frame.header.trigger_time = 0;  // union with the RX timestamp; must not schedule the TX
  // buffer_len was the RX capacity (8); for TX it must agree with header.dlc
  // or the driver rejects the frame (esp_twai_onchip.c _node_queue_tx).
  // Classic CAN allows receiving DLC 9-15 on the wire (payload is still 8
  // bytes; the HAL passes the raw value through), but that same driver check
  // refuses to transmit dlc > 8 — normalize data-frame DLC to the clamped
  // value so such frames still cross. RTR frames keep their raw DLC: with
  // buffer_len 0 the agreement check is skipped and DLC 0-15 transmits.
  if (!rtr)
    slot.frame.header.dlc = dlc;
  slot.frame.buffer_len = rtr ? 0 : dlc;
  // Gate the hand-off on the destination's occupancy, exactly like inject():
  // when the driver TX queue is full, twai_node_transmit logs "tx queue full"
  // from inside this call even in ISR context (ESP_EARLY_LOGE — a blocking
  // UART write whose format string lives in flash, fatal if this ISR runs
  // during a cache-off OTA/NVS window). outstanding_ mirrors the driver FIFO;
  // never call the driver when it would reject and log. Reading it here is
  // safe: dest's tx_done ISR runs at the same priority (never preempts), and
  // loop-context accesses mask interrupts.
  bool accepted = false;
  if (dest->outstanding_.size() < dest->tx_queue_depth_) {
    accepted = twai_node_transmit(dest->node_, &slot.frame, 0) == ESP_OK;
  }
  if (!accepted) {
    route->pool_.release(slot_index);
    count(route->counters_.tx_full);
    return;
  }
  // Track the hand-off for bus-off orphan reclamation.
  dest->outstanding_.push(&slot);
  count(route->counters_.forwarded);
}

#ifdef USE_CAN_GATEWAY_OBSERVE
void IRAM_ATTR GatewayPort::observe_frame_(uint32_t can_id, bool extended, bool rtr, uint8_t dlc, const uint8_t *data) {
  if (this->observe_ring_ == nullptr)
    return;
  // The bounded N7 cost: one membership binary search. observe_all skips it.
  if (!this->observe_all_ && !this->subscribed_set_->contains(can_id, extended))
    return;
  FrameRecord record;
  record.can_id = can_id;
  record.extended = extended;
  record.rtr = rtr;
  record.dlc = dlc;
  record.reserved = 0;
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    record.data[i] = i < dlc ? data[i] : 0;
  if (this->observe_ring_->push(record)) {
    count(this->counters_.observed);
  } else {
    count(this->counters_.observe_overflow);  // ring full: shed newest (B22)
  }
}
#endif  // USE_CAN_GATEWAY_OBSERVE

#ifdef CAN_GATEWAY_HAS_RX_TAPS
void IRAM_ATTR GatewayPort::receive_into_staging_(bool shed) {
  // Receive path for a frame that is not being forwarded: a port with no
  // outbound route at all (A12) and the shed path below. There is no TX slot to
  // receive into, so retrieve into the static staging frame and tap from there.
  // The staging frame is per-port and used only here, and a port either has an
  // outbound route or it does not — the two callers can never be live on the
  // same port, and the forwarding fast path never reaches this function.
  this->rx_staging_frame_.header = {};
  this->rx_staging_frame_.buffer = this->rx_staging_payload_;
  this->rx_staging_frame_.buffer_len = MAX_FRAME_DATA_LEN;
  if (twai_node_receive_from_isr(this->node_, &this->rx_staging_frame_) != ESP_OK)
    return;
  uint32_t can_id = this->rx_staging_frame_.header.id;
  bool extended = this->rx_staging_frame_.header.ide != 0;
  bool rtr = this->rx_staging_frame_.header.rtr != 0;
  uint8_t dlc = this->rx_staging_frame_.header.dlc > MAX_FRAME_DATA_LEN
                    ? MAX_FRAME_DATA_LEN
                    : static_cast<uint8_t>(this->rx_staging_frame_.header.dlc);
  this->run_rx_taps_(can_id, extended, rtr, dlc, this->rx_staging_payload_, shed);
}

void IRAM_ATTR GatewayPort::tap_shed_frame_() {
  // A shed forward still occupied this port's bus and its non-forwarding
  // consumers must still see it (Issue #1), so pay one extra copy through the
  // staging frame — the price of a shed, never of a forward. When no tap on
  // this port has a consumer there is nothing to pay it for: skip the receive
  // and let the driver discard the frame, leaving a shed exactly as cheap as it
  // was before (one counter increment, already done by the caller).
  if (!this->rx_taps_active_())
    return;
  this->receive_into_staging_(/*shed=*/true);
}
#endif  // CAN_GATEWAY_HAS_RX_TAPS

void IRAM_ATTR GatewayPort::handle_tx_done_isr(const twai_tx_done_event_data_t *edata) {
  if (!edata->is_tx_success)
    count(this->counters_.tx_fail);
#ifdef USE_CAN_GATEWAY_STATS
  // Our own transmissions occupy this port's bus too.
  const auto &tx_header = edata->done_tx_frame->header;
  uint8_t tx_dlc = tx_header.dlc > MAX_FRAME_DATA_LEN ? MAX_FRAME_DATA_LEN : static_cast<uint8_t>(tx_header.dlc);
  this->tx_bits_.fetch_add(estimate_frame_bits(tx_header.ide != 0, tx_header.rtr != 0, tx_dlc),
                           std::memory_order_relaxed);
#endif
  // frame is the first member of TxSlot (static_assert above).
  auto *slot = reinterpret_cast<TxSlot *>(const_cast<twai_frame_t *>(edata->done_tx_frame));
  // Completions arrive in submission order (see OutstandingTracker): any
  // tracked entry older than this one was orphaned by a bus-off the loop
  // never got to handle — reclaim those first (lazy self-healing).
  //
  // Release ONLY what the tracker actually owned. complete() mutates nothing
  // when `slot` is untracked and always removes at least the completed entry
  // when it is tracked, so the occupancy delta is the "was it tracked?" answer.
  // Untracked means the slot was already reclaimed by the eager bus-off path,
  // and by now it may have been re-acquired and refilled by another frame:
  // releasing it a second time would hand the same slot to two owners and
  // corrupt the pool. Dropping the release leaks at most that one slot, which
  // is recoverable; a double release is not. This cannot happen under the
  // driver contract the component compiles against (the top of this file
  // rejects the HAL variant where it could) — it is the belt to that braces.
  uint8_t tracked_before = this->outstanding_.size();
  this->outstanding_.complete(slot, [this](TxSlot *orphan) { this->reclaim_orphan_(orphan); });
  if (this->outstanding_.size() != tracked_before)
    release_slot(slot);
}

void IRAM_ATTR GatewayPort::reclaim_orphan_(TxSlot *orphan) {
  // A frame the driver halted at bus-off and will never complete: its
  // transmission failed on the wire, so it counts as tx_fail on this port.
  release_slot(orphan);
  count(this->counters_.tx_fail);
}

void IRAM_ATTR GatewayPort::handle_state_change_isr(const twai_state_change_event_data_t *edata) {
  // Nothing but the atomic store happens in interrupt context.
  this->state_.store(static_cast<uint8_t>(edata->new_sta), std::memory_order_relaxed);
}

void IRAM_ATTR GatewayPort::handle_error_isr(twai_error_flags_t err_flags) {
  // Count every reported error event as raw activity. This cannot be the
  // genuine-fault signal on its own: a benign arbitration loss (normal on any
  // bus the gateway transmits onto -- the frame just retransmits) is reported
  // here as an ordinary bit/form/stuff error with TEC/REC unaffected, and on the
  // ESP32-C6 TWAIFD it is indistinguishable by flag from a real error. The v2
  // HAL never sets err_flags.arb_lost (twaifd_ll_get_err_reason fills only
  // bit/form/stuff/ack; arbitration loss is a separate event bit that never
  // reaches this callback), so masking a flag does not work -- confirmed on
  // hardware: err_events climbs ~27/s with TEC/REC pinned at 0 under a
  // bidirectional flood. bus_err (the genuine-fault counter) is therefore
  // derived from upward TEC/REC movement in loop_(), which a lost arbitration
  // never causes. TEC/REC cannot be read cache-safely from an ISR.
  if (err_flags.bit_err || err_flags.form_err || err_flags.stuff_err || err_flags.ack_err)
    count(this->counters_.err_events);
}

// ---------------------------------------------------------------------------
// CanGateway
// ---------------------------------------------------------------------------

void CanGateway::setup() {
  // v0.6: 1 or 2 ports; routes optional (a single-port block is a monitor/node
  // with no forwarding). Config validation guarantees the count; guard anyway.
  if (this->ports_.empty()) {
    this->mark_failed();
    return;
  }
  // One route per direction (enforced at config time too). Routes are only
  // collected here, NOT wired into the ports yet — see below. route_for[i] is
  // nullptr for a port with no outbound route (destination-only, or a
  // single-bus node): the port comes up and its RX ISR simply discards frames
  // (v0.6 observation, added next, gives it a receive path).
  GatewayRoute *route_for[2] = {nullptr, nullptr};
  for (auto *route : this->routes_) {
    uint8_t from = route->from_->index_;
    if (route_for[from] != nullptr) {
      ESP_LOGE(TAG, "Port %u already has an outbound route", from);
      this->mark_failed();
      return;
    }
    route_for[from] = route;
  }
#ifdef USE_CAN_GATEWAY_ID_STATS
  if (this->id_timings_) {
    // Allocated once, pre-enable, in internal RAM: both buses carry IDs.
    for (auto *port : this->ports_)
      port->id_timing_ = new IdTimingTable<CAN_GATEWAY_ID_STATS_MAX>();  // NOLINT(cppcoreguidelines-owning-memory)
  }
#endif
  // Declaration order = controller order: TWAI0 first, TWAI1 second.
  // Both controllers come up BEFORE any route is wired: a port's RX interrupt
  // is live from twai_node_enable on, and only a wired route makes it forward.
  // Wiring afterwards guarantees the ISR can never hand a frame to a
  // destination whose node does not exist yet (the driver rejects a null node
  // with a log from ISR context, once per received frame).
  for (size_t i = 0; i < this->ports_.size(); i++) {
    if (!this->ports_[i]->start_(route_for[i], &this->enabled_, this->interrupt_priority_)) {
      // Leave no half-alive data plane behind: no route was wired, so started
      // ports only discard frames — and disabling their nodes removes the
      // interrupt sources entirely, since a FAILED component never runs
      // loop() again to service them.
      for (size_t j = 0; j < i; j++)
        twai_node_disable(this->ports_[j]->node_);
      this->mark_failed();
      return;
    }
  }
  // Both nodes are up; make the ports forward. The ISRs may already be
  // running: a single aligned pointer store publishes each route (a frame
  // arriving mid-loop sees either nullptr — discarded — or the full route).
  for (size_t i = 0; i < this->ports_.size(); i++)
    this->ports_[i]->route_out_ = route_for[i];
}

void CanGateway::loop() {
  uint32_t now = millis();
  for (auto *port : this->ports_)
    port->loop_(now);
#ifdef USE_CAN_GATEWAY_CYCLIC
  for (auto *cyclic : this->cyclic_sends_)
    cyclic->loop(now);
#endif
#ifdef USE_CAN_GATEWAY_STATS
  if (this->stats_log_interval_ms_ > 0 && now - this->last_stats_log_ms_ >= this->stats_log_interval_ms_) {
    this->last_stats_log_ms_ = now;
    for (auto *port : this->ports_)
      port->log_statistics_(now);
  }
#ifdef USE_CAN_GATEWAY_ID_STATS
  // Continue the per-ID timing dump the interval tick armed — a few lines per
  // pass, so a full table cannot monopolise one loop iteration. One bool test
  // per port when no dump is in flight (the normal case).
  for (auto *port : this->ports_)
    port->log_id_timings_step_();
#endif
#endif
}

#ifdef USE_TEXT_SENSOR
void CanGateway::set_last_frame_text_sensor(GatewayPort *port, text_sensor::TextSensor *sensor, uint32_t throttle_ms) {
#ifdef USE_CAN_GATEWAY_SNAPSHOT
  port->last_frame_sensor_ = sensor;
  port->snapshot_throttle_ms_ = throttle_ms;
  // Allocated once, pre-setup, in internal RAM; ports without
  // the sensor never pay for the ring, and the ISR path checks one pointer.
  port->snapshot_ring_ = new SnapshotRing<GatewayPort::SNAPSHOT_RING_SIZE>();  // NOLINT
#endif
}
#endif

void CanGateway::dump_config() {
  ESP_LOGCONFIG(TAG,
                "CAN gateway:\n"
                "  Enabled: %s\n"
                "  Interrupt priority: %u",
                YESNO(this->is_enabled()), this->interrupt_priority_);
  for (auto *port : this->ports_) {
    ESP_LOGCONFIG(TAG,
                  "  Port %u (TWAI%u):\n"
                  "    TX pin: GPIO%d, RX pin: GPIO%d\n"
                  "    Bit rate: %" PRIu32 " bit/s\n"
                  "    TX queue depth: %u\n"
                  "    Hardware filter offload: %s",
                  port->index_, port->index_, port->tx_pin_, port->rx_pin_, port->bit_rate_, port->tx_queue_depth_,
                  YESNO(port->hw_filter_engaged_));
    if (port->listen_only_)
      ESP_LOGCONFIG(TAG, "    Listen only: YES");
    if (port->self_test_)
      ESP_LOGCONFIG(TAG, "    Self test (bench aid): YES");
    if (port->open_drain_tx_)
      ESP_LOGCONFIG(TAG, "    Open-drain TX (bench aid): YES");
#if defined(USE_CAN_GATEWAY_SNAPSHOT) && defined(USE_TEXT_SENSOR)
    if (port->snapshot_ring_ != nullptr) {
      ESP_LOGCONFIG(TAG, "    Snapshot ring: %" PRIu32 " pushed, %" PRIu32 " overwritten unseen",
                    port->snapshot_ring_->pushed(), port->snapshot_ring_->missed());
    }
#endif
  }
  for (auto *route : this->routes_) {
    ESP_LOGCONFIG(TAG,
                  "  Route: port %u -> port %u\n"
                  "    Rules: %u, default: %s\n"
                  "    Slots in use: %u/%u",
                  route->from_->index_, route->to_->index_, route->table_.rule_count,
                  route->table_.default_drop ? "drop" : "accept", route->pool_.in_use(), route->pool_.capacity());
  }
#ifdef USE_CAN_GATEWAY_CYCLIC
  for (auto *cyclic : this->cyclic_sends_) {
    ESP_LOGCONFIG(TAG, "  Cyclic send: port %u, ID 0x%" PRIX32 ", every %" PRIu32 " ms, running: %s",
                  cyclic->port()->index_, cyclic->can_id(), cyclic->interval_ms(), YESNO(cyclic->is_running()));
  }
#endif
#ifdef USE_CAN_GATEWAY_STATS
  if (this->stats_log_interval_ms_ > 0)
    ESP_LOGCONFIG(TAG, "  Statistics log interval: %" PRIu32 " ms", this->stats_log_interval_ms_);
#ifdef USE_CAN_GATEWAY_ID_STATS
  ESP_LOGCONFIG(TAG, "  Per-ID timings: %s", YESNO(this->id_timings_));
#endif
#endif
}

// ---------------------------------------------------------------------------
// Sensor hub
// ---------------------------------------------------------------------------

#ifdef USE_SENSOR
void CanGatewaySensorHub::update() {
  // Kind indices follow ALL_KINDS in sensor.py; see the KindIndex enum.
  if (this->route_ != nullptr) {
    const RouteCounters &counters = this->route_->counters();
    const std::atomic<uint32_t> *route_counters[] = {&counters.forwarded, &counters.filtered, &counters.tx_full,
                                                     &counters.bus_off, &counters.disabled};
    for (uint8_t i = KIND_FORWARDED; i <= KIND_DISABLED; i++) {
      if (this->sensors_[i] != nullptr)
        this->sensors_[i]->publish_state(route_counters[i - KIND_FORWARDED]->load(std::memory_order_relaxed));
    }
  }
  if (this->port_ != nullptr) {
    const PortCounters &counters = this->port_->counters();
    const std::atomic<uint32_t> *port_counters[] = {&counters.injected, &counters.tx_fail, &counters.bus_err,
                                                    &counters.recoveries};
    for (uint8_t i = KIND_INJECTED; i <= KIND_RECOVERIES; i++) {
      if (this->sensors_[i] != nullptr)
        this->sensors_[i]->publish_state(port_counters[i - KIND_INJECTED]->load(std::memory_order_relaxed));
    }
    if (this->sensors_[KIND_TEC] != nullptr || this->sensors_[KIND_REC] != nullptr) {
      uint16_t tec = 0, rec = 0;
      if (this->port_->read_error_counters(tec, rec)) {
        if (this->sensors_[KIND_TEC] != nullptr)
          this->sensors_[KIND_TEC]->publish_state(tec);
        if (this->sensors_[KIND_REC] != nullptr)
          this->sensors_[KIND_REC]->publish_state(rec);
      }
    }
#ifdef USE_CAN_GATEWAY_STATS
    if (this->sensors_[KIND_BUS_LOAD] != nullptr) {
      uint32_t now = millis();
      uint32_t rx = this->port_->rx_bits();
      uint32_t tx = this->port_->tx_bits();
      if (this->bus_load_seeded_) {
        uint32_t elapsed_ms = now - this->last_bus_load_ms_;
        uint32_t bits = (rx - this->last_bus_load_rx_bits_) + (tx - this->last_bus_load_tx_bits_);
        float capacity = static_cast<float>(this->port_->bit_rate()) * static_cast<float>(elapsed_ms) / 1000.0f;
        if (capacity > 0.0f)
          this->sensors_[KIND_BUS_LOAD]->publish_state(100.0f * static_cast<float>(bits) / capacity);
      }
      this->last_bus_load_rx_bits_ = rx;
      this->last_bus_load_tx_bits_ = tx;
      this->last_bus_load_ms_ = now;
      this->bus_load_seeded_ = true;
    }
#endif
    // Observation counters (v0.6): always present in PortCounters, zero unless a
    // consumer targets the port.
    if (this->sensors_[KIND_OBSERVED] != nullptr)
      this->sensors_[KIND_OBSERVED]->publish_state(counters.observed.load(std::memory_order_relaxed));
    if (this->sensors_[KIND_OBSERVE_OVERFLOW] != nullptr)
      this->sensors_[KIND_OBSERVE_OVERFLOW]->publish_state(counters.observe_overflow.load(std::memory_order_relaxed));
  }
}

void CanGatewaySensorHub::dump_config() {
  ESP_LOGCONFIG(TAG, "CAN gateway %s sensors:", this->route_ != nullptr ? "route" : "port");
  for (auto *sensor : this->sensors_) {
    if (sensor != nullptr)
      LOG_SENSOR("  ", "Counter", sensor);
  }
}
#endif

// ---------------------------------------------------------------------------
// Signal decode entities (v0.6) — loop-context, publish on change (B23)
// ---------------------------------------------------------------------------

#ifdef USE_CAN_GATEWAY_OBSERVE
#ifdef USE_SENSOR
void CanGatewayDecodeSensor::on_frame(const FrameView &view) {
  // Presence: the signal's highest byte must lie within this frame's DLC.
  uint8_t last_byte = this->bit_mode_ ? static_cast<uint8_t>((this->offset_ + this->length_ - 1) / 8)
                                      : static_cast<uint8_t>(this->offset_ + this->length_ - 1);
  if (last_byte >= view.dlc)
    return;
  uint32_t raw = this->bit_mode_ ? extract_bits(view.data, view.dlc, this->offset_, this->length_)
                                 : extract_bytes(view.data, view.dlc, this->offset_, this->length_, this->big_endian_);
  if (this->have_last_ && raw == this->last_raw_)
    return;  // publish on change
  uint32_t now = millis();
  if (this->throttle_ms_ > 0 && this->have_last_ && now - this->last_publish_ms_ < this->throttle_ms_)
    return;  // rate-limit a signal that changes every frame
  this->last_raw_ = raw;
  this->last_publish_ms_ = now;
  this->have_last_ = true;
  uint8_t width = this->bit_mode_ ? this->length_ : static_cast<uint8_t>(this->length_ * 8);
  // SNA is decided on the raw value, ahead of sign extension and of the sensor
  // filters: at that point 0xFF in an 8-bit signal is still the reserved marker
  // and not yet an ordinary -1. NaN publishes as unknown.
  if (raw_is_sna(raw, width, this->sna_mode_, this->sna_raw_)) {
    this->publish_state(NAN);
    return;
  }
  float value = this->is_signed_ ? static_cast<float>(sign_extend(raw, width)) : static_cast<float>(raw);
  this->publish_state(value);  // engineering-units scaling is left to sensor filters
}
#endif  // USE_SENSOR

#ifdef USE_BINARY_SENSOR
void CanGatewayDecodeBinarySensor::on_frame(const FrameView &view) {
  uint8_t byte_index = static_cast<uint8_t>(this->bit_ / 8);
  if (byte_index >= view.dlc)
    return;
  bool state = ((view.data[byte_index] >> (this->bit_ % 8)) & 0x01) != 0;
  if (this->have_last_ && state == this->last_state_)
    return;
  this->last_state_ = state;
  this->have_last_ = true;
  this->publish_state(state);
}
#endif  // USE_BINARY_SENSOR

#ifdef USE_TEXT_SENSOR
void CanGatewayDecodeTextSensor::on_frame(const FrameView &view) {
  uint8_t last_byte = static_cast<uint8_t>(this->offset_ + this->length_ - 1);
  if (last_byte >= view.dlc)
    return;
  uint32_t raw = extract_bytes(view.data, view.dlc, this->offset_, this->length_, this->big_endian_);
  if (this->have_last_ && raw == this->last_raw_)
    return;
  uint32_t now = millis();
  if (this->throttle_ms_ > 0 && this->have_last_ && now - this->last_publish_ms_ < this->throttle_ms_)
    return;
  this->last_raw_ = raw;
  this->last_publish_ms_ = now;
  this->have_last_ = true;
  for (uint16_t i = 0; i < this->map_count_; i++) {
    if (this->map_[i].key == raw) {
      this->publish_state(this->map_[i].value);
      return;
    }
  }
  // No mapping: publish the raw value as hex (like the last_frame snapshot).
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "0x%" PRIX32, raw);
  this->publish_state(buffer);
}
#endif  // USE_TEXT_SENSOR
#endif  // USE_CAN_GATEWAY_OBSERVE

}  // namespace esphome::can_gateway

#endif  // USE_ESP32
