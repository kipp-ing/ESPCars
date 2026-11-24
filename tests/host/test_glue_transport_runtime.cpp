#include "harness.h"

#define USE_CCP_WRITE
#include "transport_runtime.h"

using namespace esphome;

namespace {
void reply(ccp::CcpHub &hub, const can_gateway::GatewayPort &port, uint8_t code = 0) {
  const uint8_t data[]{0xFF, code, port.sent.back().data[1], 1, 2, 3, 4, 5};
  hub.on_frame({0x701, data, 8, false, false});
}

ccp::DaqReadRequest daq_request() {
  ccp::DaqReadRequest req;
  req.fields.push_back({1, 0, 0x10203060});
  req.require_anchor = false;
  req.samples = 1;
  return req;
}

void collect(ccp::CcpHub &hub, can_gateway::GatewayPort &port, std::function<void(const ccp::DaqReadResult &)> done,
             bool refuse_stop = false) {
  hub.register_receive_id(0x702);  // Generated main registers IDs before gateway setup.
  CHECK(hub.daq_read(daq_request(), std::move(done)));
  // GET_DAQ_SIZE, SET_DAQ_PTR, WRITE_DAQ, SET_S_STATUS, START_STOP.
  for (unsigned i = 0; i < 5; i++)
    reply(hub, port);
  port.accept = !refuse_stop;
  const uint8_t sample[]{0, 42};
  hub.on_frame({0x702, sample, 2, false, false});
}
}  // namespace

TEST(ccp_runtime_pre_setup_registration_receives_response_and_daq_once) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  // Generated main can register action and response IDs in either order.
  hub.register_receive_id(0x701);
  hub.set_response_id(0x703);
  hub.register_receive_id(0x703);
  hub.register_receive_id(0x701);
  port.freeze();
  hub.setup();
  CHECK_EQ(port.subscribers.size(), 2u);
  CHECK(hub.connect());
  const uint8_t crm[]{0xFF, 0, port.sent.back().data[1], 0, 0, 0, 0, 0};
  port.receive({0x703, crm, 8, false, false});
  CHECK(hub.is_connected());
  unsigned samples = 0;
  hub.add_on_daq_callback([&](uint8_t, std::vector<uint8_t>) { samples++; });
  const uint8_t sample[]{0, 42};
  port.receive({0x701, sample, 2, false, false});
  CHECK_EQ(samples, 1u);
}

TEST(ccp_runtime_read_initial_refusal_releases_state) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  hub.set_enabled(false);
  unsigned errors = 0;
  hub.add_on_error_callback([&](uint8_t, uint8_t) { errors++; });
  CHECK(!hub.read_memory(0, 0x1000, 1, {}));
  CHECK_EQ(errors, 1u);
  hub.set_enabled(true);
  CHECK(hub.read_memory(0, 0x1000, 1, {}));
}

TEST(ccp_runtime_read_continuation_refusal_completes_once_and_reenters) {
  for (unsigned fail_at : {0u, 1u}) {
    can_gateway::GatewayPort port;
    ccp::CcpHub hub(&port);
    unsigned errors = 0;
    hub.add_on_error_callback([&](uint8_t, uint8_t) {
      errors++;
      port.accept = true;
      CHECK(hub.read_memory(0, 0x2000, 1, {}));
    });
    CHECK(hub.read_memory(0, 0x1000, 6, {}));
    if (fail_at == 1)
      reply(hub, port);  // First UPLOAD accepted; reject the second.
    port.accept = false;
    reply(hub, port);
    CHECK_EQ(errors, 1u);
    CHECK_EQ(port.sent.back().data[0], ccp::SET_MTA);
    hub.loop();
    CHECK_EQ(errors, 1u);
  }
}

TEST(ccp_runtime_write_refusal_completes_once_and_recovers) {
  for (unsigned fail_at : {0u, 1u, 2u}) {
    can_gateway::GatewayPort port;
    ccp::CcpHub hub(&port);
    hub.set_allow_write(true);
    unsigned calls = 0;
    const auto done = [&](bool ok) {
      CHECK(!ok);
      calls++;
      port.accept = true;
      CHECK(hub.write_memory(0, 0x2000, {9}, {}));
    };
    if (fail_at == 0)
      port.accept = false;
    CHECK_EQ(hub.write_memory(0, 0x1000, {1, 2, 3, 4, 5, 6, 7}, done), fail_at != 0);
    if (fail_at != 0) {
      if (fail_at == 2)
        reply(hub, port);
      port.accept = false;
      reply(hub, port);
    }
    CHECK_EQ(calls, 1u);
    if (!port.sent.empty())
      CHECK_EQ(port.sent.back().data[0], ccp::SET_MTA);
  }
}

TEST(ccp_runtime_teardown_negative_reply_reports_failure) {
  for (unsigned fail_at : {0u, 1u}) {
    can_gateway::GatewayPort port;
    ccp::CcpHub hub(&port);
    hub.set_allow_write(true);
    unsigned calls = 0;
    collect(hub, port, [&](const ccp::DaqReadResult &result) {
      calls++;
      CHECK(!result.ok);
      CHECK(result.failed_step != nullptr);
    });
    reply(hub, port, fail_at == 0 ? 1 : 0);
    reply(hub, port, fail_at == 1 ? 1 : 0);
    CHECK_EQ(calls, 1u);
  }
}

TEST(ccp_runtime_teardown_refusal_reports_failure_once) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  hub.set_allow_write(true);
  unsigned calls = 0;
  collect(hub, port, [&](const ccp::DaqReadResult &result) {
    calls++;
    CHECK(!result.ok);
    CHECK(result.failed_step != nullptr);
  });
  port.accept = false;  // Stop reply arrives; SET_S_STATUS submission fails.
  reply(hub, port);
  CHECK_EQ(calls, 1u);
}

TEST(ccp_runtime_teardown_stop_refusal_and_timeouts_report_failure) {
  for (unsigned fail_at : {0u, 1u, 2u}) {
    transport_now = 0;
    can_gateway::GatewayPort port;
    ccp::CcpHub hub(&port);
    hub.set_allow_write(true);
    unsigned calls = 0;
    collect(
        hub, port,
        [&](const ccp::DaqReadResult &result) {
          calls++;
          CHECK(!result.ok);
          CHECK(result.failed_step != nullptr);
        },
        fail_at == 0);
    if (fail_at != 0) {
      if (fail_at == 2)
        reply(hub, port);  // Stop succeeds; status times out.
      transport_now = 101;
      hub.loop();
      if (fail_at == 1)
        reply(hub, port);  // Stop times out; status succeeds.
    }
    CHECK_EQ(calls, 1u);
    CHECK(hub.warning);
    hub.loop();
    CHECK_EQ(calls, 1u);
  }
  transport_now = 0;
}

TEST(ccp_runtime_daq_routes_response_and_sample_ids_separately) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  hub.set_allow_write(true);
  hub.register_receive_id(0x702);
  hub.register_receive_id(0x703);
  unsigned calls = 0;
  CHECK(hub.daq_read(daq_request(), [&](const ccp::DaqReadResult &result) {
    calls++;
    CHECK(result.ok);
    CHECK_EQ(result.frames_seen, 1u);
    CHECK_EQ(result.values[0][0], 42u);
  }));
  const uint8_t impostor[]{0xFF, 0, port.sent.back().data[1], 1, 2, 3, 4, 5};
  hub.on_frame({0x702, impostor, 8, false, false});
  CHECK_EQ(port.sent.size(), 1u);  // A CRM on the DAQ ID cannot advance configuration.
  for (unsigned i = 0; i < 5; i++)
    reply(hub, port);
  const size_t configured = port.sent.size();
  const uint8_t sample[]{0, 42};
  for (uint32_t id : {0x701u, 0x703u, 0x704u})
    hub.on_frame({id, sample, 2, false, false});
  CHECK_EQ(port.sent.size(), configured);
  hub.on_frame({0x702, sample, 2, false, false});
  reply(hub, port);
  reply(hub, port);
  CHECK_EQ(calls, 1u);
}

TEST(ccp_runtime_undeclared_daq_id_refuses_before_tx_and_write_gate_stays_closed) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  hub.set_allow_write(true);
  CHECK(!hub.daq_read(daq_request(), {}));
  CHECK(port.sent.empty());
  hub.register_receive_id(0x702);
  hub.set_allow_write(false);
  CHECK(!hub.daq_read(daq_request(), {}));
  CHECK(!hub.select_cal_page());
  CHECK(!hub.set_s_status(0x44));
  const uint8_t raw_status[]{ccp::SET_S_STATUS, 0, 0x44};
  CHECK(!hub.raw(raw_status, sizeof(raw_status)));
  const uint8_t raw_page[]{ccp::SELECT_CAL_PAGE};
  CHECK(!hub.raw(raw_page, sizeof(raw_page)));
  CHECK(port.sent.empty());
}

TEST(isotp_runtime_extended_rejects_other_address_sf_ff_cf_fc) {
  using namespace isotp;
  can_gateway::GatewayPort port;
  IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.set_address_extension(0x13);
  protocol.set_receive_address(0xF1);
  protocol.setup();
  unsigned messages = 0;
  protocol.add_on_message_callback([&](MessageView) { messages++; });
  const uint8_t wrong_sf[]{0x14, 1, 42};
  const uint8_t wrong_ff[]{0x14, 0x10, 8, 1, 2, 3, 4, 5};
  protocol.on_frame({0x708, wrong_sf, 3, false, false});
  protocol.on_frame({0x708, wrong_ff, 8, false, false});
  CHECK_EQ(messages, 0u);
  CHECK(port.sent.empty());
  CHECK(!protocol.receiving());

  const uint8_t ff[]{0xF1, 0x10, 8, 1, 2, 3, 4, 5};
  const uint8_t wrong_cf[]{0x14, 0x21, 6, 7, 8};
  const uint8_t cf[]{0xF1, 0x21, 6, 7, 8};
  protocol.on_frame({0x708, ff, 8, false, false});
  CHECK_EQ(port.sent.back().data[0], 0x13);  // Outgoing FC still targets the peer.
  protocol.on_frame({0x708, wrong_cf, 5, false, false});
  CHECK(protocol.receiving());
  CHECK_EQ(messages, 0u);
  protocol.on_frame({0x708, cf, 5, false, false});
  CHECK_EQ(messages, 1u);

  const uint8_t request[8]{};
  CHECK(protocol.send(request, 8));
  protocol.loop();  // Sends FF, awaits FC.
  const size_t sent = port.sent.size();
  const uint8_t wrong_fc[]{0x14, 0x30, 8, 0};
  protocol.on_frame({0x708, wrong_fc, 4, false, false});
  protocol.loop();
  CHECK_EQ(port.sent.size(), sent);
  const uint8_t fc[]{0xF1, 0x30, 8, 0};
  protocol.on_frame({0x708, fc, 4, false, false});
  protocol.loop();
  CHECK_EQ(port.sent.size(), sent + 1);
  const uint8_t sf[]{0xF1, 1, 42};
  protocol.on_frame({0x708, sf, 3, false, false});
  CHECK_EQ(messages, 2u);
  protocol.on_frame({0x708, sf, 0, false, false});
  protocol.on_frame({0x708, sf, 1, false, false});
  CHECK_EQ(messages, 2u);
}

TEST(isotp_runtime_foreign_cf_does_not_extend_receive_timeout) {
  using namespace isotp;
  transport_now = 0;
  can_gateway::GatewayPort port;
  IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.set_address_extension(0x13);
  protocol.set_receive_address(0xF1);
  protocol.setup();
  const uint8_t ff[]{0xF1, 0x10, 8, 1, 2, 3, 4, 5};
  protocol.on_frame({0x708, ff, 8, false, false});
  transport_now = 999;
  const uint8_t foreign_cf[]{0x14, 0x21, 6, 7};
  protocol.on_frame({0x708, foreign_cf, 4, false, false});
  transport_now = 1000;
  protocol.loop();
  CHECK(!protocol.receiving());
  CHECK_EQ(protocol.transfers_failed(), 1u);
  CHECK_EQ(port.sent.size(), 1u);
  transport_now = 0;
}

TEST(isotp_runtime_overflow_notifies_even_when_fc_is_refused) {
  using namespace isotp;
  can_gateway::GatewayPort port;
  port.accept = false;
  IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.set_max_message_size(8);
  protocol.setup();
  unsigned errors = 0;
  protocol.add_on_error_callback([&](IsoTpError error) {
    CHECK_EQ(error, IsoTpError::OVERFLOW_LOCAL);
    errors++;
  });
  const uint8_t ff[]{0x10, 9, 1, 2, 3, 4, 5, 6};
  protocol.on_frame({0x708, ff, 8, false, false});
  CHECK_EQ(errors, 1u);
  CHECK_EQ(protocol.transfers_failed(), 1u);
  CHECK(port.sent.empty());
  port.accept = true;
  protocol.loop();
  CHECK_EQ(errors, 1u);
  CHECK_EQ(port.sent.size(), 1u);
  CHECK_EQ(port.sent.back().data[0], 0x32);
}

TEST(isotp_runtime_overflow_notifies_consumer_callback_counter_once) {
  using namespace isotp;
  struct Consumer : IsoTpConsumer {
    unsigned errors{0};
    void on_message(const MessageView &) override {}
    void on_error(IsoTpError error) override {
      CHECK_EQ(error, IsoTpError::OVERFLOW_LOCAL);
      this->errors++;
    }
  } consumer;
  can_gateway::GatewayPort port;
  IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.set_max_message_size(8);
  protocol.set_consumer(&consumer);
  protocol.setup();
  unsigned errors = 0;
  protocol.add_on_error_callback([&](IsoTpError error) {
    CHECK_EQ(error, IsoTpError::OVERFLOW_LOCAL);
    errors++;
  });
  const uint8_t ff[]{0x10, 9, 1, 2, 3, 4, 5, 6};
  protocol.on_frame({0x708, ff, 8, false, false});
  CHECK_EQ(errors, 1u);
  CHECK_EQ(consumer.errors, 1u);
  CHECK_EQ(protocol.transfers_failed(), 1u);
  CHECK_EQ(port.sent.size(), 1u);
  CHECK_EQ(port.sent.back().data[0], 0x32);
  protocol.loop();
  CHECK_EQ(errors, 1u);
  // A subsequent valid transfer must not inherit OVERFLOW_LOCAL.
  const uint8_t valid_ff[]{0x10, 8, 1, 2, 3, 4, 5, 6};
  const uint8_t cf[]{0x21, 7, 8};
  protocol.on_frame({0x708, valid_ff, 8, false, false});
  protocol.on_frame({0x708, cf, 3, false, false});
  CHECK_EQ(protocol.messages_received(), 1u);
  CHECK_EQ(errors, 1u);
}

TEST(ccp_b2_connect_busy_preserves_first_callback) {
  for (bool success : {false, true}) {
    can_gateway::GatewayPort port;
    ccp::CcpHub hub(&port);
    unsigned first = 0, second = 0;
    CHECK(hub.connect_and([&](bool ok) {
      CHECK_EQ(ok, success);
      first++;
    }));
    CHECK(!hub.connect_and([&](bool) { second++; }));
    CHECK_EQ(port.sent.size(), 1u);
    reply(hub, port, success ? 0 : 1);
    CHECK_EQ(first, 1u);
    CHECK_EQ(second, 0u);
  }
}

TEST(ccp_b2_connect_refusal_recovers_and_callback_can_reenter) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  unsigned refused = 0, completed = 0;
  port.accept = false;
  CHECK(!hub.connect_and([&](bool) { refused++; }));
  port.accept = true;
  CHECK(hub.connect_and([&](bool ok) {
    CHECK(ok);
    completed++;
    CHECK(hub.get_ccp_version());
  }));
  reply(hub, port);
  CHECK_EQ(completed, 1u);
  CHECK_EQ(refused, 0u);
  CHECK_EQ(port.sent.back().data[0], ccp::GET_CCP_VERSION);
}

TEST(ccp_b2_payload_actions_reject_before_narrowing) {
  for (bool templated : {false, true}) {
    for (size_t size : {0u, 6u, 9u, 257u}) {
      can_gateway::GatewayPort port;
      ccp::CcpHub hub(&port);
      hub.set_allow_write(true);
      ccp::CcpDownloadAction<> download;
      download.set_parent(&hub);
      const std::vector<uint8_t> data(size, ccp::DNLOAD);
      if (templated)
        download.set_data_template([data]() { return data; });
      else
        download.set_data_static(data);
      download.play();
      CHECK(port.sent.empty());
    }
    for (size_t size : {0u, 9u, 257u}) {
      can_gateway::GatewayPort port;
      ccp::CcpHub hub(&port);
      hub.set_allow_write(true);
      ccp::CcpRawAction<> raw;
      raw.set_parent(&hub);
      const std::vector<uint8_t> data(size, ccp::CONNECT);
      if (templated)
        raw.set_data_template([data]() { return data; });
      else
        raw.set_data_static(data);
      raw.play();
      CHECK(port.sent.empty());
    }
  }
}

TEST(isotp_b2_payload_action_rejects_before_narrowing) {
  for (bool templated : {false, true}) {
    for (size_t size : {0u, 9u, 4096u, 65537u}) {
      can_gateway::GatewayPort port;
      isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
      protocol.set_max_message_size(8);
      protocol.setup();
      isotp::SendAction<> action;
      action.set_parent(&protocol);
      const std::vector<uint8_t> data(size, 42);
      if (templated)
        action.set_data_template([data]() { return data; });
      else
        action.set_data_static(data);
      action.play();
      protocol.loop();
      CHECK(port.sent.empty());
      CHECK(!protocol.is_busy());
    }
  }
}

TEST(isotp_b2_pending_fc_dropped_on_transfer_end) {
  for (unsigned end : {0u, 1u, 2u, 3u, 4u}) {
    transport_now = 0;
    can_gateway::GatewayPort port;
    isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
    protocol.setup();
    port.accept = false;
    const uint8_t ff[]{0x10, 8, 1, 2, 3, 4, 5, 6};
    protocol.on_frame({0x708, ff, 8, false, false});
    CHECK(port.sent.empty());
    if (end == 0) {
      transport_now = 1000;  // Retry must not precede timeout detection.
    } else if (end == 1) {
      const uint8_t cf[]{0x21, 7, 8};
      protocol.on_frame({0x708, cf, 3, false, false});
    } else if (end == 2) {
      const uint8_t sf[]{1, 42};
      protocol.on_frame({0x708, sf, 2, false, false});
    } else if (end == 3) {
      const uint8_t bad_sn[]{0x22, 7, 8};
      protocol.on_frame({0x708, bad_sn, 3, false, false});
    } else {
      port.accept = true;  // Replacement FF's successful FC supersedes refused FC.
      protocol.on_frame({0x708, ff, 8, false, false});
      CHECK_EQ(port.sent.size(), 1u);
    }
    port.accept = true;
    const size_t sent = port.sent.size();
    protocol.loop();
    CHECK_EQ(port.sent.size(), sent);
  }
  transport_now = 0;
}

TEST(isotp_b2_refused_overflow_fc_retry_expires) {
  transport_now = 0;
  can_gateway::GatewayPort port;
  isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.set_max_message_size(8);
  protocol.setup();
  port.accept = false;
  const uint8_t ff[]{0x10, 9, 1, 2, 3, 4, 5, 6};
  protocol.on_frame({0x708, ff, 8, false, false});
  transport_now = 1000;
  port.accept = true;
  protocol.loop();
  CHECK(port.sent.empty());
  CHECK_EQ(protocol.transfers_failed(), 1u);
  transport_now = 0;
}

TEST(isotp_b2_empty_cf_cannot_extend_n_cr) {
  for (bool extended : {false, true}) {
    transport_now = 0;
    can_gateway::GatewayPort port;
    isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
    if (extended) {
      protocol.set_address_extension(0x13);
      protocol.set_receive_address(0xF1);
    }
    protocol.setup();
    const uint8_t ff[]{0x10, 8, 1, 2, 3, 4, 5, 6};
    const uint8_t ext_ff[]{0xF1, 0x10, 8, 1, 2, 3, 4, 5};
    protocol.on_frame({0x708, extended ? ext_ff : ff, 8, false, false});
    for (unsigned sn = 1; sn <= 9; sn++) {
      transport_now = sn * 100;
      const uint8_t cf[]{static_cast<uint8_t>(0x20 | sn)};
      const uint8_t ext_cf[]{0xF1, cf[0]};
      protocol.on_frame({0x708, extended ? ext_cf : cf, static_cast<uint8_t>(extended ? 2 : 1), false, false});
      CHECK(protocol.receiving());
    }
    CHECK_EQ(protocol.frames_ignored(), 9u);
    CHECK_EQ(port.sent.size(), 1u);  // Empty frames must not exhaust a granted block.
    transport_now = 1000;
    protocol.loop();
    CHECK(!protocol.receiving());
    CHECK_EQ(protocol.transfers_failed(), 1u);
  }
  transport_now = 0;
}

TEST(isotp_b2_empty_cf_does_not_consume_sequence) {
  transport_now = 0;
  can_gateway::GatewayPort port;
  isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.setup();
  const uint8_t ff[]{0x10, 8, 1, 2, 3, 4, 5, 6};
  const uint8_t empty[]{0x21};
  const uint8_t valid[]{0x21, 7, 8};
  protocol.on_frame({0x708, ff, 8, false, false});
  protocol.on_frame({0x708, empty, 1, false, false});
  protocol.on_frame({0x708, valid, 3, false, false});
  CHECK_EQ(protocol.messages_received(), 1u);
  CHECK_EQ(protocol.transfers_failed(), 0u);
}

TEST(ccp_b2_negative_response_warns_without_callback_and_recovers) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  CHECK(hub.get_ccp_version());
  transport_logs.clear();
  reply(hub, port, 0x32);
  CHECK(hub.warning);
  CHECK(std::any_of(transport_logs.begin(), transport_logs.end(), [](const std::string &line) {
    return line.find("0x1B") != std::string::npos && line.find("0x32") != std::string::npos;
  }));
  CHECK(hub.get_ccp_version());
  reply(hub, port);
  CHECK(!hub.warning);
}

TEST(ccp_b2_collect_timeout_rejected_before_tx) {
  for (uint32_t timeout : {0u, 0x80000000u, 0xFFFFFFFFu}) {
    can_gateway::GatewayPort port;
    ccp::CcpHub hub(&port);
    hub.set_allow_write(true);
    hub.register_receive_id(0x702);
    auto req = daq_request();
    req.collect_timeout = timeout;
    CHECK(!hub.daq_read(req, {}));
    CHECK(port.sent.empty());
  }
}

TEST(ccp_b2_collect_timeout_maximum_across_clock_wrap) {
  transport_now = 0xFFFFFFF0;
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  hub.set_allow_write(true);
  hub.register_receive_id(0x702);
  auto req = daq_request();
  req.collect_timeout = 0x7FFFFFFF;
  CHECK(hub.daq_read(req, {}));
  for (unsigned i = 0; i < 5; i++)
    reply(hub, port);
  const size_t sent = port.sent.size();
  hub.loop();
  CHECK_EQ(port.sent.size(), sent);
  transport_now += 0x7FFFFFFE;
  hub.loop();
  CHECK_EQ(port.sent.size(), sent);
  transport_now++;
  hub.loop();
  CHECK_EQ(port.sent.size(), sent + 1);  // Bounded cleanup starts exactly at expiry.
  transport_now = 0;
}

TEST(ccp_b2_payload_boundaries_preserve_write_gate_and_wire_bytes) {
  for (bool templated : {false, true}) {
    for (size_t size : {1u, 5u}) {
      can_gateway::GatewayPort port;
      ccp::CcpHub hub(&port);
      ccp::CcpDownloadAction<> action;
      action.set_parent(&hub);
      const std::vector<uint8_t> data(size, 42);
      if (templated)
        action.set_data_template([data]() { return data; });
      else
        action.set_data_static(data);
      action.play();
      CHECK(port.sent.empty());
      hub.set_allow_write(true);
      action.play();
      CHECK_EQ(port.sent.size(), 1u);
      CHECK_EQ(port.sent.back().data[0], ccp::DNLOAD);
      CHECK_EQ(port.sent.back().data[2], size);
      CHECK_BYTES(port.sent.back().data + 3, data.data(), size);
    }
    for (size_t size : {1u, 8u}) {
      can_gateway::GatewayPort port;
      ccp::CcpHub hub(&port);
      ccp::CcpRawAction<> action;
      action.set_parent(&hub);
      std::vector<uint8_t> data(size, 0);
      data[0] = ccp::GET_CCP_VERSION;
      if (templated)
        action.set_data_template([data]() { return data; });
      else
        action.set_data_static(data);
      action.play();
      CHECK_EQ(port.sent.size(), 1u);
      CHECK_BYTES(port.sent.back().data, data.data(), size);
    }
  }
}

TEST(isotp_b2_payload_capacity_accepted_and_oversize_preserves_inflight) {
  for (bool templated : {false, true}) {
    can_gateway::GatewayPort port;
    isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
    protocol.set_max_message_size(8);
    protocol.setup();
    isotp::SendAction<> action;
    action.set_parent(&protocol);
    const std::vector<uint8_t> data(8, 42);
    if (templated)
      action.set_data_template([data]() { return data; });
    else
      action.set_data_static(data);
    action.play();
    CHECK(protocol.is_busy());
    const std::vector<uint8_t> oversize(65537, 99);
    CHECK(!protocol.send(oversize.data(), oversize.size()));
    protocol.loop();
    CHECK_EQ(port.sent.size(), 1u);
    CHECK_EQ(port.sent.back().data[0], 0x10);
    CHECK_EQ(port.sent.back().data[1], 8);
    CHECK_BYTES(port.sent.back().data + 2, data.data(), 6);
  }
}

TEST(isotp_b2_pending_fc_live_retry_and_clock_wrap) {
  transport_now = 0xFFFFFFF0;
  can_gateway::GatewayPort port;
  isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.setup();
  port.accept = false;
  const uint8_t ff[]{0x10, 8, 1, 2, 3, 4, 5, 6};
  protocol.on_frame({0x708, ff, 8, false, false});
  transport_now += 999;
  port.accept = true;
  protocol.loop();
  CHECK_EQ(port.sent.size(), 1u);
  CHECK_EQ(port.sent.back().data[0], 0x30);
  protocol.loop();
  CHECK_EQ(port.sent.size(), 1u);
  transport_now++;
  protocol.loop();
  CHECK(!protocol.receiving());
  CHECK_EQ(protocol.transfers_failed(), 1u);
  transport_now = 0;
}

TEST(ccp_b2_negative_response_callback_observes_warning_once) {
  can_gateway::GatewayPort port;
  ccp::CcpHub hub(&port);
  unsigned errors = 0;
  hub.add_on_error_callback([&](uint8_t command, uint8_t code) {
    CHECK_EQ(command, ccp::GET_CCP_VERSION);
    CHECK_EQ(code, 0x32);
    CHECK(hub.warning);
    errors++;
  });
  CHECK(hub.get_ccp_version());
  reply(hub, port, 0x32);
  CHECK_EQ(errors, 1u);
  CHECK(hub.get_ccp_version());
  const uint8_t foreign[]{0xFF, 0, static_cast<uint8_t>(port.sent.back().data[1] + 1), 0, 0, 0, 0, 0};
  hub.on_frame({0x701, foreign, 8, false, false});
  CHECK(hub.warning);
  reply(hub, port);
  CHECK(!hub.warning);
  CHECK_EQ(errors, 1u);
}

TEST(isotp_b2_timeout_callback_can_replace_transfer_before_next_retry) {
  transport_now = 0;
  can_gateway::GatewayPort port;
  isotp::IsoTpProtocol protocol(&port, 0x700, 0x708, false);
  protocol.setup();
  const uint8_t ff[]{0x10, 8, 1, 2, 3, 4, 5, 6};
  unsigned errors = 0;
  protocol.add_on_error_callback([&](isotp::IsoTpError error) {
    CHECK_EQ(error, isotp::IsoTpError::TIMEOUT_CF);
    errors++;
    transport_now++;  // The callback crosses a millisecond after loop's time snapshot.
    protocol.on_frame({0x708, ff, 8, false, false});
  });
  port.accept = false;
  protocol.on_frame({0x708, ff, 8, false, false});
  transport_now = 1000;
  protocol.loop();
  CHECK_EQ(errors, 1u);
  CHECK(protocol.receiving());
  port.accept = true;
  protocol.loop();
  CHECK_EQ(port.sent.size(), 1u);
  if (!port.sent.empty())
    CHECK_EQ(port.sent.back().data[0], 0x30);
  transport_now = 0;
}
TEST(isotp_runtime_reports_sent_only_after_final_frame) {
  using namespace isotp;
  struct Consumer : IsoTpConsumer {
    unsigned sent{0};
    void on_message(const MessageView &) override {}
    void on_sent() override { this->sent++; }
  } consumer;
  for (uint16_t size : {3, 24}) {
    transport_now = 0;
    can_gateway::GatewayPort port;
    IsoTpProtocol protocol(&port, 0x700, 0x708, false);
    protocol.set_consumer(&consumer);
    protocol.setup();
    consumer.sent = 0;
    uint8_t request[24]{};
    CHECK(protocol.send(request, size));
    port.accept = false;
    protocol.loop();
    transport_now = 500;  // CAN backpressure has already consumed several P2 periods.
    protocol.loop();
    CHECK_EQ(consumer.sent, 0u);
    port.accept = true;
    protocol.loop();
    if (size > 7) {
      CHECK_EQ(consumer.sent, 0u);
      const uint8_t fc[] = {0x30, 0, 20};
      protocol.on_frame({0x708, fc, 3, false, false});
      protocol.loop();
      CHECK_EQ(consumer.sent, 0u);
      transport_now = 520;
      protocol.loop();
      CHECK_EQ(consumer.sent, 0u);
      transport_now = 540;
      protocol.loop();
    }
    CHECK_EQ(consumer.sent, 1u);
    protocol.loop();
    CHECK_EQ(consumer.sent, 1u);
    CHECK_EQ(protocol.messages_sent(), 1u);
  }
  transport_now = 0;
}
