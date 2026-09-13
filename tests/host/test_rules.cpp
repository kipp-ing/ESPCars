// Rule engine: match_rule / process_frame / apply_patch.
//
// This is the code that decides what the gateway puts on a vehicle bus. The HIL
// bench cannot cover it (both bench routes are filter-less accept-all), so every
// assertion here is against the documented contract in gateway_core.h:
//   - first match in table order wins (gateway_core.h "First matching rule ...")
//   - standard and extended ID spaces are strictly separate
//   - match is (can_id & match_mask) == match_id
//   - RTR is only compared when RULE_FLAG_CHECK_RTR is set
//   - no match -> DROP_FILTERED iff RouteTable::default_drop
//   - a patch touches payload bytes only below DLC, never the DLC itself, and
//     never the payload of an RTR frame

#include "gateway_core.h"
#include "harness.h"

#include <cstdint>

using namespace esphome::can_gateway;

namespace {

RuleEntry make_rule(uint32_t match_id, uint32_t match_mask, uint8_t flags) {
  RuleEntry rule{};
  rule.match_id = match_id;
  rule.match_mask = match_mask;
  rule.flags = flags;
  return rule;
}

RouteTable make_table(const RuleEntry *rules, uint8_t count, bool default_drop) {
  RouteTable table{};
  table.rules = rules;
  table.rule_count = count;
  table.default_drop = default_drop;
  return table;
}

/// PatchData that only replaces the ID (identity payload masks).
PatchData id_patch(uint32_t new_id, bool new_extended) {
  PatchData patch{};
  patch.replace_id = true;
  patch.new_can_id = new_id;
  patch.new_extended = new_extended;
  return patch;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. match_rule / process_frame
// ---------------------------------------------------------------------------

TEST(rules_first_match_wins_accept_before_drop) {
  // Two rules both match 0x100. The first one accepts, so the frame forwards
  // even though a later rule would drop it.
  RuleEntry rules[2] = {
      make_rule(0x100, 0x7FF, 0),
      make_rule(0x100, 0x7FF, RULE_FLAG_DROP),
  };
  RouteTable table = make_table(rules, 2, false);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(match_rule(table, 0x100, false, false), &rules[0]);
}

TEST(rules_first_match_wins_drop_before_accept) {
  // Same pair in the other order: the drop rule is first, so the frame is
  // filtered. Order, not specificity, decides.
  RuleEntry rules[2] = {
      make_rule(0x100, 0x7FF, RULE_FLAG_DROP),
      make_rule(0x100, 0x7FF, 0),
  };
  RouteTable table = make_table(rules, 2, false);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::DROP_FILTERED);
  CHECK_EQ(match_rule(table, 0x100, false, false), &rules[0]);
}

TEST(rules_first_match_wins_among_patches) {
  // A broad catch-all rule placed first shadows a later, more specific patch:
  // the ID rewrite that runs is the first rule's, not the best-fitting one.
  RuleEntry rules[2] = {
      make_rule(0x000, 0x000, RULE_FLAG_HAS_PATCH),  // mask 0 -> matches every standard frame
      make_rule(0x100, 0x7FF, RULE_FLAG_HAS_PATCH),
  };
  rules[0].static_patch = id_patch(0x200, false);
  rules[1].static_patch = id_patch(0x300, false);
  RouteTable table = make_table(rules, 2, false);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x200u);
}

TEST(rules_standard_rule_never_matches_extended_frame) {
  // Same numeric ID, different frame type: must not match in either direction.
  RuleEntry std_rule = make_rule(0x123, 0x7FF, 0);
  RouteTable std_table = make_table(&std_rule, 1, true);

  CHECK_EQ(match_rule(std_table, 0x123, /*extended=*/false, false), &std_rule);
  CHECK_EQ(match_rule(std_table, 0x123, /*extended=*/true, false), static_cast<const RuleEntry *>(nullptr));

  // default_drop makes the miss observable through process_frame too.
  uint32_t can_id = 0x123;
  bool extended = true;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(std_table, can_id, extended, false, data, 8), FrameAction::DROP_FILTERED);
}

TEST(rules_extended_rule_never_matches_standard_frame) {
  RuleEntry ext_rule = make_rule(0x123, 0x1FFFFFFF, RULE_FLAG_EXTENDED);
  RouteTable ext_table = make_table(&ext_rule, 1, true);

  CHECK_EQ(match_rule(ext_table, 0x123, /*extended=*/true, false), &ext_rule);
  CHECK_EQ(match_rule(ext_table, 0x123, /*extended=*/false, false), static_cast<const RuleEntry *>(nullptr));

  uint32_t can_id = 0x123;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(ext_table, can_id, extended, false, data, 8), FrameAction::DROP_FILTERED);
}

TEST(rules_std_and_ext_rules_coexist_for_one_numeric_id) {
  // Both spaces in one table: each frame type picks its own rule.
  RuleEntry rules[2] = {
      make_rule(0x123, 0x7FF, RULE_FLAG_HAS_PATCH),
      make_rule(0x123, 0x1FFFFFFF, RULE_FLAG_EXTENDED | RULE_FLAG_HAS_PATCH),
  };
  rules[0].static_patch = id_patch(0x0AA, false);
  rules[1].static_patch = id_patch(0x0BB, true);
  RouteTable table = make_table(rules, 2, true);

  uint32_t can_id = 0x123;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x0AAu);
  CHECK_EQ(extended, false);

  can_id = 0x123;
  extended = true;
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x0BBu);
  CHECK_EQ(extended, true);
}

TEST(rules_mask_semantics) {
  // Contract: a rule matches when (can_id & match_mask) == match_id.
  struct Row {
    uint32_t match_id;
    uint32_t match_mask;
    uint32_t probe;
    bool expect_match;
    const char *tag;
  };
  const Row rows[] = {
      {0x100, 0x7FF, 0x100, true, "exact hit"},
      {0x100, 0x7FF, 0x101, false, "exact miss"},
      {0x100, 0x700, 0x100, true, "range low"},
      {0x100, 0x700, 0x1FF, true, "range high"},
      {0x100, 0x700, 0x0FF, false, "below range"},
      {0x100, 0x700, 0x200, false, "above range"},
      {0x000, 0x000, 0x7FF, true, "mask 0 is catch-all"},
      {0x000, 0x000, 0x000, true, "mask 0 matches zero id"},
      // A match_id with bits outside match_mask can never match: those bits are
      // masked out of can_id before the compare.
      {0x100, 0x0FF, 0x100, false, "match_id bit outside mask never matches"},
      {0x001, 0x001, 0x7FF, true, "odd ids"},
      {0x001, 0x001, 0x7FE, false, "even ids"},
      {0x7FF, 0x7FF, 0x7FF, true, "max standard id"},
  };
  for (const Row &row : rows) {
    RuleEntry rule = make_rule(row.match_id, row.match_mask, 0);
    RouteTable table = make_table(&rule, 1, true);
    const RuleEntry *hit = match_rule(table, row.probe, false, false);
    CHECK_EQ_MSG(hit != nullptr, row.expect_match, row.tag);
  }
}

TEST(rules_mask_semantics_extended_ids) {
  RuleEntry rule = make_rule(0x18DA0000, 0x1FFF0000, RULE_FLAG_EXTENDED);
  RouteTable table = make_table(&rule, 1, true);
  CHECK(match_rule(table, 0x18DAF110, true, false) != nullptr);
  CHECK(match_rule(table, 0x18DA0000, true, false) != nullptr);
  CHECK(match_rule(table, 0x18DBF110, true, false) == nullptr);
  CHECK(match_rule(table, MAX_EXTENDED_ID, true, false) == nullptr);
}

TEST(rules_rtr_ignored_without_check_flag) {
  // Without RULE_FLAG_CHECK_RTR the rule matches data and remote frames alike.
  RuleEntry rule = make_rule(0x200, 0x7FF, 0);
  RouteTable table = make_table(&rule, 1, true);
  CHECK(match_rule(table, 0x200, false, /*rtr=*/false) == &rule);
  CHECK(match_rule(table, 0x200, false, /*rtr=*/true) == &rule);
}

TEST(rules_rtr_checked_requires_true) {
  // CHECK_RTR | RTR_VALUE: only remote frames match.
  RuleEntry rule = make_rule(0x200, 0x7FF, RULE_FLAG_CHECK_RTR | RULE_FLAG_RTR_VALUE);
  RouteTable table = make_table(&rule, 1, true);
  CHECK(match_rule(table, 0x200, false, /*rtr=*/true) == &rule);
  CHECK(match_rule(table, 0x200, false, /*rtr=*/false) == nullptr);
}

TEST(rules_rtr_checked_requires_false) {
  // CHECK_RTR without RTR_VALUE: only data frames match.
  RuleEntry rule = make_rule(0x200, 0x7FF, RULE_FLAG_CHECK_RTR);
  RouteTable table = make_table(&rule, 1, true);
  CHECK(match_rule(table, 0x200, false, /*rtr=*/false) == &rule);
  CHECK(match_rule(table, 0x200, false, /*rtr=*/true) == nullptr);
}

TEST(rules_rtr_value_without_check_flag_is_inert) {
  // RTR_VALUE alone must not gate anything (only CHECK_RTR enables the compare).
  RuleEntry rule = make_rule(0x200, 0x7FF, RULE_FLAG_RTR_VALUE);
  RouteTable table = make_table(&rule, 1, true);
  CHECK(match_rule(table, 0x200, false, false) == &rule);
  CHECK(match_rule(table, 0x200, false, true) == &rule);
}

TEST(rules_rtr_split_rules_pick_the_right_one) {
  RuleEntry rules[2] = {
      make_rule(0x300, 0x7FF, RULE_FLAG_CHECK_RTR | RULE_FLAG_RTR_VALUE | RULE_FLAG_DROP),
      make_rule(0x300, 0x7FF, 0),
  };
  RouteTable table = make_table(rules, 2, false);

  uint32_t can_id = 0x300;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(table, can_id, extended, /*rtr=*/true, data, 8), FrameAction::DROP_FILTERED);
  CHECK_EQ(process_frame(table, can_id, extended, /*rtr=*/false, data, 8), FrameAction::FORWARD);
}

TEST(rules_no_match_honours_default_action) {
  RuleEntry rule = make_rule(0x100, 0x7FF, 0);

  RouteTable accept_table = make_table(&rule, 1, /*default_drop=*/false);
  RouteTable drop_table = make_table(&rule, 1, /*default_drop=*/true);

  uint32_t can_id = 0x555;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(accept_table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(process_frame(drop_table, can_id, extended, false, data, 8), FrameAction::DROP_FILTERED);
  CHECK_EQ(can_id, 0x555u);  // an unmatched frame is never rewritten
}

TEST(rules_empty_table_honours_default_action) {
  // rule_count 0 with a null rules pointer must not be dereferenced.
  RouteTable accept_table = make_table(nullptr, 0, false);
  RouteTable drop_table = make_table(nullptr, 0, true);

  uint32_t can_id = 0x123;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(match_rule(accept_table, 0x123, false, false), static_cast<const RuleEntry *>(nullptr));
  CHECK_EQ(process_frame(accept_table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(process_frame(drop_table, can_id, extended, false, data, 8), FrameAction::DROP_FILTERED);
}

TEST(rules_drop_rule_skips_its_patch) {
  // A rule flagged DROP must filter before any payload rewrite happens.
  RuleEntry rule = make_rule(0x100, 0x7FF, RULE_FLAG_DROP | RULE_FLAG_HAS_PATCH);
  rule.static_patch = id_patch(0x7AA, false);
  rule.static_patch.and_mask[0] = 0x00;
  rule.static_patch.or_value[0] = 0xEE;
  RouteTable table = make_table(&rule, 1, false);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  const uint8_t before[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::DROP_FILTERED);
  CHECK_EQ(can_id, 0x100u);
  CHECK_BYTES(data, before, 8);
}

TEST(rules_accept_without_patch_flag_leaves_frame_alone) {
  // static_patch is populated but HAS_PATCH is clear: the patch must not run.
  RuleEntry rule = make_rule(0x100, 0x7FF, 0);
  rule.static_patch = id_patch(0x7AA, true);
  RouteTable table = make_table(&rule, 1, false);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0xDE, 0xAD, 0, 0, 0, 0, 0, 0};
  const uint8_t before[8] = {0xDE, 0xAD, 0, 0, 0, 0, 0, 0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x100u);
  CHECK_EQ(extended, false);
  CHECK_BYTES(data, before, 8);
}

TEST(rules_effective_patch_prefers_banks_over_static) {
  // RuleEntry::effective_patch(): the published bank wins for runtime-updatable
  // rules; static_patch is only used when banks == nullptr.
  PatchBanks banks;
  banks.init(id_patch(0x0C0, false));

  RuleEntry rule = make_rule(0x100, 0x7FF, RULE_FLAG_HAS_PATCH);
  rule.static_patch = id_patch(0x0FF, false);
  CHECK_EQ(rule.effective_patch().new_can_id, 0x0FFu);

  rule.banks = &banks;
  CHECK_EQ(rule.effective_patch().new_can_id, 0x0C0u);

  RouteTable table = make_table(&rule, 1, false);
  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x0C0u);
}

TEST(rules_scan_visits_every_entry_in_order) {
  // Guards the uint8_t loop counter in match_rule against an off-by-one at the
  // end of a large table: only the last rule matches.
  static RuleEntry rules[200];
  for (uint8_t i = 0; i < 200; i++)
    rules[i] = make_rule(0x400 + i, 0x7FF, 0);
  RouteTable table = make_table(rules, 200, true);

  for (uint8_t i = 0; i < 200; i++)
    CHECK_EQ(match_rule(table, 0x400u + i, false, false), &rules[i]);
  CHECK_EQ(match_rule(table, 0x400u + 200, false, false), static_cast<const RuleEntry *>(nullptr));
}

// ---------------------------------------------------------------------------
// 2. apply_patch
// ---------------------------------------------------------------------------

TEST(patch_and_or_applied_below_dlc_only) {
  PatchData patch{};
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    patch.and_mask[i] = 0x0F;  // keep the low nibble
    patch.or_value[i] = 0xA0;  // force the high nibble
  }

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  apply_patch(patch, can_id, extended, /*rtr=*/false, data, /*dlc=*/4);

  const uint8_t expected[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0x55, 0x66, 0x77, 0x88};
  CHECK_BYTES(data, expected, 8);
  CHECK_EQ(can_id, 0x100u);  // replace_id clear -> ID untouched
  CHECK_EQ(extended, false);
}

TEST(patch_dlc_zero_touches_nothing) {
  PatchData patch{};
  patch.and_mask[0] = 0x00;
  patch.or_value[0] = 0xFF;

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  const uint8_t before[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  apply_patch(patch, can_id, extended, false, data, /*dlc=*/0);
  CHECK_BYTES(data, before, 8);
}

TEST(patch_full_dlc_patches_all_eight_bytes) {
  PatchData patch{};
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    patch.and_mask[i] = 0x00;
    patch.or_value[i] = static_cast<uint8_t>(0xF0 + i);
  }

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0};
  apply_patch(patch, can_id, extended, false, data, /*dlc=*/8);

  const uint8_t expected[8] = {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7};
  CHECK_BYTES(data, expected, 8);
}

TEST(patch_dlc_above_eight_is_clamped) {
  // Defensive: a bogus DLC from the driver must not walk past the payload
  // buffer. The heap allocation makes an overrun an ASan error, not luck.
  PatchData patch{};
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    patch.and_mask[i] = 0x00;
    patch.or_value[i] = 0x5A;
  }

  uint8_t *data = new uint8_t[MAX_FRAME_DATA_LEN];
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++)
    data[i] = 0;

  uint32_t can_id = 0x100;
  bool extended = false;
  apply_patch(patch, can_id, extended, false, data, /*dlc=*/15);

  const uint8_t expected[8] = {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
  CHECK_BYTES(data, expected, MAX_FRAME_DATA_LEN);
  delete[] data;
}

TEST(patch_identity_masks_leave_payload_unchanged) {
  // The default PatchData (and 0xFF / or 0x00) is the identity transform, which
  // is what a rule with only an ID replacement compiles to.
  PatchData patch = id_patch(0x1ABCDEF, true);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  const uint8_t before[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  apply_patch(patch, can_id, extended, false, data, 8);

  CHECK_BYTES(data, before, 8);
  CHECK_EQ(can_id, 0x1ABCDEFu);
  CHECK_EQ(extended, true);
}

TEST(patch_replace_id_switches_frame_type_both_ways) {
  uint8_t data[8] = {0};

  uint32_t can_id = 0x100;
  bool extended = false;
  apply_patch(id_patch(0x1FFFFFFF, true), can_id, extended, false, data, 8);
  CHECK_EQ(can_id, MAX_EXTENDED_ID);
  CHECK_EQ(extended, true);

  can_id = 0x1FFFFFFF;
  extended = true;
  apply_patch(id_patch(0x7FF, false), can_id, extended, false, data, 8);
  CHECK_EQ(can_id, MAX_STANDARD_ID);
  CHECK_EQ(extended, false);
}

TEST(patch_rtr_frame_gets_new_id_but_keeps_payload) {
  // Contract: "an RTR frame carries no data bytes to patch". The ID replacement
  // still applies; the buffer must be left exactly as received.
  PatchData patch = id_patch(0x2AB, false);
  for (uint8_t i = 0; i < MAX_FRAME_DATA_LEN; i++) {
    patch.and_mask[i] = 0x00;
    patch.or_value[i] = 0xFF;
  }

  uint32_t can_id = 0x100;
  bool extended = true;
  uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  const uint8_t before[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  apply_patch(patch, can_id, extended, /*rtr=*/true, data, /*dlc=*/8);

  CHECK_EQ(can_id, 0x2ABu);
  CHECK_EQ(extended, false);
  CHECK_BYTES(data, before, 8);
}

TEST(patch_rtr_frame_without_id_replacement_is_a_no_op) {
  PatchData patch{};
  patch.and_mask[0] = 0x00;
  patch.or_value[0] = 0xFF;

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0x11, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t before[8] = {0x11, 0, 0, 0, 0, 0, 0, 0};
  apply_patch(patch, can_id, extended, true, data, 8);

  CHECK_EQ(can_id, 0x100u);
  CHECK_EQ(extended, false);
  CHECK_BYTES(data, before, 8);
}

TEST(patch_or_value_wins_over_masked_bits) {
  // The YAML {index, value, mask} contract compiles to and = ~mask,
  // or = value & mask, so masked bits are replaced and the rest survive.
  PatchData patch{};
  patch.and_mask[0] = static_cast<uint8_t>(~0xF0);  // mask 0xF0
  patch.or_value[0] = 0xA0 & 0xF0;                  // value 0xA5 -> 0xA0

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0x5C, 0, 0, 0, 0, 0, 0, 0};
  apply_patch(patch, can_id, extended, false, data, 1);
  CHECK_EQ(data[0], 0xAC);  // high nibble replaced, low nibble preserved
}

TEST(patch_process_frame_leaves_dlc_out_of_scope) {
  // DLC is a by-value parameter of process_frame/apply_patch: the rule engine
  // has no way to resize a frame. This pins that property so a future signature
  // change (dlc by reference) has to be a deliberate one.
  PatchData patch{};
  patch.and_mask[3] = 0x00;
  patch.or_value[3] = 0x99;

  RuleEntry rule = make_rule(0x100, 0x7FF, RULE_FLAG_HAS_PATCH);
  rule.static_patch = patch;
  RouteTable table = make_table(&rule, 1, false);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t dlc = 8;
  uint8_t data[8] = {0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, dlc), FrameAction::FORWARD);
  CHECK_EQ(dlc, 8);
  CHECK_EQ(data[3], 0x99);
}
