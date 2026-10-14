/// Host tests for the portal core, spec v1.5 (docs/portal-spec.md PS-M6-05..07): value `level`
/// from limits, node `limit` with evidence, the derivation precedence, `on_demand` /
/// `not_requested`, and the `switch_off` absent provider's verdict + evidence. Fictional data.

#include "harness.h"
#include "portal_core.h"

#include <string>

using namespace esphome::portal::core;

namespace {

bool lim_flush(void *ctx, const char *data, size_t size) {
  static_cast<std::string *>(ctx)->append(data, size);
  return true;
}

ValueSample lnum(const char *key, float x, const ValueLimit *limit, uint32_t age = 0, int8_t decimals = 0) {
  ValueSample v{};
  v.key = key;
  v.label = key;
  v.number = x;
  v.decimals = decimals;
  v.has_value = true;
  v.age_known = true;
  v.age_ms = age;
  v.limit = limit;
  return v;
}

ValueSample lbool(const char *key, bool b, const ValueLimit *limit) {
  ValueSample v = lnum(key, b ? 1.0f : 0.0f, limit);
  v.is_bool = true;
  return v;
}

ValueSample lnever(const char *key, const ValueLimit *limit = nullptr) {
  ValueSample v{};
  v.key = key;
  v.label = key;
  v.limit = limit;
  return v;
}

NodeDef lnode(const char *id, uint16_t parent, uint16_t first, uint16_t count, uint32_t budget = 0) {
  NodeDef n{};
  n.id = id;
  n.kind = "module";
  n.label = id;
  n.parent = parent;
  n.value_first = first;
  n.value_count = count;
  n.budget_ms = budget;
  n.budget_known = budget != 0;
  return n;
}

Snapshot lsnap(const NodeDef *nodes, size_t n, const ValueSample *values, size_t v) {
  Snapshot s{};
  s.seq = 1;
  s.identity.build = "demo-1";
  s.nodes = nodes;
  s.node_count = n;
  s.values = values;
  s.value_count = v;
  return s;
}

struct Out {
  State states[16];
  uint32_t ages[16];
  bool known[16];
  Resolved r{states, ages, known, 16};
  std::string json;
};

void run(const Snapshot &s, Out &o, const char *path = nullptr) {
  resolve(s, o.r);
  static char buf[512];  // small frame: exercises chunk splits through the new keys too
  ChunkWriter w;
  w.init(buf, sizeof(buf), lim_flush, &o.json);
  CHECK(write_envelope(w, s, o.r, path));
  CHECK(w.finish());
}

bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

ValueLimit above(float warn, float bad) {
  ValueLimit l{};
  l.warn.has_above = true;
  l.warn.above = warn;
  l.bad.has_above = true;
  l.bad.above = bad;
  return l;
}

ValueLimit below(float warn, float bad) {
  ValueLimit l{};
  l.warn.has_below = true;
  l.warn.below = warn;
  l.bad.has_below = true;
  l.bad.below = bad;
  return l;
}

bool switch_state = true;
bool switch_off_fn(void *ctx) { return !*static_cast<bool *>(ctx); }

}  // namespace

// --- level derivation (PS-M6-05) -----------------------------------------------------------

TEST(portal_level_is_null_without_limits_and_without_a_reading) {
  const ValueLimit lim = above(95, 127);
  CHECK(detail::limit_hit(lnum("a", 500, nullptr)).level == Level::NONE);
  CHECK(detail::limit_hit(lnever("b", &lim)).level == Level::NONE);
  ValueSample nan = lnum("c", __builtin_nanf(""), &lim);
  CHECK(detail::limit_hit(nan).level == Level::NONE);
  ValueSample text = lnum("d", 0, &lim);
  text.is_text = true;
  text.text = "x";
  CHECK(detail::limit_hit(text).level == Level::NONE);
}

TEST(portal_level_above_and_below_are_strict) {
  const ValueLimit hi = above(95, 127);
  CHECK(detail::limit_hit(lnum("a", 95, &hi)).level == Level::OK);
  CHECK(detail::limit_hit(lnum("a", 95.5f, &hi)).level == Level::WARN);
  CHECK(detail::limit_hit(lnum("a", 127, &hi)).level == Level::WARN);
  CHECK(detail::limit_hit(lnum("a", 128, &hi)).level == Level::BAD);
  const ValueLimit lo = below(12.0f, 11.0f);
  CHECK(detail::limit_hit(lnum("v", 12.0f, &lo)).level == Level::OK);
  CHECK(detail::limit_hit(lnum("v", 11.9f, &lo)).level == Level::WARN);
  CHECK(detail::limit_hit(lnum("v", 11.0f, &lo)).level == Level::WARN);
  CHECK(detail::limit_hit(lnum("v", 10.9f, &lo)).level == Level::BAD);
}

TEST(portal_level_bad_beats_warn_and_a_one_sided_limit_works) {
  ValueLimit l{};
  l.warn.has_below = true;
  l.warn.below = 12.0f;
  l.warn.has_above = true;
  l.warn.above = 15.0f;
  l.bad.has_below = true;
  l.bad.below = 11.0f;
  CHECK(detail::limit_hit(lnum("v", 10.0f, &l)).level == Level::BAD);  // matches warn too
  CHECK(detail::limit_hit(lnum("v", 15.5f, &l)).level == Level::WARN);
  CHECK(detail::limit_hit(lnum("v", 13.0f, &l)).level == Level::OK);
  const detail::LimitHit hit = detail::limit_hit(lnum("v", 10.0f, &l));
  CHECK_EQ(hit.which, 0);
  CHECK(hit.bound == 11.0f);
}

TEST(portal_level_boolean_bad_when_then_warn_when) {
  ValueLimit l{};
  l.has_bad_when = true;
  l.bad_when = true;
  CHECK(detail::limit_hit(lbool("off", true, &l)).level == Level::BAD);
  CHECK(detail::limit_hit(lbool("off", false, &l)).level == Level::OK);
  ValueLimit w{};
  w.has_warn_when = true;
  w.warn_when = false;
  CHECK(detail::limit_hit(lbool("awake", false, &w)).level == Level::WARN);
  CHECK(detail::limit_hit(lbool("awake", true, &w)).level == Level::OK);
}

TEST(portal_value_emits_level_after_the_v2_keys) {
  const ValueLimit lim = above(95, 127);
  ValueSample values[] = {lnum("rec", 10, &lim), lnum("plain", 1, nullptr), lnever("gone", &lim)};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 3)};
  Out o;
  run(lsnap(nodes, 2, values, 3), o);
  CHECK(has(o.json, "\"key\":\"rec\""));
  CHECK(has(o.json, "\"state\":\"ok\",\"why\":null,\"level\":\"ok\"}"));
  CHECK(has(o.json, "\"key\":\"plain\",\"label\":\"plain\",\"unit\":null,\"value\":1,\"raw\":1,\"raw_hex\":null,"
                    "\"classification\":\"real\",\"state\":\"ok\",\"why\":null,\"level\":null}"));
  // no reading: level null whatever the limits
  CHECK(has(o.json, "\"code\":\"value_not_available\",\"detail\":null,\"since_ms\":null,\"evidence\":{}},\"level\":null}"));
}

// --- node `limit` (PS-M6-05) ---------------------------------------------------------------

TEST(portal_limit_degrades_the_node_with_value_level_reading_and_bound_evidence) {
  const ValueLimit lim = above(95, 127);
  ValueSample values[] = {lnum("tec", 3, &lim), lnum("rec", 129, &lim)};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 2)};
  Out o;
  run(lsnap(nodes, 2, values, 2), o);
  CHECK_EQ(o.states[1], State::DEGRADED);
  CHECK(has(o.json, "\"state\":\"degraded\",\"why\":{\"code\":\"limit\",\"detail\":null,\"since_ms\":null,"
                    "\"evidence\":{\"value\":\"rec\",\"level\":\"bad\",\"reading\":129,\"bound\":{\"above\":127}}}"));
  // the root is upgraded as for any non-ok child
  CHECK_EQ(o.states[0], State::DEGRADED);
  CHECK(has(o.json, "\"code\":\"child_degraded\",\"detail\":null,\"since_ms\":null,\"evidence\":{\"children\":{\"bus\":\"degraded\"}}"));
}

TEST(portal_limit_worst_value_wins_and_a_tie_goes_to_the_first) {
  const ValueLimit lim = above(95, 127);
  {
    ValueSample values[] = {lnum("a", 100, &lim), lnum("b", 200, &lim), lnum("c", 300, &lim)};
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 3)};
    Out o;
    run(lsnap(nodes, 2, values, 3), o);
    CHECK(has(o.json, "\"evidence\":{\"value\":\"b\",\"level\":\"bad\",\"reading\":200"));
  }
  {
    ValueSample values[] = {lnum("a", 100, &lim), lnum("b", 101, &lim)};
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 2)};
    Out o;
    run(lsnap(nodes, 2, values, 2), o);
    CHECK(has(o.json, "\"evidence\":{\"value\":\"a\",\"level\":\"warn\",\"reading\":100,\"bound\":{\"above\":95}}"));
  }
}

TEST(portal_limit_evidence_bound_uses_the_shortest_decimals_and_bool_uses_when) {
  CHECK_EQ(detail::shortest_decimals(127.0f), 0);
  CHECK_EQ(detail::shortest_decimals(11.5f), 1);
  CHECK_EQ(detail::shortest_decimals(0.05f), 2);
  CHECK_EQ(detail::shortest_decimals(-3.25f), 2);
  const ValueLimit lo = below(12.0f, 11.5f);
  ValueLimit off{};
  off.has_bad_when = true;
  off.bad_when = true;
  {
    ValueSample values[] = {lnum("vcc", 11.234f, &lo, 0, 2)};
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("dev", 0, 0, 1)};
    Out o;
    run(lsnap(nodes, 2, values, 1), o);
    CHECK(has(o.json, "\"evidence\":{\"value\":\"vcc\",\"level\":\"bad\",\"reading\":11.23,\"bound\":{\"below\":11.5}}"));
  }
  {
    ValueSample values[] = {lbool("bus_off", true, &off)};
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 1)};
    Out o;
    run(lsnap(nodes, 2, values, 1), o);
    CHECK(has(o.json, "\"evidence\":{\"value\":\"bus_off\",\"level\":\"bad\",\"reading\":true,\"bound\":{\"when\":true}}"));
    CHECK(has(o.json, "\"value\":true,\"raw\":null,\"raw_hex\":null,\"classification\":\"real\",\"state\":\"ok\",\"why\":null,\"level\":\"bad\"}"));
  }
}

// --- precedence (PS-M6-06) -----------------------------------------------------------------

TEST(portal_precedence_limit_beats_stale) {
  const ValueLimit lim = above(95, 127);
  ValueSample values[] = {lnum("rec", 100, &lim, /*age=*/9000)};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 1, /*budget=*/1000)};
  Out o;
  run(lsnap(nodes, 2, values, 1), o);
  CHECK_EQ(o.states[1], State::DEGRADED);
  CHECK(has(o.json, "\"code\":\"limit\""));
  CHECK(!has(o.json, "\"code\":\"stale_data\",\"detail\":null,\"since_ms\":null,\"evidence\":{}},\"provenance\""));
}

TEST(portal_precedence_entities_unavailable_beats_limit) {
  const ValueLimit lim = above(95, 127);
  ValueSample values[] = {lnum("rec", 200, &lim), lnever("tec")};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 2)};
  Out o;
  run(lsnap(nodes, 2, values, 2), o);
  CHECK_EQ(o.states[1], State::DEGRADED);
  CHECK(has(o.json, "\"state\":\"degraded\",\"why\":{\"code\":\"entities_unavailable\""));
  CHECK(!has(o.json, "\"code\":\"limit\""));
}

TEST(portal_precedence_unknown_when_and_absent_beat_limit) {
  const ValueLimit lim = above(95, 127);
  {
    ValueSample values[] = {lnum("rec", 200, &lim), {}};
    values[1].key = "mode";
    values[1].label = "mode";
    values[1].is_text = true;
    values[1].text = "NO_RAIL";
    values[1].has_value = true;
    values[1].age_known = true;
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 2)};
    nodes[1].unknown_value_key = "mode";
    nodes[1].unknown_equals = "NO_RAIL";
    nodes[1].unknown_why = "no_rail_instrument";
    Out o;
    run(lsnap(nodes, 2, values, 2), o);
    CHECK_EQ(o.states[1], State::UNKNOWN);
    CHECK(has(o.json, "\"code\":\"no_rail_instrument\""));
  }
  {
    bool on = false;
    ValueSample values[] = {lnum("rec", 200, &lim)};
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bus", 0, 0, 1)};
    nodes[1].absent_why = "switched_off";
    nodes[1].absent_fn = switch_off_fn;
    nodes[1].absent_ctx = &on;
    Out o;
    run(lsnap(nodes, 2, values, 1), o);
    CHECK_EQ(o.states[1], State::ABSENT);
    CHECK(!has(o.json, "\"code\":\"limit\""));
  }
}

// --- not_requested (PS-M6-06) --------------------------------------------------------------

TEST(portal_on_demand_node_without_any_reading_is_unknown_not_requested_and_quiet) {
  ValueSample values[] = {lnum("vcc", 12, nullptr), lnever("dtc"), lnever("sw")};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("ecus", 0, 0, 1), lnode("ecus.a", 1, 1, 2)};
  nodes[2].on_demand = true;
  Out o;
  run(lsnap(nodes, 3, values, 3), o);
  CHECK_EQ(o.states[2], State::UNKNOWN);
  CHECK(!o.known[2]);
  CHECK(has(o.json, "\"state\":\"unknown\",\"why\":{\"code\":\"not_requested\",\"detail\":null,\"since_ms\":null,\"evidence\":{}}"));
  // no child_degraded upward: parent and root stay ok
  CHECK_EQ(o.states[1], State::OK);
  CHECK_EQ(o.states[0], State::OK);
  // its values still emit, as sna
  CHECK(has(o.json, "\"key\":\"dtc\",\"label\":\"dtc\",\"unit\":null,\"value\":null"));
}

TEST(portal_not_requested_child_is_left_out_of_a_parents_child_degraded_evidence) {
  ValueSample values[] = {lnever("dtc"), lnever("x")};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("ecus", 0, 0, 0), lnode("ecus.a", 1, 0, 1),
                     lnode("ecus.b", 1, 1, 1)};
  nodes[2].on_demand = true;  // quiet
  Out o;                      // ecus.b is a plain node with no reading -> degraded
  run(lsnap(nodes, 4, values, 2), o);
  CHECK_EQ(o.states[1], State::DEGRADED);
  CHECK(has(o.json, "\"evidence\":{\"children\":{\"ecus.b\":\"degraded\"}}"));
  CHECK(!has(o.json, "\"ecus.a\":\"unknown\""));
}

TEST(portal_on_demand_node_derives_normally_after_the_first_reading) {
  const ValueLimit lim = above(95, 127);
  {
    ValueSample values[] = {lnum("dtc", 1, nullptr), lnever("sw")};
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("a", 0, 0, 2)};
    nodes[1].on_demand = true;
    Out o;
    run(lsnap(nodes, 2, values, 2), o);
    CHECK_EQ(o.states[1], State::DEGRADED);
    CHECK(has(o.json, "\"code\":\"entities_unavailable\""));
    CHECK_EQ(o.states[0], State::DEGRADED);
  }
  {
    ValueSample values[] = {lnum("dtc", 200, &lim)};
    NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("a", 0, 0, 1)};
    nodes[1].on_demand = true;
    Out o;
    run(lsnap(nodes, 2, values, 1), o);
    CHECK(has(o.json, "\"code\":\"limit\""));
  }
}

TEST(portal_absent_beats_not_requested) {
  bool on = false;
  ValueSample values[] = {lnever("dtc")};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("a", 0, 0, 1)};
  nodes[1].on_demand = true;
  nodes[1].absent_why = "switched_off";
  nodes[1].absent_fn = switch_off_fn;
  nodes[1].absent_ctx = &on;
  Out o;
  run(lsnap(nodes, 2, values, 1), o);
  CHECK_EQ(o.states[1], State::ABSENT);
  CHECK(has(o.json, "\"code\":\"switched_off\""));
}

// --- switch_off (PS-M6-07) -----------------------------------------------------------------

TEST(portal_switch_off_absent_with_action_evidence_and_values_still_emitted) {
  switch_state = false;
  ValueSample values[] = {lnum("soc", 55, nullptr), lnum("spread", 12, nullptr)};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bat", 0, 0, 2)};
  nodes[1].absent_why = "switched_off";
  nodes[1].absent_fn = switch_off_fn;
  nodes[1].absent_ctx = &switch_state;
  nodes[1].absent_evidence = "\"action\":\"bat/polling\"";
  Out o;
  run(lsnap(nodes, 2, values, 2), o);
  CHECK_EQ(o.states[1], State::ABSENT);
  CHECK(has(o.json, "\"state\":\"absent\",\"why\":{\"code\":\"switched_off\",\"detail\":null,\"since_ms\":null,"
                    "\"evidence\":{\"action\":\"bat/polling\"}}"));
  CHECK(has(o.json, "\"key\":\"soc\",\"label\":\"soc\",\"unit\":null,\"value\":55"));

  switch_state = true;
  Out on;
  run(lsnap(nodes, 2, values, 2), on);
  CHECK_EQ(on.states[1], State::OK);
}

TEST(portal_switch_off_without_an_action_has_empty_evidence) {
  bool off = false;
  ValueSample values[] = {lnum("soc", 55, nullptr)};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bat", 0, 0, 1)};
  nodes[1].absent_why = "switched_off";
  nodes[1].absent_fn = switch_off_fn;
  nodes[1].absent_ctx = &off;
  Out o;
  run(lsnap(nodes, 2, values, 1), o);
  CHECK(has(o.json, "\"code\":\"switched_off\",\"detail\":null,\"since_ms\":null,\"evidence\":{}}"));
}

TEST(portal_fingerprint_follows_an_absent_when_verdict) {
  bool on = true;
  ValueSample values[] = {lnum("soc", 55, nullptr)};
  NodeDef nodes[] = {lnode("car", NO_PARENT, 0, 0), lnode("bat", 0, 0, 1)};
  nodes[1].absent_why = "switched_off";
  nodes[1].absent_fn = switch_off_fn;
  nodes[1].absent_ctx = &on;
  const Snapshot s = lsnap(nodes, 2, values, 1);
  const uint32_t a = fingerprint(s);
  on = false;
  const uint32_t b = fingerprint(s);
  on = true;
  CHECK(a != b);
  CHECK_EQ(fingerprint(s), a);
}
