#include "harness.h"
#include "uds_blob_builder.h"
#include "../../components/uds/uds.cpp"

using namespace esphome;
using namespace esphome::uds;

namespace {
std::vector<uint8_t> hub_blob(uint16_t flags = GROUP_FLAG_SAFE_READ, size_t request_size = 3) {
  udstest::BlobBuilder b;
  b.add_ecu({"MINI", 0x7E7, 0x7EF, 0, 8, 0, 150, 2000, 0, 2});
  std::vector<uint8_t> req(request_size, 0);
  req[0] = 0x22;
  b.add_group({"mini", req, flags, 0, 1, 0x0101, 4});
  b.add_field({"value", "", 24, 8, 0, 0, 0, 1, 0, 0});
  b.add_group({"other", {0x22, 0x01, 0x02}, GROUP_FLAG_SAFE_READ, 1, 0, 0x0102, 3});
  return b.build();
}

class Hub : public UdsHub {
 public:
  using UdsHub::UdsHub;
  using UdsHub::report_group_error_;
  using UdsHub::send_group_;
};

struct Rig {
  std::vector<uint8_t> blob;
  isotp::IsoTpProtocol tp;
  Hub hub{&tp};
  sensor::Sensor sensor;
  std::vector<UdsError> errors;

  Rig(uint16_t flags = GROUP_FLAG_SAFE_READ, size_t request_size = 3) : blob(hub_blob(flags, request_size)) {
    uds_test_now = 0;
    uds_test_logs.clear();
    hub.set_catalog_embedded(blob.data(), blob.size());
    hub.reserve_bindings(1);
    hub.reserve_poll_slots(1);
    hub.add_sensor_binding("mini", "value", 0, 1000, &sensor);
    hub.add_on_error_callback([this](UdsError e) { errors.push_back(e); });
  }
  void tick(uint32_t now) {
    uds_test_now = now;
    hub.loop();
  }
};
}  // namespace

TEST(uds_hub_unsafe_poll_refused_even_with_active_services) {
  Rig r(0);  // a newer flashed catalog removed SAFE_READ
  r.hub.set_allow_active_services(true);
  r.hub.setup();
  r.tick(1000);
  r.tick(2000);
  CHECK_EQ(r.tp.attempts, 0u);
  CHECK_EQ(r.hub.groups_suspended(), 1u);
  CHECK_EQ(r.errors.size(), 1u);
  CHECK(std::string(r.errors[0].what).find("not SAFE_READ") != std::string::npos);
  CHECK(r.hub.warning);
  CHECK(!uds_test_logs.empty());
  CHECK(uds_test_logs.back().find("no transmission") != std::string::npos);
  CHECK(std::isnan(r.sensor.state));
  // Exercise the choke point directly as well as the scheduler.
  r.hub.send_group_(0);
  CHECK_EQ(r.tp.attempts, 0u);
}

TEST(uds_hub_unresolved_ecu_refuses_every_action) {
  Rig r;
  r.hub.set_ecu_name("missing");
  r.hub.set_allow_active_services(true);
  r.hub.setup();
  CHECK(!r.hub.request_read("mini"));
  CHECK(!r.hub.request_execute("mini"));
  const uint8_t raw[] = {0x22, 0x01, 0x01};
  CHECK(!r.hub.send_raw(raw, sizeof(raw)));
  r.tick(1000);
  CHECK_EQ(r.tp.attempts, 0u);
  CHECK_EQ(r.errors.size(), 3u);
  CHECK(r.hub.is_failed());
  CHECK(std::isnan(r.sensor.state));
  r.hub.send_group_(0);
  CHECK_EQ(r.tp.attempts, 0u);
}

TEST(uds_hub_error_reason_survives_delayed_automation) {
  Rig r;
  r.hub.setup();
  const char *first = "response timeout";
  r.hub.report_group_error_(0, first, 0);
  r.hub.report_group_error_(1, "negative response", 0x31);
  CHECK_EQ(r.errors.size(), 2u);
  // DelayAction copies the event by value. The original reason must survive later errors.
  CHECK(r.errors[0].what == first);
  CHECK(std::string(r.errors[0].what) == "response timeout");
  CHECK(std::string(r.errors[1].what) == "negative response");
  CHECK_EQ(r.errors[1].nrc, 0x31);
}

TEST(uds_hub_oversized_raw_fails_once_and_polling_progresses) {
  Rig r;
  r.tp.capacity = 8;
  r.hub.set_allow_active_services(true);
  r.hub.setup();
  const uint8_t raw[32] = {0x31};
  r.hub.send_raw(raw, sizeof(raw));
  r.tick(1000);
  r.tick(1001);
  CHECK_EQ(r.errors.size(), 1u);
  CHECK_EQ(r.tp.sent.size(), 1u);
  CHECK_EQ(r.tp.attempts, 1u);
}

TEST(uds_hub_oversized_group_is_suspended_once) {
  Rig r(GROUP_FLAG_SAFE_READ, 9);
  r.tp.capacity = 8;
  r.hub.setup();
  r.tick(1000);
  r.tick(2000);
  CHECK_EQ(r.tp.attempts, 0u);
  CHECK_EQ(r.hub.groups_suspended(), 1u);
  CHECK_EQ(r.errors.size(), 1u);
  CHECK(r.hub.request_read("other"));
  r.tick(2001);
  CHECK_EQ(r.tp.sent.size(), 1u);
}

TEST(uds_hub_oversized_ondemand_does_not_starve_next_group) {
  Rig r(GROUP_FLAG_SAFE_READ, 9);
  r.tp.capacity = 8;
  r.hub.setup();
  CHECK(r.hub.request_read("mini"));
  CHECK(r.hub.request_read("other"));
  r.tick(1);
  r.tick(2);
  CHECK_EQ(r.errors.size(), 1u);
  CHECK_EQ(r.tp.sent.size(), 1u);
  CHECK_EQ(r.tp.attempts, 1u);
}

TEST(uds_hub_busy_transport_retries_safe_poll_without_error) {
  Rig r;
  r.hub.setup();
  r.tp.busy = true;
  r.tick(1000);
  CHECK(r.tp.sent.empty());
  CHECK(r.errors.empty());
  r.tp.busy = false;
  r.tick(1001);
  CHECK_EQ(r.tp.sent.size(), 1u);
  CHECK_EQ(r.hub.requests_sent(), 1u);
  CHECK_EQ(r.hub.groups_suspended(), 0u);
}

TEST(uds_hub_explicit_execute_preserves_active_permission) {
  Rig r(0);
  r.hub.set_allow_active_services(true);
  r.hub.setup();
  CHECK(!r.hub.request_read("mini"));
  CHECK(r.hub.request_execute("mini"));
  r.tick(1);
  CHECK_EQ(r.tp.sent.size(), 1u);
  CHECK(r.errors.empty());
}

TEST(uds_hub_busy_raw_retries_and_sends_at_capacity) {
  Rig r;
  r.tp.capacity = 8;
  r.hub.set_allow_active_services(true);
  r.hub.setup();
  const uint8_t raw[8] = {0x31, 1, 2, 3, 4, 5, 6, 7};
  CHECK(r.hub.send_raw(raw, sizeof(raw)));
  r.tp.busy = true;
  r.tick(1);
  CHECK(r.tp.sent.empty());
  r.tp.busy = false;
  r.tick(2);
  CHECK_EQ(r.tp.sent.size(), 1u);
  CHECK(r.tp.sent[0] == std::vector<uint8_t>(raw, raw + sizeof(raw)));
  CHECK(r.errors.empty());
}

TEST(uds_hub_queued_raw_capacity_rechecked_at_send) {
  Rig r;
  r.hub.set_allow_active_services(true);
  r.hub.setup();
  const uint8_t raw[9] = {0x31};
  CHECK(r.hub.send_raw(raw, sizeof(raw)));
  r.tp.capacity = 8;
  r.tick(1);
  r.tick(1000);
  CHECK_EQ(r.errors.size(), 1u);
  CHECK_EQ(r.tp.sent.size(), 1u);
  CHECK_EQ(r.tp.attempts, 1u);
}

TEST(uds_hub_failed_setup_publishes_all_bindings_before_mark_failed) {
  const auto blob = hub_blob();
  isotp::IsoTpProtocol tp;
  Hub hub(&tp);
  sensor::Sensor sensors[UDS_PUBLISH_PER_LOOP + 1];
  hub.set_catalog_embedded(blob.data(), blob.size());
  hub.set_ecu_name("missing");
  hub.reserve_bindings(UDS_PUBLISH_PER_LOOP + 1);
  for (auto &s : sensors) {
    s.state = 42;
    hub.add_sensor_binding("mini", "value", 0, 1000, &s);
  }
  hub.setup();
  CHECK(hub.is_failed());
  CHECK(!hub.catalog_ready());
  for (const auto &s : sensors)
    CHECK(std::isnan(s.state));
}
