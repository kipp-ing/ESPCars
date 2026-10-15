/// Emits fictional portal envelopes for tests/portal/test_envelope_parity.py (PS-M1-07, PS-M1-08,
/// PS-M3-03). Its own main(); the pytest compiles it with
/// `c++ -std=c++17 -I components/portal tests/host/emit_portal_envelope.cpp`.
///
///   emit_portal_envelope <scenario> [path]     compact JSON to stdout, no trailing newline
///
/// Scenarios: actions (new core only, -DEMIT_SERIES_SCRATCH), basic, narrowed, no_identity, stale, large, series,
/// daumen (spec v1.5: value limits / level, not_requested, switched_off).
/// `basic`, `narrowed`, `no_identity`, `stale`, `large` and `series` use no action field. Since
/// spec v1.5 every value carries `level` (null without limits).

#include "portal_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::portal::core;

namespace {

bool flush_stdout(void *, const char *data, size_t size) { return std::fwrite(data, 1, size, stdout) == size; }

ValueSample num(const char *key, const char *label, float v, uint32_t age, const char *unit = nullptr, int8_t dec = 1) {
  ValueSample s{};
  s.key = key;
  s.label = label;
  s.unit = unit;
  s.number = v;
  s.age_ms = age;
  s.decimals = dec;
  s.has_value = true;
  s.age_known = true;
  return s;
}
ValueSample txt(const char *key, const char *label, const char *t, uint32_t age) {
  ValueSample s{};
  s.key = key;
  s.label = label;
  s.text = t;
  s.is_text = true;
  s.has_value = true;
  s.age_known = true;
  s.age_ms = age;
  return s;
}
ValueSample boolean(const char *key, const char *label, bool b) {
  ValueSample s{};
  s.key = key;
  s.label = label;
  s.number = b ? 1.0f : 0.0f;
  s.is_bool = true;
  s.has_value = true;
  s.age_known = true;
  return s;
}
ValueSample never(const char *key, const char *label) {
  ValueSample s{};
  s.key = key;
  s.label = label;
  return s;
}

NodeDef node(const char *id, const char *kind, const char *label, uint16_t parent, uint16_t vf, uint16_t vc,
             uint32_t budget, bool known = true) {
  NodeDef n{};
  n.id = id;
  n.kind = kind;
  n.label = label;
  n.parent = parent;
  n.value_first = vf;
  n.value_count = vc;
  n.budget_ms = budget;
  n.budget_known = known;
  return n;
}

bool cell(void *, uint16_t i, SeriesElement *out) {
  if (i == 1) {
    out->cls = 's';
    return true;
  }
  if (i == 2)
    return false;
  out->cls = i == 3 ? 'i' : 'r';
  out->has_value = true;
  out->value = 3300.0f + static_cast<float>(i);
  return true;
}

bool always_off(void *) { return true; }

const char *OWNS[] = {"ecu"};
const char *COMPONENTS[] = {"logger", "portal", "sensor"};

Identity identity(bool full) {
  Identity id{};
  id.board = full ? "demo" : nullptr;
  id.node_id = full ? "0xA" : nullptr;
  id.mac = full ? "02:00:00:00:00:01" : nullptr;
  id.build = "demo-1";
  id.owns = full ? OWNS : nullptr;
  id.owns_count = full ? 1 : 0;
  id.components = COMPONENTS;
  id.components_count = 3;
  return id;
}

int run(const std::string &scenario, const char *path) {
  static char buf[4096];
  State states[256];
  uint32_t ages[256];
  bool known[256];
  static SeriesElement scratch[SERIES_MAX_ELEMENTS];
  Resolved r{states, ages, known, 256};
#ifdef EMIT_SERIES_SCRATCH
  r.series_scratch = scratch;
#else
  (void) scratch;
#endif

  std::vector<NodeDef> nodes;
  std::vector<ValueSample> values;
  static SeriesDef sdef{"cells", "Cell voltages", "mV", "submodule_cell", 1, 0, false, 5, 0, cell, nullptr};
  static std::vector<std::string> names;  // keeps generated ids alive
  bool full = true;
  uint32_t seq = 7;
  const ValueLimit *s_limits = nullptr;
  size_t s_limit_count = 0;

  nodes.push_back(node("car", "car", "Vehicle", NO_PARENT, 0, 0, 0, false));
  nodes.push_back(node("ecu", "ecu", "Demo ECU", 0, 0, 3, 1500));
  values.push_back(num("vcc", "VCC", 3.3f, 20, "V", 2));
  values.push_back(txt("mode", "Mode", "run \"fast\"\n", 30));
  values.push_back(boolean("link", "Link", true));
  nodes[1].budget_known = false;
  nodes.push_back(node("ecu.unit", "module", "Unit", 1, 3, 2, 1500));
  values.push_back(num("temp", "Temperature", 21.5f, 40, "C", 1));
  values.push_back(never("spare", "Spare"));
  nodes[2].role = "role_a";
  nodes[2].roles_available = "role_a,role_b";
  nodes.push_back(node("gw", "gateway", "Gateway", 0, 5, 0, 0, false));
  nodes.back().absent_why = "not_compiled";

  if (scenario == "no_identity") {
    full = false;
  } else if (scenario == "stale") {
    values[0].age_ms = 9000;
    nodes[1].budget_known = true;
  } else if (scenario == "series") {
    nodes[2].series = &sdef;
  } else if (scenario == "large") {
    for (int g = 0; g < 8; g++) {
      for (int k = 0; k < 6; k++) {
        char id[48], lab[32];
        std::snprintf(id, sizeof id, "ecu.g%d.n%d", g, k);
        names.emplace_back(id);
        std::snprintf(lab, sizeof lab, "Group %d node %d", g, k);
        names.emplace_back(lab);
      }
    }
    // parents first: 8 group nodes, then 48 leaves
    size_t ni = 0;
    for (int g = 0; g < 8; g++) {
      char gid[32];
      std::snprintf(gid, sizeof gid, "ecu.g%d", g);
      names.emplace_back(gid);
      nodes.push_back(node(names.back().c_str(), "module", "Group", 1, 5, 0, 0, false));
    }
    for (int g = 0; g < 8; g++) {
      for (int k = 0; k < 6; k++) {
        const char *id = names[ni * 2].c_str();
        const char *lab = names[ni * 2 + 1].c_str();
        ni++;
        const uint16_t first = static_cast<uint16_t>(values.size());
        values.push_back(num("v", "Value", static_cast<float>(g * 10 + k), 10, "V", 1));
        values.push_back(num("w", "Other", static_cast<float>(k), 10, "A", 0));
        nodes.push_back(node(id, "sensor", lab, static_cast<uint16_t>(4 + g), first, 2, 1500));
      }
    }
  } else if (scenario == "narrowed") {
    nodes[2].series = &sdef;
  } else if (scenario == "daumen") {
    static ValueLimit limits[3]{};
    ValueLimit &rec_limit = limits[0];
    rec_limit.warn.has_above = true;
    rec_limit.warn.above = 95.0f;
    rec_limit.bad.has_above = true;
    rec_limit.bad.above = 127.0f;
    ValueLimit &off_limit = limits[1];
    off_limit.has_bad_when = true;
    off_limit.bad_when = true;
    ValueLimit &vcc_limit = limits[2];
    vcc_limit.warn.has_below = true;
    vcc_limit.warn.below = 12.0f;
    vcc_limit.bad.has_below = true;
    vcc_limit.bad.below = 11.5f;
    values[0].limit_slot = 3;  // ecu/vcc 3.3 V: bad (fictional limits)
    nodes.push_back(node("bus", "bus", "Demo bus", 0, 5, 2, 0, false));
    values.push_back(num("rec", "RX errors", 100.0f, 10, nullptr, 0));
    values.back().limit_slot = 1;
    values.push_back(boolean("bus_off", "Bus off", false));
    values.back().limit_slot = 2;
    nodes.push_back(node("ecus", "ecu", "Units", 0, 7, 0, 0, false));
    nodes.push_back(node("ecus.a", "ecu", "Unit A", 5, 7, 1, 0, false));
    nodes.back().on_demand = true;
    values.push_back(never("dtc", "DTC count"));
    nodes.push_back(node("bat", "battery", "Demo battery", 0, 8, 1, 0, false));
    nodes.back().absent_why = "switched_off";
    nodes.back().absent_fn = always_off;
    nodes.back().absent_evidence = "\"action\":\"bat/polling\"";
    values.push_back(num("soc", "SOC", 55.0f, 10, "%", 0));
    s_limits = limits;
    s_limit_count = 3;
  } else if (scenario != "basic" && scenario != "actions") {
    std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
    return 2;
  }

#ifdef EMIT_SERIES_SCRATCH
  static const char *OPTS[] = {"one", "two"};
  static ActionDef adefs[4];
  static ActionSample asamples[4];
  if (scenario == "actions") {
    adefs[0] = ActionDef{"logging", "Logging", Control::SWITCH, 2, nullptr, 0};
    adefs[1] = ActionDef{"restart", "Restart", Control::BUTTON, 2, nullptr, 0};
    adefs[2] = ActionDef{"mode", "Mode", Control::SELECT, 2, OPTS, 2};
    adefs[3] = ActionDef{"interval", "Interval", Control::NUMBER, 1, nullptr, 0};
    asamples[0].has_value = true;
    asamples[0].boolean = true;
    asamples[0].result = ActionResult{ActionState::ACCEPTED, ActionCode::APPLIED, 2140, {ValueKind::BOOL, true, 0.0f, {}}};
    asamples[1].result = ActionResult{ActionState::RUNNING, ActionCode::QUEUED, 5, {}};
    asamples[2].has_value = true;
    std::snprintf(asamples[2].text, sizeof asamples[2].text, "two");
    asamples[3].enabled = false;
    asamples[3].has_min = asamples[3].has_max = asamples[3].has_step = true;
    asamples[3].min = 1.0f;
    asamples[3].max = 60.0f;
    asamples[3].step = 0.5f;
    asamples[3].unit = "s";
    nodes[1].action_first = 0;
    nodes[1].action_count = 4;
  }
#endif

  Snapshot s{};
#ifdef EMIT_SERIES_SCRATCH
  if (scenario == "actions") {
    s.actions = adefs;
    s.action_samples = asamples;
    s.action_count = 4;
  }
#endif
  s.seq = seq;
  s.uptime_ms = 123456;
  s.identity = identity(full);
  s.nodes = nodes.data();
  s.node_count = nodes.size();
  s.values = values.data();
  s.value_count = values.size();
  s.limits = s_limits;
  s.limit_count = s_limit_count;
  resolve(s, r);
  ChunkWriter w;
  w.init(buf, sizeof buf, flush_stdout, nullptr);
  const char *p = scenario == "narrowed" ? (path != nullptr ? path : "ecu.unit") : nullptr;
  if (p != nullptr && !has_node(s, p)) {
    std::fprintf(stderr, "no such node\n");
    return 3;
  }
  if (!write_envelope(w, s, r, p) || !w.finish())
    return 1;
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  return run(argv[1], argc > 2 ? argv[2] : nullptr);
}
