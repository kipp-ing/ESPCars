/// Host tests for the portal core's action surface (docs/portal-spec.md sections 5.4, 5.5, 6.2):
/// PS-M3-02 (no actions -> "actions":[]), PS-M3-03 (entry emission), PS-M3-04 (request evaluation),
/// PS-M3-05 (fingerprint), PS-M2-05 (route matcher), PS-M2-08 (request parsing helpers).
/// Written from the spec; the API names are the ones the core exports.

#include "harness.h"
#include "portal_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::portal::core;

namespace {

struct Capture {
  std::string data;
};

bool capture_flush(void *ctx, const char *data, size_t size) {
  static_cast<Capture *>(ctx)->data.append(data, size);
  return true;
}

const char *OPTS[] = {"one", "two", "three"};

ActionDef def(const char *key, Control c, int8_t decimals = 2) {
  ActionDef d{};
  d.key = key;
  d.label = key;
  d.control = c;
  d.decimals = decimals;
  d.options = c == Control::SELECT ? OPTS : nullptr;
  d.option_count = c == Control::SELECT ? 3 : 0;
  return d;
}

ActionSample number_sample(float v, float lo, float hi, float step) {
  ActionSample s;
  s.has_value = true;
  s.number = v;
  s.has_min = s.has_max = s.has_step = true;
  s.min = lo;
  s.max = hi;
  s.step = step;
  s.unit = "V";
  return s;
}

struct Tree {
  NodeDef nodes[2]{};
  ActionDef defs[4]{};
  ActionSample samples[4]{};
  const char *owns[1] = {"dev"};
  Snapshot snap{};
  size_t action_count = 0;

  Tree() {
    nodes[0].id = "car";
    nodes[0].kind = "car";
    nodes[0].label = "Vehicle";
    nodes[0].parent = NO_PARENT;
    nodes[1].id = "dev";
    nodes[1].kind = "device";
    nodes[1].label = "Device";
    nodes[1].parent = 0;
  }
  Snapshot &build() {
    nodes[1].action_first = 0;
    nodes[1].action_count = static_cast<uint16_t>(action_count);
    snap.seq = 1;
    snap.uptime_ms = 5;
    snap.identity = Identity{"demo", "0xA", "02:00:00:00:00:01", "test-build", owns, 1, nullptr, 0};
    snap.nodes = nodes;
    snap.node_count = 2;
    snap.values = nullptr;
    snap.value_count = 0;
    snap.actions = defs;
    snap.action_samples = samples;
    snap.action_count = action_count;
    return snap;
  }
  void add(const ActionDef &d, const ActionSample &s) {
    defs[action_count] = d;
    samples[action_count] = s;
    action_count++;
  }
};

std::string emit(const Snapshot &snap, const char *path = nullptr) {
  State st[8];
  uint32_t ages[8];
  bool known[8];
  Resolved r{st, ages, known, 8};
  resolve(snap, r);
  char buf[4096];
  Capture cap;
  ChunkWriter w;
  w.init(buf, sizeof buf, capture_flush, &cap);
  CHECK(write_envelope(w, snap, r, path));
  CHECK(w.finish());
  return cap.data;
}

bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

// --- PS-M3-02 -----------------------------------------------------------------------------

TEST(actions_ps_m3_02_node_without_actions_emits_empty_array) {
  Tree t;
  std::string j = emit(t.build());
  CHECK(has(j, "\"actions\":[]"));
  CHECK(!has(j, "\"method\""));
}

TEST(actions_ps_m3_02_post_route_is_not_claimed_without_actions) {
  CHECK(match_route(true, "/api/v1/actions/dev/x", false).route == Route::NONE);
  CHECK(match_route(true, "/api/v1/actions/dev/x", true).route == Route::ACTION);
}

// --- PS-M3-03 -----------------------------------------------------------------------------

TEST(actions_ps_m3_03_switch_entry_matches_spec_5_4_exactly) {
  Tree t;
  ActionSample s;
  s.has_value = true;
  s.boolean = true;
  t.add(def("logging", Control::SWITCH), s);
  std::string j = emit(t.build());
  CHECK(has(j,
            "{\"key\":\"logging\",\"label\":\"logging\",\"method\":\"POST\","
            "\"path\":\"/api/v1/actions/dev/logging\",\"enabled\":true,\"why\":null,"
            "\"body\":{\"value\":\"bool\"},\"control\":\"switch\",\"value\":true,\"options\":null,"
            "\"min\":null,\"max\":null,\"step\":null,\"unit\":null,\"result\":null}"));
}

TEST(actions_ps_m3_03_button_has_null_body_and_null_value) {
  Tree t;
  t.add(def("go", Control::BUTTON), ActionSample{});
  std::string j = emit(t.build());
  CHECK(has(j, "\"body\":null,\"control\":\"button\",\"value\":null,\"options\":null"));
  CHECK(has(j, "\"enabled\":true,\"why\":null"));
}

TEST(actions_ps_m3_03_select_carries_options_and_string_value) {
  Tree t;
  ActionSample s;
  s.has_value = true;
  std::snprintf(s.text, sizeof s.text, "two");
  t.add(def("mode", Control::SELECT), s);
  std::string j = emit(t.build());
  CHECK(has(j, "\"body\":{\"value\":\"string\"},\"control\":\"select\",\"value\":\"two\","
               "\"options\":[\"one\",\"two\",\"three\"],\"min\":null"));
}

TEST(actions_ps_m3_03_number_carries_min_max_step_unit_with_decimals) {
  Tree t;
  t.add(def("level", Control::NUMBER, 1), number_sample(2.5f, 0.0f, 10.0f, 0.5f));
  std::string j = emit(t.build());
  CHECK(has(j, "\"body\":{\"value\":\"number\"},\"control\":\"number\",\"value\":2.5,\"options\":null,"
               "\"min\":0.0,\"max\":10.0,\"step\":0.5,\"unit\":\"V\",\"result\":null"));
}

TEST(actions_ps_m3_03_select_and_number_without_state_are_disabled_entity_unavailable) {
  Tree t;
  ActionSample none;
  none.enabled = false;
  none.has_value = false;
  t.add(def("mode", Control::SELECT), none);
  t.add(def("level", Control::NUMBER), none);
  std::string j = emit(t.build());
  size_t first = j.find("\"enabled\":false");
  CHECK(first != std::string::npos);
  CHECK(j.find("\"enabled\":false", first + 1) != std::string::npos);
  CHECK(has(j, "\"code\":\"entity_unavailable\""));
  CHECK(has(j, "\"control\":\"select\",\"value\":null"));
  CHECK(has(j, "\"control\":\"number\",\"value\":null"));
}

TEST(actions_ps_m3_03_switch_and_button_are_always_enabled) {
  Tree t;
  ActionSample s;
  s.enabled = false;  // must be ignored for switch/button
  t.add(def("a", Control::SWITCH), s);
  t.add(def("b", Control::BUTTON), s);
  std::string j = emit(t.build());
  CHECK(!has(j, "\"enabled\":false"));
}

TEST(actions_ps_m3_03_result_is_emitted_per_5_5) {
  Tree t;
  ActionSample s;
  s.has_value = true;
  s.boolean = true;
  s.result.state = ActionState::ACCEPTED;
  s.result.code = ActionCode::APPLIED;
  s.result.age_ms = 2140;
  s.result.value.kind = ValueKind::BOOL;
  s.result.value.boolean = true;
  t.add(def("logging", Control::SWITCH), s);
  std::string j = emit(t.build());
  CHECK(has(j, "\"result\":{\"state\":\"accepted\",\"why\":{\"code\":\"applied\",\"detail\":null,"
               "\"since_ms\":null,\"evidence\":{}},\"age_ms\":2140,\"value\":true}"));
}

TEST(actions_ps_m3_03_running_and_refused_results_have_a_why_object) {
  Tree t;
  ActionSample a;
  a.result.state = ActionState::RUNNING;
  a.result.code = ActionCode::QUEUED;
  ActionSample b;
  b.result.state = ActionState::REFUSED;
  b.result.code = ActionCode::OUT_OF_RANGE;
  b.has_value = true;
  b.has_min = b.has_max = b.has_step = true;
  b.max = 1;
  b.step = 1;
  b.result.value.kind = ValueKind::NUMBER;
  b.result.value.number = 99.0f;
  t.add(def("s", Control::BUTTON), a);
  t.add(def("n", Control::NUMBER, 0), b);
  std::string j = emit(t.build());
  CHECK(has(j, "\"state\":\"running\",\"why\":{\"code\":\"queued\""));
  CHECK(has(j, "\"state\":\"refused\",\"why\":{\"code\":\"out_of_range\""));
  CHECK(!has(j, "\"why\":null,\"age_ms\""));  // result.why is never null
}

TEST(actions_ps_m3_03_every_refusal_code_has_its_wire_name) {
  const struct {
    ActionCode c;
    const char *n;
  } table[] = {{ActionCode::INVALID_BODY, "invalid_body"},   {ActionCode::WRONG_TYPE, "wrong_type"},
               {ActionCode::OUT_OF_RANGE, "out_of_range"},   {ActionCode::UNKNOWN_OPTION, "unknown_option"},
               {ActionCode::DISABLED, "disabled"},           {ActionCode::BODY_TOO_LARGE, "body_too_large"}};
  for (auto &row : table) {
    Tree t;
    ActionSample s;
    s.result.state = ActionState::REFUSED;
    s.result.code = row.c;
    t.add(def("b", Control::BUTTON), s);
    std::string j = emit(t.build());
    std::string want = std::string("\"why\":{\"code\":\"") + row.n + "\"";
    CHECK(has(j, want.c_str()));
  }
}

// --- PS-M3-04 -----------------------------------------------------------------------------

ActionOutcome json(const ActionDef &d, const ActionSample &s, const char *body, bool busy = false) {
  return evaluate_action_json(d, s, busy, body, std::strlen(body));
}

ActionSample plain() {
  ActionSample s;
  s.has_value = true;
  return s;
}

TEST(actions_ps_m3_04_switch_json_accepts_bool) {
  auto o = json(def("a", Control::SWITCH), plain(), "{\"value\":true}");
  CHECK(o.verdict == Verdict::ACCEPT);
  CHECK_EQ(o.http, 202);
  CHECK(o.code == ActionCode::QUEUED);
  CHECK(o.value.kind == ValueKind::BOOL && o.value.boolean);
  auto f = json(def("a", Control::SWITCH), plain(), " { \"value\" : false } ");
  CHECK(f.verdict == Verdict::ACCEPT && f.value.kind == ValueKind::BOOL && !f.value.boolean);
}

TEST(actions_ps_m3_04_wrong_json_type_is_400_wrong_type) {
  const char *bodies[] = {"{\"value\":1}", "{\"value\":\"true\"}", "{\"value\":null}", "{\"value\":[true]}"};
  for (const char *b : bodies) {
    auto o = json(def("a", Control::SWITCH), plain(), b);
    CHECK(o.verdict == Verdict::REFUSE);
    CHECK_EQ(o.http, 400);
    CHECK(o.code == ActionCode::WRONG_TYPE || o.code == ActionCode::INVALID_BODY);
  }
  auto n = json(def("n", Control::NUMBER), number_sample(1, 0, 10, 1), "{\"value\":\"5\"}");
  CHECK(n.verdict == Verdict::REFUSE && n.http == 400 && n.code == ActionCode::WRONG_TYPE);
  auto s = json(def("s", Control::SELECT), plain(), "{\"value\":3}");
  CHECK(s.verdict == Verdict::REFUSE && s.http == 400 && s.code == ActionCode::WRONG_TYPE);
}

TEST(actions_ps_m3_04_unparseable_or_missing_value_is_400_invalid_body) {
  const char *bodies[] = {"", "not json", "{", "{\"value\"}", "{\"other\":1}", "[true]", "{\"value\":true"};
  for (const char *b : bodies) {
    auto o = json(def("a", Control::SWITCH), plain(), b);
    CHECK(o.verdict == Verdict::REFUSE);
    CHECK_EQ(o.http, 400);
    CHECK(o.code == ActionCode::INVALID_BODY);
    CHECK(o.value.kind == ValueKind::NONE);  // value null for an unparseable body
  }
}

TEST(actions_ps_m3_04_button_accepts_empty_and_empty_object) {
  for (const char *b : {"", "{}"}) {
    auto o = json(def("go", Control::BUTTON), ActionSample{}, b);
    CHECK(o.verdict == Verdict::ACCEPT);
    CHECK_EQ(o.http, 202);
    CHECK(o.value.kind == ValueKind::NONE);
  }
}

TEST(actions_ps_m3_04_number_range_and_non_finite) {
  ActionDef d = def("n", Control::NUMBER);
  ActionSample s = number_sample(1, 0, 10, 1);
  CHECK(json(d, s, "{\"value\":0}").verdict == Verdict::ACCEPT);
  CHECK(json(d, s, "{\"value\":10}").verdict == Verdict::ACCEPT);
  CHECK(json(d, s, "{\"value\":5.5}").verdict == Verdict::ACCEPT);
  for (const char *b : {"{\"value\":10.01}", "{\"value\":-0.01}", "{\"value\":1e39}", "{\"value\":-1e39}"}) {
    auto o = json(d, s, b);
    CHECK(o.verdict == Verdict::REFUSE);
    CHECK_EQ(o.http, 422);
    CHECK(o.code == ActionCode::OUT_OF_RANGE);
  }
  for (const char *b : {"{\"value\":NaN}", "{\"value\":Infinity}"}) {
    auto o = json(d, s, b);
    CHECK(o.verdict == Verdict::REFUSE);
    CHECK(o.code == ActionCode::OUT_OF_RANGE || o.code == ActionCode::INVALID_BODY);
  }
}

TEST(actions_ps_m3_04_select_unknown_option_is_422) {
  ActionDef d = def("m", Control::SELECT);
  auto ok = json(d, plain(), "{\"value\":\"three\"}");
  CHECK(ok.verdict == Verdict::ACCEPT && ok.value.kind == ValueKind::TEXT);
  CHECK(std::string(ok.value.text) == "three");
  for (const char *b : {"{\"value\":\"four\"}", "{\"value\":\"\"}", "{\"value\":\"One\"}", "{\"value\":\"one \"}"}) {
    auto o = json(d, plain(), b);
    CHECK(o.verdict == Verdict::REFUSE);
    CHECK_EQ(o.http, 422);
    CHECK(o.code == ActionCode::UNKNOWN_OPTION);
  }
}

TEST(actions_ps_m3_04_body_over_256_bytes_is_400_body_too_large) {
  std::string big = "{\"value\":true,\"pad\":\"" + std::string(300, 'x') + "\"}";
  auto o = evaluate_action_json(def("a", Control::SWITCH), plain(), false, big.c_str(), big.size());
  CHECK(o.verdict == Verdict::REFUSE);
  CHECK_EQ(o.http, 400);
  CHECK(o.code == ActionCode::BODY_TOO_LARGE);
  std::string edge(256, ' ');
  edge.replace(0, 14, "{\"value\":true}");
  auto e = evaluate_action_json(def("a", Control::SWITCH), plain(), false, edge.c_str(), edge.size());
  CHECK(e.verdict == Verdict::ACCEPT);  // exactly 256 bytes is still fine
  std::string one_more(257, ' ');
  one_more.replace(0, 14, "{\"value\":true}");
  auto m = evaluate_action_json(def("a", Control::SWITCH), plain(), false, one_more.c_str(), one_more.size());
  CHECK(m.verdict == Verdict::REFUSE && m.code == ActionCode::BODY_TOO_LARGE);
}

TEST(actions_ps_m3_04_disabled_is_409_recorded_disabled) {
  ActionSample s;
  s.enabled = false;
  for (Control c : {Control::SELECT, Control::NUMBER}) {
    auto o = json(def("x", c), s, "{\"value\":1}");
    CHECK(o.verdict == Verdict::REFUSE);
    CHECK_EQ(o.http, 409);
    CHECK(o.code == ActionCode::DISABLED);
  }
}

TEST(actions_ps_m3_04_busy_is_409_and_not_recorded) {
  auto o = json(def("a", Control::SWITCH), plain(), "{\"value\":true}", /*busy=*/true);
  CHECK(o.verdict == Verdict::BUSY);
  CHECK_EQ(o.http, 409);
}

TEST(actions_ps_m3_04_form_body_per_control) {
  char v[64];
  CHECK(form_value("value=on", 8, v, sizeof v));
  CHECK(std::string(v) == "on");
  CHECK(form_value("a=1&value=two%20words&b=2", 25, v, sizeof v));
  CHECK(std::string(v) == "two words");
  CHECK(form_value("value=a+b", 9, v, sizeof v));
  CHECK(std::string(v) == "a b");
  CHECK(!form_value("other=1", 7, v, sizeof v));

  ActionDef sw = def("a", Control::SWITCH);
  for (const char *t : {"true", "on", "1"}) {
    auto o = evaluate_action_text(sw, plain(), false, true, t, std::strlen(t));
    CHECK(o.verdict == Verdict::ACCEPT);
    CHECK(o.value.kind == ValueKind::BOOL && o.value.boolean);
  }
  for (const char *t : {"false", "off", "0"}) {
    auto o = evaluate_action_text(sw, plain(), false, true, t, std::strlen(t));
    CHECK(o.verdict == Verdict::ACCEPT);
    CHECK(o.value.kind == ValueKind::BOOL && !o.value.boolean);
  }
  auto bad = evaluate_action_text(sw, plain(), false, true, "maybe", 5);
  CHECK(bad.verdict == Verdict::REFUSE && bad.http == 400);
  auto none = evaluate_action_text(sw, plain(), false, false, "", 0);
  CHECK(none.verdict == Verdict::REFUSE && none.http == 400 && none.code == ActionCode::INVALID_BODY);

  ActionDef num = def("n", Control::NUMBER);
  ActionSample ns = number_sample(1, 0, 10, 1);
  auto okn = evaluate_action_text(num, ns, false, true, "7.5", 3);
  CHECK(okn.verdict == Verdict::ACCEPT && okn.value.kind == ValueKind::NUMBER);
  auto oor = evaluate_action_text(num, ns, false, true, "70", 2);
  CHECK(oor.verdict == Verdict::REFUSE && oor.http == 422 && oor.code == ActionCode::OUT_OF_RANGE);
  auto junk = evaluate_action_text(num, ns, false, true, "abc", 3);
  CHECK(junk.verdict == Verdict::REFUSE && junk.http == 400);

  ActionDef sel = def("m", Control::SELECT);
  CHECK(evaluate_action_text(sel, plain(), false, true, "two", 3).verdict == Verdict::ACCEPT);
  auto uo = evaluate_action_text(sel, plain(), false, true, "zzz", 3);
  CHECK(uo.verdict == Verdict::REFUSE && uo.http == 422 && uo.code == ActionCode::UNKNOWN_OPTION);

  ActionDef btn = def("go", Control::BUTTON);
  CHECK(evaluate_action_text(btn, ActionSample{}, false, false, "", 0).verdict == Verdict::ACCEPT);
}

TEST(actions_ps_m3_04_evaluation_has_no_side_effect_on_the_sample) {
  ActionSample s = number_sample(1, 0, 10, 1);
  ActionSample before = s;
  json(def("n", Control::NUMBER), s, "{\"value\":5}");
  CHECK(std::memcmp(&before.number, &s.number, sizeof(float)) == 0);
  CHECK(s.result.state == ActionState::NONE);
}

// --- PS-M3-05 -----------------------------------------------------------------------------

TEST(actions_ps_m3_05_fingerprint_follows_value_enabled_result_but_not_age) {
  auto fp = [](const ActionSample &s, Control c = Control::SWITCH) {
    Tree t;
    t.add(def("a", c), s);
    return fingerprint(t.build());
  };
  ActionSample base = plain();
  base.result.state = ActionState::ACCEPTED;
  base.result.code = ActionCode::APPLIED;
  base.result.age_ms = 10;
  uint32_t f0 = fp(base);

  ActionSample aged = base;
  aged.result.age_ms = 99999;
  CHECK_EQ(fp(aged), f0);  // result.age_ms alone never advances seq

  ActionSample v = base;
  v.boolean = true;
  CHECK(fp(v) != f0);

  ActionSample st = base;
  st.result.state = ActionState::RUNNING;
  st.result.code = ActionCode::QUEUED;
  CHECK(fp(st) != f0);

  ActionSample code = base;
  code.result.state = ActionState::REFUSED;
  code.result.code = ActionCode::INVALID_BODY;
  ActionSample code2 = code;
  code2.result.code = ActionCode::WRONG_TYPE;
  CHECK(fp(code) != fp(code2));

  ActionSample none = base;
  none.result = ActionResult{};
  CHECK(fp(none) != f0);

  ActionSample dis = plain();
  dis.enabled = true;
  dis.has_value = true;
  ActionSample dis2 = dis;
  dis2.enabled = false;
  CHECK(fp(dis, Control::SELECT) != fp(dis2, Control::SELECT));
}

TEST(actions_ps_m3_05_number_and_select_state_change_the_fingerprint) {
  auto n = [](float v) {
    Tree t;
    t.add(def("n", Control::NUMBER), number_sample(v, 0, 10, 1));
    return fingerprint(t.build());
  };
  CHECK(n(1.0f) != n(2.0f));
  auto sel = [](const char *text) {
    Tree t;
    ActionSample s = plain();
    std::snprintf(s.text, sizeof s.text, "%s", text);
    t.add(def("m", Control::SELECT), s);
    return fingerprint(t.build());
  };
  CHECK(sel("one") != sel("two"));
}

// --- PS-M2-05 / PS-M2-08 ------------------------------------------------------------------

TEST(actions_ps_m2_05_match_route_claims_exactly_the_three_routes) {
  CHECK(match_route(false, "/", false).route == Route::PAGE);
  CHECK(match_route(false, "/api/v1/state", false).route == Route::STATE);
  CHECK(match_route(false, "/api/v1/state?after=3", false).route == Route::STATE);
  CHECK(match_route(false, "/api/v1/state?path=a.b&after=3", true).route == Route::STATE);
  auto a = match_route(true, "/api/v1/actions/dev.logger/logging", true);
  CHECK(a.route == Route::ACTION);
  CHECK(std::string(a.node, a.node_len) == "dev.logger");
  CHECK(std::string(a.key, a.key_len) == "logging");
  // Not ours: everything else falls through to other handlers.
  for (const char *p : {"/index.html", "/api", "/api/v1", "/api/v1/state/", "/api/v1/stat", "/config.json", "/wifisave",
                        "/generate_204", "/hotspot-detect.html", "//", "/x", "", "/api/v1/actions/dev/x"}) {
    if (std::strcmp(p, "/api/v1/actions/dev/x") == 0)
      continue;  // GET on the action route is not claimed, checked below
    CHECK(match_route(false, p, true).route == Route::NONE);
    CHECK(match_route(true, p, true).route == Route::NONE);
  }
  CHECK(match_route(false, "/api/v1/actions/dev/x", true).route == Route::NONE);  // GET on the POST route
  CHECK(match_route(true, "/", true).route == Route::NONE);                       // POST / is not ours
  CHECK(match_route(true, "/api/v1/state", true).route == Route::NONE);
  CHECK(match_route(true, nullptr, true).route == Route::NONE);
}

TEST(actions_ps_m2_05_malformed_action_paths_are_not_claimed) {
  for (const char *p : {"/api/v1/actions", "/api/v1/actions/", "/api/v1/actions/dev", "/api/v1/actions/dev/",
                        "/api/v1/actions//key", "/api/v1/actions/dev/key/extra"}) {
    CHECK(match_route(true, p, true).route == Route::NONE);
  }
}

TEST(actions_ps_m2_08_parse_after_accepts_only_decimal_uint32) {
  uint32_t v = 0;
  CHECK(parse_after("0", &v) && v == 0);
  CHECK(parse_after("4294967295", &v) && v == 4294967295u);
  for (const char *bad : {"", "-1", "+1", "0x10", "1.5", "abc", "4294967296", " 1", "1 ", "99999999999999999999"})
    CHECK(!parse_after(bad, &v));
}

}  // namespace
