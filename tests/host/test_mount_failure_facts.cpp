// CONTRACT-sdlog-mount-instruments.md §2.1: format_mount_failure() renders the facts that tell
// ESP_ERR_NO_MEM's three IDF causes apart. Deliberately no verdict — the struct is state, the
// operator concludes — so every case here is about the RENDER being exactly right: field order,
// field width, and what a signed esp_err_t actually does under a hex conversion, because a line
// that looks plausible but drifted from the format string is worse than one that visibly broke.
//
// Round 2: MountFailureFacts gains `largest_block_default` immediately after `largest_block`,
// rendered as `largest_block_default=%u B` right after the existing `largest_block=%u B` token.
// The original instrument measured heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) — a
// superset pool — while IDF's ff_memalloc() (the call that actually decides the mount) allocates
// from MALLOC_CAP_DEFAULT, a strict subset. A board can show 28,672 B free under INTERNAL and
// still fail a 20,680 B ff_memalloc() because DEFAULT alone has far less. This field is the pool
// that actually decides it.
//
// A `programmer` is adding the field to components/sd_logger/sd_diagnostics.h in parallel; until
// it lands, `f.largest_block_default` and the new render token below fail to compile. That is the
// correct interim state for this round, same as it was for the struct's first round.

#include "harness.h"
#include "sd_diagnostics.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using esphome::sd_logger::format_mount_failure;
using esphome::sd_logger::MountFailureFacts;
using esphome::sd_logger::sd_fat_fil_array_bytes;
using esphome::sd_logger::sd_fat_fil_array_is_large;
using esphome::sd_logger::SD_LOGGER_FAT_ALLOC_WARN_DENOMINATOR;
using esphome::sd_logger::SD_LOGGER_FAT_ALLOC_WARN_NUMERATOR;
using esphome::sd_logger::SD_LOGGER_FAT_MAX_FILES;
using esphome::sd_logger::SD_MOUNT_FAILURE_LINE_BYTES;
using esphome::sd_logger::SD_MOUNT_FAILURE_LINE_WORST_CASE;

namespace {

/// All fields poisoned with values distinct enough that a swap between any two same-type fields
/// (free_internal/largest_block/largest_block_default, vfs_fil_array_bytes/fatfs_volume_slots,
/// mounts_ok/unmounts_ok) changes the rendered string rather than passing by coincidence.
/// largest_block_default=12288 is deliberately its own number, not a copy of largest_block=24576
/// and not derived from it (e.g. not largest_block/2) — the two are readings of different heap
/// pools that can move independently, and a renderer that swapped, transposed, or duplicated one
/// of them into the other's slot must not still pass here. `err` gets its own unmistakable value
/// (0xF00D = 61453) so a case can prove it does NOT appear in the render (§2.1: "err is NOT in the
/// rendered line ... it is in the struct so a host test can carry it").
MountFailureFacts populated_facts() {
  MountFailureFacts f{};
  f.err = 61453;  // 0xF00D — must never appear in the rendered line
  f.free_internal = 28672;
  f.largest_block = 24576;
  f.largest_block_default = 12288;
  f.vfs_fil_array_bytes = 3092;
  f.fatfs_volume_slots = 1;
  f.last_unmount_err = 7;
  f.last_unmount_skipped = false;
  f.mounts_ok = 4;
  f.unmounts_ok = 3;
  return f;
}

}  // namespace

// ------------------------------------------------------------------------ sizing

TEST(mount_failure_facts_max_files_matches_the_documented_concurrent_openers_plus_one_spare) {
  // Pinned value, not arithmetic: one writer fd, one collection-server chunk-read fd, one spare.
  // Raising this grows every mount's single contiguous FAT VFS allocation by another sizeof(FIL);
  // lowering it risks closing collection serving while the writer holds its log file open.
  CHECK_EQ(SD_LOGGER_FAT_MAX_FILES, 3u);
}

TEST(mount_failure_facts_fil_array_bytes_pins_the_measured_bad_idf55_shape) {
  // Hardware measured sizeof(FIL)==4136 when CONFIG_WL_SECTOR_SIZE leaves FF_MAX_SS at 4096.
  // With sd_logger's max_files=3 the visible, scalable part of the VFS mount malloc is 12,408 B;
  // IDF's private vfs_fat_ctx_t fixed overhead is explicitly not included in this field.
  CHECK_EQ(sd_fat_fil_array_bytes(4136, SD_LOGGER_FAT_MAX_FILES), 12408u);
}

TEST(mount_failure_facts_fil_array_pressure_warning_accepts_the_measured_bad_ratio) {
  // 12,408 B against a 28,672 B DEFAULT block is already an allocation worth shouting about: the
  // mount asks for one contiguous block, and the private IDF ctx bytes only push the true request up.
  CHECK(sd_fat_fil_array_is_large(12408, 28672));
  CHECK_EQ(SD_LOGGER_FAT_ALLOC_WARN_NUMERATOR, 3u);
  CHECK_EQ(SD_LOGGER_FAT_ALLOC_WARN_DENOMINATOR, 1u);
}

TEST(mount_failure_facts_fil_array_pressure_warning_rejects_the_512_byte_sector_shape) {
  // A 512-byte-sector build's FIL is roughly the fixed overhead plus one small cache, not 4 KiB.
  // Pin a deliberately conservative 600 B FIL: three files need 1,800 B, not large beside 28,672 B.
  CHECK(!sd_fat_fil_array_is_large(1800, 28672));
  CHECK(!sd_fat_fil_array_is_large(12408, 0));
}

// --------------------------------------------------------------------------------- exact renders

TEST(mount_failure_facts_full_render_matches_the_documented_field_order_exactly) {
  const MountFailureFacts f = populated_facts();
  char buf[256];
  const int n = format_mount_failure(buf, sizeof(buf), f);

  static const char kExpected[] =
      "free=28672 B largest_block=24576 B largest_block_default=12288 B vfs_fil_array=3092 B fatfs_slots=1 mounts_ok=4 "
      "unmounts_ok=3 last_unmount=0x0007 unmount_skipped=no";
  CHECK_EQ(n, static_cast<int>(sizeof(kExpected) - 1));
  CHECK(std::string(buf) == kExpected);
}

TEST(mount_failure_facts_err_field_is_carried_but_never_rendered) {
  // §2.1 is explicit that `err` is struct-only (the caller prints esp_err_to_name(err) itself).
  // A renderer that "helpfully" adds it back would still pass every other case here, since none of
  // them assert *absence* — this is the one that would catch that regression.
  const MountFailureFacts f = populated_facts();
  char buf[256];
  format_mount_failure(buf, sizeof(buf), f);
  const std::string out(buf);
  CHECK(out.find("61453") == std::string::npos);
  CHECK(out.find("F00D") == std::string::npos);
  CHECK(out.find("f00d") == std::string::npos);
  CHECK(out.find("err=") == std::string::npos);
}

TEST(mount_failure_facts_all_zero_reads_never_mounted_never_unmounted) {
  // A board that has never attempted a mount at all — the state at cold boot before the first
  // mount_card_() call, not just "a mount that happened to succeed with zero counters".
  const MountFailureFacts f{};  // every field zero-/false-initialized
  char buf[256];
  const int n = format_mount_failure(buf, sizeof(buf), f);

  static const char kExpected[] =
      "free=0 B largest_block=0 B largest_block_default=0 B vfs_fil_array=0 B fatfs_slots=0 mounts_ok=0 unmounts_ok=0 "
      "last_unmount=0x0000 unmount_skipped=no";
  CHECK_EQ(n, static_cast<int>(sizeof(kExpected) - 1));
  CHECK(std::string(buf) == kExpected);
}

TEST(mount_failure_facts_unmount_skipped_yes_is_the_leak_signature) {
  // This is THE state the whole round exists to make visible: mount_card_()'s failure branch set
  // card_ = nullptr before unmount_card_() ever ran, so the FAT VFS registration was never released
  // and — with FF_VOLUMES == 1 — the next mount has nowhere to go.
  MountFailureFacts f = populated_facts();
  f.last_unmount_skipped = true;
  f.last_unmount_err = 0;  // never actually returned anything; the struct still carries 0
  f.mounts_ok = 1;
  f.unmounts_ok = 0;
  char buf[256];
  const int n = format_mount_failure(buf, sizeof(buf), f);

  static const char kExpected[] =
      "free=28672 B largest_block=24576 B largest_block_default=12288 B vfs_fil_array=3092 B fatfs_slots=1 mounts_ok=1 "
      "unmounts_ok=0 last_unmount=0x0000 unmount_skipped=yes";
  CHECK_EQ(n, static_cast<int>(sizeof(kExpected) - 1));
  CHECK(std::string(buf) == kExpected);
}

// ----------------------------------------------- the shape this round exists to make visible

TEST(mount_failure_facts_internal_pool_looks_fine_at_28672_while_default_pool_ff_memalloc_uses_is_starved_at_8192) {
  // The bench-measured incident this field was added for: heap_caps_get_largest_free_block
  // (MALLOC_CAP_INTERNAL) reported 28,672 B free — plenty, superficially — while the 20,680 B
  // ff_memalloc() call that actually needed the memory failed, because ff_memalloc() allocates
  // from MALLOC_CAP_DEFAULT, a strict subset of INTERNAL. Everything not central to that point
  // (free_internal, vfs_fil_array, fatfs_slots, the counters, last_unmount) is left at zero on purpose,
  // so a reader's eye goes straight to the two numbers that matter and their relationship: the
  // superset pool (largest_block) reads bigger than the subset pool (largest_block_default) that
  // actually gates the mount. Neither number alone tells this story; only both together do.
  MountFailureFacts f{};
  f.err = 257;  // ESP_ERR_NO_MEM
  f.largest_block = 28672;
  f.largest_block_default = 8192;
  char buf[256];
  const int n = format_mount_failure(buf, sizeof(buf), f);

  static const char kExpected[] =
      "free=0 B largest_block=28672 B largest_block_default=8192 B vfs_fil_array=0 B fatfs_slots=0 mounts_ok=0 "
      "unmounts_ok=0 last_unmount=0x0000 unmount_skipped=no";
  CHECK_EQ(n, static_cast<int>(sizeof(kExpected) - 1));
  CHECK(std::string(buf) == kExpected);
  // The exact-string check above already pins the two pool readings adjacent and in this order
  // (superset, then subset); restated here as an explicit substring check because that ordering,
  // not just the two values' presence, is the entire point of this case.
  CHECK(std::string(buf).find("largest_block=28672 B largest_block_default=8192 B") != std::string::npos);
}

// ------------------------------------------------------ the field most likely to surprise (§5)

TEST(mount_failure_facts_negative_last_unmount_err_renders_eight_hex_digits_not_the_contracts_claimed_0xFFFF) {
  // §5 of the contract itself asserts "-1 must render as 0xFFFF under %04X on a 16-bit mask". That
  // claim is wrong and this case proves it against the format string §2.1 actually pins:
  //   last_unmount_err is int32_t; snprintf's %X conversion takes the argument's own width (no
  //   promotion narrows a 32-bit int to 16 bits — there is no 16-bit mask anywhere in this format
  //   string or this struct). "%04X" sets a MINIMUM field width of 4, zero-padded; it does not cap
  //   the maximum. So -1, whose bit pattern is 0xFFFFFFFF, prints all eight hex digits.
  // Verified directly against this exact format string on the host toolchain before this file was
  // written (snprintf("...last_unmount=0x%04X...", (int32_t)-1) -> "...0xFFFFFFFF...", 8 digits).
  // If a real implementation narrows the value before formatting (e.g. `static_cast<uint16_t>(...)`)
  // it would produce 0xFFFF instead and this case would catch that too — either way, the point is
  // that nobody should have to discover which one it is from a live bench log.
  MountFailureFacts f{};
  f.last_unmount_err = -1;
  char buf[256];
  const int n = format_mount_failure(buf, sizeof(buf), f);
  const std::string out(buf);
  CHECK_EQ(n, static_cast<int>(std::strlen("free=0 B largest_block=0 B largest_block_default=0 B vfs_fil_array=0 B "
                                           "fatfs_slots=0 mounts_ok=0 unmounts_ok=0 last_unmount=0xFFFFFFFF "
                                           "unmount_skipped=no")));
  CHECK(out.find("last_unmount=0xFFFFFFFF") != std::string::npos);
  CHECK_MSG(out.find("last_unmount=0xFFFF ") == std::string::npos,
            "rendered the contract's own (incorrect) §5 prediction of a 16-bit-masked 0xFFFF");
}

TEST(mount_failure_facts_uint32_max_in_every_numeric_field_at_once) {
  MountFailureFacts f{};
  f.free_internal = std::numeric_limits<uint32_t>::max();
  f.largest_block = std::numeric_limits<uint32_t>::max();
  f.largest_block_default = std::numeric_limits<uint32_t>::max();
  f.vfs_fil_array_bytes = std::numeric_limits<uint32_t>::max();
  f.fatfs_volume_slots = std::numeric_limits<uint32_t>::max();
  f.mounts_ok = std::numeric_limits<uint32_t>::max();
  f.unmounts_ok = std::numeric_limits<uint32_t>::max();
  f.last_unmount_err = -1;
  f.last_unmount_skipped = true;
  char buf[256];
  const int n = format_mount_failure(buf, sizeof(buf), f);

  static const char kExpected[] =
      "free=4294967295 B largest_block=4294967295 B largest_block_default=4294967295 B vfs_fil_array=4294967295 B "
      "fatfs_slots=4294967295 mounts_ok=4294967295 unmounts_ok=4294967295 last_unmount=0xFFFFFFFF "
      "unmount_skipped=yes";
  CHECK_EQ(n, static_cast<int>(sizeof(kExpected) - 1));
  CHECK(std::string(buf) == kExpected);
  // Same shape as worst_case_facts() below (all seven %u fields maxed, "yes"), so it renders to the
  // same named worst case — pinned there against SD_MOUNT_FAILURE_LINE_WORST_CASE, not restated here.
  CHECK_EQ(n, static_cast<int>(SD_MOUNT_FAILURE_LINE_WORST_CASE));
}

// ---------------------------------------------------- the buffer cases nobody wants (§5, §4)
//
// True worst-case length: every %u field at UINT32_MAX (10 digits each, now SEVEN of them —
// largest_block_default added a seventh), the hex field at its widest — a negative int32_t under
// %X is 8 digits, since %04X sets a MINIMUM width and does not truncate — and unmount_skipped at
// its longer spelling ("yes", 3 bytes, vs "no", 2).
//
// HISTORY: before largest_block_default existed, the worst case was 176 B against the contract's
// 192-byte `char line[192]` (§2.3(b)), 15 bytes to spare. Adding the field grew the worst case past
// 192 without anyone touching that buffer, and a `TEST_KNOWN_DEFECT` case here reproduced the
// resulting truncation on every run. The fix (this repo, not this file): sd_diagnostics.h now
// declares SD_MOUNT_FAILURE_LINE_WORST_CASE and SD_MOUNT_FAILURE_LINE_BYTES beside
// format_mount_failure() itself, ties them with a static_assert, and sd_logger.cpp's call site
// reads `char line[SD_MOUNT_FAILURE_LINE_BYTES]` instead of a bare literal. That closed the
// specific hole; it did not close the general one — nothing stops an EIGHTH field from repeating
// this unless something re-derives the worst case from the real formatter and checks it against
// the constant every run. That is exactly what the coupling case below does, and it is the reason
// no other case in this file restates the worst-case length as a bare literal: every one of them
// reads SD_MOUNT_FAILURE_LINE_WORST_CASE (or a fixed offset from it) instead.

namespace {
MountFailureFacts worst_case_facts() {
  MountFailureFacts f{};
  f.free_internal = std::numeric_limits<uint32_t>::max();
  f.largest_block = std::numeric_limits<uint32_t>::max();
  f.largest_block_default = std::numeric_limits<uint32_t>::max();
  f.vfs_fil_array_bytes = std::numeric_limits<uint32_t>::max();
  f.fatfs_volume_slots = std::numeric_limits<uint32_t>::max();
  f.mounts_ok = std::numeric_limits<uint32_t>::max();
  f.unmounts_ok = std::numeric_limits<uint32_t>::max();
  f.last_unmount_err = std::numeric_limits<int32_t>::min();  // 0x80000000 — same 8-hex-digit width as -1
  f.last_unmount_skipped = true;
  return f;
}
}  // namespace

// The coupling that is meant to prevent an eighth-field repeat of this. `buf` is sized independently
// of BOTH named constants (a plain oversized scratch buffer) specifically so this case measures the
// formatter itself, not whatever the current buffer policy happens to be — that measurement is then
// checked against SD_MOUNT_FAILURE_LINE_WORST_CASE (add a field and forget to update that constant,
// or update it to the wrong number, and this line fails immediately, naming the real new length
// instead of truncating silently in a caller two files away), and SD_MOUNT_FAILURE_LINE_BYTES is
// checked to still exceed it (shrink the shipped buffer, or grow the worst case past it again some
// other way, and this catches that too). Every number that matters here is read from
// sd_diagnostics.h, not restated — the one place this file compares a MEASURED length to the named
// constant is the CHECK_EQ two lines below; nowhere else in this file writes the current worst-case
// length as a bare digit.
TEST(mount_failure_facts_worst_case_length_is_coupled_to_the_named_constant_not_restated_as_a_literal) {
  const MountFailureFacts f = worst_case_facts();
  char buf[1024];  // deliberately independent of SD_MOUNT_FAILURE_LINE_BYTES: this measures the
                   // formatter, not the buffer policy
  const int n = format_mount_failure(buf, sizeof(buf), f);
  CHECK_EQ(n, static_cast<int>(SD_MOUNT_FAILURE_LINE_WORST_CASE));
  CHECK_EQ(std::strlen(buf), SD_MOUNT_FAILURE_LINE_WORST_CASE);
  CHECK_MSG(SD_MOUNT_FAILURE_LINE_BYTES > SD_MOUNT_FAILURE_LINE_WORST_CASE,
            "SD_MOUNT_FAILURE_LINE_BYTES no longer exceeds SD_MOUNT_FAILURE_LINE_WORST_CASE — the shipped "
            "call-site buffer in sd_logger.cpp cannot hold the true worst-case render plus its NUL");
}

TEST(mount_failure_facts_buffer_exactly_the_required_capacity_does_not_truncate) {
  constexpr size_t kRequiredCapacity = SD_MOUNT_FAILURE_LINE_WORST_CASE + 1;  // content bytes + NUL
  const MountFailureFacts f = worst_case_facts();
  char buf[kRequiredCapacity];
  const int n = format_mount_failure(buf, sizeof(buf), f);
  CHECK_EQ(n, static_cast<int>(SD_MOUNT_FAILURE_LINE_WORST_CASE));
  CHECK_EQ(static_cast<size_t>(n), sizeof(buf) - 1);  // exact fit, zero bytes to spare
  CHECK_EQ(std::strlen(buf), SD_MOUNT_FAILURE_LINE_WORST_CASE);
  CHECK(buf[SD_MOUNT_FAILURE_LINE_WORST_CASE - 1] == 's');  // the trailing 's' of "...skipped=yes"
}

TEST(mount_failure_facts_buffer_one_byte_short_truncates_mid_word_not_the_terminator) {
  // Nobody wants this one; it is the one the contract asks for anyway (§5, §4 "What a failing case
  // looks like"). snprintf's return is defined as the length it WOULD have written, uncapped by
  // `len` — a caller can only detect truncation via `n >= sizeof(buf)`, never from `n` alone. The cut
  // itself lands one byte before the end of "...unmount_skipped=yes", dropping only the final 's' —
  // a line that reads "unmount_skipped=ye" is a more dangerous failure than an obviously broken one:
  // it still looks like a well-formed word to anyone skimming a bench log.
  constexpr size_t kOneByteShort = SD_MOUNT_FAILURE_LINE_WORST_CASE;  // required capacity - 1
  const MountFailureFacts f = worst_case_facts();
  char buf[kOneByteShort];
  const int n = format_mount_failure(buf, sizeof(buf), f);
  CHECK_EQ(n, static_cast<int>(SD_MOUNT_FAILURE_LINE_WORST_CASE));  // untruncated length, still reported
  CHECK(static_cast<size_t>(n) >= sizeof(buf));                     // ...which is how a caller detects this
  CHECK_EQ(std::strlen(buf), sizeof(buf) - 1);                      // one byte short of the render, then NUL
  CHECK(buf[sizeof(buf) - 2] != 's');                               // the final 's' of "yes" is exactly what got cut

  static const char kExpectedPrefix[] =
      "free=4294967295 B largest_block=4294967295 B largest_block_default=4294967295 B vfs_fil_array=4294967295 B "
      "fatfs_slots=4294967295 mounts_ok=4294967295 unmounts_ok=4294967295 last_unmount=0x80000000 "
      "unmount_skipped=ye";
  CHECK_EQ(sizeof(kExpectedPrefix) - 1, sizeof(buf) - 1);
  CHECK(std::memcmp(buf, kExpectedPrefix, sizeof(buf) - 1) == 0);
}

TEST(mount_failure_facts_len_zero_with_non_null_out_writes_nothing_at_all) {
  // C's snprintf contract: when len == 0, nothing is written — not even a NUL — regardless of
  // whether `out` is null. A caller that probes the required length with a zero-length buffer (the
  // common "call twice" idiom) must get that length back without a single byte of `out` touched.
  const MountFailureFacts f = worst_case_facts();
  char buf[8];
  std::memset(buf, '\xEE', sizeof(buf));  // poison, so any write at all is observable
  const int n = format_mount_failure(buf, 0, f);
  CHECK_EQ(n, static_cast<int>(SD_MOUNT_FAILURE_LINE_WORST_CASE));  // length it would have written
  for (char c : buf)
    CHECK_EQ(static_cast<unsigned char>(c), 0xEEu);
}
