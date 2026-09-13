#pragma once

/// The ESPHome-facing half of the `uds` diagnostic client (docs/DESIGN-uds.md §7a).
///
/// Everything decode-shaped already lives in the four freestanding headers below and is tested on
/// the host; this file is the glue that gives them a bus, a clock and entities. It owns exactly
/// three things the freestanding layer cannot: the catalog's mapping (an `esp_partition_mmap`ed
/// partition or a generated `const uint8_t[]`), the binding table that ties a catalog field to an
/// ESPHome entity, and the split between decoding and publishing.
///
/// **That split is the reason this class is shaped the way it is** (design §4.1).
/// `IsoTpProtocol::deliver_message_()` calls its consumer from inside can_gateway's bounded
/// observation drain, so `on_message()` may only parse and buffer. A single group's response
/// decoded and published from there could push dozens of entity states through the API/MQTT/logger
/// stack in one drain.
/// So `on_message()` decodes into per-binding value slots and marks them dirty, and `loop()`
/// publishes at most `UDS_PUBLISH_PER_LOOP` of them. The slots are `float`s, not a copy of the
/// response: copying a wide response out would defer exactly as much work for ten times the RAM,
/// and the decode would still have to happen somewhere.
///
/// Allocation happens once, from the `reserve_*()` setters codegen emits ahead of `App.setup()`,
/// and never again — a diagnostic session runs for weeks and must not fragment the heap.

#include "esphome/components/isotp/isotp.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_ESP32
#include <esp_partition.h>
#endif

#include "uds_catalog.h"
#include "uds_decode.h"
#include "uds_proto.h"
#include "uds_sched.h"

#include <functional>
#include <memory>
#include <vector>

namespace esphome::uds {

/// §4.1: at most this many entity states leave per loop iteration.
static constexpr uint8_t UDS_PUBLISH_PER_LOOP = 8;

/// One text binding's value slot. Format §7a caps a byte block at 248 bits (31 bytes) and HEXDUMP
/// renders two characters per byte, so 62 characters plus the terminator is the widest a
/// conforming catalog can ask for; codegen refuses anything that would not fit.
static constexpr size_t UDS_TEXT_SLOT_CAP = 64;

/// Widest request this glue assembles. Every service the SAFE_READ flag admits is 2-4 bytes; a
/// `uds.raw` payload longer than this is refused at the action rather than silently truncated.
static constexpr size_t UDS_MAX_REQUEST = 32;

/// `update_interval: never` — the binding is decoded whenever its group is read on demand, and the
/// group is never added to the poll table (design §5).
static constexpr uint32_t UDS_INTERVAL_NEVER = 0;

/// On-demand requests (`uds.read`, `uds.execute`) wait here for the one in-flight slot. Four deep
/// because the actions are an operator's control plane, not a data path: a fifth queued read is a
/// config that should have declared an `update_interval`.
static constexpr uint8_t UDS_ONDEMAND_QUEUE = 4;

/// How many consecutive `0x78` "response pending" answers are honoured before the request is
/// failed. The design calls for "bounded retries": each 0x78 re-arms the deadline, so without a
/// bound an ECU that answers nothing but 0x78 holds the hub's single in-flight slot forever — and
/// because in-flight is global, that stops every group from being polled, not just this one.
static constexpr uint8_t UDS_MAX_PENDING = 5;

/// Fallbacks for a catalog whose ECU record carries no timing (`p2_ms == 0`). ISO 14229's default
/// P2 is 50 ms; 150/2000 is what a real factory database has been seen to declare and a safe floor
/// for a client that would otherwise arm a zero-length deadline and fail every request instantly.
static constexpr uint16_t UDS_DEFAULT_P2_MS = 150;
static constexpr uint16_t UDS_DEFAULT_P2_EXT_MS = 2000;

/// The `on_value` trigger's argument. Small and trivially copyable, like isotp's MessageView: it is
/// passed by value into YAML lambdas, where a std::string member would cost a heap allocation per
/// published value. `field` and `unit` point into the catalog (flash) and live as long as the
/// mapping; `text` points into the binding's own slot and is valid for the duration of the call.
struct UdsValue {
  const char *field;
  const char *unit;
  const char *text;  ///< the enum/sentinel text, or "" for a numeric field
  float value;       ///< NaN when !valid
  bool valid;
};

/// The `on_error` trigger's argument. `what` is a static string, never a formatted buffer, so the
/// error path allocates nothing; `nrc` is the raw negative response code, 0 when the failure was a
/// timeout or a transport error.
struct UdsError {
  const char *what;
  uint8_t nrc;
};

/// One entity bound to one catalog field.
///
/// A plain struct owned by the hub rather than a component per entity (design §7a): the entities
/// are ordinary ESPHome sensors registered by their own platform, and this is just what the hub
/// iterates. `group_name`/`field_name` are the *names* codegen emitted — never indices — because
/// the flashed catalog may legitimately be newer than the firmware, and a name that no longer
/// resolves must cost one log line and an unavailable entity rather than a wrong value.
struct UdsBinding {
  const char *group_name;
  const char *field_name;
  uint32_t interval_ms;  ///< UDS_INTERVAL_NEVER for a read-on-demand binding
  uint16_t element;      ///< index into a collapsed array (format §5.1b); 0 for a scalar
  uint16_t group_index;  ///< resolved in setup()
  uint16_t field_index;
  char *text;  ///< slice of the hub's text block; nullptr for a numeric binding
  float value;
  bool resolved;
  bool dirty;
  bool valid;
  /// The ECU has answered this group, but never with enough bytes to cover this field. Logged once
  /// with the field's name and then left alone: a factory database can declare more array elements
  /// than a given unit has wired, so a large share of bindings can land in this state at once, and
  /// one line each is a report while one line each per poll is a flood.
  bool uncovered;
  bool uncovered_logged;
#ifdef USE_SENSOR
  sensor::Sensor *sensor;
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text_sensor;
#endif
};

/// One diagnostic client: a catalog, an ISO-TP address pair, a poll table and a set of entities.
class UdsHub : public Component, public isotp::IsoTpConsumer {
 public:
  /// The transport is required and never changes, so it is a constructor parameter rather than a
  /// setter (project convention: no partially-initialised object).
  explicit UdsHub(isotp::IsoTpProtocol *isotp) : isotp_(isotp) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  /// After isotp (DATA), because the transport allocates its reassembly buffers in its own
  /// setup() and this hub hands it a consumer pointer.
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  // -- configuration, all set by codegen before setup() ------------------------------------

  /// Catalog source. Exactly one of these is called; both end at `Catalog::open(base, size)`, so
  /// one reader is under test either way (design §2).
  void set_catalog_partition(const char *name) { this->partition_name_ = name; }
  void set_catalog_embedded(const uint8_t *data, size_t len) {
    this->embedded_ = data;
    this->embedded_len_ = len;
  }
  void set_expected_crc(uint32_t crc) { this->expected_crc_ = crc; }
  void set_ecu_name(const char *name) { this->ecu_name_ = name; }
  /// The hub's YAML id, so every line this hub logs names which hub said it.
  ///
  /// Not cosmetic, and not optional. A config that talks to two ECUs runs two hubs under one `uds`
  /// tag, and without this their lines are indistinguishable — two hubs reading the same catalog
  /// even emit a byte-identical `catalog ok:` line. On the bench that cost a diagnosis: the hub
  /// whose YAML happened to carry an `on_value` lambda was the only one anything could be attributed
  /// to, and the silence of the other one was read as the component failing rather than as the log
  /// having no way to say "this one is fine too".
  void set_log_name(const char *name) { this->log_name_ = name; }
  void set_max_consecutive_failures(uint8_t n) { this->max_failures_ = n; }
  void set_allow_active_services(bool allow) { this->allow_active_services_ = allow; }

  /// Both fixed arrays are sized by codegen and allocated here, from statements that run ahead of
  /// `App.setup()`. Nothing allocates after that.
  void reserve_bindings(uint16_t count);
  void reserve_poll_slots(uint8_t count);
  void reserve_text_slots(uint16_t count);

#ifdef USE_SENSOR
  void add_sensor_binding(const char *group, const char *field, uint16_t element, uint32_t interval_ms,
                          sensor::Sensor *s);
#endif
#ifdef USE_TEXT_SENSOR
  void add_text_binding(const char *group, const char *field, uint16_t element, uint32_t interval_ms,
                        text_sensor::TextSensor *s);
#endif

  // -- the IsoTpConsumer hooks -------------------------------------------------------------
  /// Called inside can_gateway's bounded drain. Validates and decodes into value slots; publishes
  /// nothing (§4.1).
  void on_message(const isotp::MessageView &msg) override;
  void on_error(isotp::IsoTpError error) override;

  // -- the action surface (design §6) ------------------------------------------------------

  /// Queue a one-shot read of a SAFE_READ group by canonical name, alias or DID. Refuses anything
  /// else whatever `allow_active_services` says.
  bool request_read(const char *group_name);
  /// Queue a group that is not a pure read. Refused unless `allow_active_services: true`; the
  /// config-time validator rejects the same case earlier, so reaching this log means the YAML was
  /// changed under a compiled binary.
  bool request_execute(const char *group_name);
  /// Send bytes the catalog never saw. Gated exactly like `request_execute`.
  bool send_raw(const uint8_t *data, size_t len);
  void set_enabled(bool enabled);

  // -- YAML automation hooks ---------------------------------------------------------------
  template<typename F> void add_on_value_callback(F &&callback) {
    this->value_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_error_callback(F &&callback) {
    this->error_callback_.add(std::forward<F>(callback));
  }

  // -- diagnostics -------------------------------------------------------------------------
  uint32_t requests_sent() const { return this->requests_sent_; }
  uint32_t responses_accepted() const { return this->responses_accepted_; }
  uint32_t timeouts() const { return this->timeouts_; }
  uint32_t negative_responses() const { return this->negative_responses_; }
  uint32_t decode_unmatched() const { return this->decode_unmatched_; }
  uint32_t groups_suspended() const { return this->groups_suspended_; }
  uint32_t foreign_responses() const { return this->foreign_responses_; }
  uint32_t short_responses() const { return this->short_responses_; }
  /// Responses accepted but shorter than the group's `resp_min_len`. Not an error — but "some
  /// fields never decode" has to be visible without a debugger, which is what this is for.
  uint32_t partial_responses() const { return this->partial_responses_; }
  /// Bindings whose field no response has ever covered. Read together with partial_responses: a
  /// steady partial count and a stable uncovered count is an ECU that simply answers with fewer
  /// array elements than its database declares, which is ordinary.
  uint32_t fields_uncovered() const { return this->fields_uncovered_; }
  uint8_t last_nrc() const { return this->last_nrc_; }
  bool catalog_ready() const { return this->catalog_.is_open(); }
  /// So a diagnostics block can name the hub it reports on, for the same reason the hub names itself.
  const char *log_name() const { return this->log_name_; }

 protected:
  /// Which kind of request holds the single in-flight slot. ISO-TP has no multiplexing and isotp
  /// enforces one transfer at a time, so the poller and the actions have to share one slot; this is
  /// what says whose it currently is, and therefore who owns the deadline.
  enum class InFlight : uint8_t {
    NONE,
    POLLED,    ///< GroupScheduler owns the slot and the deadline
    ONDEMAND,  ///< uds.read / uds.execute; the hub owns the deadline
    RAW,       ///< uds.raw; no group, so no field claims to validate against
  };

  /// Why a send did not happen, because the two reasons need opposite reactions. BUSY is isotp
  /// refusing while another transfer is in flight — retry the identical request next iteration.
  /// UNSENDABLE is a group whose request bytes this build cannot assemble, which will be just as
  /// true next iteration: retrying it logs a warning per loop forever, so the group is dropped.
  enum class SendResult : uint8_t { SENT, BUSY, UNSENDABLE };

  bool open_catalog_();
  bool resolve_ecu_();
  bool resolve_binding_(UdsBinding &b);
  void build_poll_table_();
  void log_report_();

  void issue_next_(uint32_t now);
  SendResult send_group_(uint16_t group_index);
  void finish_in_flight_();
  void handle_negative_(const ResponseCheck &rc, uint32_t now);
  void fail_in_flight_(uint32_t now, const char *what, uint8_t nrc);
  void suspend_in_flight_(uint8_t nrc);

  void decode_group_(uint16_t group_index, const uint8_t *resp, size_t resp_len, bool partial);
  void decode_text_(UdsBinding &b, const FieldView &f, const uint8_t *resp, size_t resp_len);
  /// Note that `b`'s field was not covered by the response. Marks it unavailable, counts it once,
  /// and logs it once by name.
  void note_uncovered_(UdsBinding &b);
  void publish_dirty_();
  void publish_group_unavailable_(uint16_t group_index);
  void mark_all_unavailable_();
  void report_error_(const char *what, uint8_t nrc);
  /// report_error_ with the group named in the message. The hub prefix says who was asking;
  /// without the group it never says what went unanswered — eleven bare "response timeout"
  /// lines against a resting pack are unattributable (uds follow-up session §2.1 (2026-07-30, git history)).
  /// 0xFFFF (uds.raw — no group) falls back to the plain message.
  void report_group_error_(uint16_t group_index, const char *what, uint8_t nrc);

  /// Resolve a group by name against the flashed catalog and confirm it belongs to our ECU.
  /// Canonical names and original service qualifiers both work — format §7 gives every alias its
  /// own index entry. `service: 0x0207` is turned into a canonical name by codegen, so no DID
  /// parsing happens on the device.
  bool find_group_(const char *name, uint16_t *out) const;
  bool enqueue_(uint16_t group_index);

  static void copy_bounded_(char *dst, size_t cap, const char *src);

  isotp::IsoTpProtocol *isotp_;

  /// Defaults to the component name so a hub whose codegen predates set_log_name() still logs
  /// something sensible rather than an empty prefix.
  const char *log_name_{"uds"};

  // -- catalog ---------------------------------------------------------------------------
  Catalog catalog_;
  const char *partition_name_{nullptr};
  const uint8_t *embedded_{nullptr};
  size_t embedded_len_{0};
  size_t mapped_size_{0};
  /// Why the catalog is unavailable, or nullptr when it opened. One string, no formatting: the
  /// failure path must not allocate.
  const char *catalog_error_{nullptr};
  uint32_t expected_crc_{0};
  const char *ecu_name_{nullptr};
  EcuView ecu_{};
  bool ecu_valid_{false};
  uint16_t p2_ms_{UDS_DEFAULT_P2_MS};
  uint16_t p2_ext_ms_{UDS_DEFAULT_P2_EXT_MS};
#ifdef USE_ESP32
  esp_partition_mmap_handle_t mmap_handle_{0};
#endif

  // -- bindings and the poll table -------------------------------------------------------
  std::unique_ptr<UdsBinding[]> bindings_;
  uint16_t binding_capacity_{0};
  uint16_t binding_count_{0};
  uint16_t publish_cursor_{0};
  std::unique_ptr<char[]> text_block_;
  uint16_t text_capacity_{0};
  uint16_t text_used_{0};

  std::unique_ptr<PollEntry[]> poll_entries_;
  uint8_t poll_capacity_{0};
  GroupScheduler sched_;

  // -- in flight -------------------------------------------------------------------------
  uint8_t req_[UDS_MAX_REQUEST]{};
  uint8_t req_len_{0};
  uint16_t in_flight_group_{0xFFFF};
  uint16_t in_flight_resp_min_{0};
  uint8_t in_flight_slot_{0};
  InFlight in_flight_{InFlight::NONE};
  uint32_t own_deadline_ms_{0};
  uint8_t pending_streak_{0};

  uint16_t ondemand_[UDS_ONDEMAND_QUEUE]{};
  uint8_t ondemand_head_{0};
  uint8_t ondemand_count_{0};
  uint8_t raw_pending_[UDS_MAX_REQUEST]{};
  uint8_t raw_pending_len_{0};

  // -- knobs and counters ----------------------------------------------------------------
  uint8_t max_failures_{3};
  bool allow_active_services_{false};
  bool enabled_{true};

  uint32_t requests_sent_{0};
  uint32_t responses_accepted_{0};
  uint32_t timeouts_{0};
  uint32_t negative_responses_{0};
  uint32_t decode_unmatched_{0};
  uint32_t groups_suspended_{0};
  uint32_t foreign_responses_{0};
  uint32_t short_responses_{0};
  uint32_t partial_responses_{0};
  uint32_t fields_uncovered_{0};
  uint8_t last_nrc_{0};

  LazyCallbackManager<void(UdsValue)> value_callback_;
  LazyCallbackManager<void(UdsError)> error_callback_;
};

#ifdef USE_SENSOR
/// Optional diagnostic counters for one hub, modelled on `IsoTpDiagnostics`.
///
/// One polling component serves every counter rather than one per sensor: they are read together,
/// and a config that talks to several ECUs would otherwise carry a PollingComponent per counter
/// per ECU.
class UdsDiagnostics : public PollingComponent, public Parented<UdsHub> {
 public:
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_requests_sent_sensor(sensor::Sensor *s) { this->requests_sent_sensor_ = s; }
  void set_responses_accepted_sensor(sensor::Sensor *s) { this->responses_accepted_sensor_ = s; }
  void set_timeouts_sensor(sensor::Sensor *s) { this->timeouts_sensor_ = s; }
  void set_negative_responses_sensor(sensor::Sensor *s) { this->negative_responses_sensor_ = s; }
  void set_decode_unmatched_sensor(sensor::Sensor *s) { this->decode_unmatched_sensor_ = s; }
  void set_groups_suspended_sensor(sensor::Sensor *s) { this->groups_suspended_sensor_ = s; }
  void set_partial_responses_sensor(sensor::Sensor *s) { this->partial_responses_sensor_ = s; }
  void set_fields_uncovered_sensor(sensor::Sensor *s) { this->fields_uncovered_sensor_ = s; }
  void set_last_nrc_sensor(sensor::Sensor *s) { this->last_nrc_sensor_ = s; }

 protected:
  /// Publish only on change: these counters are mostly idle, and a diagnostic session should not
  /// generate API traffic in proportion to its poll rate.
  static void publish_changed_(sensor::Sensor *s, uint32_t value, uint32_t &last);

  sensor::Sensor *requests_sent_sensor_{nullptr};
  sensor::Sensor *responses_accepted_sensor_{nullptr};
  sensor::Sensor *timeouts_sensor_{nullptr};
  sensor::Sensor *negative_responses_sensor_{nullptr};
  sensor::Sensor *decode_unmatched_sensor_{nullptr};
  sensor::Sensor *groups_suspended_sensor_{nullptr};
  sensor::Sensor *partial_responses_sensor_{nullptr};
  sensor::Sensor *fields_uncovered_sensor_{nullptr};
  sensor::Sensor *last_nrc_sensor_{nullptr};

  uint32_t last_requests_sent_{UINT32_MAX};
  uint32_t last_responses_accepted_{UINT32_MAX};
  uint32_t last_timeouts_{UINT32_MAX};
  uint32_t last_negative_responses_{UINT32_MAX};
  uint32_t last_decode_unmatched_{UINT32_MAX};
  uint32_t last_groups_suspended_{UINT32_MAX};
  uint32_t last_partial_responses_{UINT32_MAX};
  uint32_t last_fields_uncovered_{UINT32_MAX};
  uint32_t last_last_nrc_{UINT32_MAX};
};
#endif

/// `uds.read` — a SAFE_READ group, queued once. The group is carried as the name codegen resolved
/// at `esphome config` time, not as an index, for the same reason bindings are.
template<typename... Ts> class UdsReadAction : public Action<Ts...>, public Parented<UdsHub> {
 public:
  void set_group(const char *group) { this->group_ = group; }
  void play(const Ts &...x) override { this->parent_->request_read(this->group_); }

 protected:
  const char *group_{nullptr};
};

/// `uds.execute` — a group that is not a pure read. Rejected at config time unless the hub sets
/// `allow_active_services: true`; the runtime check exists so a binary cannot be talked into
/// transmitting by a YAML edit it was not compiled with.
template<typename... Ts> class UdsExecuteAction : public Action<Ts...>, public Parented<UdsHub> {
 public:
  void set_group(const char *group) { this->group_ = group; }
  void play(const Ts &...x) override { this->parent_->request_execute(this->group_); }

 protected:
  const char *group_{nullptr};
};

/// `uds.raw` — bytes the catalog never saw. Same gate as `uds.execute`.
///
/// Tier 1, loop context: this is the control plane, so the allocation a templatable payload costs
/// (the lambda returns a vector by value) is paid only when an automation actually fires.
template<typename... Ts> class UdsRawAction : public Action<Ts...>, public Parented<UdsHub> {
 public:
  void set_data_template(std::function<std::vector<uint8_t>(Ts...)> func) {
    this->data_func_ = std::move(func);
    this->is_static_ = false;
  }
  void set_data_static(const std::vector<uint8_t> &data) {
    this->data_static_ = data;
    this->is_static_ = true;
  }

  void play(const Ts &...x) override {
    if (this->is_static_) {
      this->parent_->send_raw(this->data_static_.data(), this->data_static_.size());
      return;
    }
    const std::vector<uint8_t> data = this->data_func_(x...);
    this->parent_->send_raw(data.data(), data.size());
  }

 protected:
  bool is_static_{true};
  std::function<std::vector<uint8_t>(Ts...)> data_func_{};
  std::vector<uint8_t> data_static_{};
};

/// `uds.set_enabled` — stop or resume polling without a reflash. Disabling does not cancel a
/// request already on the wire; it stops the next one being issued.
template<typename... Ts> class UdsSetEnabledAction : public Action<Ts...>, public Parented<UdsHub> {
 public:
  TEMPLATABLE_VALUE(bool, enabled)
  void play(const Ts &...x) override { this->parent_->set_enabled(this->enabled_.value(x...)); }
};

}  // namespace esphome::uds
