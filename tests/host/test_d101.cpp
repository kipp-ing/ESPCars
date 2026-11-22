#include "gateway_core.h"
#include "harness.h"

#include <mutex>
#include <thread>

using namespace esphome::can_gateway;

TEST(d101_idle_match_is_strict) {
  auto callback = +[]() {};
  auto other = +[]() {};
  int group, foreign;
  CHECK(matches_idle_notify(callback, callback, &group, &group, 1));
  CHECK(!matches_idle_notify(other, callback, &group, &group, 1));
  CHECK(!matches_idle_notify(callback, callback, &foreign, &group, 1));
  CHECK(!matches_idle_notify(callback, callback, &group, &group, 0));
  CHECK(!matches_idle_notify(callback, callback, &group, &group, 3));
  CHECK(!matches_idle_notify(callback, callback, static_cast<int *>(nullptr), static_cast<int *>(nullptr), 1));
}

TEST(d101_allowlist_superset_preserves_software_decisions) {
  RuleEntry rules[4]{};
  rules[0].match_id = 0x120;
  rules[0].match_mask = 0x7FF;
  rules[0].flags = RULE_FLAG_DROP;  // First match still wins in software.
  rules[1].match_id = 0x120;
  rules[1].match_mask = 0x7F0;
  rules[1].flags = RULE_FLAG_CHECK_RTR | RULE_FLAG_RTR_VALUE;
  rules[2].match_id = 0x231;
  rules[2].match_mask = 0x7FF;
  rules[2].flags = RULE_FLAG_HAS_PATCH;
  RouteTable table{rules, 3, true};
  AllowlistFilter filter;
  const char *reason;
  CHECK(derive_allowlist_filter(table, filter, reason));
  CHECK(!filter.extended);
  CHECK(filter.mask == (0x7F0 & ~(0x120 ^ 0x231)));
  for (uint32_t id = 0; id <= MAX_STANDARD_ID; ++id) {
    for (bool rtr : {false, true}) {
      auto *rule = match_rule(table, id, false, rtr);
      if (rule != nullptr && !rule->is_drop())
        CHECK((id & filter.mask) == filter.id);
    }
  }
  CHECK(match_rule(table, 0x120, false, true)->is_drop());
  table.default_drop = false;
  CHECK(!derive_allowlist_filter(table, filter, reason));
  table.default_drop = true;
  rules[2].flags |= RULE_FLAG_EXTENDED;
  CHECK(!derive_allowlist_filter(table, filter, reason));
  rules[1].flags |= RULE_FLAG_EXTENDED;
  CHECK(derive_allowlist_filter(table, filter, reason));
  CHECK(filter.extended);
  rules[1].flags = rules[2].flags = RULE_FLAG_DROP;
  CHECK(!derive_allowlist_filter(table, filter, reason));
  table.rule_count = 0;
  CHECK(!derive_allowlist_filter(table, filter, reason));
  table.rule_count = 1;
  rules[0].flags = 0;
  rules[0].match_id = rules[0].match_mask = 0;
  CHECK(!derive_allowlist_filter(table, filter, reason));
}

TEST(d101_extended_allowlist_superset) {
  RuleEntry rules[2]{};
  rules[0].match_id = 0x18DA0000;
  rules[0].match_mask = 0x1FFF0000;
  rules[0].flags = RULE_FLAG_EXTENDED | RULE_FLAG_CHECK_RTR;
  rules[1].match_id = 0x18DBFF00;
  rules[1].match_mask = 0x1FFFFF00;
  rules[1].flags = RULE_FLAG_EXTENDED | RULE_FLAG_HAS_PATCH;
  RouteTable table{rules, 2, true};
  AllowlistFilter filter;
  const char *reason;
  CHECK(derive_allowlist_filter(table, filter, reason));
  CHECK(filter.extended);
  CHECK(filter.id == 0x18DA0000);
  CHECK(filter.mask == 0x1FFE0000);
  uint32_t sample = 42;
  for (const auto &rule : rules) {
    for (unsigned i = 0; i < 4096; ++i) {
      sample = sample * 1664525 + 1013904223;
      uint32_t id = rule.match_id | (sample & ~rule.match_mask & MAX_EXTENDED_ID);
      CHECK((id & filter.mask) == filter.id);
      CHECK(match_rule(table, id, true, false) != nullptr);
    }
  }
}

TEST(d101_refusal_edges_and_time_wrap) {
  InjectRefusalLog log;
  CHECK(!log.stop(10000));
  CHECK(log.refuse(0xFFFFFF00));
  CHECK(!log.refuse(0xFFFFFF01));
  CHECK(!log.stop(0x100));
  CHECK(log.stop(0x400));
  CHECK(log.count() == 2);
  CHECK(!log.stop(0x500));
  CHECK(log.refuse(0x600));
  for (unsigned i = 0; i < 10000; ++i)
    CHECK(!log.refuse(0x600 + i));
  CHECK(!log.stop(0x600 + 9999 + 999));
  CHECK(log.stop(0x600 + 9999 + 1000));
  CHECK(log.count() == 10001);
}

TEST(d101_refusal_serialized_task_producers) {
  InjectRefusalLog log;
  std::mutex mux;  // Device callers use the port mux, including the loop poll.
  unsigned starts = 0;
  auto producer = [&]() {
    for (unsigned i = 0; i < 10000; ++i) {
      std::lock_guard<std::mutex> lock(mux);
      starts += log.refuse(100);
    }
  };
  std::thread a(producer), b(producer);
  a.join();
  b.join();
  CHECK(starts == 1);
  CHECK(log.count() == 20000);
  CHECK(log.stop(1100));
  CHECK(!log.stop(1101));
}
