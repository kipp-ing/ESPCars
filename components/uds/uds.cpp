#include "uds.h"

#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::uds {

static const char *const TAG = "uds";

// -------------------------------------------------------------------------------------------
// Allocation — all of it, once, from codegen statements that run ahead of App.setup()
// -------------------------------------------------------------------------------------------

void UdsHub::reserve_bindings(uint16_t count) {
  if (this->bindings_ != nullptr || count == 0)
    return;
  this->bindings_ = std::make_unique<UdsBinding[]>(count);
  this->binding_capacity_ = count;
}

void UdsHub::reserve_poll_slots(uint8_t count) {
  if (this->poll_entries_ != nullptr || count == 0)
    return;
  this->poll_entries_ = std::make_unique<PollEntry[]>(count);
  this->poll_capacity_ = count;
}

void UdsHub::reserve_text_slots(uint16_t count) {
  if (this->text_block_ != nullptr || count == 0)
    return;
  this->text_block_ = std::make_unique<char[]>(static_cast<size_t>(count) * UDS_TEXT_SLOT_CAP);
  this->text_capacity_ = count;
}

#ifdef USE_SENSOR
void UdsHub::add_sensor_binding(const char *group, const char *field, uint16_t element, uint32_t interval_ms,
                                sensor::Sensor *s) {
  if (this->binding_count_ >= this->binding_capacity_)
    return;  // codegen sizes the array from the same list; unreachable without a codegen bug
  UdsBinding &b = this->bindings_[this->binding_count_++];
  b.group_name = group;
  b.field_name = field;
  b.interval_ms = interval_ms;
  b.element = element;
  b.text = nullptr;
  b.value = NAN;
  b.sensor = s;
#ifdef USE_TEXT_SENSOR
  b.text_sensor = nullptr;
#endif
}
#endif

#ifdef USE_TEXT_SENSOR
void UdsHub::add_text_binding(const char *group, const char *field, uint16_t element, uint32_t interval_ms,
                              text_sensor::TextSensor *s) {
  if (this->binding_count_ >= this->binding_capacity_ || this->text_used_ >= this->text_capacity_)
    return;
  UdsBinding &b = this->bindings_[this->binding_count_++];
  b.group_name = group;
  b.field_name = field;
  b.interval_ms = interval_ms;
  b.element = element;
  b.text = this->text_block_.get() + static_cast<size_t>(this->text_used_++) * UDS_TEXT_SLOT_CAP;
  b.text[0] = '\0';
  b.value = NAN;
  b.text_sensor = s;
#ifdef USE_SENSOR
  b.sensor = nullptr;
#endif
}
#endif

// -------------------------------------------------------------------------------------------
// setup()
// -------------------------------------------------------------------------------------------

void UdsHub::setup() {
  this->sched_.init(this->poll_entries_.get(), this->poll_capacity_, this->max_failures_);

  if (!this->open_catalog_() || !this->resolve_ecu_()) {
    // Inert, deliberately: no requests, every entity unavailable, one explanatory line. A missing
    // or stale catalog is a flashing mistake, and the failure mode has to be "no data", never a
    // decode against bytes we do not understand.
    ESP_LOGE(TAG, "%s: catalog unavailable: %s — this hub will not transmit and its entities stay unavailable",
             this->log_name_, this->catalog_error_ != nullptr ? this->catalog_error_ : "unknown");
    this->mark_all_unavailable_();
    return;
  }

  if (this->expected_crc_ != 0 && this->expected_crc_ != this->catalog_.crc32()) {
    // Not an error: the catalog is a separately flashed artifact and may legitimately be newer
    // than the firmware. Names are what codegen emitted, so a newer catalog still resolves.
    ESP_LOGW(TAG, "%s: catalog CRC 0x%08" PRIX32 " differs from the 0x%08" PRIX32 " this firmware was built against",
             this->log_name_, this->catalog_.crc32(), this->expected_crc_);
  }

  for (uint16_t i = 0; i < this->binding_count_; i++) {
    UdsBinding &b = this->bindings_[i];
    b.resolved = this->resolve_binding_(b);
    if (!b.resolved) {
      // §3.2: one line per entity that failed to resolve, naming what is missing.
      ESP_LOGW(TAG, "%s: unresolved: field '%s' in group '%s' — entity unavailable", this->log_name_, b.field_name,
               b.group_name);
      b.valid = false;
      b.dirty = true;
    }
  }

  this->build_poll_table_();
  this->isotp_->set_consumer(this);
  this->log_report_();
}

bool UdsHub::open_catalog_() {
  const uint8_t *base = nullptr;
  size_t size = 0;

  if (this->embedded_ != nullptr) {
    base = this->embedded_;
    size = this->embedded_len_;
  } else if (this->partition_name_ != nullptr) {
#ifdef USE_ESP32
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, this->partition_name_);
    if (part == nullptr) {
      this->catalog_error_ = "no such data partition (flash the catalog, or check the partition name)";
      return false;
    }
    const void *ptr = nullptr;
    // Map the whole partition: total_size is inside the header, so the mapping has to exist before
    // it can be read, and open() then validates total_size against what we mapped.
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &this->mmap_handle_) != ESP_OK) {
      this->catalog_error_ = "esp_partition_mmap failed";
      return false;
    }
    base = static_cast<const uint8_t *>(ptr);
    size = part->size;
    this->mapped_size_ = size;
#else
    this->catalog_error_ = "a catalog partition needs an esp32 target; use 'embed: true'";
    return false;
#endif
  } else {
    this->catalog_error_ = "no catalog source configured";
    return false;
  }

  if (!this->catalog_.open(base, size)) {
    // open() refuses magic, version, table bounds, pool framing and the CRC. A blank partition
    // lands here on the magic check, which is exactly right: nothing has been flashed yet.
    this->catalog_error_ = "not a valid .dcat blob (bad magic, version, bounds or CRC)";
    return false;
  }
  return true;
}

bool UdsHub::resolve_ecu_() {
  if (!this->catalog_.is_open())
    return false;
  uint32_t index = 0;
  if (this->ecu_name_ != nullptr) {
    if (!this->catalog_.find(this->ecu_name_, NameKind::ECU, &index)) {
      this->catalog_error_ = "the configured 'ecu:' name is not in this catalog";
      return false;
    }
  } else if (this->catalog_.ecu_count() != 1) {
    this->catalog_error_ = "catalog holds several ECUs and no 'ecu:' was configured";
    return false;
  }
  if (!this->catalog_.ecu(index, &this->ecu_)) {
    this->catalog_error_ = "ECU record out of range";
    return false;
  }
  this->ecu_valid_ = true;
  this->p2_ms_ = this->ecu_.p2_ms != 0 ? this->ecu_.p2_ms : UDS_DEFAULT_P2_MS;
  this->p2_ext_ms_ = this->ecu_.p2_ext_ms != 0 ? this->ecu_.p2_ext_ms : UDS_DEFAULT_P2_EXT_MS;
  return true;
}

bool UdsHub::find_group_(const char *name, uint16_t *out) const {
  uint32_t gi = 0;
  if (name == nullptr || !this->catalog_.find(name, NameKind::GROUP, &gi))
    return false;
  // A group belongs to exactly one ECU, and a catalog may hold several. Binding to a group outside
  // our ECU would put another ECU's request bytes on our address pair.
  if (this->ecu_valid_ && (gi < this->ecu_.group_first || gi >= this->ecu_.group_first + this->ecu_.group_count))
    return false;
  *out = static_cast<uint16_t>(gi);
  return true;
}

bool UdsHub::resolve_binding_(UdsBinding &b) {
  uint16_t gi = 0;
  if (!this->find_group_(b.group_name, &gi))
    return false;
  GroupView g;
  if (!this->catalog_.group(gi, &g))
    return false;
  // Format §5.1a makes a group's field names unique, so a linear walk over its own fields is both
  // the cheapest and the only unambiguous lookup — the name index cannot distinguish two groups'
  // identically named fields, and codegen already decided which group this binding meant.
  for (uint16_t i = 0; i < g.field_count; i++) {
    const uint32_t fi = static_cast<uint32_t>(g.field_first) + i;
    FieldView f;
    if (!this->catalog_.field(fi, &f))
      return false;
    if (std::strcmp(f.name, b.field_name) != 0)
      continue;
    if (b.element >= f.repeat_count) {
      ESP_LOGW(TAG, "%s: field '%s' holds %u element(s); this catalog cannot serve element %u", this->log_name_,
               b.field_name, f.repeat_count, b.element);
      return false;
    }
    b.group_index = gi;
    b.field_index = static_cast<uint16_t>(fi);
    return true;
  }
  return false;
}

void UdsHub::build_poll_table_() {
  const uint32_t now = millis();
  for (uint16_t i = 0; i < this->binding_count_; i++) {
    const UdsBinding &b = this->bindings_[i];
    if (!b.resolved || b.interval_ms == UDS_INTERVAL_NEVER)
      continue;
    bool seen = false;
    for (uint16_t j = 0; j < i && !seen; j++) {
      const UdsBinding &e = this->bindings_[j];
      seen = e.resolved && e.interval_ms != UDS_INTERVAL_NEVER && e.group_index == b.group_index;
    }
    if (seen)
      continue;
    // Design §5: the group's interval is the minimum across the entities bound to it, and every
    // one of them publishes from each response. Per-sensor due times against a shared request
    // would put the same three bytes on the wire once per sensor.
    uint32_t interval = b.interval_ms;
    for (uint16_t j = i + 1; j < this->binding_count_; j++) {
      const UdsBinding &e = this->bindings_[j];
      if (e.resolved && e.interval_ms != UDS_INTERVAL_NEVER && e.group_index == b.group_index &&
          e.interval_ms < interval)
        interval = e.interval_ms;
    }
    if (!this->sched_.add(b.group_index, interval, now))
      ESP_LOGW(TAG, "%s: poll table full at %u groups; '%s' will not be polled", this->log_name_,
               this->sched_.capacity(), b.group_name);
  }
}

void UdsHub::log_report_() {
  uint16_t resolved = 0;
  for (uint16_t i = 0; i < this->binding_count_; i++)
    resolved = static_cast<uint16_t>(resolved + (this->bindings_[i].resolved ? 1 : 0));
  // The hub's own name leads, because two hubs reading one catalog otherwise emit a byte-identical
  // line here — which is exactly what happened on the bench: the only way to tell them apart was to
  // count the bindings.
  ESP_LOGI(TAG,
           "%s: catalog ok: ECU '%s' 0x%03" PRIX32 "/0x%03" PRIX32 ", %u groups, %u fields; %u/%u bindings resolved, "
           "%u group(s) polled",
           this->log_name_, this->ecu_.name, this->ecu_.request_id, this->ecu_.response_id,
           this->catalog_.group_count(), this->catalog_.field_count(), resolved, this->binding_count_,
           this->sched_.size());
}

// -------------------------------------------------------------------------------------------
// loop() — issue one request, then publish a bounded slice of what arrived (§4.1)
// -------------------------------------------------------------------------------------------

void UdsHub::loop() {
  if (this->catalog_.is_open()) {
    const uint32_t now = millis();
    // P2 bounds the START of the response, not its completion (ISO 14229). Once the transport
    // is mid-reassembly, the answer has started: a multi-block response paced at the granted
    // STmin of 20 ms can easily run past half a second of consecutive frames, every one of them
    // on time. So while isotp is receiving, the deadline slides instead of expiring — it cannot
    // slide forever, because N_Cr bounds every inter-frame gap and its failure releases this
    // slot through on_error(). Without this, the client killed every response longer than one
    // flow-control block (~8 CFs ≈ P2 at that pacing) and blamed the ECU: the bench measured
    // exactly that on 2026-07-30, against the rig and a real battery alike, and the completed
    // answers then landed as foreign_responses on a freed slot.
    if (this->in_flight_ != InFlight::NONE && this->isotp_->receiving()) {
      if (this->in_flight_ == InFlight::POLLED) {
        this->sched_.extend_deadline(now, this->p2_ms_);
      } else {
        this->own_deadline_ms_ = now + this->p2_ms_;
      }
    }
    if (this->in_flight_ == InFlight::POLLED) {
      // The scheduler owns this deadline, and it is the only signal a silent ECU gives: isotp's
      // N_Bs/N_Cr timers run only during a transfer, so total silence yields no IsoTpError at all.
      const SchedTimeout to = this->sched_.poll(now);
      if (to.slot != SCHED_NONE) {
        const uint16_t gi = this->sched_.entry(static_cast<uint8_t>(to.slot)).group_index;
        this->timeouts_++;
        this->finish_in_flight_();
        if (to.publish_unavailable)
          this->publish_group_unavailable_(gi);
        this->report_group_error_(gi, "response timeout", 0);
      }
    } else if (this->in_flight_ != InFlight::NONE && sched_time_reached(now, this->own_deadline_ms_)) {
      const uint16_t gi = this->in_flight_group_;
      this->timeouts_++;
      this->finish_in_flight_();
      this->report_group_error_(gi, "response timeout (on demand)", 0);
    }

    if (this->in_flight_ == InFlight::NONE && this->enabled_)
      this->issue_next_(now);
  }
  // Runs even for an inert hub: that is how the entities of a catalog that failed to open, or of a
  // binding that failed to resolve, reach Home Assistant as unavailable rather than as nothing.
  this->publish_dirty_();
}

void UdsHub::issue_next_(uint32_t now) {
  // An operator's explicit action outranks the poller; a refused send is backpressure, so the same
  // bytes are simply re-offered on the next iteration.
  if (this->raw_pending_len_ != 0) {
    if (!this->isotp_->send(this->raw_pending_, this->raw_pending_len_))
      return;
    std::memcpy(this->req_, this->raw_pending_, this->raw_pending_len_);
    this->req_len_ = this->raw_pending_len_;
    this->raw_pending_len_ = 0;
    this->in_flight_ = InFlight::RAW;
    this->in_flight_group_ = 0xFFFF;
    this->in_flight_resp_min_ = 0;
    this->own_deadline_ms_ = now + this->p2_ms_;
    this->requests_sent_++;
    return;
  }

  if (this->ondemand_count_ != 0) {
    const uint16_t gi = this->ondemand_[this->ondemand_head_];
    const SendResult result = this->send_group_(gi);
    if (result == SendResult::BUSY)
      return;  // the identical request is re-offered next iteration
    this->ondemand_head_ = static_cast<uint8_t>((this->ondemand_head_ + 1) % UDS_ONDEMAND_QUEUE);
    this->ondemand_count_--;
    if (result == SendResult::UNSENDABLE)
      return;  // dropped: retrying would log once per loop and never succeed
    this->in_flight_ = InFlight::ONDEMAND;
    this->own_deadline_ms_ = now + this->p2_ms_;
    return;
  }

  const int16_t slot = this->sched_.next(now, this->p2_ms_);
  if (slot == SCHED_NONE)
    return;
  const uint16_t gi = this->sched_.entry(static_cast<uint8_t>(slot)).group_index;
  const SendResult result = this->send_group_(gi);
  if (result == SendResult::BUSY) {
    // Release the slot without judging it: isotp refuses while another transfer is in flight, and
    // that is backpressure, not an answer the ECU failed to give. on_failure() here would apply a
    // doubling backoff and an unavailability count to a request that was never asked.
    this->sched_.cancel_in_flight();
    return;
  }
  if (result == SendResult::UNSENDABLE) {
    // Suspend rather than back off: the request bytes will not become assemblable, so this is the
    // same "stop asking" case as NRC 0x31, and it keeps the warning to one line for the run.
    this->sched_.suspend(static_cast<uint8_t>(slot));
    this->groups_suspended_++;
    this->publish_group_unavailable_(gi);
    return;
  }
  this->in_flight_ = InFlight::POLLED;
  this->in_flight_slot_ = static_cast<uint8_t>(slot);
}

UdsHub::SendResult UdsHub::send_group_(uint16_t group_index) {
  GroupView g;
  if (!this->catalog_.group(group_index, &g)) {
    ESP_LOGE(TAG, "%s: group index %u is outside the catalog", this->log_name_, group_index);
    return SendResult::UNSENDABLE;
  }
  const uint8_t n = build_request(this->catalog_, g, this->req_, sizeof(this->req_));
  if (n == 0) {
    // build_request() also refuses a group whose `sid` disagrees with its first request byte — a
    // compiler bug that response validation would otherwise judge with the wrong SID.
    ESP_LOGE(TAG, "%s: group '%s' has a %u-byte request this build cannot assemble (limit %u); not asking again",
             this->log_name_, g.name, g.req_len, static_cast<unsigned>(UDS_MAX_REQUEST));
    return SendResult::UNSENDABLE;
  }
  if (!this->isotp_->send(this->req_, n)) {
    this->req_len_ = 0;  // nothing on the wire, so nothing may be matched against these bytes
    return SendResult::BUSY;
  }
  this->req_len_ = n;
  this->in_flight_group_ = group_index;
  this->in_flight_resp_min_ = g.resp_min_len;
  this->pending_streak_ = 0;
  this->requests_sent_++;
  return SendResult::SENT;
}

void UdsHub::finish_in_flight_() {
  this->in_flight_ = InFlight::NONE;
  this->req_len_ = 0;
  this->pending_streak_ = 0;
}

// -------------------------------------------------------------------------------------------
// on_message() — inside can_gateway's drain: validate, decode into slots, publish nothing
// -------------------------------------------------------------------------------------------

void UdsHub::on_message(const isotp::MessageView &msg) {
  if (this->in_flight_ == InFlight::NONE || this->req_len_ == 0) {
    this->foreign_responses_++;
    return;
  }
  const uint32_t now = millis();
  const ResponseCheck rc = classify_response(this->req_, this->req_len_, msg.data, msg.size, this->in_flight_resp_min_);
  switch (rc.verdict) {
    case RespVerdict::NOT_OURS:
      // A stale answer to the previous request, or another tester's. Counted and ignored — it must
      // never complete the request we are actually waiting for.
      this->foreign_responses_++;
      return;
    case RespVerdict::TOO_SHORT:
      // Too short to check the SID echo against, so it cannot complete the request either. The
      // deadline still runs, and a real answer may yet arrive.
      this->short_responses_++;
      return;
    case RespVerdict::NEGATIVE:
      this->negative_responses_++;
      this->last_nrc_ = rc.nrc;
      this->handle_negative_(rc, now);
      return;
    case RespVerdict::PARTIAL:
    case RespVerdict::ACCEPT:
      break;
  }

  // PARTIAL is a success. An ECU answering with fewer array elements than its database declares is
  // ordinary — a real factory database can model more elements than a particular unit has wired —
  // so the covered fields decode, the uncovered ones are skipped and counted, and the group's
  // failure streak resets exactly as it would for a full response.
  const bool partial = rc.verdict == RespVerdict::PARTIAL;
  this->responses_accepted_++;
  if (partial)
    this->partial_responses_++;
  if (this->in_flight_ == InFlight::RAW) {
    ESP_LOGD(TAG, "%s: raw response accepted, %u bytes", this->log_name_, msg.size);
  } else {
    this->decode_group_(this->in_flight_group_, msg.data, msg.size, partial);
  }
  if (this->in_flight_ == InFlight::POLLED)
    this->sched_.on_success(this->in_flight_slot_, now);
  this->finish_in_flight_();
}

void UdsHub::handle_negative_(const ResponseCheck &rc, uint32_t now) {
  switch (rc.nrc_class) {
    case NrcClass::PENDING:
      // 0x78: correctly received, response pending. Extend and keep waiting — but bounded, or an
      // ECU that answers nothing else holds the single in-flight slot and stops every group.
      if (++this->pending_streak_ > UDS_MAX_PENDING) {
        this->fail_in_flight_(now, "too many 'response pending' answers", rc.nrc);
        return;
      }
      if (this->in_flight_ == InFlight::POLLED) {
        this->sched_.extend_deadline(now, this->p2_ext_ms_);
      } else {
        this->own_deadline_ms_ = now + this->p2_ext_ms_;
      }
      return;
    case NrcClass::UNSUPPORTED:
    case NrcClass::DENIED:
      // Permanent. Polling a DID this ECU does not implement forever is how a diagnostic client
      // makes itself a nuisance on a bus (design §4).
      this->suspend_in_flight_(rc.nrc);
      return;
    default:  // BUSY, OTHER
      this->fail_in_flight_(now, "negative response", rc.nrc);
      return;
  }
}

void UdsHub::fail_in_flight_(uint32_t now, const char *what, uint8_t nrc) {
  const uint16_t gi = this->in_flight_group_;
  if (this->in_flight_ == InFlight::POLLED) {
    if (this->sched_.on_failure(this->in_flight_slot_, now))
      this->publish_group_unavailable_(gi);
  }
  this->finish_in_flight_();
  this->report_group_error_(gi, what, nrc);
}

void UdsHub::suspend_in_flight_(uint8_t nrc) {
  const uint16_t gi = this->in_flight_group_;
  GroupView g;
  const char *name = this->catalog_.group(gi, &g) ? g.name : "?";
  if (this->in_flight_ == InFlight::POLLED) {
    this->sched_.suspend(this->in_flight_slot_);
    this->groups_suspended_++;
    ESP_LOGW(TAG, "%s: group '%s' answered NRC 0x%02X — permanently unsupported, no longer polled", this->log_name_,
             name, nrc);
  } else {
    ESP_LOGW(TAG, "%s: group '%s' answered NRC 0x%02X", this->log_name_, name, nrc);
  }
  this->publish_group_unavailable_(gi);
  this->finish_in_flight_();
  this->report_group_error_(gi, "group unsupported", nrc);
}

void UdsHub::on_error(isotp::IsoTpError error) {
  if (this->in_flight_ == InFlight::NONE)
    return;  // a transport failure on someone else's transfer; isotp already logged it
  this->fail_in_flight_(millis(), "transport error", 0);
}

// -------------------------------------------------------------------------------------------
// Decode (in the callback) and publish (from loop) — the §4.1 split
// -------------------------------------------------------------------------------------------

void UdsHub::decode_group_(uint16_t group_index, const uint8_t *resp, size_t resp_len, bool partial) {
  for (uint16_t i = 0; i < this->binding_count_; i++) {
    UdsBinding &b = this->bindings_[i];
    if (!b.resolved || b.group_index != group_index)
      continue;
    FieldView f;
    if (!this->catalog_.field(b.field_index, &f))
      continue;
    // `resp_min_len` is the fast-path hint: at or above it every field of the group is covered, so
    // only a short response needs the per-field coverage question asked at all (format §4).
    if (partial && !field_covered(f, resp_len, b.element)) {
      this->note_uncovered_(b);
      continue;
    }
    if (b.text != nullptr) {
      this->decode_text_(b, f, resp, resp_len);
    } else {
      const Decoded d = decode_numeric(this->catalog_, f, resp, resp_len, b.element);
      b.value = d.value;
      b.valid = d.valid;
      if (!d.valid)
        this->decode_unmatched_++;
    }
    b.uncovered = false;
    b.dirty = true;
  }
}

void UdsHub::note_uncovered_(UdsBinding &b) {
  if (b.uncovered)
    return;  // already reported and already published unavailable — do not flap
  b.uncovered = true;
  b.valid = false;
  b.dirty = true;
  this->fields_uncovered_++;
  if (!b.uncovered_logged) {
    b.uncovered_logged = true;
    ESP_LOGW(TAG, "%s: field '%s'%s is past the end of what this ECU answers for group '%s' — unavailable",
             this->log_name_, b.field_name, b.element != 0 ? " (an array element)" : "", b.group_name);
  }
}

void UdsHub::decode_text_(UdsBinding &b, const FieldView &f, const uint8_t *resp, size_t resp_len) {
  b.text[0] = '\0';
  b.valid = false;
  if ((f.flags & FIELD_FLAG_ASCII) != 0) {
    b.valid = extract_ascii(resp, resp_len, f, b.element, b.text, UDS_TEXT_SLOT_CAP);
  } else if ((f.flags & FIELD_FLAG_HEXDUMP) != 0) {
    b.valid = extract_hexdump(resp, resp_len, f, b.element, b.text, UDS_TEXT_SLOT_CAP) != 0;
  } else {
    const Decoded d = decode_numeric(this->catalog_, f, resp, resp_len, b.element);
    if (d.text != nullptr)
      copy_bounded_(b.text, UDS_TEXT_SLOT_CAP, d.text);
    // A matched INVALID row still carries its text ("Signal not available"), and for a text entity
    // that text *is* the honest state — so it publishes, while the decode still counts as
    // unmatched for the diagnostics counter.
    b.valid = d.text != nullptr;
  }
  if (!b.valid)
    this->decode_unmatched_++;
}

void UdsHub::publish_dirty_() {
  if (this->binding_count_ == 0)
    return;
  uint8_t published = 0;
  // A round-robin cursor rather than a scan from zero: a long response marks many slots dirty at
  // once, and restarting at zero every iteration would starve the tail of the table.
  for (uint16_t examined = 0; examined < this->binding_count_ && published < UDS_PUBLISH_PER_LOOP; examined++) {
    UdsBinding &b = this->bindings_[this->publish_cursor_];
    this->publish_cursor_ = static_cast<uint16_t>((this->publish_cursor_ + 1) % this->binding_count_);
    if (!b.dirty)
      continue;
    b.dirty = false;
    published++;
    FieldView f{};
    const bool have_field = b.resolved && this->catalog_.field(b.field_index, &f);
#ifdef USE_TEXT_SENSOR
    if (b.text != nullptr && b.text_sensor != nullptr) {
      // A text entity has no NaN. An unavailable one publishes the empty string, which Home
      // Assistant shows as blank rather than as a stale reading.
      b.text_sensor->publish_state(b.valid ? b.text : "");
    }
#endif
#ifdef USE_SENSOR
    if (b.text == nullptr && b.sensor != nullptr)
      b.sensor->publish_state(b.valid ? b.value : NAN);
#endif
    if (this->value_callback_.empty())
      continue;
    const UdsValue v{b.field_name, have_field ? f.unit : "", b.text != nullptr ? b.text : "", b.valid ? b.value : NAN,
                     b.valid};
    this->value_callback_.call(v);
  }
}

void UdsHub::publish_group_unavailable_(uint16_t group_index) {
  for (uint16_t i = 0; i < this->binding_count_; i++) {
    UdsBinding &b = this->bindings_[i];
    if (!b.resolved || b.group_index != group_index)
      continue;
    b.valid = false;
    b.dirty = true;
  }
}

void UdsHub::mark_all_unavailable_() {
  for (uint16_t i = 0; i < this->binding_count_; i++) {
    this->bindings_[i].valid = false;
    this->bindings_[i].dirty = true;
  }
}

void UdsHub::report_error_(const char *what, uint8_t nrc) {
  if (nrc != 0) {
    ESP_LOGW(TAG, "%s: %s (NRC 0x%02X)", this->log_name_, what, nrc);
  } else {
    ESP_LOGW(TAG, "%s: %s", this->log_name_, what);
  }
  this->error_callback_.call(UdsError{what, nrc});
}

void UdsHub::report_group_error_(uint16_t group_index, const char *what, uint8_t nrc) {
  GroupView g;
  if (group_index == 0xFFFF || !this->catalog_.group(group_index, &g)) {
    // uds.raw, or a group the catalog cannot resolve any more: nothing truthful to name.
    this->report_error_(what, nrc);
    return;
  }
  // Group first: the hub prefix in report_error_ says who was asking, this says for what.
  char msg[96];
  snprintf(msg, sizeof(msg), "group '%s': %s", g.name, what);
  this->report_error_(msg, nrc);
}

void UdsHub::copy_bounded_(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

// -------------------------------------------------------------------------------------------
// Actions
// -------------------------------------------------------------------------------------------

bool UdsHub::enqueue_(uint16_t group_index) {
  if (this->ondemand_count_ >= UDS_ONDEMAND_QUEUE) {
    ESP_LOGW(TAG, "%s: on-demand queue full (%u); request dropped", this->log_name_, UDS_ONDEMAND_QUEUE);
    return false;
  }
  const uint8_t tail = static_cast<uint8_t>((this->ondemand_head_ + this->ondemand_count_) % UDS_ONDEMAND_QUEUE);
  this->ondemand_[tail] = group_index;
  this->ondemand_count_++;
  return true;
}

bool UdsHub::request_read(const char *group_name) {
  if (!this->catalog_.is_open()) {
    ESP_LOGW(TAG, "%s: uds.read ignored: catalog unavailable", this->log_name_);
    return false;
  }
  uint16_t gi = 0;
  GroupView g;
  if (!this->find_group_(group_name, &gi) || !this->catalog_.group(gi, &g)) {
    ESP_LOGW(TAG, "%s: uds.read: group '%s' is not in this catalog", this->log_name_, group_name);
    return false;
  }
  // The design's floor: uds.read accepts SAFE_READ groups only, whatever allow_active_services
  // says. The flag covers only service identifiers that cannot change ECU state.
  if ((g.flags & GROUP_FLAG_SAFE_READ) == 0) {
    ESP_LOGW(TAG, "%s: uds.read refused: group '%s' is not a safe read — use uds.execute", this->log_name_, g.name);
    return false;
  }
  return this->enqueue_(gi);
}

bool UdsHub::request_execute(const char *group_name) {
  if (!this->allow_active_services_) {
    ESP_LOGW(TAG, "%s: uds.execute refused: this hub sets allow_active_services: false", this->log_name_);
    return false;
  }
  if (!this->catalog_.is_open()) {
    ESP_LOGW(TAG, "%s: uds.execute ignored: catalog unavailable", this->log_name_);
    return false;
  }
  uint16_t gi = 0;
  if (!this->find_group_(group_name, &gi)) {
    ESP_LOGW(TAG, "%s: uds.execute: group '%s' is not in this catalog", this->log_name_, group_name);
    return false;
  }
  return this->enqueue_(gi);
}

bool UdsHub::send_raw(const uint8_t *data, size_t len) {
  if (!this->allow_active_services_) {
    ESP_LOGW(TAG, "%s: uds.raw refused: this hub sets allow_active_services: false", this->log_name_);
    return false;
  }
  if (data == nullptr || len == 0 || len > UDS_MAX_REQUEST) {
    ESP_LOGW(TAG, "%s: uds.raw refused: %u bytes is outside 1..%u", this->log_name_, static_cast<unsigned>(len),
             static_cast<unsigned>(UDS_MAX_REQUEST));
    return false;
  }
  if (this->raw_pending_len_ != 0) {
    ESP_LOGW(TAG, "%s: uds.raw refused: a raw request is already queued", this->log_name_);
    return false;
  }
  std::memcpy(this->raw_pending_, data, len);
  this->raw_pending_len_ = static_cast<uint8_t>(len);
  return true;
}

void UdsHub::set_enabled(bool enabled) {
  if (this->enabled_ == enabled)
    return;
  this->enabled_ = enabled;
  ESP_LOGI(TAG, "%s: polling %s", this->log_name_, enabled ? "enabled" : "disabled");
}

// -------------------------------------------------------------------------------------------
// dump_config — §3.2: state what was FOUND, not what was configured
// -------------------------------------------------------------------------------------------

void UdsHub::dump_config() {
  ESP_LOGCONFIG(TAG, "UDS client '%s':", this->log_name_);
  if (!this->catalog_.is_open()) {
    ESP_LOGE(TAG, "  Catalog UNAVAILABLE: %s", this->catalog_error_ != nullptr ? this->catalog_error_ : "unknown");
    ESP_LOGE(TAG, "  This hub sends nothing and every entity stays unavailable.");
    return;
  }
  if (this->embedded_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Catalog: embedded, %u bytes", static_cast<unsigned>(this->embedded_len_));
  } else {
    ESP_LOGCONFIG(TAG, "  Catalog: partition '%s', %u bytes mapped", this->partition_name_,
                  static_cast<unsigned>(this->mapped_size_));
  }
  ESP_LOGCONFIG(TAG, "  Catalog size: %" PRIu32 " bytes, format version %u", this->catalog_.total_size(),
                CATALOG_FORMAT_VERSION);
  if (this->expected_crc_ == 0) {
    ESP_LOGCONFIG(TAG, "  CRC: 0x%08" PRIX32 " (no config-time value to compare)", this->catalog_.crc32());
  } else if (this->expected_crc_ == this->catalog_.crc32()) {
    ESP_LOGCONFIG(TAG, "  CRC: 0x%08" PRIX32 " (matches the config-time catalog)", this->catalog_.crc32());
  } else {
    ESP_LOGW(TAG,
             "  CRC: 0x%08" PRIX32 " DIFFERS from the config-time 0x%08" PRIX32 " — the flashed catalog is not "
             "the one this YAML validated against",
             this->catalog_.crc32(), this->expected_crc_);
  }
  ESP_LOGCONFIG(TAG, "  ECU: '%s', request 0x%03" PRIX32 ", response 0x%03" PRIX32 "%s", this->ecu_.name,
                this->ecu_.request_id, this->ecu_.response_id,
                (this->ecu_.flags & ECU_FLAG_EXTENDED_ID) != 0 ? " (29-bit)" : "");
  ESP_LOGCONFIG(TAG, "  Timing: P2 %u ms, P2* %u ms; catalog suggests block size %u, STmin raw 0x%02X", this->p2_ms_,
                this->p2_ext_ms_, this->ecu_.block_size, this->ecu_.st_min_raw);
  ESP_LOGCONFIG(TAG, "  Catalog holds %u groups and %u fields", this->catalog_.group_count(),
                this->catalog_.field_count());
  ESP_LOGCONFIG(TAG, "  Bindings: %u of %u reserved", this->binding_count_, this->binding_capacity_);
  ESP_LOGCONFIG(TAG, "  Polled groups: %u of %u slots", this->sched_.size(), this->sched_.capacity());
  for (uint8_t i = 0; i < this->sched_.size(); i++) {
    const PollEntry &e = this->sched_.entry(i);
    GroupView g;
    if (this->catalog_.group(e.group_index, &g))
      ESP_LOGCONFIG(TAG, "    '%s' (DID 0x%04X, response >= %u B) every %" PRIu32 " ms", g.name, g.did, g.resp_min_len,
                    e.interval_ms);
  }
  uint16_t uncovered = 0;
  for (uint16_t i = 0; i < this->binding_count_; i++) {
    const UdsBinding &b = this->bindings_[i];
    if (!b.resolved)
      ESP_LOGE(TAG, "  UNRESOLVED: field '%s' in group '%s' — this entity stays unavailable", b.field_name,
               b.group_name);
    if (b.uncovered)
      uncovered = static_cast<uint16_t>(uncovered + 1);
  }
  // Reported even though it is not an error: a config that reads array elements the ECU does not
  // send should say so on every boot, or "unavailable" looks like a wiring fault.
  if (this->partial_responses_ != 0 || uncovered != 0)
    ESP_LOGCONFIG(TAG, "  Partial responses: %" PRIu32 " (%u binding(s) never covered)", this->partial_responses_,
                  uncovered);
  ESP_LOGCONFIG(TAG, "  Max consecutive failures: %u", this->max_failures_);
  ESP_LOGCONFIG(TAG, "  Active services: %s", this->allow_active_services_ ? "ALLOWED" : "refused");
}

#ifdef USE_SENSOR
void UdsDiagnostics::publish_changed_(sensor::Sensor *s, uint32_t value, uint32_t &last) {
  if (s == nullptr || value == last)
    return;
  last = value;
  s->publish_state(static_cast<float>(value));
}

void UdsDiagnostics::update() {
  UdsHub *p = this->parent_;
  publish_changed_(this->requests_sent_sensor_, p->requests_sent(), this->last_requests_sent_);
  publish_changed_(this->responses_accepted_sensor_, p->responses_accepted(), this->last_responses_accepted_);
  publish_changed_(this->timeouts_sensor_, p->timeouts(), this->last_timeouts_);
  publish_changed_(this->negative_responses_sensor_, p->negative_responses(), this->last_negative_responses_);
  publish_changed_(this->decode_unmatched_sensor_, p->decode_unmatched(), this->last_decode_unmatched_);
  publish_changed_(this->groups_suspended_sensor_, p->groups_suspended(), this->last_groups_suspended_);
  publish_changed_(this->partial_responses_sensor_, p->partial_responses(), this->last_partial_responses_);
  publish_changed_(this->fields_uncovered_sensor_, p->fields_uncovered(), this->last_fields_uncovered_);
  publish_changed_(this->last_nrc_sensor_, p->last_nrc(), this->last_last_nrc_);
}

void UdsDiagnostics::dump_config() {
  ESP_LOGCONFIG(TAG, "UDS diagnostics for '%s':", this->parent_->log_name());
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Requests sent", this->requests_sent_sensor_);
  LOG_SENSOR("  ", "Responses accepted", this->responses_accepted_sensor_);
  LOG_SENSOR("  ", "Timeouts", this->timeouts_sensor_);
  LOG_SENSOR("  ", "Negative responses", this->negative_responses_sensor_);
  LOG_SENSOR("  ", "Decode unmatched", this->decode_unmatched_sensor_);
  LOG_SENSOR("  ", "Groups suspended", this->groups_suspended_sensor_);
  LOG_SENSOR("  ", "Partial responses", this->partial_responses_sensor_);
  LOG_SENSOR("  ", "Fields uncovered", this->fields_uncovered_sensor_);
  LOG_SENSOR("  ", "Last NRC", this->last_nrc_sensor_);
}
#endif

}  // namespace esphome::uds
