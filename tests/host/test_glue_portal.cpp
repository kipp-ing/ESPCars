#include "harness.h"
#include "portal_runtime.h"

#include <condition_variable>

using namespace esphome;
using namespace esphome::portal;

namespace {
class TestPortal : public Portal {
 public:
  using Portal::copy_snapshot_;
  using Portal::sample_;
  using Portal::serve_state_;
  const core::ActionSample &action_sample() const { return this->action_samples_[0]; }
  const core::ActionDef &action_def() const { return this->action_defs_[0]; }
};
struct Rig {
  web_server_base::WebServerBase base;
  TestPortal portal;
  Rig(const char *id = "dev", core::NodeAbsentFn absent = nullptr, void *ctx = nullptr) {
    portal_test_mutex_failure = false;
    portal_test_after_unlock = nullptr;
    portal_test_http_thread = {};
    portal_test_logs.clear();
    captive_portal::portal_test_ap.active = false;
    portal.set_base(&base);
    portal.add_node("car", "car", "Vehicle", core::NO_PARENT, 0, false, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, nullptr);
    portal.add_node(id, "device", "Device", 0, 0, false, absent ? "switched_off" : nullptr, nullptr, nullptr, nullptr,
                    nullptr, nullptr, nullptr, absent, ctx);
  }
};
}  // namespace

TEST(portal_b2_absence_snapshot_is_frozen_from_main_loop) {
  struct Source {
    switch_::Switch sw;
    std::thread::id loop_thread;
    unsigned outside_loop{0};
  } source;
  auto predicate = +[](void *ctx) {
    auto &s = *static_cast<Source *>(ctx);
    if (std::this_thread::get_id() != s.loop_thread)
      s.outside_loop++;
    return !s.sw.state;
  };
  Rig r("dev", predicate, &source);
  r.portal.setup();
  std::thread loop([&] {
    source.loop_thread = std::this_thread::get_id();
    r.portal.sample_();
  });
  loop.join();
  core::Snapshot snap{};
  CHECK(r.portal.copy_snapshot_(&snap));
  // Change the source on the loop thread after copy-out; HTTP must see the old verdict.
  std::thread change([&] { source.sw.turn_on(); });
  change.join();
  core::State states[8];
  uint32_t ages[8];
  bool known[8];
  core::Resolved resolved{states, ages, known, 8};
  core::resolve(snap, resolved);
  CHECK_EQ(states[1], core::State::ABSENT);
  CHECK_EQ(source.outside_loop, 0u);
}

TEST(portal_b2_link_count_copied_before_mutex_release) {
  Rig r;
  r.portal.setup();
  r.portal.sample_();
  std::mutex gate;
  std::condition_variable cv;
  bool released = false, done = false;
  std::thread loop([&] {
    std::unique_lock<std::mutex> lock(gate);
    cv.wait(lock, [&] { return released; });
    captive_portal::portal_test_ap.active = true;
    r.portal.sample_();
    done = true;
    cv.notify_one();
  });
  portal_test_http_thread = std::this_thread::get_id();
  portal_test_after_unlock = [&] {
    std::unique_lock<std::mutex> lock(gate);
    released = true;
    cv.notify_one();
    cv.wait(lock, [&] { return done; });
  };
  core::Snapshot snap{};
  CHECK(r.portal.copy_snapshot_(&snap));
  portal_test_after_unlock = nullptr;
  loop.join();
  CHECK_EQ(snap.link_count, 0u);
  CHECK_EQ(snap.seq, 1u);
  CHECK(r.portal.copy_snapshot_(&snap));
  CHECK_EQ(snap.link_count, 1u);
  CHECK_EQ(snap.seq, 2u);
}

TEST(portal_b2_configured_long_paths_with_after_are_served) {
  std::string id(27, 'p');
  id += "." + std::string(40, 'c');
  Rig r(id.c_str());
  r.portal.setup();
  r.portal.sample_();
  for (const auto &query : {"path=" + id, "path=" + id + "&after=4294967295", "after=4294967295&path=" + id}) {
    httpd_req_t req;
    req.query = query;
    r.portal.serve_state_(&req);
    CHECK_EQ(req.status, std::string("200 OK"));
    CHECK(req.response.find("\"path\":\"" + id + "\"") != std::string::npos);
  }
}

TEST(portal_b2_dynamic_select_overflow_is_disabled_without_partial_utf8) {
  Rig r;
  std::string oversized(62, 'x');
  oversized += "é";
  select::Select select;
  select.traits.options = {"one", oversized.c_str()};
  select.active = 1;
  r.portal.add_action_select("mode", "Mode", &select);
  r.portal.setup();
  r.portal.sample_();
  CHECK(!r.portal.action_sample().enabled);
  CHECK(!r.portal.action_sample().has_value);
  CHECK_EQ(r.portal.action_def().option_count, 0u);
  CHECK(r.portal.warning);
  CHECK(!portal_test_logs.empty());
}

TEST(portal_b2_select_exact_byte_capacity_preserved) {
  Rig r;
  std::string option(61, 'x');
  option += "é";
  select::Select select;
  select.traits.options = {option.c_str()};
  r.portal.add_action_select("mode", "Mode", &select);
  r.portal.setup();
  r.portal.sample_();
  CHECK(r.portal.action_sample().enabled);
  CHECK_EQ(std::string(r.portal.action_sample().text), option);
}

TEST(portal_b2_decoded_form_nul_cannot_alias_select_option) {
  Rig r;
  select::Select select;
  select.traits.options = {"run"};
  r.portal.add_action_select("mode", "Mode", &select);
  r.portal.setup();
  r.portal.sample_();
  AsyncWebServerRequest request;
  request.req.method = HTTP_POST;
  request.req.uri = "/api/v1/actions/dev/mode";
  request.form = true;
  request.form_value = std::string("run\0suffix", 10);
  r.portal.handleRequest(&request);
  CHECK_EQ(request.req.status, std::string("400 Bad Request"));
  CHECK(request.req.response.find("invalid_body") != std::string::npos);
  CHECK(!portal_test_logs.empty());
  if (!portal_test_logs.empty())
    CHECK(portal_test_logs.back().find("no action dispatched") != std::string::npos);
}

TEST(portal_b2_mutex_failure_keeps_reason) {
  Rig r;
  portal_test_mutex_failure = true;
  r.portal.setup();
  CHECK(r.portal.failed);
  CHECK(r.portal.failure_reason != nullptr);
  if (r.portal.failure_reason != nullptr)
    CHECK_EQ(std::string(r.portal.failure_reason), std::string("Mutex allocation failed"));
}

TEST(portal_b2_dynamic_select_state_overflow_is_unavailable) {
  Rig r;
  select::Select select;
  select.traits.options = {"one"};
  r.portal.add_action_select("mode", "Mode", &select);
  r.portal.setup();
  r.portal.sample_();
  CHECK(r.portal.action_sample().enabled);
  const std::string oversized = std::string(62, 'x') + "é";
  select.traits.options[0] = oversized.c_str();
  r.portal.sample_();
  CHECK(!r.portal.action_sample().enabled);
  CHECK(!r.portal.action_sample().has_value);
  CHECK(r.portal.warning);
  CHECK(!portal_test_logs.empty());
}

TEST(portal_b2_form_nul_preserves_busy_result) {
  Rig r;
  select::Select select;
  select.traits.options = {"run"};
  r.portal.add_action_select("mode", "Mode", &select);
  r.portal.setup();
  r.portal.sample_();
  AsyncWebServerRequest request;
  request.req.method = HTTP_POST;
  request.req.uri = "/api/v1/actions/dev/mode";
  request.form = true;
  request.form_value = "run";
  r.portal.handleRequest(&request);
  CHECK_EQ(request.req.status, std::string("202 Accepted"));
  request.form_value = std::string("run\0suffix", 10);
  r.portal.handleRequest(&request);
  CHECK_EQ(request.req.status, std::string("409 Conflict"));
  CHECK_EQ(r.portal.action_sample().result.state, core::ActionState::RUNNING);
}
