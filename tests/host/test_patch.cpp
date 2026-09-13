// Double-banked runtime patch: PatchBanks (gateway_core.h) and the RulePatch
// handle that sits on top of it (can_gateway.h).
//
// RulePatch cannot be reached by including can_gateway.h on the host — that
// header pulls in ESPHome and ESP-IDF. The Makefile therefore extracts the
// RulePatch class text verbatim from can_gateway.h into
// build/rule_patch_extracted.h and the tests below compile the real code, not a
// copy. If the extraction ever stops matching, the build fails loudly rather
// than silently testing nothing.
//
// Contract under test:
//   - init() seeds both banks; the ISR-side active_patch() sees it immediately
//   - stage() seeds the inactive bank from the active one on the first call of a
//     cycle, so a partial update keeps the codegen values of untouched bytes
//   - the flip is atomic: a reader sees the complete old patch until commit(),
//     the complete new patch afterwards, never a mix
//   - RulePatch::set_byte re-applies the codegen-fixed mask on every stage
//   - RulePatch::set_can_id masks to the rule's codegen frame-type width, so a
//     lambda-computed ID can never be truncated onto the wire

#include "gateway_core.h"
#include "harness.h"
#include "rule_patch_extracted.h"

#include <cstdint>

using namespace esphome::can_gateway;

namespace {

/// Exposes RulePatch's protected codegen entry points (what GatewayRoute::
/// add_rule() calls) so a test can build a realistic handle.
struct TestPatch : public RulePatch {
  using RulePatch::banks_ptr_;
  using RulePatch::init_;
};

/// A patch shaped the way codegen emits it: byte 1 fully replaced, byte 5
/// nibble-masked, the rest identity, plus a standard-ID replacement.
PatchData codegen_shape() {
  PatchData patch{};
  patch.replace_id = true;
  patch.new_can_id = 0x111;
  patch.new_extended = false;
  patch.and_mask[1] = 0x00;  // mask 0xFF
  patch.or_value[1] = 0x11;
  patch.and_mask[5] = 0x0F;  // mask 0xF0
  patch.or_value[5] = 0x50;
  return patch;
}

}  // namespace

// ---------------------------------------------------------------------------
// PatchBanks
// ---------------------------------------------------------------------------

TEST(banks_init_publishes_to_both_banks) {
  PatchBanks banks;
  banks.init(codegen_shape());

  const PatchData &active = banks.active_patch();
  CHECK_EQ(active.new_can_id, 0x111u);
  CHECK_EQ(active.replace_id, true);
  CHECK_EQ(active.and_mask[1], 0x00);
  CHECK_EQ(active.or_value[1], 0x11);
  CHECK_EQ(active.and_mask[5], 0x0F);
  CHECK_EQ(active.or_value[5], 0x50);
  CHECK_EQ(active.and_mask[0], 0xFF);  // identity elsewhere
  CHECK_EQ(active.or_value[0], 0x00);

  // Both banks hold the initial value, so the first commit after a partial
  // stage cannot expose an uninitialised bank.
  banks.stage().or_value[1] = 0x22;
  banks.commit();
  CHECK_EQ(banks.active_patch().and_mask[5], 0x0F);
  CHECK_EQ(banks.active_patch().or_value[5], 0x50);
}

TEST(banks_commit_without_stage_is_a_no_op) {
  PatchBanks banks;
  banks.init(codegen_shape());
  const PatchData *before = &banks.active_patch();

  banks.commit();
  banks.commit();

  CHECK_EQ(&banks.active_patch(), before);  // no flip happened
  CHECK_EQ(banks.active_patch().new_can_id, 0x111u);
}

TEST(banks_partial_stage_preserves_unstaged_values) {
  PatchBanks banks;
  banks.init(codegen_shape());

  PatchData &staged = banks.stage();
  staged.or_value[1] = 0x99;  // touch one byte only
  banks.commit();

  const PatchData &active = banks.active_patch();
  CHECK_EQ(active.or_value[1], 0x99);
  // Everything the caller did not stage keeps its codegen value.
  CHECK_EQ(active.and_mask[1], 0x00);
  CHECK_EQ(active.and_mask[5], 0x0F);
  CHECK_EQ(active.or_value[5], 0x50);
  CHECK_EQ(active.new_can_id, 0x111u);
  CHECK_EQ(active.replace_id, true);
  CHECK_EQ(active.new_extended, false);
}

TEST(banks_flip_is_atomic_old_visible_until_commit) {
  PatchBanks banks;
  banks.init(codegen_shape());

  PatchData &staged = banks.stage();
  staged.or_value[1] = 0xAA;
  staged.new_can_id = 0x222;

  // Reader side (the RX ISR) still sees the complete old patch.
  CHECK_EQ(banks.active_patch().or_value[1], 0x11);
  CHECK_EQ(banks.active_patch().new_can_id, 0x111u);

  banks.commit();

  // ... and the complete new one afterwards. Both staged fields flip together.
  CHECK_EQ(banks.active_patch().or_value[1], 0xAA);
  CHECK_EQ(banks.active_patch().new_can_id, 0x222u);
}

TEST(banks_reader_never_aliases_the_staging_bank) {
  // The writer must only ever touch the inactive bank: staging must not alias
  // the storage a concurrent reader is reading from.
  PatchBanks banks;
  banks.init(codegen_shape());
  const PatchData *active_before = &banks.active_patch();
  PatchData *staging = &banks.stage();
  CHECK(staging != active_before);

  banks.commit();
  CHECK_EQ(&banks.active_patch(), staging);  // the staged bank became active

  PatchData *staging2 = &banks.stage();
  CHECK(staging2 != staging);
  CHECK_EQ(staging2, active_before);  // and the pair alternates
}

TEST(banks_stage_twice_in_one_cycle_does_not_reseed) {
  PatchBanks banks;
  banks.init(codegen_shape());

  banks.stage().or_value[1] = 0x33;
  banks.stage().or_value[5] = 0x60;  // second call must keep the first write
  banks.commit();

  CHECK_EQ(banks.active_patch().or_value[1], 0x33);
  CHECK_EQ(banks.active_patch().or_value[5], 0x60);
}

TEST(banks_successive_cycles_accumulate) {
  PatchBanks banks;
  banks.init(codegen_shape());

  banks.stage().or_value[1] = 0x01;
  banks.commit();
  banks.stage().or_value[5] = 0x02;
  banks.commit();
  banks.stage().new_can_id = 0x333;
  banks.commit();

  const PatchData &active = banks.active_patch();
  CHECK_EQ(active.or_value[1], 0x01);
  CHECK_EQ(active.or_value[5], 0x02);
  CHECK_EQ(active.new_can_id, 0x333u);
  CHECK_EQ(active.and_mask[5], 0x0F);  // still the codegen mask after 3 cycles
}

TEST(banks_uncommitted_stage_survives_into_the_next_commit) {
  // Sharp edge worth pinning: staging without commit leaves the published patch
  // alone, but the edits are NOT rolled back — the next staging cycle reuses the
  // same bank (staging_ is still set) and the abandoned write ships with the
  // next commit. Documented behaviour of the current code, not an accident of
  // this test; a change to the staging_ flag has to be deliberate.
  PatchBanks banks;
  banks.init(codegen_shape());

  banks.stage().or_value[1] = 0xBB;
  CHECK_EQ(banks.active_patch().or_value[1], 0x11);  // never published

  banks.stage().or_value[5] = 0x70;
  banks.commit();

  CHECK_EQ(banks.active_patch().or_value[5], 0x70);
  CHECK_EQ(banks.active_patch().or_value[1], 0xBB);  // the abandoned edit rides along
}

// ---------------------------------------------------------------------------
// RulePatch (extracted from can_gateway.h)
// ---------------------------------------------------------------------------

TEST(rule_patch_set_byte_reapplies_the_codegen_mask) {
  TestPatch patch;
  patch.init_(codegen_shape());

  patch.set_byte(5, 0xFF);  // byte 5 is declared with mask 0xF0
  patch.commit();
  CHECK_EQ(patch.banks_ptr_()->active_patch().or_value[5], 0xF0);
  CHECK_EQ(patch.banks_ptr_()->active_patch().and_mask[5], 0x0F);

  // The masked-out bits of the frame therefore survive the patch.
  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0, 0, 0, 0, 0, 0x0C, 0, 0};
  apply_patch(patch.banks_ptr_()->active_patch(), can_id, extended, false, data, 8);
  CHECK_EQ(data[5], 0xFC);
}

TEST(rule_patch_set_byte_on_a_fully_replaced_byte) {
  TestPatch patch;
  patch.init_(codegen_shape());

  patch.set_byte(1, 0xA5);  // byte 1 is declared with mask 0xFF
  patch.commit();
  CHECK_EQ(patch.banks_ptr_()->active_patch().or_value[1], 0xA5);
}

TEST(rule_patch_set_byte_on_an_undeclared_byte_stays_inert) {
  // Byte 0 has identity masks (and 0xFF): whatever is staged is masked to 0, so
  // an out-of-contract set_byte cannot start rewriting payload.
  TestPatch patch;
  patch.init_(codegen_shape());

  patch.set_byte(0, 0xFF);
  patch.commit();
  CHECK_EQ(patch.banks_ptr_()->active_patch().or_value[0], 0x00);
  CHECK_EQ(patch.banks_ptr_()->active_patch().and_mask[0], 0xFF);

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0x3C, 0, 0, 0, 0, 0, 0, 0};
  apply_patch(patch.banks_ptr_()->active_patch(), can_id, extended, false, data, 8);
  CHECK_EQ(data[0], 0x3C);
}

TEST(rule_patch_set_byte_out_of_range_is_ignored) {
  TestPatch patch;
  patch.init_(codegen_shape());

  patch.set_byte(MAX_FRAME_DATA_LEN, 0xFF);
  patch.set_byte(200, 0xFF);
  patch.commit();  // nothing was staged, so this must not flip anything either

  const PatchData &active = patch.banks_ptr_()->active_patch();
  CHECK_EQ(active.or_value[1], 0x11);
  CHECK_EQ(active.or_value[5], 0x50);
  CHECK_EQ(active.new_can_id, 0x111u);
}

TEST(rule_patch_set_can_id_masks_to_standard_width) {
  // The guard from the handover: a templated/lambda ID must never be able to
  // hand the hardware an ID wider than the rule's codegen frame type.
  struct Row {
    uint32_t input;
    uint32_t expected;
    const char *tag;
  };
  const Row rows[] = {
      {0x000, 0x000, "zero"},
      {0x123, 0x123, "in range"},
      {MAX_STANDARD_ID, MAX_STANDARD_ID, "max standard"},
      {0x800, 0x000, "one past max"},
      {0x1234, 0x234, "wider value"},
      {0x1FFFFFFF, MAX_STANDARD_ID, "max extended"},
      {0xFFFFFFFF, MAX_STANDARD_ID, "all ones"},
  };
  for (const Row &row : rows) {
    TestPatch patch;
    patch.init_(codegen_shape());  // new_extended == false
    patch.set_can_id(row.input);
    patch.commit();
    CHECK_EQ_MSG(patch.banks_ptr_()->active_patch().new_can_id, row.expected, row.tag);
    CHECK_EQ_MSG(patch.banks_ptr_()->active_patch().new_can_id <= MAX_STANDARD_ID, true, row.tag);
  }
}

TEST(rule_patch_set_can_id_masks_to_extended_width) {
  struct Row {
    uint32_t input;
    uint32_t expected;
    const char *tag;
  };
  const Row rows[] = {
      {0x000, 0x000, "zero"},
      {0x18DAF110, 0x18DAF110, "in range"},
      {MAX_EXTENDED_ID, MAX_EXTENDED_ID, "max extended"},
      {0x20000000, 0x00000000, "one past max"},
      {0xFFFFFFFF, MAX_EXTENDED_ID, "all ones"},
  };
  PatchData shape = codegen_shape();
  shape.new_extended = true;
  for (const Row &row : rows) {
    TestPatch patch;
    patch.init_(shape);
    patch.set_can_id(row.input);
    patch.commit();
    CHECK_EQ_MSG(patch.banks_ptr_()->active_patch().new_can_id, row.expected, row.tag);
    CHECK_EQ_MSG(patch.banks_ptr_()->active_patch().new_can_id <= MAX_EXTENDED_ID, true, row.tag);
  }
}

TEST(rule_patch_set_can_id_keeps_the_codegen_frame_type) {
  // set_can_id must not be able to flip the output frame type — only the value.
  TestPatch patch;
  patch.init_(codegen_shape());
  patch.set_can_id(0x1FFFFFFF);
  patch.commit();
  CHECK_EQ(patch.banks_ptr_()->active_patch().new_extended, false);
  CHECK_EQ(patch.banks_ptr_()->active_patch().replace_id, true);
}

TEST(rule_patch_changes_are_invisible_until_commit) {
  TestPatch patch;
  patch.init_(codegen_shape());

  patch.set_byte(1, 0x77);
  patch.set_can_id(0x2AA);
  CHECK_EQ(patch.banks_ptr_()->active_patch().or_value[1], 0x11);
  CHECK_EQ(patch.banks_ptr_()->active_patch().new_can_id, 0x111u);

  patch.commit();
  CHECK_EQ(patch.banks_ptr_()->active_patch().or_value[1], 0x77);
  CHECK_EQ(patch.banks_ptr_()->active_patch().new_can_id, 0x2AAu);
}

TEST(rule_patch_drives_a_live_rule_through_process_frame) {
  // End to end: a runtime-updatable rule sees the new patch only after commit.
  TestPatch patch;
  patch.init_(codegen_shape());

  RuleEntry rule{};
  rule.match_id = 0x100;
  rule.match_mask = 0x7FF;
  rule.flags = RULE_FLAG_HAS_PATCH;
  rule.banks = patch.banks_ptr_();

  RouteTable table{};
  table.rules = &rule;
  table.rule_count = 1;
  table.default_drop = false;

  uint32_t can_id = 0x100;
  bool extended = false;
  uint8_t data[8] = {0, 0, 0, 0, 0, 0x00, 0, 0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x111u);
  CHECK_EQ(data[1], 0x11);
  CHECK_EQ(data[5], 0x50);

  patch.set_can_id(0x2BC);
  patch.set_byte(1, 0xEE);
  can_id = 0x100;
  uint8_t data2[8] = {0, 0, 0, 0, 0, 0x00, 0, 0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data2, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x111u);  // still the old patch: nothing committed yet
  CHECK_EQ(data2[1], 0x11);

  patch.commit();
  can_id = 0x100;
  uint8_t data3[8] = {0, 0, 0, 0, 0, 0x00, 0, 0};
  CHECK_EQ(process_frame(table, can_id, extended, false, data3, 8), FrameAction::FORWARD);
  CHECK_EQ(can_id, 0x2BCu);
  CHECK_EQ(data3[1], 0xEE);
  CHECK_EQ(data3[5], 0x50);  // untouched byte keeps its codegen value
}
