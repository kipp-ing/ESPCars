/// Host tests for the portal freestanding core (docs/portal-spec.md: PS-M1-05, PS-M1-07,
/// PS-M1-08, PS-M3-02, PS-M3-05). Vehicle-neutral fixtures only.
///   - resolve()-only cases check State severity/derivation without touching JSON.
///   - write_envelope()/ChunkWriter cases capture compact JSON into a std::string and grep it.

#include "harness.h"
#include "portal_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace esphome::portal::core;

namespace {

// --- capture plumbing ---------------------------------------------------------------

struct Capture {
  std::string data;
  std::vector<size_t> flush_sizes;
  size_t fail_at = static_cast<size_t>(-1);  // 0-based index of the flush call to fail
};

bool capture_flush(void *ctx, const char *data, size_t size) {
  auto *c = static_cast<Capture *>(ctx);
  size_t index = c->flush_sizes.size();
  c->flush_sizes.push_back(size);
  if (index == c->fail_at)
    return false;
  c->data.append(data, size);
  return true;
}

// --- fixture builders -----------------------------------------------------------------

ValueSample value(const char *key, bool has_value, bool age_known, uint32_t age_ms, bool is_text, const char *text,
                  float number, int8_t decimals = 0, bool text_overflow = false, const char *unit = nullptr,
                  const char *label = "Value") {
  ValueSample v{};
  v.key = key;
  v.label = label;
  v.unit = unit;
  v.text = text;
  v.number = number;
  v.age_ms = age_ms;
  v.decimals = decimals;
  v.is_text = is_text;
  v.has_value = has_value;
  v.age_known = age_known;
  v.text_overflow = text_overflow;
  return v;
}

ValueSample never_published(const char *key) { return value(key, false, false, 0, false, nullptr, 0.0f); }

ValueSample fresh_number(const char *key, float number, uint32_t age_ms = 0, int8_t decimals = 0) {
  return value(key, true, true, age_ms, false, nullptr, number, decimals);
}

ValueSample fresh_bool(const char *key, bool reading, uint32_t age_ms = 0) {
  // Note: a boolean rides `number` as 0.0f/1.0f (is_text stays false) so no
  // fresh field has to be threaded through every call site that only cares about "the
  // reading" -- see portal_core.h's own comment on ValueSample::number.
  ValueSample v = value(key, true, true, age_ms, false, nullptr, reading ? 1.0f : 0.0f);
  v.is_bool = true;
  return v;
}

ValueSample fresh_text(const char *key, const char *text, uint32_t age_ms = 0) {
  return value(key, true, true, age_ms, true, text, 0.0f);
}

NodeDef node(const char *id, uint16_t parent, uint16_t value_first, uint16_t value_count, uint32_t budget_ms,
             const char *label = nullptr, const char *kind = "kind", const char *absent_why = nullptr,
             const char *role = nullptr, const char *roles_available = nullptr, const char *unknown_value_key = nullptr,
             const char *unknown_equals = nullptr, const char *unknown_why = nullptr,
             const char *trust_value_key = nullptr) {
  NodeDef n{};
  n.id = id;
  n.kind = kind;
  n.label = label != nullptr ? label : id;
  n.parent = parent;
  n.value_first = value_first;
  n.value_count = value_count;
  n.budget_ms = budget_ms;
  n.budget_known = true;  // every existing caller wants ordinary (non-null) budget semantics;
                          // node_with_null_budget() below is the one that wants the opposite.
  n.absent_why = absent_why;
  n.role = role;
  n.roles_available = roles_available;
  n.unknown_value_key = unknown_value_key;
  n.unknown_equals = unknown_equals;
  n.unknown_why = unknown_why;
  n.trust_value_key = trust_value_key;
  return n;
}

/// Note: `budget_known = false` is the honest state for an on-change publisher with
/// no cadence -- no value under this node can ever be STALE, and "budget_ms" must
/// render as JSON null.
NodeDef node_with_null_budget(const char *id, uint16_t parent, uint16_t value_first, uint16_t value_count,
                              const char *label = nullptr, const char *kind = "kind") {
  NodeDef n = node(id, parent, value_first, value_count, /*budget_ms=*/0, label, kind);
  n.budget_known = false;
  return n;
}

Identity no_identity() {
  Identity id{};
  id.board = nullptr;
  id.node_id = nullptr;
  id.mac = nullptr;
  id.build = "test-build";
  id.owns = nullptr;
  id.owns_count = 0;
  id.components = nullptr;
  id.components_count = 0;
  return id;
}

Snapshot make_snapshot(const NodeDef *nodes, size_t node_count, const ValueSample *values, size_t value_count,
                       uint32_t seq = 1, uint32_t uptime_ms = 1000) {
  Snapshot s{};
  s.seq = seq;
  s.uptime_ms = uptime_ms;
  s.identity = no_identity();
  s.nodes = nodes;
  s.node_count = node_count;
  s.values = values;
  s.value_count = value_count;
  return s;
}

/// Minimal JSON string-literal decoder: reverses \", \\, \/, \b\f\n\r\t and \uXXXX so a
/// test can round-trip a ChunkWriter::json_string() payload without a full JSON parser.
/// A raw byte not preceded by a backslash passes straight through -- which is what makes
/// this decoder agnostic to whether the writer escapes bytes >= 0x80 as \u00XX or lets
/// them through raw the way the sibling core's escaped() does. Either choice round-trips
/// correctly here; only the *value under test* has to match, not the writer's style.
std::string json_decode(const std::string &encoded) {
  std::string out;
  for (size_t i = 0; i < encoded.size(); i++) {
    if (encoded[i] != '\\') {
      out += encoded[i];
      continue;
    }
    i++;
    if (i >= encoded.size())
      break;
    switch (encoded[i]) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        if (i + 4 < encoded.size()) {
          unsigned code = 0;
          for (int k = 1; k <= 4; k++) {
            char c = encoded[static_cast<size_t>(i + k)];
            code <<= 4;
            if (c >= '0' && c <= '9')
              code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
              code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
              code |= static_cast<unsigned>(c - 'A' + 10);
          }
          out += static_cast<char>(code & 0xFF);
          i += 4;
        }
        break;
      }
      default:
        out += encoded[i];
        break;
    }
  }
  return out;
}

/// The content of the first JSON string literal in `s` starting at or after `from`,
/// quotes stripped, still escaped (caller decodes if needed). Returns "" and sets
/// *found=false if there is no quote at all -- callers that need to tell "no match" from
/// "an empty string" must check *found.
std::string first_quoted(const std::string &s, size_t from, bool *found = nullptr) {
  size_t open = s.find('"', from);
  if (open == std::string::npos) {
    if (found != nullptr)
      *found = false;
    return "";
  }
  size_t i = open + 1;
  while (i < s.size()) {
    if (s[i] == '\\') {
      i += 2;
      continue;
    }
    if (s[i] == '"')
      break;
    i++;
  }
  if (found != nullptr)
    *found = true;
  return s.substr(open + 1, i - open - 1);
}

/// The raw text of the `ancestor_role` value in `s` -- either the literal `null`
/// or a flat `{...}` object (contract §2.2: "no other keys", so it never nests).
/// Returns "" and sets *found=false when the key does not appear at all, which a
/// caller must check explicitly: this is the exact absent-vs-null distinction the
/// contract (§2.3) makes load-bearing, and a helper that could not tell "no key"
/// from "key with no content" would be unable to test it.
std::string ancestor_role_raw(const std::string &s, bool *found = nullptr) {
  const std::string needle = "\"ancestor_role\":";
  size_t pos = s.find(needle);
  if (pos == std::string::npos) {
    if (found != nullptr)
      *found = false;
    return "";
  }
  if (found != nullptr)
    *found = true;
  size_t start = pos + needle.size();
  if (s.compare(start, 4, "null") == 0)
    return "null";
  size_t open = s.find('{', start);
  size_t close = s.find('}', open);
  if (open == std::string::npos || close == std::string::npos)
    return "";
  return s.substr(open, close - open + 1);
}

/// The string value of `"key":"..."` inside a flat ancestor_role object block
/// (as returned by ancestor_role_raw), independent of field order.
std::string ancestor_role_field(const std::string &block, const char *key) {
  std::string needle = std::string("\"") + key + "\":";
  size_t pos = block.find(needle);
  if (pos == std::string::npos)
    return "<missing>";
  bool found = false;
  std::string value = first_quoted(block, pos + needle.size(), &found);
  return found ? value : "<missing>";
}

// --- 4c Part 1 (contract the v2 contract):
// `series:` fixture builders -----------------------------------------------------------

/// One element's scripted answer. `answered=false` reproduces "callback returns false"
/// (§1.3's row 8) independent of whatever garbage sits in the other three fields --
/// exercising that they are simply never looked at in that case.
struct ScriptedElement {
  bool answered;
  char cls;
  bool has_value;
  float value;
};

/// A `SeriesElementFn` driven by a `std::vector<ScriptedElement>` passed as `ctx`.
/// Answering false past the end of the script (rather than reading out of bounds) also
/// lets the clamp test hand this a `count` larger than the script without crashing --
/// the clamp itself is what must stop the emitter from ever asking past
/// SERIES_MAX_ELEMENTS, not this stub.
bool scripted_element(void *ctx, uint16_t index, SeriesElement *out) {
  auto *script = static_cast<std::vector<ScriptedElement> *>(ctx);
  if (index >= script->size())
    return false;
  const ScriptedElement &e = (*script)[index];
  if (!e.answered)
    return false;
  out->cls = e.cls;
  out->has_value = e.has_value;
  out->value = e.value;
  return true;
}

/// A `SeriesElementFn` that also records the highest index it was ever asked for, via
/// `ctx` pointing at a `{script, *max_index_seen}` pair -- what the clamp test uses to
/// prove `write_series` never even ASKS past `SERIES_MAX_ELEMENTS - 1`, not merely that
/// it discards the answer.
struct RecordingScript {
  std::vector<ScriptedElement> script;
  int32_t max_index_seen = -1;
};

bool recording_element(void *ctx, uint16_t index, SeriesElement *out) {
  auto *rec = static_cast<RecordingScript *>(ctx);
  if (static_cast<int32_t>(index) > rec->max_index_seen)
    rec->max_index_seen = static_cast<int32_t>(index);
  if (index >= rec->script.size())
    return false;
  const ScriptedElement &e = rec->script[index];
  if (!e.answered)
    return false;
  out->cls = e.cls;
  out->has_value = e.has_value;
  out->value = e.value;
  return true;
}

SeriesDef series_def(const char *key, const char *label, const char *unit, int32_t index_base, const char *index_frame,
                     uint16_t count, int8_t decimals, SeriesElementFn element, void *ctx, bool pack_index_known = false,
                     int32_t pack_index_base = 0) {
  SeriesDef def{};
  def.key = key;
  def.label = label;
  def.unit = unit;
  def.index_base = index_base;
  def.index_frame = index_frame;
  def.count = count;
  def.decimals = decimals;
  def.element = element;
  def.ctx = ctx;
  def.pack_index_known = pack_index_known;
  def.pack_index_base = pack_index_base;
  return def;
}

/// Renders exactly one `write_series()` call into a captured JSON string -- the direct,
/// node-free path for every test that only needs the series object itself, not a whole
/// envelope/tree (the legend and `?path=` tests further down DO need the whole tree, and
/// go through `write_envelope` instead).
std::string render_series(const SeriesDef &def) {
  char buf[8192];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  if (!detail::write_series(w, def))
    return "<write_series returned false>";
  if (!w.finish())
    return "<finish returned false>";
  return cap.data;
}

/// Splits a flat JSON array (elements are only ever a bare number or the literal `null`,
/// never nested) into its raw element strings, given the position of its opening `[` --
/// enough for the clamp test's "exactly SERIES_MAX_ELEMENTS elements" assertion without
/// pulling in a real JSON parser for values that are never anything but a float or null.
std::vector<std::string> split_flat_array(const std::string &s, size_t open_bracket) {
  std::vector<std::string> out;
  size_t close = s.find(']', open_bracket);
  if (close == std::string::npos)
    return out;
  std::string inner = s.substr(open_bracket + 1, close - open_bracket - 1);
  if (inner.empty())
    return out;
  size_t start = 0;
  while (true) {
    size_t comma = inner.find(',', start);
    if (comma == std::string::npos) {
      out.push_back(inner.substr(start));
      break;
    }
    out.push_back(inner.substr(start, comma - start));
    start = comma + 1;
  }
  return out;
}

}  // namespace

// --- pinned constants -----------------------------------------------------------------

TEST(pinned_constants_match_the_contract) {
  // DEFAULT_BUDGET_MS deleted, C8: dead -- its only other
  // reference was this pin, and no runtime path ever read it (every node's budget is
  // derived at config time by _apply_derived_budget or set explicitly; there was no
  // fallback constant left to consult). The pin is removed with the constant, not kept
  // as a "used to exist" marker -- there is nothing left on the header to pin.
  CHECK_EQ(SCHEMA_VERSION, 2u);
  CHECK_EQ(CHUNK_BYTES, static_cast<size_t>(4096));
  CHECK_EQ(NO_PARENT, static_cast<uint16_t>(0xFFFF));
}

TEST(state_name_covers_exactly_the_nine_non_down_tokens) {
  // State has deliberately no DOWN member (contract §2.2/§4): the false pass this whole
  // schema exists to prevent is structurally unrepresentable, not merely avoided by
  // convention. Enumerating every declared value and checking none renders "down" pins
  // that as a fact about the header, not a promise about behaviour.
  std::set<std::string> names;
  for (uint8_t i = 0; i <= static_cast<uint8_t>(State::INCOMPATIBLE); i++)
    names.insert(state_name(static_cast<State>(i)));
  CHECK_EQ(names.size(), static_cast<size_t>(9));
  CHECK(names.count("down") == 0);
  CHECK(names.count("ok") == 1);
  CHECK(names.count("stale") == 1);
  CHECK(names.count("degraded") == 1);
  CHECK(names.count("refused") == 1);
  CHECK(names.count("silent") == 1);
  CHECK(names.count("absent") == 1);
  CHECK(names.count("unreachable") == 1);
  CHECK(names.count("unknown") == 1);
  CHECK(names.count("incompatible") == 1);
}

TEST(classification_name_matches_the_schemas_own_spelling) {
  CHECK_EQ(std::string(classification_name(Classification::REAL)), std::string("real"));
  CHECK_EQ(std::string(classification_name(Classification::SNA)), std::string("sna"));
  CHECK_EQ(std::string(classification_name(Classification::IMPLAUSIBLE)), std::string("implausible"));
}

// --- ChunkWriter: framing -------------------------------------------------------------

TEST(chunkwriter_short_content_is_flushed_once_by_finish) {
  char buf[64];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(w.literal("hello"));
  CHECK(w.finish());
  CHECK(w.ok());
  CHECK_EQ(cap.data, std::string("hello"));
}

TEST(chunkwriter_a_single_write_larger_than_one_whole_frame_is_split) {
  char buf[4];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(w.literal("HelloWorld"));  // 10 bytes into a 4-byte frame
  CHECK(w.finish());
  CHECK_EQ(cap.data, std::string("HelloWorld"));
  CHECK(cap.flush_sizes.size() >= 2);  // proves it actually split, not just buffered whole
  for (size_t size : cap.flush_sizes)
    CHECK(size <= 4);  // "nothing is ever buffered beyond capacity" (contract §2.2)
}

TEST(chunkwriter_a_token_straddles_an_already_partially_filled_frame) {
  char buf[4];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(w.write("AB", 2));     // 2 bytes, below capacity
  CHECK(w.write("CDEFG", 5));  // straddles: completes a frame, then continues past it
  CHECK(w.finish());
  CHECK_EQ(cap.data, std::string("ABCDEFG"));
  for (size_t size : cap.flush_sizes)
    CHECK(size <= 4);
  CHECK(cap.flush_sizes.size() >= 2);
}

TEST(chunkwriter_splits_a_token_across_the_pinned_4096_byte_chunk_boundary) {
  // The contract pins CHUNK_BYTES=4096 as "the v2 chunk size" --
  // exercise the real constant, not just a small synthetic capacity.
  std::vector<char> buf(CHUNK_BYTES);
  Capture cap;
  ChunkWriter w;
  w.init(buf.data(), CHUNK_BYTES, capture_flush, &cap);
  std::string token(CHUNK_BYTES + 10, 'A');
  CHECK(w.literal(token.c_str()));
  CHECK(w.finish());
  CHECK_EQ(cap.data.size(), token.size());
  CHECK(cap.data == token);
  bool saw_full_frame = false;
  for (size_t size : cap.flush_sizes) {
    CHECK(size <= CHUNK_BYTES);
    if (size == CHUNK_BYTES)
      saw_full_frame = true;
  }
  CHECK(saw_full_frame);
}

TEST(chunkwriter_finish_flushes_a_partial_tail) {
  char buf[8];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(w.write("abc", 3));  // 3 bytes, below the 8-byte capacity: nothing forces a flush
  CHECK(w.finish());         // must flush the partial tail itself
  CHECK(!cap.flush_sizes.empty());
  for (size_t size : cap.flush_sizes)
    CHECK(size <= 8);
  CHECK_EQ(cap.data, std::string("abc"));
}

TEST(chunkwriter_a_flush_that_returns_false_is_reported_and_latches) {
  char buf[4];
  Capture cap;
  cap.fail_at = 0;  // the very first flush call fails
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  bool wrote = w.write("0123456789", 10);  // forces at least one flush well before finish()
  CHECK(!wrote);
  CHECK(!w.ok());
  // Once broken, further calls must not crash and must keep reporting failure -- an
  // envelope writer that "succeeds" after a dropped frame would silently truncate.
  CHECK(!w.literal("more"));
  CHECK(!w.finish());
}

// --- ChunkWriter: json_string escaping --------------------------------------------------

TEST(json_string_round_trips_quote_and_backslash) {
  char buf[64];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  const std::string original = "a\"b\\c";
  CHECK(w.json_string(original.c_str()));
  CHECK(w.finish());
  CHECK(cap.data.size() >= 2);
  CHECK_EQ(cap.data.front(), '"');
  CHECK_EQ(cap.data.back(), '"');
  std::string inner = cap.data.substr(1, cap.data.size() - 2);
  CHECK_EQ(json_decode(inner), original);
}

TEST(json_string_escapes_control_bytes_so_none_appear_raw) {
  char buf[64];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  // Adjacent string literals, not one run-on literal: \x hex escapes are greedy and
  // would otherwise swallow the digits of "1fb" as a single (out-of-range) escape.
  const std::string original = "a\x01"
                               "\x1f"
                               "b\n\t";
  CHECK(w.json_string(original.c_str()));
  CHECK(w.finish());
  std::string inner = cap.data.substr(1, cap.data.size() - 2);
  for (unsigned char c : inner)
    CHECK(c >= 0x20);  // every control byte must have been escaped, none passed raw
  CHECK_EQ(json_decode(inner), original);
}

TEST(json_string_round_trips_a_byte_at_or_above_0x80) {
  char buf[64];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  const std::string original = std::string("A") + static_cast<char>(0x80) + "B";
  CHECK(w.json_string(original.c_str()));
  CHECK(w.finish());
  std::string inner = cap.data.substr(1, cap.data.size() - 2);
  // Deliberately not pinning *how* 0x80 is represented (raw passthrough, as
  // the sibling core's escaped() does, and  are both valid JSON) -- only that it
  // round-trips to the same byte, so no reading is silently corrupted or dropped.
  CHECK_EQ(json_decode(inner), original);
}

TEST(json_string_empty_value_is_a_pair_of_quotes) {
  char buf[16];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(w.json_string(""));
  CHECK(w.finish());
  CHECK_EQ(cap.data, std::string("\"\""));
}

// --- ChunkWriter: numbers --------------------------------------------------------------

TEST(number_u32_boundaries) {
  auto render = [](uint32_t value) {
    char buf[32];
    Capture cap;
    ChunkWriter w;
    w.init(buf, sizeof(buf), capture_flush, &cap);
    bool ok = w.number_u32(value) && w.finish();
    CHECK(ok);
    return cap.data;
  };
  CHECK_EQ(render(0), std::string("0"));
  CHECK_EQ(render(4294967295u), std::string("4294967295"));
}

TEST(number_float_decimal_places_0_through_6_with_no_rounding_ambiguity) {
  auto render = [](float value, int8_t decimals) {
    char buf[64];
    Capture cap;
    ChunkWriter w;
    w.init(buf, sizeof(buf), capture_flush, &cap);
    bool ok = w.number_float(value, decimals) && w.finish();
    CHECK(ok);
    return cap.data;
  };
  CHECK_EQ(render(7.0f, 0), std::string("7"));
  CHECK_EQ(render(7.0f, 1), std::string("7.0"));
  CHECK_EQ(render(1.0f, 6), std::string("1.000000"));
  CHECK_EQ(render(-4.0f, 2), std::string("-4.00"));
  CHECK_EQ(render(0.0000001f, 6), std::string("0.000000"));
}

TEST(number_float_negative_zero_does_not_produce_garbage) {
  // Not pinning a single expected string -- IEEE-754 negative zero is a genuine
  // implementation choice here (both "0" and "-0" are defensible), but the output must
  // be one of exactly those two, not something malformed.
  char buf[32];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(w.number_float(-0.0f, 0));
  CHECK(w.finish());
  CHECK(cap.data == "0" || cap.data == "-0");
}

TEST(number_float_at_the_float_limits_does_not_crash_or_emit_garbage) {
  char buf[128];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(w.number_float(3.40282347e+38f, 0));  // FLT_MAX
  CHECK(w.finish());
  CHECK(!cap.data.empty());
  CHECK(cap.data.size() < 64);
  for (char c : cap.data)
    CHECK((c >= '0' && c <= '9') || c == '-' || c == '.');
}

// --- parse_after -------------------------------------------------------------------

TEST(parse_after_accepts_zero_and_uint32_max) {
  uint32_t out = 12345;
  CHECK(parse_after("0", &out));
  CHECK_EQ(out, 0u);
  CHECK(parse_after("4294967295", &out));
  CHECK_EQ(out, 4294967295u);
}

TEST(parse_after_rejects_overflow_by_exactly_one) {
  uint32_t out = 0;
  CHECK(!parse_after("4294967296", &out));
}

TEST(parse_after_rejects_empty_string) {
  uint32_t out = 0;
  CHECK(!parse_after("", &out));
}

TEST(parse_after_rejects_trailing_garbage) {
  uint32_t out = 0;
  CHECK(!parse_after("12x", &out));
}

TEST(parse_after_rejects_a_minus_sign) {
  uint32_t out = 0;
  CHECK(!parse_after("-1", &out));
}

TEST(parse_after_rejects_a_leading_plus_sign) {
  // "no sign" (contract §2.2) -- a leading '+' is a sign, not merely a minus exclusion.
  uint32_t out = 0;
  CHECK(!parse_after("+1", &out));
}

// --- has_node ------------------------------------------------------------------------

namespace {
struct ThreeLevelTree {
  NodeDef nodes[3];
  Snapshot snapshot;
};

ThreeLevelTree three_level_tree() {
  ThreeLevelTree t{};
  t.nodes[0] = node("car", NO_PARENT, 0, 0, 500);
  t.nodes[1] = node("ecu", 0, 0, 0, 500);
  t.nodes[2] = node("ecu.pack", 1, 0, 0, 500);
  t.snapshot = make_snapshot(t.nodes, 3, nullptr, 0);
  return t;
}
}  // namespace

TEST(has_node_true_for_a_known_deep_path) {
  ThreeLevelTree t = three_level_tree();
  CHECK(has_node(t.snapshot, "ecu.pack"));
}

TEST(has_node_true_for_the_root_itself) {
  ThreeLevelTree t = three_level_tree();
  CHECK(has_node(t.snapshot, "car"));
}

TEST(has_node_false_for_an_unknown_path) {
  ThreeLevelTree t = three_level_tree();
  CHECK(!has_node(t.snapshot, "ecu.vehicle"));
}

TEST(has_node_false_for_the_empty_string) {
  ThreeLevelTree t = three_level_tree();
  CHECK(!has_node(t.snapshot, ""));
}

TEST(has_node_is_case_sensitive) {
  ThreeLevelTree t = three_level_tree();
  CHECK(!has_node(t.snapshot, "ECU"));
  CHECK(!has_node(t.snapshot, "Ecu.Pack"));
}

// --- resolve(): per-value table (§3), observed through the one-value node it lands on --

TEST(resolve_never_published_value_makes_the_node_degraded) {
  ValueSample values[] = {never_published("hv")};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::DEGRADED);
  CHECK(!r.age_known[0]);  // provenance.age_ms is null when the node has no known-age value
}

TEST(resolve_nan_numeric_value_makes_the_node_degraded) {
  ValueSample values[] = {value("t", true, true, 100, false, nullptr, NAN)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::DEGRADED);
}

TEST(resolve_positive_infinity_makes_the_node_degraded) {
  ValueSample values[] = {value("t", true, true, 100, false, nullptr, INFINITY)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::DEGRADED);
}

TEST(resolve_text_overflow_value_must_not_leave_the_node_ok) {
  // KNOWN AMBIGUITY, flagged for the spec owner: contract §3's
  // per-node table only promotes a node off "ok" when an own value is "absent" (rule 3) or
  // "stale" (rule 4) -- neither rule names a value in its own "degraded" state, which is
  // exactly what a text_overflow value produces. Taken literally, rule 5 ("otherwise ->
  // ok") would then leave this node "ok" while it carries a non-ok value, which is exactly
  // what portal_schema.py's node_ok_with_failing_value check exists to catch (tools/
  // portal_schema.py, validate_envelope -> _validate_node). This test pins the NECESSARY
  // consequence of that cross-reference (the node must not end up "ok"), not one specific
  // reading of §3's literal wording -- if this fails, the fix belongs in the derivation
  // rule, not in this test.
  ValueSample values[] = {value("txt", true, true, 10, true, "some very long text", 0.0f, 0, true)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK(r.states[0] != State::OK);
}

TEST(resolve_any_one_absent_value_among_several_degrades_the_whole_node) {
  ValueSample values[] = {fresh_number("a", 1.0f, 0), never_published("b")};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 2, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 2);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::DEGRADED);
}

TEST(resolve_value_older_than_budget_makes_the_node_stale) {
  ValueSample values[] = {fresh_number("v", 1.0f, 501)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::STALE);
  CHECK(r.age_known[0]);
  // "stale_within_budget cannot fire" (contract §3): pin the structural consequence
  // directly -- a stale node's own reported age must itself exceed its own budget.
  CHECK(r.ages[0] > nodes[0].budget_ms);
}

TEST(resolve_value_age_exactly_at_the_budget_is_still_ok_not_stale) {
  // Boundary case: the rule is "age_ms > node.budget_ms", strictly greater. An off-by-one
  // (>=) would make every value exactly at its budget falsely "stale".
  ValueSample values[] = {fresh_number("v", 1.0f, 500)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::OK);
}

TEST(resolve_value_one_ms_under_budget_is_ok) {
  ValueSample values[] = {fresh_number("v", 1.0f, 499)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::OK);
}

TEST(resolve_value_one_ms_over_budget_is_stale) {
  ValueSample values[] = {fresh_number("v", 1.0f, 501)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::STALE);
}

// --- resolve(): age semantics (v2) ---------------------------------------------------------------------
//
// The actual bug (add_on_state_callback stamping age only when a hash moved, so a
// counter republished unchanged kept ageing) lives in the ESPHome binding
// (components/portal/portal.cpp), which touches real esphome::sensor::Sensor
// types this host toolchain does not build against -- see
// for why that layer is not reachable from here. What the freestanding core DOES own is
// what it does with whatever age_ms it is handed, and that is exactly the behavioural
// contract this round pins: a small age_ms must read as fresh regardless of whether the
// reading changed, and the core must not need the reading to be "interesting" (nonzero)
// to call it real.

TEST(resolve_a_value_of_exactly_zero_is_ok_not_treated_as_absent) {
  // The exact real-world shape: A counter such as "observe overflow" is healthy at 0,
  // republished every 5 s. A `!value`-shaped bug would mistake this reading for "no
  // reading at all" -- has_value/age_known are the only things allowed to say that.
  ValueSample values[] = {fresh_number("observe_overflow", 0.0f, 10)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::OK);
  CHECK(r.age_known[0]);
  CHECK_EQ(r.ages[0], 10u);
}

TEST(resolve_distinguishes_republished_fresh_from_stopped_publishing) {
  // Same reading (0), same node/value shape, only age_ms differs -- one is what a
  // republish-every-5s counter looks like moments after a publish, the other is what it
  // looks like once nothing has republished it past budget. The two must not collapse.
  ValueSample republished[] = {fresh_number("observe_overflow", 0.0f, 10)};
  ValueSample stopped[] = {fresh_number("observe_overflow", 0.0f, 600)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};

  Snapshot fresh_snap = make_snapshot(nodes, 1, republished, 1);
  State fresh_states[1];
  uint32_t fresh_ages[1];
  bool fresh_known[1];
  Resolved fresh_r{fresh_states, fresh_ages, fresh_known, 1};
  resolve(fresh_snap, fresh_r);
  CHECK_EQ(fresh_r.states[0], State::OK);

  Snapshot stale_snap = make_snapshot(nodes, 1, stopped, 1);
  State stale_states[1];
  uint32_t stale_ages[1];
  bool stale_known[1];
  Resolved stale_r{stale_states, stale_ages, stale_known, 1};
  resolve(stale_snap, stale_r);
  CHECK_EQ(stale_r.states[0], State::STALE);
}

// --- resolve(): budget_ms may be null (v2) -------------------------------------
//
// On-change publishers (role_a, bcu_power, other_ctl, probe) and `update_interval:
// never` template sensors have no cadence, so no finite budget is honest for them --
// `budget_known = false` is the struct-level fact that makes staleness switch off for
// that node entirely, without touching the OTHER real signals (absent-for-never-
// published, absent-for-NaN) that have nothing to do with cadence.

TEST(resolve_null_budget_node_with_a_huge_age_stays_ok_never_stale) {
  ValueSample values[] = {fresh_number("v", 1.0f, 0xFFFFFFFFu)};  // as old as a value can be
  NodeDef nodes[] = {node_with_null_budget("leaf", NO_PARENT, 0, 1)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::OK);
}

TEST(resolve_null_budget_node_value_can_still_be_absent_for_never_published) {
  ValueSample values[] = {never_published("v")};
  NodeDef nodes[] = {node_with_null_budget("leaf", NO_PARENT, 0, 1)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::DEGRADED);  // node rollup rule 3, unaffected by budget_known
}

TEST(resolve_null_budget_node_value_can_still_be_absent_for_nan) {
  ValueSample values[] = {value("v", true, true, 10, false, nullptr, NAN)};
  NodeDef nodes[] = {node_with_null_budget("leaf", NO_PARENT, 0, 1)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::DEGRADED);
}

// --- resolve(): a boolean can never age into stale (C7, D3 / R16 clause 4, contract ------
// v2) -----------------------------------------------------------------
//
// MEASURED in the installed ESPHome: StatefulEntityBase::set_new_state
// (entity_base.h:339-365) returns false -- before firing state_callbacks_/
// full_state_callbacks_ -- when a boolean republishes an unchanged value, so a
// binary_sensor-bound value's age is "since last CHANGE", not "since last publish", and
// there is no public hook that fires on an unchanged republish to fix that from the
// ESPHome glue side. What classify_value() (and therefore resolve()) DOES own is what it
// does with whatever age_ms it is handed -- so this legitimately pre-assigns a huge
// age_ms, unlike a disqualified D3 test that would pre-assign a REALISTIC age and call
// that a demonstration of the bug: the property under test here is "a bool can never be
// judged stale", and an adversarial age_ms far past a KNOWN, finite budget is exactly the
// input that would trip the ordinary age > budget rule for any other value kind. The
// config-side half of C7 (cadence_ms / explicit positive budget_ms refused on a
// binary_sensor value) is tested at tests/portal/test_schema.py; this is the core
// invariant a Snapshot built outside the YAML path (a future peer mount, the emitter)
// still cannot escape.

TEST(resolve_a_boolean_value_never_becomes_stale_even_with_a_known_budget_and_huge_age) {
  ValueSample values[] = {fresh_bool("ring_up", true, /*age_ms=*/999999)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, /*budget_ms=*/500)};  // budget_known=true
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK(r.states[0] != State::STALE);
  CHECK_EQ(r.states[0], State::OK);
}

TEST(classify_value_never_returns_stale_for_a_boolean_regardless_of_budget) {
  // Same property, exercised directly against classify_value() rather than through
  // resolve()'s node-level rollup, so a future change to the rollup logic cannot mask a
  // regression in the per-value rule itself.
  ValueSample v = fresh_bool("ring_up", false, /*age_ms=*/0xFFFFFFFFu);  // as old as possible
  const detail::ValueVerdict verdict = detail::classify_value(v, /*node_budget_ms=*/1, /*budget_known=*/true);
  CHECK(verdict.state != State::STALE);
}

// --- resolve(): unknown_when (§4) --------------------------------------------------

TEST(resolve_unknown_when_matching_value_reports_unknown) {
  ValueSample values[] = {fresh_text("wake_state", "DOWN", 10)};
  NodeDef nodes[] = {node("ctl", NO_PARENT, 0, 1, 2000, nullptr, "ctl", nullptr, nullptr, nullptr, "wake_state", "DOWN",
                          "no_rail_instrument")};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::UNKNOWN);
}

TEST(resolve_unknown_when_non_matching_value_falls_through) {
  ValueSample values[] = {fresh_text("wake_state", "AWAKE", 10)};
  NodeDef nodes[] = {node("ctl", NO_PARENT, 0, 1, 2000, nullptr, "ctl", nullptr, nullptr, nullptr, "wake_state", "DOWN",
                          "no_rail_instrument")};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::OK);
}

// --- resolve(): the five per-node rules and child propagation ----------------------

namespace {
struct ParentChild {
  NodeDef nodes[2];
  ValueSample values[2];
  Snapshot snapshot;
};
}  // namespace

TEST(resolve_child_degraded_upgrades_an_ok_parent) {
  ParentChild pc{};
  pc.values[0] = never_published("child_value");       // makes the child DEGRADED
  pc.nodes[0] = node("parent", NO_PARENT, 0, 0, 500);  // no own values -> own state ok
  pc.nodes[1] = node("parent.child", 0, 0, 1, 500);
  pc.snapshot = make_snapshot(pc.nodes, 2, pc.values, 1);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(pc.snapshot, r);
  CHECK_EQ(r.states[1], State::DEGRADED);
  CHECK_EQ(r.states[0], State::DEGRADED);
}

TEST(resolve_child_degraded_upgrades_a_stale_parent_to_degraded) {
  // Contract §3: "if any child's final state is not ok and this node's own state severity
  // is below DEGRADED, the node becomes degraded". Severity order pins STALE(1) below
  // DEGRADED(2) -- so a stale parent with a degraded child must become degraded, losing
  // none of its own staleness information to a rule that reads "only upgrade from ok".
  ParentChild pc{};
  pc.values[0] = fresh_number("parent_value", 1.0f, 999);  // stale on its own (budget 500)
  pc.values[1] = never_published("child_value");
  pc.nodes[0] = node("parent", NO_PARENT, 0, 1, 500);
  pc.nodes[1] = node("parent.child", 0, 1, 1, 500);
  pc.snapshot = make_snapshot(pc.nodes, 2, pc.values, 2);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(pc.snapshot, r);
  CHECK_EQ(r.states[1], State::DEGRADED);
  CHECK_EQ(r.states[0], State::DEGRADED);
}

TEST(resolve_child_degraded_does_not_upgrade_an_absent_parent) {
  ParentChild pc{};
  pc.values[0] = never_published("child_value");
  pc.nodes[0] = node("parent", NO_PARENT, 0, 0, 500, nullptr, "kind", "not_compiled");
  pc.nodes[1] = node("parent.child", 0, 0, 1, 500);
  pc.snapshot = make_snapshot(pc.nodes, 2, pc.values, 1);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(pc.snapshot, r);
  CHECK_EQ(r.states[1], State::DEGRADED);
  CHECK_EQ(r.states[0], State::ABSENT);  // not upgraded: severity 3 is not below DEGRADED(2)
}

TEST(resolve_child_degraded_does_not_upgrade_an_unknown_parent) {
  ParentChild pc{};
  pc.values[0] = fresh_text("wake_state", "DOWN", 0);
  pc.values[1] = never_published("child_value");
  pc.nodes[0] = node("parent", NO_PARENT, 0, 1, 2000, nullptr, "ctl", nullptr, nullptr, nullptr, "wake_state", "DOWN",
                     "no_rail_instrument");
  pc.nodes[1] = node("parent.child", 0, 1, 1, 500);
  pc.snapshot = make_snapshot(pc.nodes, 2, pc.values, 2);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(pc.snapshot, r);
  CHECK_EQ(r.states[1], State::DEGRADED);
  CHECK_EQ(r.states[0], State::UNKNOWN);  // not upgraded: severity 4 is not below DEGRADED(2)
}

TEST(resolve_ok_child_does_not_degrade_an_ok_parent) {
  ParentChild pc{};
  pc.values[0] = fresh_number("child_value", 1.0f, 0);
  pc.nodes[0] = node("parent", NO_PARENT, 0, 0, 500);
  pc.nodes[1] = node("parent.child", 0, 0, 1, 500);
  pc.snapshot = make_snapshot(pc.nodes, 2, pc.values, 1);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(pc.snapshot, r);
  CHECK_EQ(r.states[1], State::OK);
  CHECK_EQ(r.states[0], State::OK);
}

// --- write_envelope(): the DOWN case (§4), the gate's single most important assertion --

TEST(write_envelope_never_emits_state_down_and_reports_unknown_no_rail_instrument) {
  ValueSample values[] = {fresh_text("wake_state", "DOWN", 10)};
  NodeDef nodes[] = {node("ctl", NO_PARENT, 0, 1, 2000, "Control unit", "ctl", nullptr, nullptr, nullptr,
                          "wake_state", "DOWN", "no_rail_instrument")};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::UNKNOWN);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"state\":\"down\"") == std::string::npos);
  CHECK(cap.data.find("\"state\":\"unknown\"") != std::string::npos);
  CHECK(cap.data.find("\"no_rail_instrument\"") != std::string::npos);
  // Transparency: the literal DOWN reading is still reported as the device's own field, not
  // hidden by the unknown_when substitution (contract §4 point 2).
  CHECK(cap.data.find("\"DOWN\"") != std::string::npos);
}

// --- is_skeleton_node() / write_provenance() (C6, D8 / R35, contract v2) --
//
// Schema §3: the whole provenance object is null on a node no board measured -- the
// skeleton above every subtree a board `owns` (or a peer mounts, once 5b lands; `peers`
// is hardcoded `[]` today). String prefixing alone is wrong: the synthetic root's id is
// "car" while its children's ids are bare ("ecu"), so "car" is nobody's string prefix --
// is_skeleton_node instead walks NodeDef.parent upward from each s.identity.owns[k]'s own
// node index (found once), bounded by s.node_count like write_ancestor_role, and returns
// true iff the queried index is passed on that walk. With owns_count == 0 nothing is
// skeleton, which is why a no-identity board keeps every node's provenance object.

TEST(is_skeleton_node_with_owns_count_zero_marks_nothing_skeleton) {
  // A board with no identity: nothing is skeleton, so a no-identity board does not lose
  // every node's age_ms/budget_ms merely because it cannot name itself.
  const char *owns[] = {"ecu"};  // present in identity, but owns_count below is 0
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"),
      node("ecu", 0, 0, 0, 500, "ECU", "component"),
  };
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  snap.identity.owns = owns;
  snap.identity.owns_count = 0;
  CHECK(!detail::is_skeleton_node(snap, 0));  // "car"
  CHECK(!detail::is_skeleton_node(snap, 1));  // "ecu"
}

TEST(is_skeleton_node_marks_the_car_root_skeleton_above_an_owned_subtree) {
  // car -> ecu (owned). "car" is a strict ancestor of the owned root and must be
  // skeleton; "ecu" itself (the owned root) must NOT be -- it is INSIDE the owned
  // subtree, not above it.
  const char *owns[] = {"ecu"};
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"),
      node("ecu", 0, 0, 0, 500, "ECU", "component"),
  };
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  snap.identity.owns = owns;
  snap.identity.owns_count = 1;
  CHECK(detail::is_skeleton_node(snap, 0));   // "car": skeleton
  CHECK(!detail::is_skeleton_node(snap, 1));  // "ecu": the owned root itself, not skeleton
}

TEST(is_skeleton_node_is_false_for_a_node_inside_the_owned_subtree) {
  const char *owns[] = {"ecu"};
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"),
      node("ecu", 0, 0, 0, 500, "ECU", "component"),
      node("ecu.pack", 1, 0, 0, 500, "Pack", "pack"),
  };
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  snap.identity.owns = owns;
  snap.identity.owns_count = 1;
  CHECK(!detail::is_skeleton_node(snap, 2));  // "ecu.pack": inside the owned subtree
}

TEST(is_skeleton_node_is_false_for_a_node_unrelated_to_any_owned_root) {
  // car has two children: "ecu" (owned) and "charger" (neither inside nor a skeleton
  // ancestor of anything owned -- a sibling, not an ancestor, of the owned root).
  const char *owns[] = {"ecu"};
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"),
      node("ecu", 0, 0, 0, 500, "ECU", "component"),
      node("charger", 0, 0, 0, 500, "Charger", "component"),
  };
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  snap.identity.owns = owns;
  snap.identity.owns_count = 1;
  CHECK(!detail::is_skeleton_node(snap, 2));  // "charger": neither inside nor above
}

TEST(write_provenance_emits_null_on_a_skeleton_node_and_an_object_on_the_owned_root) {
  // Wire-level companion to the four unit tests above: the same tree, driven through
  // write_envelope(), so a regression in write_provenance's OWN call to is_skeleton_node
  // (not just the function in isolation) is caught too.
  const char *owns[] = {"ecu"};
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"),
      node("ecu", 0, 0, 0, 500, "ECU", "component"),
  };
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  snap.identity.board = "demo";
  snap.identity.node_id = "0xA";
  snap.identity.mac = "02:00:00:00:00:01";
  snap.identity.owns = owns;
  snap.identity.owns_count = 1;
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  // "car" is the very first node written, so its provenance is the first occurrence.
  size_t car_provenance = cap.data.find("\"provenance\":");
  CHECK(car_provenance != std::string::npos);
  CHECK(cap.data.compare(car_provenance + 13, 4, "null") == 0);

  // "ecu" is the owned root: its own provenance is the SECOND occurrence, and must be a
  // real object, not null.
  size_t ecu_provenance = cap.data.find("\"provenance\":", car_provenance + 13);
  CHECK(ecu_provenance != std::string::npos);
  CHECK(cap.data.compare(ecu_provenance + 13, 4, "null") != 0);
  CHECK(cap.data.compare(ecu_provenance + 13, 1, "{") == 0);
}

// --- write_envelope(): ?path= narrowing ---------------------------------------------

TEST(write_envelope_path_narrowing_sets_the_path_key_to_the_nodes_id) {
  NodeDef nodes[] = {node("car", NO_PARENT, 0, 0, 500), node("car.ecu", 0, 0, 0, 500)};
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "car.ecu"));
  CHECK(w.finish());

  CHECK(cap.data.find("\"path\":\"car.ecu\"") != std::string::npos);
  // The narrowed tree's own root id must be the requested node, not the car root --
  // find the first "id" key (belongs to the top of "tree") and read its value.
  size_t id_key = cap.data.find("\"id\":");
  CHECK(id_key != std::string::npos);
  bool found = false;
  std::string id_value = first_quoted(cap.data, id_key + 5, &found);
  CHECK(found);
  CHECK_EQ(id_value, std::string("car.ecu"));
}

TEST(write_envelope_without_a_path_omits_the_path_key_entirely) {
  NodeDef nodes[] = {node("car", NO_PARENT, 0, 0, 500)};
  Snapshot snap = make_snapshot(nodes, 1, nullptr, 0);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());
  CHECK(cap.data.find("\"path\":") == std::string::npos);
}

// --- write_envelope(): ancestor_role (contract:
// the v2 contract §2/§3.1) -------------------
//
// "after emitting path, walk NodeDef.parent from the resolved root_index upward and
// emit the first ancestor with role != nullptr, else null" (§3.1) -- a STRICT
// ancestor walk, so the node named by `path` never counts itself, only what is
// above it. Emitted only alongside `path` (§3.3), never on a full envelope.

TEST(write_envelope_ancestor_role_nearest_ancestor_wins_over_a_farther_one) {
  NodeDef nodes[] = {
      node("root", NO_PARENT, 0, 0, 500, "Root", "kind", nullptr, "farRole", "farRole"),
      node("root.mid", 0, 0, 0, 500, "Mid", "kind", nullptr, "nearRole", "nearRole"),
      node("root.mid.leaf", 1, 0, 0, 500, "Leaf"),
  };
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "root.mid.leaf"));
  CHECK(w.finish());

  bool found = false;
  std::string block = ancestor_role_raw(cap.data, &found);
  CHECK(found);
  CHECK(block != "null");
  CHECK_EQ(ancestor_role_field(block, "node"), std::string("root.mid"));
  CHECK_EQ(ancestor_role_field(block, "label"), std::string("Mid"));
  CHECK_EQ(ancestor_role_field(block, "role"), std::string("nearRole"));
}

TEST(write_envelope_ancestor_role_the_declaring_node_itself_yields_null) {
  // "root" has no role of its own here either, so this also confirms a role IS
  // reachable in this tree (via "root.pack") -- the null is specifically because
  // the walk is STRICT (never includes the fetched node itself), not because
  // nothing in the tree has a role at all.
  NodeDef nodes[] = {
      node("root", NO_PARENT, 0, 0, 500),
      node("root.pack", 0, 0, 0, 500, "Pack", "kind", nullptr, "role_a", "role_a"),
      node("root.pack.child", 1, 0, 0, 500),
  };
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "root.pack"));
  CHECK(w.finish());

  bool found = false;
  CHECK_EQ(ancestor_role_raw(cap.data, &found), std::string("null"));
  CHECK(found);
}

TEST(write_envelope_ancestor_role_a_path_with_no_role_above_it_yields_null) {
  NodeDef nodes[] = {
      node("root", NO_PARENT, 0, 0, 500),
      node("root.mid", 0, 0, 0, 500),
      node("root.mid.leaf", 1, 0, 0, 500),
  };
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "root.mid.leaf"));
  CHECK(w.finish());

  bool found = false;
  CHECK_EQ(ancestor_role_raw(cap.data, &found), std::string("null"));
  CHECK(found);
}

TEST(write_envelope_ancestor_role_the_true_root_yields_null_even_when_the_root_itself_has_a_role) {
  // The true root has NO_PARENT, so the walk terminates immediately: even a role
  // declared on the root itself must not leak out as its own ancestor.
  NodeDef nodes[] = {
      node("root", NO_PARENT, 0, 0, 500, "Root", "kind", nullptr, "rootRole", "rootRole"),
      node("root.child", 0, 0, 0, 500),
  };
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "root"));
  CHECK(w.finish());

  bool found = false;
  CHECK_EQ(ancestor_role_raw(cap.data, &found), std::string("null"));
  CHECK(found);
}

TEST(write_envelope_full_envelope_emits_neither_path_nor_ancestor_role) {
  // A role exists in this tree (so an implementation that always looks would have
  // something to find) -- the full envelope must still carry neither key at all
  // (contract §2.1: "the key is ABSENT, exactly as path is").
  NodeDef nodes[] = {
      node("root", NO_PARENT, 0, 0, 500),
      node("root.pack", 0, 0, 0, 500, "Pack", "kind", nullptr, "role_a", "role_a"),
      node("root.pack.child", 1, 0, 0, 500),
  };
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"path\":") == std::string::npos);
  bool found = true;
  ancestor_role_raw(cap.data, &found);
  CHECK(!found);
}

TEST(write_envelope_ancestor_role_emission_order_is_path_then_ancestor_role_then_tree) {
  // Contract §2.1, explicit and load-bearing: "Emission order is part of the
  // contract: immediately after path, before tree." Checked on both the non-null
  // and the null branch of ancestor_role -- an implementation that only gets the
  // order right when there is an object to write would still violate it for null.
  NodeDef nodes[] = {
      node("root", NO_PARENT, 0, 0, 500, "Root", "kind", nullptr, "role_a", "role_a"),
      node("root.child", 0, 0, 0, 500),
  };
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(snap, r);

  auto check_order = [&](const char *path) {
    char buf[4096];
    Capture cap;
    ChunkWriter w;
    w.init(buf, sizeof(buf), capture_flush, &cap);
    CHECK(write_envelope(w, snap, r, path));
    CHECK(w.finish());
    size_t path_pos = cap.data.find("\"path\":");
    size_t ancestor_pos = cap.data.find("\"ancestor_role\":");
    size_t tree_pos = cap.data.find("\"tree\":");
    CHECK(path_pos != std::string::npos);
    CHECK(ancestor_pos != std::string::npos);
    CHECK(tree_pos != std::string::npos);
    CHECK(path_pos < ancestor_pos);
    CHECK(ancestor_pos < tree_pos);
  };

  check_order("root.child");  // ancestor_role: non-null branch (root declares role_a)
  check_order("root");        // ancestor_role: null branch (the true root)
}

TEST(write_envelope_ancestor_role_walk_terminates_on_a_parent_cycle_instead_of_hanging) {
  // §3.1's bound, verbatim: "the walk must terminate on NO_PARENT and must also
  // stop after s.node_count steps, so a malformed parent cycle cannot hang the
  // HTTP handler." Two nodes whose `parent` fields point at each other, neither
  // ever reaching NO_PARENT -- if the walk is unbounded this test (and the whole
  // binary) hangs rather than failing a CHECK, which is itself the failure this
  // case is written to catch: a `write_envelope` that never returns is a worse
  // outcome than one that returns the wrong JSON, and this is the only way a host
  // test can put that possibility in front of the gate at all.
  NodeDef nodes[] = {
      node("root", NO_PARENT, 0, 0, 500), node("a", 2, 0, 0, 500),  // a's parent is index 2 ("b")
      node("b", 1, 0, 0, 500),                                      // b's parent is index 1 ("a") -- a <-> b cycle
  };
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "a"));  // must return -- see the comment above
  CHECK(w.finish());

  bool found = false;
  // Neither "a" nor "b" declares a role, so however many cycle iterations the
  // bounded walk takes before giving up, the honest answer is null.
  CHECK_EQ(ancestor_role_raw(cap.data, &found), std::string("null"));
  CHECK(found);
}

// --- write_envelope(): a flush failure propagates as false --------------------------

TEST(write_envelope_propagates_a_flush_failure) {
  NodeDef nodes[] = {node("car", NO_PARENT, 0, 0, 500)};
  Snapshot snap = make_snapshot(nodes, 1, nullptr, 0);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[8];
  Capture cap;
  cap.fail_at = 0;  // fail on the very first frame
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(!write_envelope(w, snap, r, nullptr));
}

// --- write_envelope(): budget_ms may be null (v2) ------------------------------

TEST(write_envelope_null_budget_node_emits_budget_ms_as_json_null_not_zero_or_omitted) {
  ValueSample values[] = {fresh_number("v", 1.0f, 10)};
  NodeDef nodes[] = {node_with_null_budget("leaf", NO_PARENT, 0, 1)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  // Schema doc §6: "key absent" and "value null" arriving at a UI as the same
  // `undefined` is exactly what this schema pins one convention against -- the key
  // must be present, spelled "budget_ms", with a literal JSON null, not 0.
  CHECK(cap.data.find("\"budget_ms\":null") != std::string::npos);
  CHECK(cap.data.find("\"budget_ms\":0") == std::string::npos);
}

// --- write_envelope(): a boolean value (v2) -------------------

TEST(write_envelope_boolean_true_renders_as_json_true_with_null_raw) {
  ValueSample values[] = {fresh_bool("ring_up", true, 10)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::OK);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"value\":true") != std::string::npos);
  CHECK(cap.data.find("\"raw\":null") != std::string::npos);
  CHECK(cap.data.find("\"classification\":\"real\"") != std::string::npos);
  // Never a quoted string: "true"/"false" would be JSON-legal but is not a boolean, and
  // a naive UI branch on typeof would then treat it as text.
  CHECK(cap.data.find("\"value\":\"true\"") == std::string::npos);
}

TEST(write_envelope_boolean_false_renders_as_json_false_not_absent) {
  // False is not "no reading" -- a `!value`-shaped bug would collapse this into sna/null.
  ValueSample values[] = {fresh_bool("peer_sleep_ack", false, 10)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::OK);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"value\":false") != std::string::npos);
  CHECK(cap.data.find("\"raw\":null") != std::string::npos);
  CHECK(cap.data.find("\"classification\":\"real\"") != std::string::npos);
}

TEST(write_envelope_never_published_boolean_is_sna_not_false) {
  // The other direction of the same "falsy zero" landmine: a boolean that never
  // published must be absent/sna, never silently rendered as a confident `false`.
  ValueSample values[] = {value("ring_up", false, false, 0, false, nullptr, 0.0f)};
  values[0].is_bool = true;
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::DEGRADED);  // rule 3: own value absent -> node degraded

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"value\":null") != std::string::npos);
  CHECK(cap.data.find("\"classification\":\"sna\"") != std::string::npos);
  CHECK(cap.data.find("\"value\":false") == std::string::npos);
}

// --- write_envelope(): per-value rows of §3, observed in the emitted JSON -----------

TEST(write_envelope_never_published_value_is_sna_null_value_not_available) {
  ValueSample values[] = {never_published("hv")};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"classification\":\"sna\"") != std::string::npos);
  CHECK(cap.data.find("\"value\":null") != std::string::npos);
  CHECK(cap.data.find("\"value_not_available\"") != std::string::npos);
  CHECK(cap.data.find("\"entities_unavailable\"") != std::string::npos);  // node-level rollup
}

TEST(write_envelope_text_overflow_value_carries_a_non_null_detail) {
  ValueSample values[] = {value("txt", true, true, 10, true, "way too long for the buffer", 0.0f, 0, true)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  size_t why = cap.data.find("\"value_not_available\"");
  CHECK(why != std::string::npos);
  size_t detail_key = cap.data.find("\"detail\":", why);
  CHECK(detail_key != std::string::npos);
  CHECK(cap.data.compare(detail_key + 9, 4, "null") != 0);  // must not be null (contract §3: "+ detail")
}

TEST(write_envelope_stale_value_keeps_the_reading_and_reports_stale_data) {
  ValueSample values[] = {fresh_number("v", 42.0f, 999, 1)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"classification\":\"real\"") != std::string::npos);
  CHECK(cap.data.find("42.0") != std::string::npos);  // the reading is kept, not nulled
  CHECK(cap.data.find("\"stale_data\"") != std::string::npos);
  CHECK(cap.data.find("\"state\":\"stale\"") != std::string::npos);
}

TEST(write_envelope_ok_value_has_null_why) {
  ValueSample values[] = {fresh_number("v", 1.0f, 0)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot snap = make_snapshot(nodes, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"state\":\"ok\"") != std::string::npos);
}

TEST(write_envelope_escapes_a_quote_embedded_in_a_label) {
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 0, 500, "Quo\"te")};
  Snapshot snap = make_snapshot(nodes, 1, nullptr, 0);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("Quo\\\"te") != std::string::npos);
  CHECK(cap.data.find("Quo\"te") == std::string::npos);  // never emitted unescaped
}

// --- write_envelope(): a real, large tree actually crosses a chunk boundary --------

TEST(write_envelope_of_a_large_tree_spans_multiple_chunk_boundaries) {
  // Round-2 finding: every small fixture above (and every scenario emit_portal_envelope
  // stays under one 4096-byte frame, so write_envelope's multi-chunk
  // path was only ever exercised by the synthetic ChunkWriter cases further up this
  // file, never through a real envelope. This builds a deliberately large tree -- 3
  // GRPs x 6 TIs x several scalar values, ~111 values, no attempt at realism beyond
  // schema validity -- and drives it through a real ChunkWriter, counting frames so a
  // regression that silently stops splitting cannot pass just because every *test* tree
  // happens to be small.
  const uint32_t budget = 500;
  const int GRP_COUNT = 3;
  const int TI_COUNT = 6;
  const int CELLS_PER_TI = 5;

  // Arena: built to completion before any .c_str() is taken below (see
  // emit_portal_envelope.cpp's identical discipline for the same reason).
  std::vector<std::string> text;
  auto add = [&text](const std::string &s) -> size_t {
    text.push_back(s);
    return text.size() - 1;
  };
  char fmt[64];

  struct Spec {
    size_t id, label, kind;
    uint16_t parent;
    std::vector<size_t> keys, labels;
  };
  std::vector<Spec> specs;
  specs.push_back({add("car"), add("Vehicle"), add("car"), NO_PARENT, {}, {}});
  specs.push_back({add("ecu"), add("Battery management"), add("component"), 0, {}, {}});
  specs.push_back({add("ecu.pack"), add("Battery pack"), add("pack"), 1, {}, {}});

  for (int grp = 0; grp < GRP_COUNT; grp++) {
    std::snprintf(fmt, sizeof(fmt), "ecu.pack.grp%d", grp);
    size_t grp_id = add(fmt);
    std::snprintf(fmt, sizeof(fmt), "GRP %d", grp);
    Spec grp_spec{grp_id, add(fmt), add("grp"), 2, {add("module_mv")}, {add("Module voltage")}};
    uint16_t grp_index = static_cast<uint16_t>(specs.size());
    specs.push_back(grp_spec);
    for (int ti = 0; ti < TI_COUNT; ti++) {
      std::snprintf(fmt, sizeof(fmt), "ecu.pack.grp%d.seg%d", grp, ti);
      size_t ti_id = add(fmt);
      std::snprintf(fmt, sizeof(fmt), "SEG %d", ti);
      Spec ti_spec{ti_id, add(fmt), add("seg"), grp_index, {add("segment_mv")}, {add("Segment voltage")}};
      for (int cell = 0; cell < CELLS_PER_TI; cell++) {
        std::snprintf(fmt, sizeof(fmt), "cell%d", cell);
        size_t key = add(fmt);
        std::snprintf(fmt, sizeof(fmt), "Cell %d", cell);
        ti_spec.keys.push_back(key);
        ti_spec.labels.push_back(add(fmt));
      }
      specs.push_back(ti_spec);
    }
  }

  std::vector<ValueSample> values;
  std::vector<NodeDef> nodes;
  nodes.reserve(specs.size());
  for (const Spec &spec : specs) {
    uint16_t value_first = static_cast<uint16_t>(values.size());
    for (size_t i = 0; i < spec.keys.size(); i++)
      values.push_back(fresh_number(text[spec.keys[i]].c_str(), 3441.0f + static_cast<float>(i), 10));
    nodes.push_back(node(text[spec.id].c_str(), spec.parent, value_first, static_cast<uint16_t>(spec.keys.size()),
                         budget, text[spec.label].c_str(), text[spec.kind].c_str()));
  }

  Snapshot snap = make_snapshot(nodes.data(), nodes.size(), values.data(), values.size());
  std::vector<State> states(nodes.size());
  std::vector<uint32_t> ages(nodes.size());
  auto age_known = std::make_unique<bool[]>(nodes.size());  // not vector<bool>: no .data()
  Resolved r{states.data(), ages.data(), age_known.get(), nodes.size()};
  resolve(snap, r);

  std::vector<char> writer_buf(CHUNK_BYTES);
  Capture cap;
  ChunkWriter w;
  w.init(writer_buf.data(), CHUNK_BYTES, capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  // The coverage hole itself: this envelope must actually cross a boundary, and by a
  // margin ("past 8192, three-plus frames" per the round-2 ask), not just barely.
  CHECK(cap.data.size() > CHUNK_BYTES);
  CHECK(cap.data.size() > 8192);

  // "The frame count is what the body size implies": every flush is at most CHUNK_BYTES,
  // any short flush is the one tail frame, and the full-size flushes plus the tail
  // account for exactly the captured byte count.
  size_t total = 0;
  size_t full_frames = 0;
  for (size_t i = 0; i < cap.flush_sizes.size(); i++) {
    size_t size = cap.flush_sizes[i];
    CHECK(size <= CHUNK_BYTES);
    total += size;
    if (size == CHUNK_BYTES)
      full_frames++;
    else
      CHECK(i == cap.flush_sizes.size() - 1);  // a short frame may only be the final one
  }
  CHECK_EQ(total, cap.data.size());
  CHECK_EQ(full_frames, cap.data.size() / CHUNK_BYTES);
  size_t remainder = cap.data.size() % CHUNK_BYTES;
  if (remainder == 0) {
    CHECK_EQ(cap.flush_sizes.size(), full_frames);
  } else {
    CHECK_EQ(cap.flush_sizes.size(), full_frames + 1);
    CHECK_EQ(cap.flush_sizes.back(), remainder);
  }
}

// --- 4c Part 1: write_series() -- the §1.3 pairing table -----------------------------
//
// "The emitter writes both [class and value] from ONE pull per element. The pairing
// invariant is structural, not remembered." Every row of the table is exercised on the
// SAME series in one write_series() call below, so a bug that only appears when rows are
// mixed (e.g. state bleeding from one element's resolved[] slot into the next) cannot
// hide behind a suite that only ever tests one row in isolation.

TEST(write_series_pairing_table_every_row_in_one_series) {
  std::vector<ScriptedElement> script = {
      {true, 'r', true, 3439.0f},  // row 1: 'r' with a value -> class 'r', the number
      {true, 'i', true, 42.0f},    // row 2: 'i' with a value -> class 'i', the number
      {true, 's', true, 999.0f},   // row 3: 's' -> class 's', value null REGARDLESS of has_value
      {true, '-', false, 0.0f},    // row 4: '-' -> class '-', value null
      {true, 'r', false, 0.0f},    // row 5: 'r' WITHOUT a value -> class '-', value null
      {true, 'i', false, 0.0f},    // row 6: 'i' WITHOUT a value -> class '-', value null
      {true, 'X', true, 7.0f},     // row 7: any other char -> class '-', value null
      {false, 'r', true, 1.0f},    // row 8: callback returns false -> class '-', value null
  };
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 7, "submodule_cell", static_cast<uint16_t>(script.size()),
                             0, scripted_element, &script);
  std::string json = render_series(def);

  CHECK(json.find("\"values\":[3439,42,null,null,null,null,null,null]") != std::string::npos);
  CHECK(json.find("\"class\":\"ris-----\"") != std::string::npos);
}

TEST(write_series_no_callback_bound_makes_every_element_a_hole) {
  // Contract §0: "a series with no bound callback is not a stub: it is ... every element
  // class '-' (not measured), every value null." This is Part 3's seam today -- Python
  // passes nullptr for every series (§0) -- so this is the shape every shipping image
  // actually renders right now, not a hypothetical.
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  std::string json = render_series(def);
  CHECK(json.find("\"values\":[null,null,null,null,null]") != std::string::npos);
  CHECK(json.find("\"class\":\"-----\"") != std::string::npos);
}

TEST(write_series_sna_class_nulls_the_value_even_when_has_value_is_true) {
  // Isolated, crisp restatement of row 3 -- the one row where the callback's own
  // has_value is deliberately overridden rather than trusted, so a regression here
  // cannot hide inside the combined pairing-table test above.
  std::vector<ScriptedElement> script = {{true, 's', true, 12345.0f}};
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 1, 0, scripted_element, &script);
  std::string json = render_series(def);
  CHECK(json.find("\"values\":[null]") != std::string::npos);
  CHECK(json.find("\"class\":\"s\"") != std::string::npos);
  CHECK(json.find("12345") == std::string::npos);  // the SNA value must never reach the wire
}

TEST(write_series_does_not_leak_a_previous_elements_value_into_the_next) {
  // "out is default-constructed fresh for every element ... so whatever a callback
  // partially writes before returning false is discarded rather than trusted"
  // (portal_core.h). Element 0 is a real reading; element 1's callback returns
  // true but never touches `out` at all -- if resolved[] were reused instead of
  // freshly defaulted per element, element 1 would render element 0's value.
  struct NoTouchThenReal {
    static bool call(void *, uint16_t index, SeriesElement *out) {
      if (index == 0) {
        out->cls = 'r';
        out->has_value = true;
        out->value = 100.0f;
        return true;
      }
      // index 1: answers true but writes nothing -- out keeps its default ('-', no value).
      return true;
    }
  };
  SeriesDef def =
      series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 2, 0, &NoTouchThenReal::call, nullptr);
  std::string json = render_series(def);
  CHECK(json.find("\"values\":[100,null]") != std::string::npos);
  CHECK(json.find("\"class\":\"r-\"") != std::string::npos);
}

TEST(write_series_clamps_count_to_series_max_elements_without_asking_past_it) {
  // "count: clamped to SERIES_MAX_ELEMENTS by the emitter" -- and the clamp must stop the
  // emitter from even ASKING past index 63, not merely from writing past it, because a
  // real provider's callback may have side effects (a CAN read, a mutex) that must not
  // run for an index the wire will never carry.
  RecordingScript rec;
  rec.script.reserve(70);
  for (int i = 0; i < 70; i++)
    rec.script.push_back({true, 'r', true, static_cast<float>(i)});
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 70, 0, recording_element, &rec);
  std::string json = render_series(def);

  CHECK_EQ(rec.max_index_seen, static_cast<int32_t>(SERIES_MAX_ELEMENTS) - 1);

  size_t values_key = json.find("\"values\":[");
  CHECK(values_key != std::string::npos);
  std::vector<std::string> values = split_flat_array(json, values_key + 9);
  CHECK_EQ(values.size(), static_cast<size_t>(SERIES_MAX_ELEMENTS));
  CHECK_EQ(values.back(), std::string("63"));  // index 63 -- never "64", "69", or beyond

  bool found = false;
  size_t class_key = json.find("\"class\":\"");
  CHECK(class_key != std::string::npos);
  std::string cls = first_quoted(json, class_key + 8, &found);
  CHECK(found);
  CHECK_EQ(cls.size(), static_cast<size_t>(SERIES_MAX_ELEMENTS));
}

// --- 4c Part 1: write_series() -- index / index_frame / pack_index_base -------------

TEST(write_series_index_is_always_the_literal_null) {
  // §1.4: "Emitted as null, unconditionally." -- checked with a non-trivial count so a
  // future "helpfully" sparse-aware emitter cannot pass this by accident.
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  std::string json = render_series(def);
  CHECK(json.find("\"index\":null") != std::string::npos);
}

TEST(write_series_index_frame_nullptr_is_literal_null_never_the_string_unstated) {
  // The single most likely bug in this build (contract §1.1): "Nothing anywhere resolves
  // a null or missing frame to a value." There is no "unstated" TOKEN -- a producer that
  // cannot name the frame emits JSON null, full stop, and the word "unstated" belongs
  // only to the JS consumer's own display row, never to the wire.
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, /*index_frame=*/nullptr, 3, 0, nullptr, nullptr);
  std::string json = render_series(def);
  CHECK(json.find("\"index_frame\":null") != std::string::npos);
  CHECK(json.find("unstated") == std::string::npos);
  CHECK(json.find("Unstated") == std::string::npos);
}

TEST(write_series_index_frame_is_emitted_verbatim_never_validated_here) {
  // The C++ emitter is not the schema validator: it writes whatever string it was
  // configured with, byte for byte, including a name nobody has documented yet -- the
  // open-vocabulary rule (contract §1.1 / §5) is enforced by script/portal_schema.py, not
  // duplicated (and therefore not risked diverging) in the emitter.
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "flux_capacitor", 1, 0, nullptr, nullptr);
  std::string json = render_series(def);
  CHECK(json.find("\"index_frame\":\"flux_capacitor\"") != std::string::npos);
}

TEST(write_series_pack_index_base_null_when_not_known) {
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 7, "submodule_cell", 1, 0, nullptr, nullptr,
                             /*pack_index_known=*/false, /*pack_index_base=*/999);
  std::string json = render_series(def);
  // 999 must not leak onto the wire merely because it sits in the struct -- only
  // pack_index_known gates whether it is ever read.
  CHECK(json.find("\"pack_index_base\":null") != std::string::npos);
  CHECK(json.find("999") == std::string::npos);
}

TEST(write_series_pack_index_base_emitted_when_known) {
  // R11's own worked example: submodule position 2, index_base 7 -> (2-1)*31+7 == 38.
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 7, "submodule_cell", 1, 0, nullptr, nullptr,
                             /*pack_index_known=*/true, /*pack_index_base=*/38);
  std::string json = render_series(def);
  CHECK(json.find("\"pack_index_base\":38") != std::string::npos);
}

TEST(write_series_emission_order_matches_the_pinned_key_order) {
  // Contract §1: "Emission order is pinned: key, label, unit, index_base, index,
  // index_frame, values, class, pack_index_base." Checked as strictly increasing
  // positions, not just "all nine keys are present somewhere".
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 7, "submodule_cell", 1, 0, nullptr, nullptr, true, 38);
  std::string json = render_series(def);
  size_t key = json.find("\"key\":");
  size_t label = json.find("\"label\":");
  size_t unit = json.find("\"unit\":");
  size_t index_base = json.find("\"index_base\":");
  size_t index = json.find("\"index\":");
  size_t index_frame = json.find("\"index_frame\":");
  size_t values = json.find("\"values\":");
  size_t cls = json.find("\"class\":");
  size_t pack_index_base = json.find("\"pack_index_base\":");
  for (size_t pos : {key, label, unit, index_base, index, index_frame, values, cls, pack_index_base})
    CHECK(pos != std::string::npos);
  CHECK(key < label);
  CHECK(label < unit);
  CHECK(unit < index_base);
  CHECK(index_base < index);
  CHECK(index < index_frame);
  CHECK(index_frame < values);
  CHECK(values < cls);
  CHECK(cls < pack_index_base);
}

// --- 4c Part 1: write_envelope() -- series_legend, including under ?path= -----------
//
// Contract §2: the complete closed legend when the EMITTED (possibly narrowed) subtree
// carries at least one series, null when it carries none -- recomputed over the narrowed
// subtree under ?path=, never over the whole tree regardless of what is requested.

TEST(write_envelope_series_legend_is_null_when_no_node_carries_a_series) {
  NodeDef nodes[] = {node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"), node("car.ecu", 0, 0, 0, 500)};
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());
  CHECK(cap.data.find("\"series_legend\":null") != std::string::npos);
}

TEST(write_envelope_series_legend_is_the_full_closed_object_when_a_series_is_present) {
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 3, 0, nullptr, nullptr);
  NodeDef nodes[] = {node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"), node("car.seg0", 0, 0, 0, 500)};
  nodes[1].series = &def;
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());
  // The COMPLETE legend, byte-identical to docs/portal-spec.md section 5.1 -- never a
  // computed used-subset (contract §2: "the legend is the complete vocabulary rather
  // than the used subset").
  CHECK(
      cap.data.find("\"series_legend\":{\"r\":\"real\",\"s\":\"SNA\",\"i\":\"implausible\",\"-\":\"not measured\"}") !=
      std::string::npos);
}

TEST(write_envelope_series_legend_is_present_even_when_the_only_series_is_fully_unbound) {
  // The non-obvious case: subtree_has_series checks "does a series OBJECT exist here",
  // never "does it carry any real class content" -- an all-hole series (element ==
  // nullptr, every shipping image today) still makes the legend appear, because the
  // legend is a statement about the vocabulary the wire COULD use, not a report on what
  // it happened to use this response.
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  NodeDef nodes[] = {node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"), node("car.seg0", 0, 0, 0, 500)};
  nodes[1].series = &def;
  Snapshot snap = make_snapshot(nodes, 2, nullptr, 0);
  State states[2];
  uint32_t ages[2];
  bool known[2];
  Resolved r{states, ages, known, 2};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());
  CHECK(cap.data.find("\"series_legend\":{") != std::string::npos);
}

TEST(write_envelope_path_narrowing_onto_a_subtree_without_series_yields_null_legend) {
  // Contract §2's own named failing case: "?path=ecu.pack.grp0.seg0 returning null while
  // seg0 carries a series" would be the defect. Here the shapes are reversed on purpose --
  // the series lives OUTSIDE the narrowed subtree -- to prove the legend is recomputed
  // over the narrowed subtree and does not simply inherit "does the whole tree have one".
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 3, 0, nullptr, nullptr);
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"), node("car.seg0", 0, 0, 0, 500),  // carries the series
      node("car.seg1", 0, 0, 0, 500),  // the narrowed target -- no series of its own
  };
  nodes[1].series = &def;
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "car.seg1"));
  CHECK(w.finish());
  CHECK(cap.data.find("\"series_legend\":null") != std::string::npos);
}

TEST(write_envelope_path_narrowing_onto_the_series_bearing_node_yields_the_legend) {
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 3, 0, nullptr, nullptr);
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"),
      node("car.seg0", 0, 0, 0, 500),
      node("car.seg1", 0, 0, 0, 500),
  };
  nodes[1].series = &def;
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "car.seg0"));
  CHECK(w.finish());
  CHECK(cap.data.find("\"series_legend\":{") != std::string::npos);
}

TEST(write_envelope_path_narrowing_onto_an_ancestor_of_the_series_bearing_node_yields_the_legend) {
  // The legend must also appear when the narrowed root is an ANCESTOR of the
  // series-bearing node, not only when it IS that node -- subtree_has_series walks up
  // from every series-carrying node, so a root anywhere on that path must see it.
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 3, 0, nullptr, nullptr);
  NodeDef nodes[] = {
      node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car"),
      node("car.pack", 0, 0, 0, 500),
      node("car.pack.seg0", 1, 0, 0, 500),
  };
  nodes[2].series = &def;
  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, "car.pack"));
  CHECK(w.finish());
  CHECK(cap.data.find("\"series_legend\":{") != std::string::npos);
}

// --- fingerprint() -------------------------------------------------------------------

TEST(fingerprint_is_unchanged_when_only_age_advances_within_budget) {
  ValueSample values_a[] = {fresh_number("v", 10.0f, 10)};
  ValueSample values_b[] = {fresh_number("v", 10.0f, 490)};  // still "ok" under budget 500
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot a = make_snapshot(nodes, 1, values_a, 1);
  Snapshot b = make_snapshot(nodes, 1, values_b, 1);
  CHECK_EQ(fingerprint(a), fingerprint(b));
}

TEST(fingerprint_republished_unchanged_value_does_not_change_fingerprint_even_though_age_resets) {
  // Round 3, the direction the up-only test above does not cover: age_ms going DOWN
  // because the entity just republished the same reading (age_ms is "since last
  // publish", not "since last change"). A republish must reset the age but must NOT
  // bump seq -- the reading and the derived state (both "ok" here, budget 500) are
  // unchanged, and those are the only two things the fingerprint is allowed to see.
  ValueSample about_to_be_stale[] = {fresh_number("observe_overflow", 0.0f, 490)};
  ValueSample just_republished[] = {fresh_number("observe_overflow", 0.0f, 10)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot a = make_snapshot(nodes, 1, about_to_be_stale, 1);
  Snapshot b = make_snapshot(nodes, 1, just_republished, 1);
  CHECK_EQ(fingerprint(a), fingerprint(b));
}

TEST(fingerprint_changes_when_a_reading_changes) {
  ValueSample values_a[] = {fresh_number("v", 10.0f, 10)};
  ValueSample values_b[] = {fresh_number("v", 20.0f, 10)};
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot a = make_snapshot(nodes, 1, values_a, 1);
  Snapshot b = make_snapshot(nodes, 1, values_b, 1);
  CHECK(fingerprint(a) != fingerprint(b));
}

TEST(fingerprint_changes_when_age_crosses_the_stale_threshold) {
  // Subtle: the fingerprint "deliberately excludes raw ages" (contract §5), but it also
  // "covers every ... derived state" -- an age change that flips ok -> stale IS a derived
  // -state change and must still move the fingerprint, or ?after= would return 204 for a
  // node that just turned red on screen.
  ValueSample values_a[] = {fresh_number("v", 10.0f, 100)};  // ok, budget 500
  ValueSample values_b[] = {fresh_number("v", 10.0f, 600)};  // stale, same reading
  NodeDef nodes[] = {node("leaf", NO_PARENT, 0, 1, 500)};
  Snapshot a = make_snapshot(nodes, 1, values_a, 1);
  Snapshot b = make_snapshot(nodes, 1, values_b, 1);
  CHECK(fingerprint(a) != fingerprint(b));
}

// --- 4c Part 3: Seam A --
// `NodeAbsentFn`, a runtime absence predicate on `NodeDef`. Case numbers below are the
// impl contract's own §5 numbering (12-16); test_schema.py covers 1-11, and
// test_role_a_values.cpp covers 17-19.

namespace {

bool absent_predicate_true(void *) { return true; }
bool absent_predicate_false(void *) { return false; }

}  // namespace

TEST(case12_absent_why_with_no_absent_fn_is_absent_exactly_as_before) {
  // "nullptr -> absent_why is unconditional (today's behaviour)" -- pinned as its own
  // case so a future change to the NEW branch (absent_fn != nullptr) cannot silently
  // regress the OLD one, which every other absent-related test in this file still
  // exercises implicitly.
  NodeDef n = node("grp0", NO_PARENT, 0, 0, 500, "GRP 0", "grp", "never_announced");
  CHECK(n.absent_fn == nullptr);
  Snapshot snap = make_snapshot(&n, 1, nullptr, 0);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::ABSENT);
  CHECK(!r.age_known[0]);
}

TEST(case13_absent_fn_returning_false_resolves_the_node_normally_and_emits_its_series) {
  SeriesDef series = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 3, 0, nullptr, nullptr);
  ValueSample values[] = {fresh_number("module_mv", 105875.0f, 10)};
  NodeDef n = node("grp0", NO_PARENT, 0, 1, 500, "GRP 0", "grp", "never_announced");
  n.absent_fn = absent_predicate_false;
  n.series = &series;
  Snapshot snap = make_snapshot(&n, 1, values, 1);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  // Resolves from its OWN values, not forced ABSENT by the (non-firing) predicate.
  CHECK_EQ(r.states[0], State::OK);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());
  CHECK(cap.data.find("\"series\":null") == std::string::npos);
  CHECK(cap.data.find("\"series\":{") != std::string::npos);
}

TEST(case14_absent_fn_returning_true_forces_absent_with_series_null_even_though_nodedef_series_is_bound) {
  // §6's own named failure: "A ti node emitting six real-looking values while its GRP is
  // absent" -- so this asserts on the EMITTED JSON STRING, not on node_emits_series()'s
  // return value, per the contract's explicit instruction.
  SeriesDef series = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  NodeDef n = node("grp0", NO_PARENT, 0, 0, 500, "GRP 0", "grp", "never_announced");
  n.absent_fn = absent_predicate_true;
  n.series = &series;
  CHECK(n.series != nullptr);  // the precondition the contract calls out explicitly
  Snapshot snap = make_snapshot(&n, 1, nullptr, 0);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);
  CHECK_EQ(r.states[0], State::ABSENT);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK(cap.data.find("\"state\":\"absent\"") != std::string::npos);
  CHECK(cap.data.find("\"code\":\"never_announced\"") != std::string::npos);
  CHECK(cap.data.find("\"series\":null") != std::string::npos);
  CHECK(cap.data.find("\"series\":{") == std::string::npos);
}

namespace {
std::string evidence_body_after(const std::string &json, const char *code) {
  size_t why_pos = json.find(std::string("\"code\":\"") + code + "\"");
  if (why_pos == std::string::npos)
    return "<code not found>";
  size_t evidence_key = json.find("\"evidence\":", why_pos);
  if (evidence_key == std::string::npos)
    return "<evidence key not found>";
  size_t open = json.find('{', evidence_key);
  size_t close = json.find('}', open);
  if (open == std::string::npos || close == std::string::npos)
    return "<evidence object not found>";
  return json.substr(open, close - open + 1);
}
}  // namespace

TEST(case14_addendum_a_never_announced_node_with_a_wired_absent_evidence_carries_a_discriminator) {
  // NodeDef::absent_evidence (portal_core.h, added beyond the impl contract's own
  // "exactly two members" text after checking the contract's own worked example against
  // script/portal_schema.py: `never_announced` needs why.evidence to carry `rx_frames` or
  // `announced`, per `_EVIDENCE_DISCRIMINATORS`). ABSENT_PROVIDERS' own
  // `role_a_slot_silent` entry (components/portal/__init__.py) declares the fragment
  // `"announced":false` -- reproduced verbatim here, not paraphrased, so this test would
  // catch either side drifting from the other.
  SeriesDef series = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  NodeDef n = node("grp0", NO_PARENT, 0, 0, 500, "GRP 0", "grp", "never_announced");
  n.absent_fn = absent_predicate_true;
  n.absent_evidence = "\"announced\":false";
  n.series = &series;
  Snapshot snap = make_snapshot(&n, 1, nullptr, 0);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  const std::string evidence_body = evidence_body_after(cap.data, "never_announced");
  CHECK_EQ(evidence_body, std::string("{\"announced\":false}"));
  CHECK(evidence_body.find("rx_frames") != std::string::npos || evidence_body.find("announced") != std::string::npos);
}

TEST(
    case14_addendum_b_a_never_announced_node_with_no_wired_absent_evidence_ships_an_empty_and_therefore_wire_invalid_object) {
  // The negative half of the same finding, kept as a permanent document rather than a
  // target the mechanism should eventually satisfy: `absent_fn`/`absent_why` alone (the
  // impl contract's own two-member text, §1) are NOT sufficient for an evidence-gated
  // code -- `absent_evidence` must ALSO be wired, or the emitted `why.evidence` stays the
  // empty `{}` every other absence code still legitimately uses, and that empty object is
  // schema-invalid specifically for `never_announced` (evidence_without_discriminator,
  // script/portal_schema.py's `_EVIDENCE_DISCRIMINATORS`). If this test ever starts failing
  // (i.e. an unwired node starts emitting non-empty evidence), something is inferring
  // evidence from the why-code string instead of carrying an explicit, provider-declared
  // fragment -- the general mechanism `absent_evidence`'s own header comment says this
  // build deliberately does not reopen.
  SeriesDef series = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  NodeDef n = node("grp0", NO_PARENT, 0, 0, 500, "GRP 0", "grp", "never_announced");
  n.absent_fn = absent_predicate_true;
  CHECK(n.absent_evidence == nullptr);
  n.series = &series;
  Snapshot snap = make_snapshot(&n, 1, nullptr, 0);
  State states[1];
  uint32_t ages[1];
  bool known[1];
  Resolved r{states, ages, known, 1};
  resolve(snap, r);

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());

  CHECK_EQ(evidence_body_after(cap.data, "never_announced"), std::string("{}"));
}

TEST(case15_series_legend_is_null_when_every_series_carrying_node_is_absent) {
  SeriesDef series0 = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  SeriesDef series1 = series_def("cells", "Cell voltages", "mV", 7, "submodule_cell", 5, 0, nullptr, nullptr);
  NodeDef nodes[3];
  nodes[0] = node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car");
  nodes[1] = node("car.seg0", 0, 0, 0, 500, "SEG 0", "seg", "never_announced");
  nodes[1].absent_fn = absent_predicate_true;
  nodes[1].series = &series0;
  nodes[2] = node("car.seg1", 0, 0, 0, 500, "SEG 1", "seg", "never_announced");
  nodes[2].absent_fn = absent_predicate_true;
  nodes[2].series = &series1;

  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);
  CHECK_EQ(r.states[1], State::ABSENT);
  CHECK_EQ(r.states[2], State::ABSENT);

  // Direct unit check on the new (Snapshot, Resolved, root_index) signature.
  CHECK(!detail::subtree_has_series(snap, r, 0));

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());
  CHECK(cap.data.find("\"series_legend\":null") != std::string::npos);
}

TEST(case15_series_legend_is_the_full_object_when_one_series_carrying_node_is_not_absent) {
  // Same tree shape as the case above with ONE node's predicate flipped to false -- the
  // case that fails if `write_node` and `series_legend` compute the absence condition
  // separately (contract §5: "the case that fails if write_node and the legend use two
  // different conditions").
  SeriesDef series0 = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", 5, 0, nullptr, nullptr);
  SeriesDef series1 = series_def("cells", "Cell voltages", "mV", 7, "submodule_cell", 5, 0, nullptr, nullptr);
  NodeDef nodes[3];
  nodes[0] = node("car", NO_PARENT, 0, 0, 500, "Vehicle", "car");
  nodes[1] = node("car.seg0", 0, 0, 0, 500, "SEG 0", "seg", "never_announced");
  nodes[1].absent_fn = absent_predicate_true;  // stays absent
  nodes[1].series = &series0;
  nodes[2] = node("car.seg1", 0, 0, 0, 500, "SEG 1", "seg", "never_announced");
  nodes[2].absent_fn = absent_predicate_false;  // the one node that is NOT absent
  nodes[2].series = &series1;

  Snapshot snap = make_snapshot(nodes, 3, nullptr, 0);
  State states[3];
  uint32_t ages[3];
  bool known[3];
  Resolved r{states, ages, known, 3};
  resolve(snap, r);
  CHECK_EQ(r.states[1], State::ABSENT);
  CHECK(r.states[2] != State::ABSENT);

  CHECK(detail::subtree_has_series(snap, r, 0));

  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof(buf), capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, nullptr));
  CHECK(w.finish());
  CHECK(
      cap.data.find("\"series_legend\":{\"r\":\"real\",\"s\":\"SNA\",\"i\":\"implausible\",\"-\":\"not measured\"}") !=
      std::string::npos);

  // The un-absent node's own series must actually be emitted, not merely counted by the
  // legend check -- seg0 stays null, seg1 must not.
  size_t seg1_pos = cap.data.find("\"car.seg1\"");
  CHECK(seg1_pos != std::string::npos);
  size_t seg1_series = cap.data.find("\"series\":", seg1_pos);
  CHECK(seg1_series != std::string::npos);
  CHECK(cap.data.compare(seg1_series + 9, 4, "null") != 0);
}

TEST(case16_series_bound_to_a_fake_provider_pairs_class_and_value_correctly_for_r_i_dash_and_false) {
  // Part 1's normalisation (§1.3's pairing table) must apply completely unchanged under
  // Part 3's plumbing -- this exercises the four wire characters the contract names
  // explicitly (r / i / - / a declining callback) on a series shaped like a real bound
  // provider (`role_a_cells`'s own `SeriesElementFn` thunk), not Part 1's own
  // already-existing exhaustive 8-row table above.
  std::vector<ScriptedElement> script = {
      {true, 'r', true, 3439.0f},  // real
      {true, 'i', true, 42.0f},    // implausible
      {true, '-', false, 0.0f},    // hole
      {false, 'r', true, 999.0f},  // provider declines to answer -- also a hole
  };
  SeriesDef def = series_def("cells", "Cell voltages", "mV", 1, "submodule_cell", static_cast<uint16_t>(script.size()),
                             0, scripted_element, &script);
  std::string json = render_series(def);
  CHECK(json.find("\"values\":[3439,42,null,null]") != std::string::npos);
  CHECK(json.find("\"class\":\"ri--\"") != std::string::npos);
}
