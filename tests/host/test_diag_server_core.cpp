// Match, session, S3 and fault logic of components/diag_server/diag_server_core.h.
//
// Expectations come from docs/diag_server-spec.md (section R, session rules, faults, behaviour
// guarantees), not from the implementation. The tables are fictional. The clock is a plain
// uint32_t the test advances; the "transport" is a pump that sends each polled reply at once.

#include "harness.h"
#include "diag_server_core.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace esphome::diag_server;

namespace {

constexpr uint16_t ANY = ANY_SESSION;

struct Row {
  const char *req;   // hex bytes, a trailing "*" makes it a prefix entry
  const char *resp;  // hex bytes
  uint16_t session{ANY};
};

std::vector<uint8_t> parse_hex(const std::string &text, bool *prefix = nullptr) {
  std::vector<uint8_t> out;
  std::istringstream in(text);
  std::string token;
  if (prefix != nullptr)
    *prefix = false;
  while (in >> token) {
    if (token == "*") {
      if (prefix != nullptr)
        *prefix = true;
      continue;
    }
    out.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
  }
  return out;
}

std::string hex(const std::vector<uint8_t> &bytes) {
  std::string out;
  char buf[4];
  for (auto byte : bytes) {
    std::snprintf(buf, sizeof(buf), "%02X", byte);
    out += out.empty() ? "" : " ";
    out += buf;
  }
  return out;
}

struct Msg {
  std::vector<uint8_t> bytes;
  bool pending;
};

/// One server with a fictional table, a reply buffer and a transport that delivers at once.
class Rig {
 public:
  explicit Rig(const std::vector<Row> &rows, uint32_t s3_ms = 5000, bool enabled = true) {
    for (const auto &row : rows) {
      bool prefix = false;
      const auto req = parse_hex(row.req, &prefix);
      const auto resp = parse_hex(row.resp);
      Entry entry{};
      entry.request_offset = static_cast<uint16_t>(bytes_.size());
      bytes_.insert(bytes_.end(), req.begin(), req.end());
      entry.reply_offset = static_cast<uint16_t>(bytes_.size());
      bytes_.insert(bytes_.end(), resp.begin(), resp.end());
      entry.request_size = static_cast<uint16_t>(req.size());
      entry.reply_size = static_cast<uint16_t>(resp.size());
      entry.session = row.session;
      entry.prefix = prefix;
      entries_.push_back(entry);
    }
    core_ = std::make_unique<ServerCore>(Table{bytes_.data(), entries_.data(), static_cast<uint16_t>(entries_.size())},
                                         s3_ms);
    core_->set_enabled(enabled);
  }

  ServerCore &core() { return *core_; }

  /// Hand one request to the server, as the ISO-TP consumer callback would.
  Decision request(uint32_t now, const char *hex_request, bool busy = false) {
    const auto req = parse_hex(hex_request);
    return core_->on_request(now, req.data(), static_cast<uint16_t>(req.size()), busy, buffer_);
  }

  /// One loop iteration: everything the server wants to send now, each sent at once.
  std::vector<Msg> pump(uint32_t now) {
    std::vector<Msg> out;
    for (int guard = 0; guard < 8; guard++) {
      uint8_t pending[3]{};
      const Reply reply = core_->poll(now, false, pending);
      if (reply.size == 0)
        break;
      const uint8_t *src = reply.pending ? pending : buffer_;
      out.push_back({std::vector<uint8_t>(src, src + reply.size), reply.pending});
      core_->reply_sent(now, reply);
    }
    return out;
  }

  /// request + pump at the same instant.
  std::vector<Msg> ask(uint32_t now, const char *hex_request) {
    request(now, hex_request);
    return pump(now);
  }

  /// The single reply as hex, or "<none>" / "<n replies>".
  std::string answer(uint32_t now, const char *hex_request) {
    const auto out = ask(now, hex_request);
    if (out.empty())
      return "<none>";
    if (out.size() > 1)
      return "<" + std::to_string(out.size()) + " replies>";
    return hex(out[0].bytes);
  }

 private:
  std::vector<uint8_t> bytes_;
  std::vector<Entry> entries_;
  std::unique_ptr<ServerCore> core_;
  uint8_t buffer_[64]{};
};

}  // namespace

#define CHECK_ANSWER(rig, now, request, expected) \
  do { \
    const std::string got_ = (rig).answer((now), (request)); \
    CHECK_MSG(got_ == (expected), \
              std::string("request ") + (request) + ": got [" + got_ + "], want [" + (expected) + "]"); \
  } while (0)

// ------------------------------------------------------------------ matching precedence

TEST(diag_server_exact_entry_answers) {
  Rig rig({{"22 F1 90", "62 F1 90 57 44 44"}});
  CHECK_ANSWER(rig, 0, "22 F1 90", "62 F1 90 57 44 44");
}

TEST(diag_server_exact_beats_prefix_in_either_table_order) {
  for (bool exact_first : {true, false}) {
    std::vector<Row> rows{{"22 01 02", "62 01 02 AA"}, {"22 01 *", "7F 22 31"}};
    if (!exact_first)
      std::swap(rows[0], rows[1]);
    Rig rig(rows);
    CHECK_ANSWER(rig, 0, "22 01 02", "62 01 02 AA");
    CHECK_ANSWER(rig, 1, "22 01 03", "7F 22 31");  // the prefix still serves what the exact entry does not
  }
}

TEST(diag_server_longest_prefix_wins_in_any_table_order) {
  const std::vector<Row> base{{"22 *", "01"}, {"22 01 *", "02"}, {"22 01 02 *", "03"}};
  static const int perms[6][3]{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (const auto &perm : perms) {
    std::vector<Row> rows;
    for (int i : perm)
      rows.push_back(base[i]);
    Rig rig(rows);
    CHECK_ANSWER(rig, 0, "22 01 02 03", "03");
    CHECK_ANSWER(rig, 1, "22 01 05", "02");
    CHECK_ANSWER(rig, 2, "22 09", "01");
  }
}

TEST(diag_server_prefix_does_not_match_other_bytes) {
  Rig rig({{"22 01 *", "7F 22 31"}});
  CHECK_ANSWER(rig, 0, "22 02 00", "7F 22 31");  // no match, but SID 22 is in the table: NRC 31
  CHECK_ANSWER(rig, 1, "22", "7F 22 31");        // shorter than the prefix key cannot match it either
}

// ----------------------------------------------------------------- section R, row by row

TEST(diag_server_r_tester_present_positive) {
  Rig rig({});
  CHECK_ANSWER(rig, 0, "3E 00", "7E 00");
  CHECK_EQ(rig.core().counters().misses, 0u);
}

TEST(diag_server_r_tester_present_suppress_is_silent) {
  Rig rig({});
  const auto d = rig.request(0, "3E 80");
  CHECK(d.reply_size == 0);
  CHECK(rig.pump(0).empty());
  CHECK(rig.pump(1000).empty());
  CHECK_EQ(rig.core().counters().misses, 0u);
}

TEST(diag_server_r_exact_entry_beats_builtin_tester_present) {
  Rig rig({{"3E 00", "7E 00 AA"}, {"3E 80", "7E 80"}});
  CHECK_ANSWER(rig, 0, "3E 00", "7E 00 AA");
  CHECK_ANSWER(rig, 1, "3E 80", "7E 80");
}

// R header: "rules built in (before the table, unless the table has an exact entry)". A prefix
// entry is not an exact entry, so the built-in 3E rows win. The Tests section lists the order as
// "exact, then longest prefix, then R", which would flip this case: the spec contradicts itself.
TEST(diag_server_r_builtin_tester_present_beats_prefix_entry_per_r_header) {
  Rig rig({{"3E *", "7F 3E 22"}});
  CHECK_ANSWER(rig, 0, "3E 00", "7E 00");
  CHECK_ANSWER(rig, 1, "3E 80", "<none>");
  CHECK_ANSWER(rig, 2, "3E 05", "7F 3E 22");  // other 3E requests: the table
}

TEST(diag_server_r_session_control_without_entry) {
  Rig rig({});
  CHECK_ANSWER(rig, 0, "10 03", "50 03 00 32 01 F4");
  CHECK_ANSWER(rig, 1, "10 7F", "50 7F 00 32 01 F4");
  CHECK_EQ(rig.core().counters().misses, 0u);
}

TEST(diag_server_r_session_control_without_entry_among_other_10_entries) {
  Rig rig({{"10 03", "50 03 11 22 33 44"}});
  CHECK_ANSWER(rig, 0, "10 02", "50 02 00 32 01 F4");  // no entry for 02, so the built-in
  CHECK_ANSWER(rig, 1, "10 03", "50 03 11 22 33 44");  // an entry for 03, so the table
}

TEST(diag_server_r_session_changes_only_on_a_50_reply) {
  Rig rig({{"10 03", "50 03 00 32 01 F4"}, {"10 04", "7F 10 22"}, {"22 03 18", "62 03 18 01", 0x03}});
  CHECK_ANSWER(rig, 0, "22 03 18", "7F 22 7F");  // session 01
  CHECK_ANSWER(rig, 1, "10 04", "7F 10 22");     // a negative table reply keeps the session
  CHECK_ANSWER(rig, 2, "22 03 18", "7F 22 7F");
  CHECK_ANSWER(rig, 3, "10 03", "50 03 00 32 01 F4");  // 50: session becomes 03
  CHECK_ANSWER(rig, 4, "22 03 18", "62 03 18 01");
}

TEST(diag_server_r_builtin_session_change_applies) {
  Rig rig({{"22 03 18", "62 03 18 01", 0x03}});
  CHECK_ANSWER(rig, 0, "22 03 18", "7F 22 7F");
  CHECK_ANSWER(rig, 1, "10 03", "50 03 00 32 01 F4");  // built-in 10 03, no table entry
  CHECK_ANSWER(rig, 2, "22 03 18", "62 03 18 01");
  CHECK_ANSWER(rig, 3, "10 01", "50 01 00 32 01 F4");  // and back to the default session
  CHECK_ANSWER(rig, 4, "22 03 18", "7F 22 7F");
}

TEST(diag_server_r_session_control_may_use_a_matching_prefix_entry) {
  Rig rig({{"10 *", "50 02 00 32 01 F4"}, {"22 02 01", "62 02 01 AA", 0x02}});
  CHECK_ANSWER(rig, 0, "10 02", "50 02 00 32 01 F4");  // "10 xx with a table entry: the table reply"
  CHECK_ANSWER(rig, 1, "22 02 01", "62 02 01 AA");
}

TEST(diag_server_r_no_match_with_sid_in_table_is_nrc_31) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  CHECK_ANSWER(rig, 0, "22 F1 91", "7F 22 31");
  CHECK_ANSWER(rig, 1, "22", "7F 22 31");
  CHECK_ANSWER(rig, 2, "22 F1 90 00", "7F 22 31");  // an exact key does not match a longer request
  CHECK_EQ(rig.core().counters().misses, 3u);
}

TEST(diag_server_r_unknown_sid_is_nrc_11) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  CHECK_ANSWER(rig, 0, "19 02 FF", "7F 19 11");
  CHECK_ANSWER(rig, 1, "3E 01", "7F 3E 11");
  CHECK_ANSWER(rig, 2, "27 01", "7F 27 11");  // security access is out of scope: serviceNotSupported
  CHECK_EQ(rig.core().counters().misses, 3u);
}

TEST(diag_server_r_session_control_with_odd_length_is_not_the_builtin) {
  Rig rig({});
  CHECK_ANSWER(rig, 0, "10", "7F 10 11");
  CHECK_ANSWER(rig, 1, "10 03 00", "7F 10 11");
}

TEST(diag_server_r_table_entry_for_3e_makes_other_3e_requests_nrc_31) {
  Rig rig({{"3E 55", "7E 55"}});
  CHECK_ANSWER(rig, 0, "3E 01", "7F 3E 31");
  CHECK_ANSWER(rig, 1, "3E 00", "7E 00");  // built-in still active
}

TEST(diag_server_r_empty_request_is_ignored) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  const auto d = rig.core().on_request(0, nullptr, 0, false, nullptr);
  CHECK(d.reply_size == 0);
  CHECK(rig.pump(0).empty());
  CHECK(rig.pump(5000).empty());
  CHECK_EQ(rig.core().counters().misses, 0u);
}

// ---------------------------------------------------------------- session, S3 and fake clock

TEST(diag_server_session_starts_at_01) {
  Rig rig({{"22 01 01", "62 01 01 AA", 0x01}, {"22 01 03", "62 01 03 BB", 0x03}});
  CHECK_EQ(rig.core().session(), 0x01);
  CHECK_ANSWER(rig, 0, "22 01 01", "62 01 01 AA");
  CHECK_ANSWER(rig, 1, "22 01 03", "7F 22 7F");
}

TEST(diag_server_s3_falls_back_to_01_after_silence) {
  Rig rig({{"22 03 18", "62 03 18 01", 0x03}}, 5000);
  CHECK_ANSWER(rig, 1000, "10 03", "50 03 00 32 01 F4");
  CHECK_ANSWER(rig, 1000 + 4999, "22 03 18", "62 03 18 01");      // still inside S3
  CHECK_ANSWER(rig, 1000 + 4999 + 5001, "22 03 18", "7F 22 7F");  // silence longer than S3
  CHECK_EQ(rig.core().session(), 0x01);
}

TEST(diag_server_s3_is_restarted_by_every_request) {
  Rig rig({{"22 03 18", "62 03 18 01", 0x03}}, 5000);
  CHECK_ANSWER(rig, 0, "10 03", "50 03 00 32 01 F4");
  for (uint32_t t = 4000; t <= 24000; t += 4000)
    CHECK_ANSWER(rig, t, "3E 00", "7E 00");  // each gap is shorter than S3, the total is far longer
  CHECK_ANSWER(rig, 28000, "22 03 18", "62 03 18 01");
}

TEST(diag_server_s3_follows_the_configured_timeout) {
  Rig rig({{"22 03 18", "62 03 18 01", 0x03}}, 200);
  CHECK_ANSWER(rig, 0, "10 03", "50 03 00 32 01 F4");
  CHECK_ANSWER(rig, 150, "22 03 18", "62 03 18 01");
  CHECK_ANSWER(rig, 150 + 250, "22 03 18", "7F 22 7F");
}

TEST(diag_server_s3_works_across_clock_wraparound) {
  Rig rig({{"22 03 18", "62 03 18 01", 0x03}}, 5000);
  const uint32_t t0 = 0xFFFFFF00u;
  CHECK_ANSWER(rig, t0, "10 03", "50 03 00 32 01 F4");
  CHECK_ANSWER(rig, t0 + 1000, "22 03 18", "62 03 18 01");  // the clock wrapped, only 1 s passed
  CHECK_ANSWER(rig, t0 + 1000 + 6000, "22 03 18", "7F 22 7F");
}

TEST(diag_server_s3_expiry_is_visible_in_the_loop_without_a_request) {
  Rig rig({}, 5000);
  CHECK_ANSWER(rig, 0, "10 03", "50 03 00 32 01 F4");
  CHECK_EQ(rig.core().session(), 0x03);
  rig.pump(5001);  // the loop runs with no request at all
  CHECK_EQ(rig.core().session(), 0x01);
}

TEST(diag_server_session_scoped_entry_returns_7f_outside_its_session) {
  Rig rig({{"22 03 18", "62 03 18 01", 0x03}, {"10 03", "50 03 00 32 01 F4"}});
  CHECK_ANSWER(rig, 0, "22 03 18", "7F 22 7F");
  CHECK_ANSWER(rig, 1, "10 03", "50 03 00 32 01 F4");
  CHECK_ANSWER(rig, 2, "22 03 18", "62 03 18 01");
}

TEST(diag_server_session_scoped_prefix_entry_returns_7f_outside_its_session) {
  Rig rig({{"22 05 *", "62 05 AA", 0x02}});
  CHECK_ANSWER(rig, 0, "22 05 01", "7F 22 7F");
  CHECK_ANSWER(rig, 1, "10 02", "50 02 00 32 01 F4");
  CHECK_ANSWER(rig, 2, "22 05 01", "62 05 AA");
}

// -------------------------------------------------------------------------------- counters

TEST(diag_server_counters_requests_replies_misses) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.ask(0, "22 F1 90");  // hit
  rig.ask(1, "22 F1 91");  // NRC 31
  rig.ask(2, "19 02");     // NRC 11
  rig.ask(3, "3E 00");     // built-in
  const auto &c = rig.core().counters();
  CHECK_EQ(c.requests, 4u);
  CHECK_EQ(c.replies, 4u);
  CHECK_EQ(c.misses, 2u);
}

// --------------------------------------------------------------------------------- delay

TEST(diag_server_delay_holds_the_reply_without_blocking) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_delay(50);
  rig.request(1000, "22 F1 90");  // returns at once
  CHECK(rig.pump(1000).empty());
  CHECK(rig.pump(1049).empty());
  const auto out = rig.pump(1050);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK_MSG(hex(out[0].bytes) == "62 F1 90 01", hex(out[0].bytes));
  CHECK(rig.pump(1100).empty());  // sent exactly once
}

TEST(diag_server_delay_applies_to_every_reply_including_nrcs) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_delay(30);
  uint32_t t = 0;
  for (const char *request : {"3E 00", "19 02", "10 03"}) {
    rig.request(t, request);
    CHECK(rig.pump(t + 29).empty());
    CHECK_EQ(rig.pump(t + 30).size(), 1u);
    t += 100;
  }
}

TEST(diag_server_delay_zero_replies_in_the_same_loop) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  CHECK_ANSWER(rig, 7, "22 F1 90", "62 F1 90 01");
}

TEST(diag_server_delay_waits_while_the_transport_is_busy) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_delay(10);
  rig.request(0, "22 F1 90");
  uint8_t pending[3];
  CHECK_EQ(rig.core().poll(20, true, pending).size, 0u);  // due, but the transport is still sending
  CHECK_EQ(rig.pump(21).size(), 1u);
}

TEST(diag_server_delay_across_clock_wraparound) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_delay(100);
  const uint32_t t0 = 0xFFFFFFC0u;
  rig.request(t0, "22 F1 90");
  CHECK(rig.pump(t0 + 99).empty());
  CHECK_EQ(rig.pump(t0 + 100).size(), 1u);
}

// ---------------------------------------------------------------------------- pending_78

TEST(diag_server_pending_78_sends_nrc_78_first_then_the_reply_after_100ms_at_delay_0) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_pending_78(true);
  const auto d = rig.request(1000, "22 F1 90");
  CHECK(d.pending);
  auto out = rig.pump(1000);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1) {
    CHECK(out[0].pending);
    CHECK_MSG(hex(out[0].bytes) == "7F 22 78", hex(out[0].bytes));
  }
  CHECK(rig.pump(1099).empty());
  out = rig.pump(1100);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1) {
    CHECK(!out[0].pending);
    CHECK_MSG(hex(out[0].bytes) == "62 F1 90 01", hex(out[0].bytes));
  }
  CHECK_EQ(rig.core().counters().misses, 0u);
  CHECK(rig.pump(1500).empty());
}

TEST(diag_server_pending_78_uses_delay_for_the_gap_when_set) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_pending_78(true);
  rig.core().set_delay(250);
  rig.request(0, "22 F1 90");
  auto out = rig.pump(0);
  CHECK_EQ(out.size(), 1u);
  CHECK(rig.pump(249).empty());
  out = rig.pump(250);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK(!out[0].pending);
}

TEST(diag_server_pending_78_names_the_requested_sid) {
  Rig rig({{"19 02 *", "59 02 FF"}});
  rig.core().set_pending_78(true);
  rig.request(0, "19 02 08");
  const auto out = rig.pump(0);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK_MSG(hex(out[0].bytes) == "7F 19 78", hex(out[0].bytes));
}

TEST(diag_server_pending_78_never_answers_a_suppressed_request) {
  Rig rig({});
  rig.core().set_pending_78(true);
  rig.request(0, "3E 80");
  CHECK(rig.pump(0).empty());
  CHECK(rig.pump(500).empty());
}

// ---------------------------------------------------------------------------------- drop_every

TEST(diag_server_drop_every_n_drops_every_nth_and_counts_misses) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_drop_every(3);
  std::string seen;
  for (uint32_t i = 1; i <= 9; i++) {
    const auto out = rig.ask(i * 10, "22 F1 90");
    seen += out.empty() ? 'D' : 'R';
  }
  CHECK_MSG(seen == "RRDRRDRRD", seen);
  CHECK_EQ(rig.core().counters().requests, 9u);
  CHECK_EQ(rig.core().counters().replies, 6u);
  CHECK_EQ(rig.core().counters().misses, 3u);
}

TEST(diag_server_drop_every_1_drops_all_and_0_none) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_drop_every(1);
  for (uint32_t i = 0; i < 4; i++)
    CHECK(rig.ask(i, "22 F1 90").empty());
  CHECK_EQ(rig.core().counters().misses, 4u);
  rig.core().set_drop_every(0);
  for (uint32_t i = 0; i < 4; i++)
    CHECK_EQ(rig.ask(10 + i, "22 F1 90").size(), 1u);
  CHECK_EQ(rig.core().counters().misses, 4u);
}

TEST(diag_server_drop_every_counts_a_default_nrc_and_a_drop_as_separate_misses) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_drop_every(2);
  CHECK_ANSWER(rig, 0, "19 02", "7F 19 11");  // 1st request: answered by NRC 11, one miss
  CHECK_ANSWER(rig, 1, "19 02", "<none>");    // 2nd request: dropped, a second miss
  CHECK_EQ(rig.core().counters().misses, 2u);
}

// ------------------------------------------------------------------------------------- busy

TEST(diag_server_busy_drops_the_request_and_counts_a_miss) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}, {"10 03", "50 03 00 32 01 F4"}});
  const auto d = rig.request(0, "22 F1 90", true);
  CHECK(d.reply_size == 0);
  CHECK(rig.pump(0).empty());
  CHECK(rig.pump(1000).empty());
  CHECK_EQ(rig.core().counters().misses, 1u);
  rig.request(1001, "10 03", true);  // a busy 10 03 must not change the session either
  CHECK(rig.pump(1001).empty());
  CHECK_EQ(rig.core().session(), 0x01);
  CHECK_ANSWER(rig, 1002, "22 F1 90", "62 F1 90 01");  // and the server is fine afterwards
}

TEST(diag_server_busy_does_not_replace_the_reply_still_held) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}, {"22 F1 91", "62 F1 91 02"}});
  rig.core().set_delay(100);
  rig.request(0, "22 F1 90");
  rig.request(10, "22 F1 91");  // arrives while the first reply is still outstanding
  CHECK(rig.pump(50).empty());
  const auto out = rig.pump(100);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK_MSG(hex(out[0].bytes) == "62 F1 90 01", hex(out[0].bytes));
  CHECK(rig.pump(300).empty());  // one held reply, nothing queued behind it
}

// -------------------------------------------------------------------------------------- off

TEST(diag_server_off_sends_nothing_at_all) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}, {"10 03", "50 03 00 32 01 F4"}}, 5000, false);
  CHECK(!rig.core().enabled());
  for (const char *request : {"22 F1 90", "22 F1 91", "19 02", "3E 00", "3E 80", "10 03", "10 05"}) {
    CHECK_ANSWER(rig, 0, request, "<none>");  // not even an NRC
    CHECK(rig.pump(1000).empty());
  }
  CHECK_EQ(rig.core().counters().replies, 0u);
  CHECK_EQ(rig.core().counters().misses, 0u);
  CHECK_EQ(rig.core().session(), 0x01);
}

TEST(diag_server_off_silences_even_with_faults_set) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}}, 5000, false);
  rig.core().set_pending_78(true);
  rig.core().set_delay(20);
  rig.request(0, "22 F1 90");
  CHECK(rig.pump(0).empty());
  CHECK(rig.pump(500).empty());
}

TEST(diag_server_switching_off_cancels_a_held_reply_and_on_resumes) {
  Rig rig({{"22 F1 90", "62 F1 90 01"}});
  rig.core().set_delay(100);
  rig.request(0, "22 F1 90");
  rig.core().set_enabled(false);
  CHECK(rig.pump(100).empty());
  CHECK(rig.pump(1000).empty());
  rig.core().set_enabled(true);
  rig.core().set_delay(0);
  CHECK_ANSWER(rig, 2000, "22 F1 90", "62 F1 90 01");
}
