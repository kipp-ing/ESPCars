#pragma once

/// Freestanding state-tree resolver, chunked JSON emitter and action parser for the portal.
///
/// <cstdint>, <cstring>, <cstddef> and nothing else. No ESPHome, no IDF, no allocation -- every
/// buffer is caller-owned. This is what makes it testable on the host (make -C tests/host)
/// without a board.
///
/// Wire format: docs/portal-spec.md section 5 (schema_version 2). Validator:
/// script/portal_schema.py. `State` has no DOWN member: `down` is reachable only from a
/// confirmed rail observation, which this producer never has; leaving it out of the enum makes
/// the false pass unrepresentable rather than merely discouraged.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::portal::core {

inline constexpr uint32_t SCHEMA_VERSION = 2;
inline constexpr size_t CHUNK_BYTES = 4096;
inline constexpr uint16_t NO_PARENT = 0xFFFF;

/// Bounds the per-series element scratch the emitter derives each element's wire class and
/// value into.
inline constexpr uint16_t SERIES_MAX_ELEMENTS = 64;

/// Producer states. There is deliberately no DOWN member (see the file header).
enum class State : uint8_t {
  OK,
  STALE,
  DEGRADED,
  REFUSED,
  SILENT,
  ABSENT,
  UNREACHABLE,
  UNKNOWN,
  INCOMPATIBLE,
};

inline const char *state_name(State state) {
  switch (state) {
    case State::OK:
      return "ok";
    case State::STALE:
      return "stale";
    case State::DEGRADED:
      return "degraded";
    case State::REFUSED:
      return "refused";
    case State::SILENT:
      return "silent";
    case State::ABSENT:
      return "absent";
    case State::UNREACHABLE:
      return "unreachable";
    case State::UNKNOWN:
      return "unknown";
    case State::INCOMPATIBLE:
      return "incompatible";
  }
  return "unknown";
}

enum class Classification : uint8_t { REAL, SNA, IMPLAUSIBLE };

inline const char *classification_name(Classification classification) {
  switch (classification) {
    case Classification::REAL:
      return "real";
    case Classification::SNA:
      return "sna";
    case Classification::IMPLAUSIBLE:
      return "implausible";
  }
  return "real";
}

/// One scalar as the emitter sees it. No ESPHome types.
struct ValueSample {
  const char *key;    ///< non-empty
  const char *label;  ///< non-empty
  const char *unit;   ///< nullptr -> JSON null
  const char *text;   ///< used iff is_text && has_value; NUL-terminated
  float number;       ///< used iff !is_text && has_value; also carries a bool as 0.0f/1.0f
                      ///< when is_bool, so a fresh field never has to be threaded through
                      ///< every call site that only cared about "the reading"
  uint32_t age_ms;
  int8_t decimals;  ///< 0..6; unused when is_bool
  bool is_text;
  bool is_bool;        ///< value is a JSON true/false, not a number or a string; "raw" is always
                       ///< null for these -- a boolean has no separate raw form
  bool has_value;      ///< false -> no reading at all
  bool age_known;      ///< false -> never published
  bool text_overflow;  ///< text was longer than PORTAL_TEXT_MAX; never true when is_bool
};

/// One series element as the emitter sees it, filled by one call to
/// the node's `SeriesElementFn`. Default-constructed as a hole ('-'/no value) so a callback
/// that only partially fills `out` before answering never accidentally reads as real.
struct SeriesElement {
  char cls{'-'};  ///< 'r' | 's' | 'i' | '-'; anything else is normalised to '-'
  bool has_value{false};
  float value{0.0f};
};

/// Pull one element. `index` is the ZERO-BASED ordinal within the series, never the wire
/// number -- a provider that needs a frame-local number adds its own base from
/// `ctx`. Returns false to say "I cannot answer for this element", which is a hole.
/// Runs on the esp_http_server task at response time; it buffers nothing.
using SeriesElementFn = bool (*)(void *ctx, uint16_t index, SeriesElement *out);

/// True means "this node is absent right now". Runs on the httpd task at response time, from
/// `resolve()`; it buffers nothing and caches nothing, which is the only reason its verdict
/// cannot go stale.
using NodeAbsentFn = bool (*)(void *ctx);

/// The generic series mechanism. A series with no bound callback is not a stub, it is every
/// element legitimately a hole, which is exactly what an unbound `SeriesElementFn` produces.
struct SeriesDef {
  const char *key;
  const char *label;
  const char *unit;         ///< nullptr -> JSON null
  const char *index_frame;  ///< nullptr -> JSON null; NEVER resolved from anything else
  int32_t index_base;
  int32_t pack_index_base;  ///< meaningless unless pack_index_known
  bool pack_index_known;    ///< false -> JSON null
  uint16_t count;           ///< clamped to SERIES_MAX_ELEMENTS by the emitter
  int8_t decimals;
  SeriesElementFn element;  ///< nullptr -> every element is a hole
  void *ctx;
};

// ---- actions (docs/portal-spec.md sections 4.7, 5.4, 5.5, 6.2) --------------------------------

inline constexpr size_t ACTION_BODY_MAX = 256;  ///< larger request bodies are refused body_too_large
inline constexpr size_t ACTION_TEXT_MAX = 64;   ///< incl. NUL; select option / text value capacity

enum class Control : uint8_t { SWITCH, BUTTON, SELECT, NUMBER };

inline const char *control_name(Control control) {
  switch (control) {
    case Control::SWITCH:
      return "switch";
    case Control::BUTTON:
      return "button";
    case Control::SELECT:
      return "select";
    case Control::NUMBER:
      return "number";
  }
  return "switch";
}

/// NONE means "never invoked since boot" (wire: `"result": null`).
enum class ActionState : uint8_t { NONE, RUNNING, ACCEPTED, REFUSED };

inline const char *action_state_name(ActionState state) {
  switch (state) {
    case ActionState::NONE:
      return "none";
    case ActionState::RUNNING:
      return "running";
    case ActionState::ACCEPTED:
      return "accepted";
    case ActionState::REFUSED:
      return "refused";
  }
  return "none";
}

/// Why-code of a result: QUEUED (running), APPLIED (accepted), or one refusal code.
enum class ActionCode : uint8_t {
  NONE,
  QUEUED,
  APPLIED,
  INVALID_BODY,
  WRONG_TYPE,
  OUT_OF_RANGE,
  UNKNOWN_OPTION,
  DISABLED,
  BODY_TOO_LARGE,
};

inline const char *action_code_name(ActionCode code) {
  switch (code) {
    case ActionCode::NONE:
      return "none";
    case ActionCode::QUEUED:
      return "queued";
    case ActionCode::APPLIED:
      return "applied";
    case ActionCode::INVALID_BODY:
      return "invalid_body";
    case ActionCode::WRONG_TYPE:
      return "wrong_type";
    case ActionCode::OUT_OF_RANGE:
      return "out_of_range";
    case ActionCode::UNKNOWN_OPTION:
      return "unknown_option";
    case ActionCode::DISABLED:
      return "disabled";
    case ActionCode::BODY_TOO_LARGE:
      return "body_too_large";
  }
  return "none";
}

enum class ValueKind : uint8_t { NONE, BOOL, NUMBER, TEXT };

/// A requested value as parsed. Plain data, copyable, no pointers.
struct ActionValue {
  ValueKind kind{ValueKind::NONE};
  bool boolean{false};
  float number{0.0f};
  char text[ACTION_TEXT_MAX]{};
};

struct ActionResult {
  ActionState state{ActionState::NONE};
  ActionCode code{ActionCode::NONE};
  uint32_t age_ms{0};  ///< ms since this result was set; never part of the fingerprint
  ActionValue value;
};

/// Static description of one action (set at setup).
struct ActionDef {
  const char *key;
  const char *label;
  Control control;
  int8_t decimals;             ///< number only; 0..6
  const char *const *options;  ///< select only
  uint16_t option_count;
};

/// The live side of one action, sampled by the binding. Plain data, copyable.
struct ActionSample {
  bool enabled{true};  ///< false only for select/number without a state (entity_unavailable)
  bool has_value{false};
  bool boolean{false};           ///< switch state
  float number{0.0f};            ///< number state
  char text[ACTION_TEXT_MAX]{};  ///< select state
  bool has_min{false};
  bool has_max{false};
  bool has_step{false};
  float min{0.0f};
  float max{0.0f};
  float step{0.0f};
  const char *unit{nullptr};  ///< number only; nullptr -> null
  ActionResult result;
};

struct NodeDef {
  const char *id;  ///< full dotted id, precomputed in Python
  const char *kind;
  const char *label;
  uint16_t parent;  ///< NO_PARENT for the root; parents always precede children
  uint16_t value_first;
  uint16_t value_count;
  uint32_t budget_ms;             ///< meaningless unless budget_known
  bool budget_known;              ///< false -> "budget_ms": null; no value under this node can be STALE
                                  ///< (most bound entities publish on change with no cadence, and
                                  ///< no invented number is honest for those)
  const char *absent_why;         ///< non-null -> node is ABSENT with this why.code
  const char *role;               ///< nullptr when the node has no role
  const char *roles_available;    ///< comma-separated; nullptr iff role is nullptr
  const char *unknown_value_key;  ///< see §4
  const char *unknown_equals;
  const char *unknown_why;
  const char *trust_value_key;       ///< see §4
  const SeriesDef *series{nullptr};  ///< nullptr -> "series":null (every node, every image, today)
  NodeAbsentFn absent_fn{nullptr};   ///< nullptr -> `absent_why` is unconditional (today's behaviour)
  void *absent_ctx{nullptr};
  // A pre-rendered `why.evidence` fragment (no surrounding braces, e.g. `"announced":false`)
  // declared beside the absence provider that backs it. nullptr -> `"evidence":{}`.
  const char *absent_evidence{nullptr};
  uint16_t action_first{0};  ///< index of this node's first action in Snapshot::actions
  uint16_t action_count{0};
};

/// The absence predicate. `resolve()` is its ONE caller: `absent_fn` reads live state at call
/// time by design, so a second evaluation later in the same response could see a different
/// answer and pair `state:"absent"` with a `why.code` from a re-read that no longer says
/// absent. `describe_node_reason()` therefore reads `r.states[index] == State::ABSENT`, the one
/// verdict `resolve()` already committed.
inline bool node_is_absent(const NodeDef &node) {
  return node.absent_why != nullptr && (node.absent_fn == nullptr || node.absent_fn(node.absent_ctx));
}

struct Identity {
  const char *board;    ///< e.g. "demo"; nullptr when unknown
  const char *node_id;  ///< "0xA"; nullptr when unknown
  const char *mac;      ///< e.g. "02:00:00:00:00:01"; nullptr when unknown
  const char *build;
  const char *const *owns;
  size_t owns_count;
  const char *const *components;
  size_t components_count;
};

/// One envelope link (§5.1): all three strings non-empty, `href` starts with '/'.
struct LinkDef {
  const char *rel;
  const char *label;
  const char *href;
};

struct Snapshot {
  uint32_t seq;
  uint32_t uptime_ms;
  Identity identity;
  const NodeDef *nodes;
  size_t node_count;  ///< nodes[0] is the root
  const ValueSample *values;
  size_t value_count;
  const ActionDef *actions{nullptr};  ///< parallel to action_samples
  const ActionSample *action_samples{nullptr};
  size_t action_count{0};
  const LinkDef *links{nullptr};
  size_t link_count{0};
};

/// Caller-owned scratch, one entry per node. Freestanding cores never allocate.
struct Resolved {
  State *states;
  uint32_t *ages;
  bool *age_known;
  size_t capacity;
  /// Optional scratch for `write_series` (SERIES_MAX_ELEMENTS entries). A device passes a
  /// member buffer so the httpd task's small stack never holds it; nullptr falls back to a
  /// stack buffer inside one non-inlined function (host tests).
  SeriesElement *series_scratch{nullptr};
};

/// Bounded chunked writer. `flush` receives whole frames of at most `capacity` bytes and
/// returns false to abort. Nothing is ever buffered beyond `capacity`: the response buffer never
/// grows with the tree.
using FlushFn = bool (*)(void *ctx, const char *data, size_t size);

class ChunkWriter {
 public:
  void init(char *buffer, size_t capacity, FlushFn flush, void *ctx) {
    this->buffer_ = buffer;
    this->capacity_ = capacity;
    this->flush_ = flush;
    this->ctx_ = ctx;
    this->used_ = 0;
    this->ok_ = buffer != nullptr && capacity != 0 && flush != nullptr;
  }

  bool write(const char *data, size_t size) {  ///< splits across frames as needed
    if (!this->ok_)
      return false;
    size_t offset = 0;
    while (offset < size) {
      const size_t space = this->capacity_ - this->used_;
      const size_t take = (size - offset) < space ? (size - offset) : space;
      if (take != 0) {
        std::memcpy(this->buffer_ + this->used_, data + offset, take);
        this->used_ += take;
        offset += take;
      }
      if (this->used_ == this->capacity_) {
        if (!this->flush_(this->ctx_, this->buffer_, this->used_)) {
          this->ok_ = false;
          return false;
        }
        this->used_ = 0;
      }
    }
    return true;
  }

  bool literal(const char *nul_terminated) {
    if (nul_terminated == nullptr)
      return true;
    return this->write(nul_terminated, std::strlen(nul_terminated));
  }

  bool json_string(const char *value) {  ///< emits the quotes and escapes the content
    if (!this->write("\"", 1))
      return false;
    if (value != nullptr) {
      while (*value != '\0') {
        const unsigned char c = static_cast<unsigned char>(*value++);
        if (c == '"' || c == '\\') {
          const char escaped[2] = {'\\', static_cast<char>(c)};
          if (!this->write(escaped, 2))
            return false;
        } else if (c == '\b') {
          if (!this->literal("\\b"))
            return false;
        } else if (c == '\f') {
          if (!this->literal("\\f"))
            return false;
        } else if (c == '\n') {
          if (!this->literal("\\n"))
            return false;
        } else if (c == '\r') {
          if (!this->literal("\\r"))
            return false;
        } else if (c == '\t') {
          if (!this->literal("\\t"))
            return false;
        } else if (c < 0x20) {
          static const char hex[] = "0123456789abcdef";
          const char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0F]};
          if (!this->write(escaped, 6))
            return false;
        } else {
          const char ch = static_cast<char>(c);
          if (!this->write(&ch, 1))
            return false;
        }
      }
    }
    return this->write("\"", 1);
  }

  bool number_u32(uint32_t value) {
    char digits[10];
    uint8_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10u);
      value /= 10u;
    } while (value != 0);
    char out[10];
    for (uint8_t i = 0; i < count; i++)
      out[i] = digits[count - 1 - i];
    return this->write(out, count);
  }

  bool number_float(float value, int8_t decimals) {
    if (decimals < 0)
      decimals = 0;
    if (decimals > 6)
      decimals = 6;
    static const uint32_t POW10[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};
    const uint32_t scale = POW10[decimals];
    bool negative = false;
    if (value < 0) {
      negative = true;
      value = -value;
    }
    double scaled = static_cast<double>(value) * static_cast<double>(scale) + 0.5;
    uint64_t scaled_int;
    if (scaled >= static_cast<double>(UINT64_MAX)) {
      scaled_int = UINT64_MAX;
    } else {
      scaled_int = static_cast<uint64_t>(scaled);
    }
    const uint64_t int_part = scaled_int / scale;
    const uint64_t frac_part = scaled_int % scale;
    if (negative && !this->write("-", 1))
      return false;
    if (!this->write_u64_(int_part))
      return false;
    if (decimals > 0) {
      if (!this->write(".", 1))
        return false;
      char frac_digits[6];
      uint64_t remaining = frac_part;
      for (int8_t i = decimals - 1; i >= 0; i--) {
        frac_digits[i] = static_cast<char>('0' + remaining % 10u);
        remaining /= 10u;
      }
      if (!this->write(frac_digits, static_cast<size_t>(decimals)))
        return false;
    }
    return true;
  }

  bool finish() {  ///< flushes the tail; false on flush failure
    if (!this->ok_)
      return false;
    if (this->used_ > 0) {
      if (!this->flush_(this->ctx_, this->buffer_, this->used_)) {
        this->ok_ = false;
        return false;
      }
      this->used_ = 0;
    }
    return true;
  }

  bool ok() const { return this->ok_; }

 private:
  bool write_u64_(uint64_t value) {
    char digits[20];
    uint8_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + static_cast<unsigned>(value % 10u));
      value /= 10u;
    } while (value != 0);
    char out[20];
    for (uint8_t i = 0; i < count; i++)
      out[i] = digits[count - 1 - i];
    return this->write(out, count);
  }

  char *buffer_{nullptr};
  size_t capacity_{0};
  FlushFn flush_{nullptr};
  void *ctx_{nullptr};
  size_t used_{0};
  bool ok_{false};
};

namespace detail {

inline bool is_finite_float(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return ((bits >> 23) & 0xFFu) != 0xFFu;
}

inline uint8_t severity(State state) {
  switch (state) {
    case State::OK:
      return 0;
    case State::STALE:
      return 1;
    case State::DEGRADED:
    case State::REFUSED:
    case State::SILENT:
      return 2;
    case State::ABSENT:
    case State::UNREACHABLE:
      return 3;
    case State::UNKNOWN:
    case State::INCOMPATIBLE:
      return 4;
  }
  return 4;
}

struct ValueVerdict {
  Classification classification;
  State state;
  const char *why_code;  ///< nullptr iff state == OK
  const char *detail;    ///< only text_overflow uses this today
};

/// §3's per-value table, first match wins. `budget_known == false` means the node has no
/// honest number to judge staleness against (most bound entities publish only on change,
/// so "since last publish" is "since last change" for them, and any finite budget eventually
/// trips a healthy constant reading) -- such a value can still be ABSENT/DEGRADED
/// from its own reading, it just can never be STALE.
///
/// `!v.is_bool` guards the STALE branch too: ESPHome fires no state callback when a boolean
/// republishes an unchanged value, so "time since last publish" is not observable for a boolean.
/// The config layer already refuses the shapes that would give a binary_sensor-carrying node a
/// positive `budget_ms`; this is the second half, for a `Snapshot` built outside the YAML path.
inline ValueVerdict classify_value(const ValueSample &v, uint32_t node_budget_ms, bool budget_known) {
  if (!v.has_value || !v.age_known)
    return {Classification::SNA, State::ABSENT, "value_not_available", nullptr};
  if (!v.is_text && !v.is_bool && !is_finite_float(v.number))
    return {Classification::SNA, State::ABSENT, "value_not_available", nullptr};
  if (v.is_text && v.text_overflow)
    return {Classification::SNA, State::DEGRADED, "value_not_available", "text longer than the configured limit"};
  if (budget_known && !v.is_bool && v.age_ms > node_budget_ms)
    return {Classification::REAL, State::STALE, "stale_data", nullptr};
  return {Classification::REAL, State::OK, nullptr, nullptr};
}

inline const ValueSample *find_value(const Snapshot &s, const NodeDef &node, const char *key) {
  if (key == nullptr)
    return nullptr;
  for (uint16_t k = 0; k < node.value_count; k++) {
    const ValueSample &v = s.values[node.value_first + k];
    if (v.key != nullptr && std::strcmp(v.key, key) == 0)
      return &v;
  }
  return nullptr;
}

/// §4: the honest alternative to a fabricated DOWN. Fires only when the named value is a
/// live, non-overflowed text reading whose content equals `unknown_equals` exactly.
inline bool unknown_when_matches(const Snapshot &s, const NodeDef &node) {
  if (node.unknown_value_key == nullptr || node.unknown_equals == nullptr)
    return false;
  const ValueSample *target = find_value(s, node, node.unknown_value_key);
  if (target == nullptr || !target->is_text || !target->has_value || target->text_overflow)
    return false;
  return target->text != nullptr && std::strcmp(target->text, node.unknown_equals) == 0;
}

inline bool trust_confirmed(const Snapshot &s, const NodeDef &node) {
  if (node.trust_value_key == nullptr)
    return false;
  const ValueSample *target = find_value(s, node, node.trust_value_key);
  return target != nullptr && target->is_text && target->has_value && !target->text_overflow &&
         target->text != nullptr && std::strcmp(target->text, "CONFIRMED") == 0;
}

/// The reason behind a non-OK node state, re-derived at write time from the same inputs
/// `resolve()` used -- never stored separately, so the two can never drift apart.
struct NodeReason {
  const char *code;
  const char *detail;
  bool evidence_children;
  // Carry-through of NodeDef::absent_evidence. nullptr -> no pre-rendered fragment.
  const char *evidence{nullptr};
};

inline NodeReason describe_node_reason(const Snapshot &s, const Resolved &r, size_t index) {
  if (r.states[index] == State::OK)
    return {nullptr, nullptr, false};
  const NodeDef &node = s.nodes[index];
  // Read the verdict `resolve()` already committed, never re-invoke `node_is_absent()`:
  // State::ABSENT is reachable only from resolve()'s own predicate branch.
  if (r.states[index] == State::ABSENT)
    return {node.absent_why, nullptr, false, node.absent_evidence};
  if (unknown_when_matches(s, node))
    return {node.unknown_why, nullptr, false};
  bool any_unavailable = false;
  bool any_stale = false;
  for (uint16_t k = 0; k < node.value_count; k++) {
    const ValueVerdict verdict = classify_value(s.values[node.value_first + k], node.budget_ms, node.budget_known);
    if (verdict.classification == Classification::SNA)
      any_unavailable = true;
    else if (verdict.state == State::STALE)
      any_stale = true;
  }
  if (any_unavailable)
    return {"entities_unavailable", nullptr, false};
  if (any_stale)
    return {"stale_data", nullptr, false};
  return {"child_degraded", nullptr, true};
}

}  // namespace detail

/// §3, one pass over own values then one reverse pass for child propagation; n<=32.
inline void resolve(const Snapshot &s, Resolved &out) {
  const size_t limit = s.node_count < out.capacity ? s.node_count : out.capacity;

  for (size_t i = 0; i < limit; i++) {
    const NodeDef &node = s.nodes[i];
    uint32_t max_age = 0;
    bool any_known_age = false;
    bool any_unavailable = false;
    bool any_stale = false;
    for (uint16_t k = 0; k < node.value_count; k++) {
      const ValueSample &v = s.values[node.value_first + k];
      if (v.age_known && v.age_ms > max_age)
        max_age = v.age_ms;
      if (v.age_known)
        any_known_age = true;
      const detail::ValueVerdict verdict = detail::classify_value(v, node.budget_ms, node.budget_known);
      if (verdict.classification == Classification::SNA)
        any_unavailable = true;
      else if (verdict.state == State::STALE)
        any_stale = true;
    }

    State state;
    uint32_t age_ms = any_known_age ? max_age : 0;
    bool age_known = any_known_age;

    if (node_is_absent(node)) {
      state = State::ABSENT;
      age_known = false;
      age_ms = 0;
    } else if (node.unknown_value_key != nullptr && detail::unknown_when_matches(s, node)) {
      state = State::UNKNOWN;
      const ValueSample *target = detail::find_value(s, node, node.unknown_value_key);
      if (target != nullptr && target->age_known) {
        age_known = true;
        age_ms = target->age_ms;
      } else {
        age_known = false;
        age_ms = 0;
      }
    } else if (any_unavailable) {
      state = State::DEGRADED;
    } else if (any_stale) {
      state = State::STALE;
    } else {
      state = State::OK;
    }

    out.states[i] = state;
    out.ages[i] = age_ms;
    out.age_known[i] = age_known;
  }

  // Reverse order: pre-order construction guarantees every descendant of node i has an index
  // > i, so by the time propagation reaches i every child (and everything below it) already
  // carries its final state.
  for (size_t idx = limit; idx-- > 0;) {
    if (detail::severity(out.states[idx]) >= detail::severity(State::DEGRADED))
      continue;
    bool any_child_not_ok = false;
    for (size_t j = idx + 1; j < limit; j++) {
      if (s.nodes[j].parent != static_cast<uint16_t>(idx))
        continue;
      if (out.states[j] != State::OK)
        any_child_not_ok = true;
    }
    if (any_child_not_ok)
      out.states[idx] = State::DEGRADED;
  }
}

inline bool has_node(const Snapshot &s, const char *path) {
  if (path == nullptr)
    return false;
  for (size_t i = 0; i < s.node_count; i++) {
    if (s.nodes[i].id != nullptr && std::strcmp(s.nodes[i].id, path) == 0)
      return true;
  }
  return false;
}

namespace detail {

inline bool write_csv_as_json_array(ChunkWriter &w, const char *csv) {
  if (csv == nullptr)
    return true;
  const char *p = csv;
  bool first = true;
  while (*p != '\0') {
    const char *start = p;
    while (*p != '\0' && *p != ',')
      p++;
    const size_t len = static_cast<size_t>(p - start);
    char token[40];
    const size_t copy_len = len < sizeof(token) - 1 ? len : sizeof(token) - 1;
    std::memcpy(token, start, copy_len);
    token[copy_len] = '\0';
    if (!first && !w.literal(","))
      return false;
    first = false;
    if (!w.json_string(token))
      return false;
    if (*p == ',')
      p++;
  }
  return true;
}

inline bool write_value(ChunkWriter &w, const ValueSample &v, uint32_t node_budget_ms, bool budget_known) {
  const ValueVerdict verdict = classify_value(v, node_budget_ms, budget_known);
  const bool is_sna = verdict.classification == Classification::SNA;
  if (!w.literal("{\"key\":") || !w.json_string(v.key))
    return false;
  if (!w.literal(",\"label\":") || !w.json_string(v.label))
    return false;
  if (!w.literal(",\"unit\":"))
    return false;
  if (!(v.unit != nullptr ? w.json_string(v.unit) : w.literal("null")))
    return false;
  if (!w.literal(",\"value\":"))
    return false;
  if (is_sna) {
    if (!w.literal("null"))
      return false;
  } else if (v.is_bool) {
    if (!w.literal(v.number != 0.0f ? "true" : "false"))
      return false;
  } else if (v.is_text) {
    if (!w.json_string(v.text))
      return false;
  } else {
    if (!w.number_float(v.number, v.decimals))
      return false;
  }
  if (!w.literal(",\"raw\":"))
    return false;
  // A boolean has no separate raw form: "raw" is always null for these,
  // never a mirrored true/false, regardless of classification.
  if (is_sna || v.is_bool) {
    if (!w.literal("null"))
      return false;
  } else if (v.is_text) {
    if (!w.json_string(v.text))
      return false;
  } else {
    if (!w.number_float(v.number, v.decimals))
      return false;
  }
  if (!w.literal(",\"raw_hex\":null,\"classification\":") ||
      !w.json_string(classification_name(verdict.classification)))
    return false;
  if (!w.literal(",\"state\":") || !w.json_string(state_name(verdict.state)))
    return false;
  if (!w.literal(",\"why\":"))
    return false;
  if (verdict.state == State::OK) {
    if (!w.literal("null"))
      return false;
  } else {
    if (!w.literal("{\"code\":") || !w.json_string(verdict.why_code))
      return false;
    if (!w.literal(",\"detail\":"))
      return false;
    if (!(verdict.detail != nullptr ? w.json_string(verdict.detail) : w.literal("null")))
      return false;
    if (!w.literal(",\"since_ms\":null,\"evidence\":{}}"))
      return false;
  }
  return w.literal("}");
}

inline bool write_node_why(ChunkWriter &w, const Snapshot &s, const Resolved &r, size_t index) {
  if (r.states[index] == State::OK)
    return w.literal("null");
  const NodeReason reason = describe_node_reason(s, r, index);
  if (!w.literal("{\"code\":") || !w.json_string(reason.code))
    return false;
  if (!w.literal(",\"detail\":"))
    return false;
  if (!(reason.detail != nullptr ? w.json_string(reason.detail) : w.literal("null")))
    return false;
  if (!w.literal(",\"since_ms\":null,\"evidence\":{"))
    return false;
  bool wrote_evidence = false;
  // A pre-rendered fragment for evidence-gated codes, authored beside the provider that backs
  // it. `w.literal()` writes it verbatim -- it is already valid `"key":value` JSON text.
  if (reason.evidence != nullptr) {
    if (!w.literal(reason.evidence))
      return false;
    wrote_evidence = true;
  }
  if (reason.evidence_children) {
    if (wrote_evidence && !w.literal(","))
      return false;
    if (!w.literal("\"children\":{"))
      return false;
    bool first = true;
    for (size_t j = index + 1; j < s.node_count; j++) {
      if (s.nodes[j].parent != static_cast<uint16_t>(index))
        continue;
      if (r.states[j] == State::OK)
        continue;
      if (!first && !w.literal(","))
        return false;
      first = false;
      if (!w.json_string(s.nodes[j].id) || !w.literal(":") || !w.json_string(state_name(r.states[j])))
        return false;
    }
    if (!w.literal("}"))
      return false;
  }
  return w.literal("}}");
}

/// Provenance is `null` on the skeleton above every subtree this board `owns` -- `index` is a
/// STRICT ancestor of an owned root, never the owned root itself. String prefixing is wrong:
/// the synthetic root's id is `car` while its children's ids are bare, so `car` is nobody's
/// string prefix -- instead this walks `NodeDef.parent` upward from each owned root's own
/// index (the same bounded walk `write_ancestor_role` uses). With `owns_count == 0` nothing is
/// skeleton: a board that cannot name itself keeps every node's provenance object.
inline bool is_skeleton_node(const Snapshot &s, size_t index) {
  for (size_t k = 0; k < s.identity.owns_count; k++) {
    const char *owned_id = s.identity.owns[k];
    if (owned_id == nullptr)
      continue;
    size_t owned_index = s.node_count;
    for (size_t i = 0; i < s.node_count; i++) {
      if (s.nodes[i].id != nullptr && std::strcmp(s.nodes[i].id, owned_id) == 0) {
        owned_index = i;
        break;
      }
    }
    if (owned_index >= s.node_count)
      continue;
    uint16_t walk = s.nodes[owned_index].parent;
    for (size_t step = 0; step < s.node_count && walk != NO_PARENT; step++) {
      if (walk >= s.node_count)
        break;
      if (walk == index)
        return true;
      walk = s.nodes[walk].parent;
    }
  }
  return false;
}

inline bool write_provenance(ChunkWriter &w, const Snapshot &s, const Resolved &r, size_t index) {
  if (is_skeleton_node(s, index))
    return w.literal("null");
  const NodeDef &node = s.nodes[index];
  if (!w.literal("{\"origin\":\"local\",\"board\":"))
    return false;
  if (!(s.identity.board != nullptr ? w.json_string(s.identity.board) : w.literal("null")))
    return false;
  if (!w.literal(",\"node_id\":"))
    return false;
  if (!(s.identity.node_id != nullptr ? w.json_string(s.identity.node_id) : w.literal("null")))
    return false;
  if (!w.literal(",\"age_ms\":"))
    return false;
  if (!(r.age_known[index] ? w.number_u32(r.ages[index]) : w.literal("null")))
    return false;
  if (!w.literal(",\"budget_ms\":"))
    return false;
  if (!(node.budget_known ? w.number_u32(node.budget_ms) : w.literal("null")))
    return false;
  if (!w.literal(",\"trust\":"))
    return false;
  if (!w.json_string(trust_confirmed(s, node) ? "confirmed" : "assumed"))
    return false;
  if (!w.literal(",\"seq\":") || !w.number_u32(s.seq))
    return false;
  return w.literal("}");
}

/// One pull per element, class and value derived together so the pairing invariant is
/// structural rather than remembered. `out` is default-constructed fresh for every element (a
/// hole), so whatever a callback partially writes before returning `false` is discarded.
inline bool write_series_with(ChunkWriter &w, const SeriesDef &def, SeriesElement *resolved) {
  const uint16_t count = def.count > SERIES_MAX_ELEMENTS ? SERIES_MAX_ELEMENTS : def.count;

  if (!w.literal("{\"key\":") || !w.json_string(def.key))
    return false;
  if (!w.literal(",\"label\":") || !w.json_string(def.label))
    return false;
  if (!w.literal(",\"unit\":"))
    return false;
  if (!(def.unit != nullptr ? w.json_string(def.unit) : w.literal("null")))
    return false;
  // index_base/pack_index_base are non-negative by construction (cv.int_range(min=0) and
  // (position-1)*submodule_size+index_base with position>=1), so number_u32 on the cast is exact.
  if (!w.literal(",\"index_base\":") || !w.number_u32(static_cast<uint32_t>(def.index_base)))
    return false;
  // §1.4: index is unconditionally null -- no producer in this design has a sparse series
  // (holes stay in place as '-' elements instead), so an index array is never built.
  if (!w.literal(",\"index\":null"))
    return false;
  if (!w.literal(",\"index_frame\":"))
    return false;
  // NEVER resolved from kind/key/label/unit/index_base -- nullptr here means the producer
  // could not name the frame, and that is the only path that may write the JSON null.
  if (!(def.index_frame != nullptr ? w.json_string(def.index_frame) : w.literal("null")))
    return false;

  // One pass over the callback (or its absence), building a local, stable verdict per
  // element before any JSON is written -- §1.3's table, applied uniformly so "class" and
  // "values" can each be walked from the same derived buffer without re-invoking the
  // callback (a second callback pass could observe a state change, e.g.
  // commit_cell_profile(), between the two passes).
  for (uint16_t i = 0; i < count; i++) {
    SeriesElement raw{};
    const bool answered = def.element != nullptr && def.element(def.ctx, i, &raw);
    SeriesElement out{};  // defaults to a hole; only the branches below promote it
    if (answered) {
      if (raw.cls == 's') {
        out.cls = 's';  // SNA always carries a null value, regardless of raw.has_value
      } else if (raw.cls == '-') {
        out.cls = '-';
      } else if ((raw.cls == 'r' || raw.cls == 'i') && raw.has_value) {
        out.cls = raw.cls;
        out.has_value = true;
        out.value = raw.value;
      }
      // else: 'r'/'i' without a value, or any other character -- stays the default hole.
    }
    resolved[i] = out;
  }

  if (!w.literal(",\"values\":["))
    return false;
  for (uint16_t i = 0; i < count; i++) {
    if (i != 0 && !w.literal(","))
      return false;
    if (resolved[i].has_value) {
      if (!w.number_float(resolved[i].value, def.decimals))
        return false;
    } else {
      if (!w.literal("null"))
        return false;
    }
  }
  // class is always an explicit string, never null (§1.3): a single-pass emitter cannot
  // know "every element is real" before it has written the values.
  if (!w.literal("],\"class\":\""))
    return false;
  for (uint16_t i = 0; i < count; i++) {
    if (!w.write(&resolved[i].cls, 1))
      return false;
  }
  if (!w.literal("\""))
    return false;

  if (!w.literal(",\"pack_index_base\":"))
    return false;
  if (!(def.pack_index_known ? w.number_u32(static_cast<uint32_t>(def.pack_index_base)) : w.literal("null")))
    return false;
  return w.literal("}");
}

/// A node emits its series iff it declares one AND is not absent. Both `write_node` and
/// `subtree_has_series` MUST call this: two copies of the condition is how the legend ends up
/// naming characters no emitted series carries.
inline bool node_emits_series(const Snapshot &s, const Resolved &r, size_t index) {
  return index < s.node_count && s.nodes[index].series != nullptr && r.states[index] != State::ABSENT;
}

__attribute__((noinline)) inline bool write_series_stack(ChunkWriter &w, const SeriesDef &def) {
  SeriesElement resolved[SERIES_MAX_ELEMENTS];
  return write_series_with(w, def, resolved);
}

inline bool write_series(ChunkWriter &w, const SeriesDef &def, SeriesElement *scratch = nullptr) {
  return scratch != nullptr ? write_series_with(w, def, scratch) : write_series_stack(w, def);
}

/// Does the subtree rooted at `s.nodes[root_index]` carry at least one EMITTED series --
/// `write_envelope` uses this to decide whether `series_legend` is the full closed legend or
/// `null`, recomputed over the narrowed subtree under `?path=`. Nodes are pre-order (a child's
/// index is always greater than its parent's), so a series-emitting node at index i is inside
/// `root_index`'s subtree iff walking `parent` upward from i reaches `root_index` before
/// NO_PARENT.
inline bool subtree_has_series(const Snapshot &s, const Resolved &r, size_t root_index) {
  if (root_index >= s.node_count)
    return false;
  if (node_emits_series(s, r, root_index))
    return true;
  for (size_t i = root_index + 1; i < s.node_count; i++) {
    if (!node_emits_series(s, r, i))
      continue;
    uint16_t walk = s.nodes[i].parent;
    for (size_t step = 0; step < s.node_count && walk != NO_PARENT; step++) {
      if (walk >= s.node_count)
        break;
      if (walk == root_index)
        return true;
      walk = s.nodes[walk].parent;
    }
  }
  return false;
}

inline bool write_action_value(ChunkWriter &w, const ActionValue &v, int8_t decimals) {
  switch (v.kind) {
    case ValueKind::BOOL:
      return w.literal(v.boolean ? "true" : "false");
    case ValueKind::NUMBER:
      return w.number_float(v.number, decimals);
    case ValueKind::TEXT:
      return w.json_string(v.text);
    case ValueKind::NONE:
      break;
  }
  return w.literal("null");
}

/// `{"state","why":{...},"age_ms","value"}` (§5.5).
inline bool write_action_result(ChunkWriter &w, const ActionResult &r, int8_t decimals) {
  if (!w.literal("{\"state\":") || !w.json_string(action_state_name(r.state)))
    return false;
  if (!w.literal(",\"why\":{\"code\":") || !w.json_string(action_code_name(r.code)))
    return false;
  if (!w.literal(",\"detail\":null,\"since_ms\":null,\"evidence\":{}},\"age_ms\":") || !w.number_u32(r.age_ms))
    return false;
  if (!w.literal(",\"value\":") || !write_action_value(w, r.value, decimals))
    return false;
  return w.literal("}");
}

inline bool write_optional_number(ChunkWriter &w, bool known, float value, int8_t decimals) {
  return known && is_finite_float(value) ? w.number_float(value, decimals) : w.literal("null");
}

/// §5.4: the seven v2 keys, then the extension keys, in the pinned order.
inline bool write_action(ChunkWriter &w, const NodeDef &node, const ActionDef &def, const ActionSample &a) {
  if (!w.literal("{\"key\":") || !w.json_string(def.key))
    return false;
  if (!w.literal(",\"label\":") || !w.json_string(def.label))
    return false;
  if (!w.literal(",\"method\":\"POST\",\"path\":\"/api/v1/actions/") || !w.write(node.id, std::strlen(node.id)))
    return false;
  if (!w.literal("/") || !w.write(def.key, std::strlen(def.key)))
    return false;
  const bool enabled = a.enabled || def.control == Control::SWITCH || def.control == Control::BUTTON;
  if (!w.literal("\",\"enabled\":") || !w.literal(enabled ? "true" : "false"))
    return false;
  if (!w.literal(",\"why\":"))
    return false;
  if (enabled) {
    if (!w.literal("null"))
      return false;
  } else if (!w.literal("{\"code\":\"entity_unavailable\",\"detail\":null,\"since_ms\":null,\"evidence\":{}}")) {
    return false;
  }
  if (!w.literal(",\"body\":"))
    return false;
  const char *body = "null";
  switch (def.control) {
    case Control::SWITCH:
      body = "{\"value\":\"bool\"}";
      break;
    case Control::SELECT:
      body = "{\"value\":\"string\"}";
      break;
    case Control::NUMBER:
      body = "{\"value\":\"number\"}";
      break;
    case Control::BUTTON:
      break;
  }
  if (!w.literal(body))
    return false;
  if (!w.literal(",\"control\":") || !w.json_string(control_name(def.control)))
    return false;
  if (!w.literal(",\"value\":"))
    return false;
  bool ok = true;
  if (!a.has_value || def.control == Control::BUTTON) {
    ok = w.literal("null");
  } else if (def.control == Control::SWITCH) {
    ok = w.literal(a.boolean ? "true" : "false");
  } else if (def.control == Control::SELECT) {
    ok = w.json_string(a.text);
  } else {
    ok = is_finite_float(a.number) ? w.number_float(a.number, def.decimals) : w.literal("null");
  }
  if (!ok)
    return false;
  if (!w.literal(",\"options\":"))
    return false;
  if (def.control == Control::SELECT) {
    if (!w.literal("["))
      return false;
    for (uint16_t i = 0; i < def.option_count; i++) {
      if (i != 0 && !w.literal(","))
        return false;
      if (!w.json_string(def.options[i]))
        return false;
    }
    if (!w.literal("]"))
      return false;
  } else if (!w.literal("null")) {
    return false;
  }
  const bool is_number = def.control == Control::NUMBER;
  if (!w.literal(",\"min\":") || !write_optional_number(w, is_number && a.has_min, a.min, def.decimals))
    return false;
  if (!w.literal(",\"max\":") || !write_optional_number(w, is_number && a.has_max, a.max, def.decimals))
    return false;
  if (!w.literal(",\"step\":") || !write_optional_number(w, is_number && a.has_step, a.step, def.decimals))
    return false;
  if (!w.literal(",\"unit\":"))
    return false;
  if (!(is_number && a.unit != nullptr ? w.json_string(a.unit) : w.literal("null")))
    return false;
  if (!w.literal(",\"result\":"))
    return false;
  if (a.result.state == ActionState::NONE)
    return w.literal("null}");
  return write_action_result(w, a.result, def.decimals) && w.literal("}");
}

/// The body of a POST response (§6.2): `{"seq":N,"node":"<id>","key":"<key>","result":{...}}`.
inline bool write_action_response(ChunkWriter &w, uint32_t seq, const char *node, const char *key,
                                  const ActionResult &result, int8_t decimals) {
  if (!w.literal("{\"seq\":") || !w.number_u32(seq))
    return false;
  if (!w.literal(",\"node\":") || !w.json_string(node))
    return false;
  if (!w.literal(",\"key\":") || !w.json_string(key))
    return false;
  if (!w.literal(",\"result\":") || !write_action_result(w, result, decimals))
    return false;
  return w.literal("}");
}

inline bool write_node(ChunkWriter &w, const Snapshot &s, const Resolved &r, size_t index) {
  const NodeDef &node = s.nodes[index];
  if (!w.literal("{\"id\":") || !w.json_string(node.id))
    return false;
  if (!w.literal(",\"kind\":") || !w.json_string(node.kind))
    return false;
  if (!w.literal(",\"label\":") || !w.json_string(node.label))
    return false;
  if (!w.literal(",\"state\":") || !w.json_string(state_name(r.states[index])))
    return false;
  if (!w.literal(",\"why\":") || !write_node_why(w, s, r, index))
    return false;
  if (!w.literal(",\"provenance\":") || !write_provenance(w, s, r, index))
    return false;
  if (!w.literal(",\"values\":["))
    return false;
  for (uint16_t k = 0; k < node.value_count; k++) {
    if (k != 0 && !w.literal(","))
      return false;
    if (!write_value(w, s.values[node.value_first + k], node.budget_ms, node.budget_known))
      return false;
  }
  if (!w.literal("],\"series\":"))
    return false;
  if (node_emits_series(s, r, index)) {
    if (!write_series(w, *node.series, r.series_scratch))
      return false;
  } else {
    if (!w.literal("null"))
      return false;
  }
  if (!w.literal(",\"children\":["))
    return false;
  bool first_child = true;
  for (size_t j = index + 1; j < s.node_count; j++) {
    if (s.nodes[j].parent != static_cast<uint16_t>(index))
      continue;
    if (!first_child && !w.literal(","))
      return false;
    first_child = false;
    if (!write_node(w, s, r, j))
      return false;
  }
  if (!w.literal("],\"actions\":["))
    return false;
  for (uint16_t k = 0; k < node.action_count; k++) {
    const size_t a = static_cast<size_t>(node.action_first) + k;
    if (a >= s.action_count || s.actions == nullptr || s.action_samples == nullptr)
      break;
    if (k != 0 && !w.literal(","))
      return false;
    if (!write_action(w, node, s.actions[a], s.action_samples[a]))
      return false;
  }
  if (!w.literal("]"))
    return false;
  if (node.role != nullptr) {
    if (!w.literal(",\"role\":") || !w.json_string(node.role))
      return false;
    if (!w.literal(",\"roles_available\":["))
      return false;
    if (!write_csv_as_json_array(w, node.roles_available))
      return false;
    if (!w.literal("]"))
      return false;
  }
  return w.literal("}");
}

inline bool write_served_by(ChunkWriter &w, const Snapshot &s) {
  const Identity &id = s.identity;
  const bool identity_complete = id.board != nullptr && id.node_id != nullptr && id.mac != nullptr;
  if (!w.literal("{\"board\":"))
    return false;
  if (!(identity_complete ? w.json_string(id.board) : w.literal("null")))
    return false;
  if (!w.literal(",\"node_id\":"))
    return false;
  if (!(identity_complete ? w.json_string(id.node_id) : w.literal("null")))
    return false;
  if (!w.literal(",\"mac\":"))
    return false;
  if (!(identity_complete ? w.json_string(id.mac) : w.literal("null")))
    return false;
  if (!w.literal(",\"state\":") || !w.json_string(identity_complete ? "ok" : "unknown"))
    return false;
  if (!w.literal(",\"why\":"))
    return false;
  if (identity_complete) {
    if (!w.literal("null"))
      return false;
  } else {
    if (!w.literal("{\"code\":\"no_identity\",\"detail\":null,\"since_ms\":null,\"evidence\":{}}"))
      return false;
  }
  if (!w.literal(",\"uptime_ms\":") || !w.number_u32(s.uptime_ms))
    return false;
  if (!w.literal(",\"build\":"))
    return false;
  if (!w.json_string(id.build != nullptr ? id.build : "unknown"))
    return false;
  if (!w.literal(",\"owns\":["))
    return false;
  if (identity_complete) {
    for (size_t i = 0; i < id.owns_count; i++) {
      if (i != 0 && !w.literal(","))
        return false;
      if (!w.json_string(id.owns[i]))
        return false;
    }
  }
  if (!w.literal("],\"components\":["))
    return false;
  for (size_t i = 0; i < id.components_count; i++) {
    if (i != 0 && !w.literal(","))
      return false;
    if (!w.json_string(id.components[i]))
      return false;
  }
  return w.literal("]}");
}

/// The nearest role-declaring STRICT ancestor of `s.nodes[root_index]`, walking `NodeDef.parent`
/// upward. Zero bytes of RAM: an O(depth) walk over caller-owned data.
///
/// Bounded twice: the walk stops at `NO_PARENT` (the true root) and, independently, after at
/// most `s.node_count` steps, so a malformed parent cycle cannot hang the HTTP handler.
inline bool write_ancestor_role(ChunkWriter &w, const Snapshot &s, size_t root_index) {
  const NodeDef *ancestor = nullptr;
  uint16_t walk = s.nodes[root_index].parent;
  for (size_t step = 0; step < s.node_count && walk != NO_PARENT; step++) {
    if (walk >= s.node_count)
      break;
    const NodeDef &candidate = s.nodes[walk];
    if (candidate.role != nullptr) {
      ancestor = &candidate;
      break;
    }
    walk = candidate.parent;
  }
  if (ancestor == nullptr)
    return w.literal("null");
  if (!w.literal("{\"node\":") || !w.json_string(ancestor->id))
    return false;
  if (!w.literal(",\"label\":") || !w.json_string(ancestor->label))
    return false;
  if (!w.literal(",\"role\":") || !w.json_string(ancestor->role))
    return false;
  return w.literal("}");
}

}  // namespace detail

/// `write_envelope` returns false only on flush failure -- every input shape must already
/// produce schema-valid JSON, which is what script/portal_schema.py checks.
inline bool write_envelope(ChunkWriter &w, const Snapshot &s, const Resolved &r, const char *path) {
  if (!w.literal("{\"schema_version\":") || !w.number_u32(SCHEMA_VERSION))
    return false;
  if (!w.literal(",\"seq\":") || !w.number_u32(s.seq))
    return false;

  // root_index is resolved here, before series_legend, so the legend (below) and
  // path/ancestor_role/tree (further below) all agree on the same narrowed-or-not root --
  // A ?path= narrowing recomputes the legend over the narrowed subtree.
  size_t root_index = 0;
  bool path_found = false;
  if (path != nullptr) {
    for (size_t i = 0; i < s.node_count; i++) {
      if (s.nodes[i].id != nullptr && std::strcmp(s.nodes[i].id, path) == 0) {
        root_index = i;
        path_found = true;
        break;
      }
    }
    if (!path_found) {
      // Caller is required to check has_node() before calling write_envelope with a path;
      // falling back to the true root keeps this function total rather than undefined.
      root_index = 0;
    }
  }

  // The complete closed legend when the emitted (possibly narrowed) subtree carries at least
  // one series, null when it carries none -- the legend names the whole closed `class`
  // vocabulary, never a computed used-subset, because a streaming emitter writes the legend
  // before it has written any node's series.
  if (!w.literal(",\"series_legend\":"))
    return false;
  if (detail::subtree_has_series(s, r, root_index)) {
    if (!w.literal("{\"r\":\"real\",\"s\":\"SNA\",\"i\":\"implausible\",\"-\":\"not measured\"}"))
      return false;
  } else {
    if (!w.literal("null"))
      return false;
  }

  if (!w.literal(",\"served_by\":") || !detail::write_served_by(w, s))
    return false;
  if (!w.literal(",\"peers\":[]"))
    return false;
  if (!w.literal(",\"links\":["))
    return false;
  for (size_t i = 0; i < s.link_count && s.links != nullptr; i++) {
    if (i != 0 && !w.literal(","))
      return false;
    if (!w.literal("{\"rel\":") || !w.json_string(s.links[i].rel))
      return false;
    if (!w.literal(",\"label\":") || !w.json_string(s.links[i].label))
      return false;
    if (!w.literal(",\"href\":") || !w.json_string(s.links[i].href))
      return false;
    if (!w.literal("}"))
      return false;
  }
  if (!w.literal("]"))
    return false;

  if (path != nullptr && path_found) {
    if (!w.literal(",\"path\":") || !w.json_string(s.nodes[root_index].id))
      return false;
    // Order is pinned: immediately after "path", before "tree"; emitted iff "path" is.
    if (!w.literal(",\"ancestor_role\":") || !detail::write_ancestor_role(w, s, root_index))
      return false;
  }

  if (!w.literal(",\"tree\":") || !detail::write_node(w, s, r, root_index))
    return false;
  return w.literal("}");
}

/// The fingerprint covers every value's reading and every derived *state*; it deliberately
/// excludes raw ages. A raw age changes every millisecond, and a seq that advanced with it
/// would make `?after=` never return 204 -- but an age crossing a node's budget_ms (ok ->
/// stale, same reading) IS a derived-state change and must still move the fingerprint, or a
/// conditionally-polling client would keep rendering a cached body claiming `state: "ok"`
/// past its budget: the one lie the validator exists to make unrepresentable, reintroduced
/// one layer up, at the polling cadence, where the validator cannot see it. So this mixes classify_value()'s verdict
/// (computed against the value's own node's budget_ms) alongside the reading, which is exactly what write_value()
/// emits.
///
/// Iterating by node rather than flatly over s.values is what supplies that budget_ms; it
/// relies on the invariant Python's codegen guarantees -- every value sits in exactly one
/// node's [value_first, value_first + value_count) range, contiguously, because a node's own
/// `values:` are always emitted immediately after that node and before its children.
inline uint32_t fingerprint(const Snapshot &s) {
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](uint32_t v) {
    hash ^= v;
    hash *= 16777619u;
  };
  const auto mix_str = [&](const char *str) {
    if (str == nullptr) {
      mix(0);
      return;
    }
    while (*str != '\0') {
      mix(static_cast<uint32_t>(static_cast<uint8_t>(*str)));
      str++;
    }
  };
  for (size_t n = 0; n < s.node_count; n++) {
    const NodeDef &node = s.nodes[n];
    for (uint16_t k = 0; k < node.value_count; k++) {
      const ValueSample &v = s.values[node.value_first + k];
      const detail::ValueVerdict verdict = detail::classify_value(v, node.budget_ms, node.budget_known);
      mix(static_cast<uint32_t>(verdict.state));
      mix(v.has_value ? 1u : 0u);
      mix(v.age_known ? 1u : 0u);
      mix(v.is_text ? 1u : 0u);
      mix(v.is_bool ? 1u : 0u);
      mix(v.text_overflow ? 1u : 0u);
      if (v.has_value) {
        if (v.is_text) {
          mix_str(v.text);
        } else {
          uint32_t bits;
          std::memcpy(&bits, &v.number, sizeof(bits));
          mix(bits);
        }
      }
    }
  }
  // Links: a change advances seq.
  mix(static_cast<uint32_t>(s.link_count));
  for (size_t i = 0; i < s.link_count && s.links != nullptr; i++) {
    mix_str(s.links[i].rel);
    mix_str(s.links[i].href);
  }
  // Actions: the emitted value, enabled flag and result state / why code. Never result.age_ms.
  for (size_t a = 0; a < s.action_count && s.actions != nullptr && s.action_samples != nullptr; a++) {
    const ActionSample &sample = s.action_samples[a];
    mix(sample.enabled ? 1u : 0u);
    mix(sample.has_value ? 1u : 0u);
    mix(sample.boolean ? 1u : 0u);
    uint32_t bits;
    std::memcpy(&bits, &sample.number, sizeof(bits));
    mix(bits);
    mix_str(sample.text);
    mix(static_cast<uint32_t>(sample.result.state));
    mix(static_cast<uint32_t>(sample.result.code));
  }
  return hash;
}

/// Decimal, uint32 range, no sign -- "" / "12x" / "-1" / "4294967296" all rejected.
inline bool parse_after(const char *text, uint32_t *out) {
  if (text == nullptr || out == nullptr || *text == '\0')
    return false;
  uint32_t value = 0;
  while (*text != '\0') {
    const uint32_t digit = static_cast<uint32_t>(*text - '0');
    if (*text < '0' || *text > '9' || value > (UINT32_MAX - digit) / 10u)
      return false;
    value = value * 10u + digit;
    text++;
  }
  *out = value;
  return true;
}

// ---- action request evaluation (§6.2) ---------------------------------------------------------
//
// Pure functions: the httpd task calls these under the portal's mutex, records the outcome in the
// tree and (for ACCEPT) hands the parsed value to loop(). No entity is touched here.

enum class Verdict : uint8_t {
  ACCEPT,  ///< valid -> 202, recorded as running/queued
  REFUSE,  ///< recorded as refused with `code`
  BUSY,    ///< previous invocation still running -> 409 {"error":"busy"}, nothing recorded
};

struct ActionOutcome {
  Verdict verdict{Verdict::REFUSE};
  int http{400};
  ActionCode code{ActionCode::NONE};  ///< QUEUED for ACCEPT, the refusal code for REFUSE
  ActionValue value;                  ///< the requested value as parsed (kind NONE if unparseable)
};

namespace detail {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool equals_ci(const char *text, size_t len, const char *word) {
  size_t i = 0;
  for (; i < len && word[i] != '\0'; i++) {
    if (lower(text[i]) != word[i])
      return false;
  }
  return i == len && word[i] == '\0';
}

inline bool double_is_finite(double v) { return v == v && v <= 3.4028234663852886e38 && v >= -3.4028234663852886e38; }

/// Strict decimal number: [+-]digits[.digits][(e|E)[+-]digits], the whole span. `allow_plus`
/// is false for JSON. Overflow yields +-infinity (a finite-check is the caller's job).
inline bool parse_number_span(const char *p, size_t len, bool allow_plus, double *out) {
  const char *end = p + len;
  bool negative = false;
  if (p < end && (*p == '-' || (allow_plus && *p == '+'))) {
    negative = *p == '-';
    p++;
  }
  double mantissa = 0.0;
  size_t int_digits = 0;
  int exp10 = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    mantissa = mantissa * 10.0 + static_cast<double>(*p - '0');
    int_digits++;
    p++;
  }
  size_t frac_digits = 0;
  if (p < end && *p == '.') {
    p++;
    while (p < end && *p >= '0' && *p <= '9') {
      if (frac_digits < 30) {
        mantissa = mantissa * 10.0 + static_cast<double>(*p - '0');
        exp10--;
      }
      frac_digits++;
      p++;
    }
    if (frac_digits == 0)
      return false;
  }
  if (int_digits == 0)
    return false;
  if (!allow_plus && int_digits > 1 && len > 0) {
    // JSON: no leading zeros ("01"). Re-scan the integer part start.
    const char *q = end - len;
    if (*q == '-')
      q++;
    if (*q == '0')
      return false;
  }
  if (p < end && (*p == 'e' || *p == 'E')) {
    p++;
    bool exp_negative = false;
    if (p < end && (*p == '+' || *p == '-')) {
      exp_negative = *p == '-';
      p++;
    }
    size_t exp_digits = 0;
    int e = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      if (e < 100000)
        e = e * 10 + (*p - '0');
      exp_digits++;
      p++;
    }
    if (exp_digits == 0)
      return false;
    exp10 += exp_negative ? -e : e;
  }
  if (p != end)
    return false;
  double value = mantissa;
  if (value != 0.0) {
    if (exp10 > 400) {
      value = __builtin_huge_val();
    } else if (exp10 < -400) {
      value = 0.0;
    } else {
      int e = exp10 < 0 ? -exp10 : exp10;
      double scale = 1.0;
      double base = 10.0;
      while (e != 0) {
        if (e & 1)
          scale *= base;
        base *= base;
        e >>= 1;
      }
      value = exp10 < 0 ? value / scale : value * scale;
    }
  }
  *out = negative ? -value : value;
  return true;
}

struct Cursor {
  const char *p;
  const char *end;
  bool done() const { return p >= end; }
  void skip_ws() {
    while (p < end && is_space(*p))
      p++;
  }
};

inline int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/// Parses a JSON string at the cursor (which is on the opening quote). Decoded bytes go to
/// `out` (NUL-terminated, truncated at `cap - 1`, `*truncated` set); out may be nullptr to skip.
inline bool parse_json_string(Cursor &c, char *out, size_t cap, bool *truncated) {
  if (c.p >= c.end || *c.p != '"')
    return false;
  c.p++;
  size_t n = 0;
  if (truncated != nullptr)
    *truncated = false;
  const auto put = [&](char ch) {
    if (out == nullptr)
      return;
    if (n + 1 < cap)
      out[n++] = ch;
    else if (truncated != nullptr)
      *truncated = true;
  };
  while (c.p < c.end) {
    const unsigned char ch = static_cast<unsigned char>(*c.p++);
    if (ch == '"') {
      if (out != nullptr)
        out[n] = '\0';
      return true;
    }
    if (ch < 0x20)
      return false;
    if (ch != '\\') {
      put(static_cast<char>(ch));
      continue;
    }
    if (c.p >= c.end)
      return false;
    const char esc = *c.p++;
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        put(esc);
        break;
      case 'b':
        put('\b');
        break;
      case 'f':
        put('\f');
        break;
      case 'n':
        put('\n');
        break;
      case 'r':
        put('\r');
        break;
      case 't':
        put('\t');
        break;
      case 'u': {
        if (c.end - c.p < 4)
          return false;
        uint32_t cp = 0;
        for (int i = 0; i < 4; i++) {
          const int h = hex_digit(c.p[i]);
          if (h < 0)
            return false;
          cp = (cp << 4) | static_cast<uint32_t>(h);
        }
        c.p += 4;
        if (cp < 0x80) {
          put(static_cast<char>(cp));
        } else if (cp < 0x800) {
          put(static_cast<char>(0xC0 | (cp >> 6)));
          put(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
          put('?');  // lone surrogate halves are not representable; never matches an option
        } else {
          put(static_cast<char>(0xE0 | (cp >> 12)));
          put(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          put(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default:
        return false;
    }
  }
  return false;  // unterminated
}

/// Skips one JSON value of any type (depth-bounded). Syntax is checked loosely for nested
/// containers: balanced brackets with valid strings, which is all a flat `{"value":...}` needs.
inline bool skip_json_value(Cursor &c, int depth) {
  if (depth > 8)
    return false;
  c.skip_ws();
  if (c.done())
    return false;
  const char ch = *c.p;
  if (ch == '"')
    return parse_json_string(c, nullptr, 0, nullptr);
  if (ch == '{' || ch == '[') {
    const char close = ch == '{' ? '}' : ']';
    c.p++;
    c.skip_ws();
    if (!c.done() && *c.p == close) {
      c.p++;
      return true;
    }
    for (;;) {
      c.skip_ws();
      if (ch == '{') {
        if (!parse_json_string(c, nullptr, 0, nullptr))
          return false;
        c.skip_ws();
        if (c.done() || *c.p != ':')
          return false;
        c.p++;
      }
      if (!skip_json_value(c, depth + 1))
        return false;
      c.skip_ws();
      if (c.done())
        return false;
      if (*c.p == ',') {
        c.p++;
        continue;
      }
      if (*c.p == close) {
        c.p++;
        return true;
      }
      return false;
    }
  }
  const char *start = c.p;
  while (c.p < c.end && !is_space(*c.p) && *c.p != ',' && *c.p != '}' && *c.p != ']')
    c.p++;
  const size_t len = static_cast<size_t>(c.p - start);
  double ignored;
  return equals_ci(start, len, "true") || equals_ci(start, len, "false") || equals_ci(start, len, "null") ||
         parse_number_span(start, len, false, &ignored);
}

enum class JsonKind : uint8_t { MISSING, BOOL, NUMBER, TEXT, NULL_VALUE, OTHER };

struct JsonValue {
  JsonKind kind{JsonKind::MISSING};
  bool boolean{false};
  double number{0.0};
  char text[ACTION_TEXT_MAX]{};
  bool truncated{false};
};

/// Finds `"value"` in a flat JSON object. Returns false if the body is not a JSON object.
/// `empty_ok` reports a body of only whitespace (valid for a button).
inline bool parse_json_body(const char *body, size_t len, JsonValue *out, bool *empty) {
  Cursor c{body, body + len};
  c.skip_ws();
  *empty = c.done();
  if (*empty)
    return true;
  if (*c.p != '{')
    return false;
  c.p++;
  c.skip_ws();
  if (!c.done() && *c.p == '}') {
    c.p++;
  } else {
    for (;;) {
      c.skip_ws();
      char key[16];
      bool key_truncated = false;
      if (!parse_json_string(c, key, sizeof(key), &key_truncated))
        return false;
      c.skip_ws();
      if (c.done() || *c.p != ':')
        return false;
      c.p++;
      c.skip_ws();
      if (c.done())
        return false;
      const bool is_value_key = !key_truncated && std::strcmp(key, "value") == 0;
      if (is_value_key) {
        JsonValue v;
        const char ch = *c.p;
        if (ch == '"') {
          v.kind = JsonKind::TEXT;
          if (!parse_json_string(c, v.text, sizeof(v.text), &v.truncated))
            return false;
        } else if (ch == '{' || ch == '[') {
          v.kind = JsonKind::OTHER;
          if (!skip_json_value(c, 0))
            return false;
        } else {
          const char *start = c.p;
          while (c.p < c.end && !is_space(*c.p) && *c.p != ',' && *c.p != '}')
            c.p++;
          const size_t n = static_cast<size_t>(c.p - start);
          if (n == 4 && std::memcmp(start, "true", 4) == 0) {
            v.kind = JsonKind::BOOL;
            v.boolean = true;
          } else if (n == 5 && std::memcmp(start, "false", 5) == 0) {
            v.kind = JsonKind::BOOL;
          } else if (n == 4 && std::memcmp(start, "null", 4) == 0) {
            v.kind = JsonKind::NULL_VALUE;
          } else if (parse_number_span(start, n, false, &v.number)) {
            v.kind = JsonKind::NUMBER;
          } else {
            return false;
          }
        }
        *out = v;
      } else if (!skip_json_value(c, 0)) {
        return false;
      }
      c.skip_ws();
      if (c.done())
        return false;
      if (*c.p == ',') {
        c.p++;
        continue;
      }
      if (*c.p == '}') {
        c.p++;
        break;
      }
      return false;
    }
  }
  c.skip_ws();
  return c.done();
}

inline ActionOutcome refuse(int http, ActionCode code) {
  ActionOutcome o;
  o.verdict = Verdict::REFUSE;
  o.http = http;
  o.code = code;
  return o;
}

inline void set_text(ActionValue *v, const char *text, size_t len) {
  v->kind = ValueKind::TEXT;
  const size_t n = len < ACTION_TEXT_MAX - 1 ? len : ACTION_TEXT_MAX - 1;
  std::memcpy(v->text, text, n);
  v->text[n] = '\0';
}

inline bool has_option(const ActionDef &def, const char *text) {
  for (uint16_t i = 0; i < def.option_count; i++) {
    if (def.options[i] != nullptr && std::strcmp(def.options[i], text) == 0)
      return true;
  }
  return false;
}

inline ActionValue value_from(const JsonValue &jv) {
  ActionValue v;
  if (jv.kind == JsonKind::BOOL) {
    v.kind = ValueKind::BOOL;
    v.boolean = jv.boolean;
  } else if (jv.kind == JsonKind::NUMBER && double_is_finite(jv.number)) {
    v.kind = ValueKind::NUMBER;
    v.number = static_cast<float>(jv.number);
  } else if (jv.kind == JsonKind::TEXT) {
    set_text(&v, jv.text, std::strlen(jv.text));
  }
  return v;
}

/// The entity can be driven: switch and button always, select/number only with a state.
inline bool action_enabled(const ActionDef &def, const ActionSample &sample) {
  return def.control == Control::SWITCH || def.control == Control::BUTTON || sample.enabled;
}

inline ActionOutcome evaluate_typed(const ActionDef &def, const ActionSample &sample, const JsonValue &jv) {
  const ActionValue parsed = value_from(jv);
  ActionOutcome out;
  out.value = parsed;
  switch (def.control) {
    case Control::SWITCH:
      if (jv.kind != JsonKind::BOOL) {
        out = refuse(400, ActionCode::WRONG_TYPE);
        out.value = parsed;
      }
      break;
    case Control::SELECT:
      if (jv.kind != JsonKind::TEXT) {
        out = refuse(400, ActionCode::WRONG_TYPE);
        out.value = parsed;
      } else if (jv.truncated || !has_option(def, jv.text)) {
        out = refuse(422, ActionCode::UNKNOWN_OPTION);
        out.value = parsed;
      }
      break;
    case Control::NUMBER:
      if (jv.kind != JsonKind::NUMBER) {
        out = refuse(400, ActionCode::WRONG_TYPE);
        out.value = parsed;
      } else if (!double_is_finite(jv.number) || (sample.has_min && jv.number < static_cast<double>(sample.min)) ||
                 (sample.has_max && jv.number > static_cast<double>(sample.max))) {
        out = refuse(422, ActionCode::OUT_OF_RANGE);
        out.value = parsed;
      }
      break;
    case Control::BUTTON:
      break;
  }
  if (out.code == ActionCode::NONE) {
    out.verdict = Verdict::ACCEPT;
    out.http = 202;
    out.code = ActionCode::QUEUED;
  }
  return out;
}

}  // namespace detail

/// `Content-Type: application/json`. `body` holds at most ACTION_BODY_MAX bytes of the request;
/// `body_len` is the full Content-Length (larger than ACTION_BODY_MAX -> body_too_large).
/// Order of checks: disabled (409, recorded), busy (409, not recorded), size, syntax, type,
/// range / option.
inline ActionOutcome evaluate_action_json(const ActionDef &def, const ActionSample &sample, bool busy, const char *body,
                                          size_t body_len) {
  if (!detail::action_enabled(def, sample))
    return detail::refuse(409, ActionCode::DISABLED);
  if (busy) {
    ActionOutcome o;
    o.verdict = Verdict::BUSY;
    o.http = 409;
    return o;
  }
  if (body_len > ACTION_BODY_MAX)
    return detail::refuse(400, ActionCode::BODY_TOO_LARGE);
  detail::JsonValue jv;
  bool empty = false;
  if (!detail::parse_json_body(body == nullptr ? "" : body, body_len, &jv, &empty))
    return detail::refuse(400, ActionCode::INVALID_BODY);
  if (def.control == Control::BUTTON) {
    ActionOutcome o;
    o.verdict = Verdict::ACCEPT;
    o.http = 202;
    o.code = ActionCode::QUEUED;
    return o;
  }
  if (empty || jv.kind == detail::JsonKind::MISSING)
    return detail::refuse(400, ActionCode::INVALID_BODY);
  return detail::evaluate_typed(def, sample, jv);
}

/// `Content-Type: application/x-www-form-urlencoded`, with the `value` field already extracted
/// and decoded (`has_value` false if the field is absent). switch: true/false/on/off/1/0;
/// number: a decimal; select: the option text.
inline ActionOutcome evaluate_action_text(const ActionDef &def, const ActionSample &sample, bool busy, bool has_value,
                                          const char *text, size_t body_len) {
  if (!detail::action_enabled(def, sample))
    return detail::refuse(409, ActionCode::DISABLED);
  if (busy) {
    ActionOutcome o;
    o.verdict = Verdict::BUSY;
    o.http = 409;
    return o;
  }
  if (body_len > ACTION_BODY_MAX)
    return detail::refuse(400, ActionCode::BODY_TOO_LARGE);
  if (def.control == Control::BUTTON) {
    ActionOutcome o;
    o.verdict = Verdict::ACCEPT;
    o.http = 202;
    o.code = ActionCode::QUEUED;
    return o;
  }
  if (!has_value || text == nullptr)
    return detail::refuse(400, ActionCode::INVALID_BODY);
  const size_t len = std::strlen(text);
  detail::JsonValue jv;
  jv.kind = detail::JsonKind::TEXT;
  jv.truncated = len >= ACTION_TEXT_MAX;
  std::memcpy(jv.text, text, len < ACTION_TEXT_MAX - 1 ? len : ACTION_TEXT_MAX - 1);
  if (def.control == Control::SWITCH) {
    if (detail::equals_ci(text, len, "true") || detail::equals_ci(text, len, "on") || std::strcmp(text, "1") == 0) {
      jv.kind = detail::JsonKind::BOOL;
      jv.boolean = true;
    } else if (detail::equals_ci(text, len, "false") || detail::equals_ci(text, len, "off") ||
               std::strcmp(text, "0") == 0) {
      jv.kind = detail::JsonKind::BOOL;
    }
  } else if (def.control == Control::NUMBER) {
    double n = 0.0;
    if (detail::parse_number_span(text, len, true, &n)) {
      jv.kind = detail::JsonKind::NUMBER;
      jv.number = n;
    } else if (detail::equals_ci(text, len, "nan") || detail::equals_ci(text, len, "inf") ||
               detail::equals_ci(text, len, "+inf") || detail::equals_ci(text, len, "-inf") ||
               detail::equals_ci(text, len, "infinity")) {
      jv.kind = detail::JsonKind::NUMBER;
      jv.number = __builtin_huge_val();  // infinity: refused as out_of_range
    }
  }
  return detail::evaluate_typed(def, sample, jv);
}

/// Finds and URL-decodes the `value` field of a form body (`a=1&value=on`). `+` is a space.
/// Returns false if the field is absent. Output is NUL-terminated and truncated at `cap - 1`.
inline bool form_value(const char *body, size_t len, char *out, size_t cap) {
  const char *p = body;
  const char *end = body + len;
  while (p <= end) {
    const char *amp = static_cast<const char *>(std::memchr(p, '&', static_cast<size_t>(end - p)));
    const char *pair_end = amp != nullptr ? amp : end;
    if (pair_end - p >= 6 && std::memcmp(p, "value=", 6) == 0) {
      size_t n = 0;
      for (const char *q = p + 6; q < pair_end; q++) {
        char ch = *q;
        if (ch == '+') {
          ch = ' ';
        } else if (ch == '%' && pair_end - q >= 3 && detail::hex_digit(q[1]) >= 0 && detail::hex_digit(q[2]) >= 0) {
          ch = static_cast<char>(detail::hex_digit(q[1]) * 16 + detail::hex_digit(q[2]));
          q += 2;
        }
        if (n + 1 < cap)
          out[n++] = ch;
      }
      out[n] = '\0';
      return true;
    }
    if (amp == nullptr)
      break;
    p = amp + 1;
  }
  return false;
}

// ---- routes (§6) -----------------------------------------------------------------------------

/// REDIRECT: while the access point is active, every other GET (captive detection probes) is
/// answered 302 to the portal's own root (§6.3).
enum class Route : uint8_t { NONE, PAGE, STATE, ACTION, REDIRECT };

struct RouteMatch {
  Route route{Route::NONE};
  const char *node{nullptr};  ///< ACTION only: points into the request path
  size_t node_len{0};
  const char *key{nullptr};
  size_t key_len{0};
};

/// Matches the three routes the portal claims. `path` is the request target; a query string
/// (from the first '?') is ignored. The action route is claimed for POST only, and only when
/// the build declares actions. With `ap_active`, every other GET is REDIRECT except /wifi,
/// /config.json and /wifisave (the captive portal's). Anything else is NONE and falls through.
inline RouteMatch match_route(bool is_post, const char *path, bool has_actions, bool ap_active = false) {
  RouteMatch m;
  if (path == nullptr)
    return m;
  size_t len = 0;
  while (path[len] != '\0' && path[len] != '?')
    len++;
  if (!is_post) {
    if (len == 1 && path[0] == '/')
      m.route = Route::PAGE;
    else if (len == 13 && std::memcmp(path, "/api/v1/state", 13) == 0)
      m.route = Route::STATE;
    else if (ap_active && !(len == 5 && std::memcmp(path, "/wifi", 5) == 0) &&
             !(len == 12 && std::memcmp(path, "/config.json", 12) == 0) &&
             !(len == 9 && std::memcmp(path, "/wifisave", 9) == 0))
      m.route = Route::REDIRECT;
    return m;
  }
  static const char PREFIX[] = "/api/v1/actions/";
  const size_t prefix_len = sizeof(PREFIX) - 1;
  if (!has_actions || len <= prefix_len || std::memcmp(path, PREFIX, prefix_len) != 0)
    return m;
  const char *rest = path + prefix_len;
  const size_t rest_len = len - prefix_len;
  size_t slash = rest_len;
  while (slash > 0 && rest[slash - 1] != '/')
    slash--;
  // rest = "<node>/<key>": the key is everything after the LAST '/', the node id has no '/'.
  if (slash < 2 || slash == rest_len)
    return m;
  for (size_t i = 0; i + 1 < slash; i++) {
    if (rest[i] == '/')
      return m;
  }
  m.route = Route::ACTION;
  m.node = rest;
  m.node_len = slash - 1;
  m.key = rest + slash;
  m.key_len = rest_len - slash;
  return m;
}

}  // namespace esphome::portal::core
