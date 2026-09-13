// Microbenchmarks for the per-frame hot paths of gateway_core.h and sd_logger/log_format.h.
//
// The bench answers "how much does the board cost per frame" only in *shape*. It is not a device
// measurement and must never be quoted as one: the host is a different ISA, a different cache and
// a different clock, and the C6 runs this code out of IRAM against a 160 MHz single core. What
// does transfer is the part the bench is for — the ratio between two implementations of the same
// function, and the algorithmic shape (constant vs linear in the table size). A change that halves
// ns/op here is worth flashing; a change that moves it 5 % is noise and is not.
//
// The device numbers stay where they already are: total CPU from the idle-task share
// (tests/hil/cpu_stats.yaml), per-task attribution (tests/hil/task_stats.yaml) and per-component
// loop attribution (tests/hil/runtime_stats.yaml). Measure there, iterate here, confirm there.
//
//   make -C tests/host bench                 build and run every case
//   make -C tests/host bench FILTER=id_      run only cases whose name contains id_
//
// Built at -O2 with the sanitizers OFF — ASan/UBSan instrumentation lands on exactly the
// atomics and bounds checks these loops are made of, and would time the instrumentation. The
// correctness gate stays `make -C tests/host`, which keeps both sanitizers.

#include "gateway_core.h"
#include "log_format.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::can_gateway;
namespace sdl = esphome::sd_logger;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

namespace bench {

/// A case runs `reps` iterations of one operation and returns a checksum. The checksum exists to
/// be consumed by main(): a case whose result is unused folds to nothing at -O2 and reports 0 ns.
using Fn = uint64_t (*)(size_t reps);

struct Case {
  const char *name;
  const char *note;  ///< what the fixture actually is, printed next to the number
  Fn fn;
};

inline std::vector<Case> &registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char *name, const char *note, Fn fn) { registry().push_back(Case{name, note, fn}); }
};

/// Keep a value observably alive without generating code for it. Needed where the operation's
/// result is a mutated buffer rather than a returned scalar (apply_patch, the line writers).
template<typename T> inline void sink(const T &value) { asm volatile("" : : "m"(value) : "memory"); }

}  // namespace bench

/// Cases self-register at static-init time, exactly like TEST() in harness.h.
#define BENCH(NAME, NOTE) \
  static uint64_t bench_##NAME(size_t reps); \
  static ::bench::Registrar bench_reg_##NAME(#NAME, NOTE, bench_##NAME); \
  static uint64_t bench_##NAME(size_t reps)

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

namespace {

/// A route table shaped like a real config: eight accept rules on distinct 11-bit IDs, first
/// match wins. Eight is the interesting size — it is what the bench rigs run, and match_rule is
/// linear, so it is also where the linear cost starts to show.
constexpr uint8_t RULE_COUNT = 8;
constexpr uint32_t RULE_BASE_ID = 0x100;

struct RuleFixture {
  RuleEntry rules[RULE_COUNT]{};
  RouteTable table{};

  explicit RuleFixture(bool with_patch) {
    for (uint8_t i = 0; i < RULE_COUNT; i++) {
      this->rules[i].match_id = RULE_BASE_ID + i;
      this->rules[i].match_mask = 0x7FF;
      this->rules[i].flags = 0;
      if (with_patch) {
        this->rules[i].flags |= RULE_FLAG_HAS_PATCH;
        // A patch that touches every byte: and_mask defaults to identity, so clear the low nibble
        // and OR a constant back in. Worst case for apply_patch, which is branch-free per byte.
        for (uint8_t b = 0; b < MAX_FRAME_DATA_LEN; b++) {
          this->rules[i].static_patch.and_mask[b] = 0xF0;
          this->rules[i].static_patch.or_value[b] = 0x0A;
        }
      }
    }
    this->table.rules = this->rules;
    this->table.rule_count = RULE_COUNT;
    this->table.default_drop = false;
  }
};

/// How many distinct IDs the observation structures hold. 64 is the production shape for the
/// datalogger configs and, for IdTimingTable, the point where the linear scan is worth looking at.
constexpr uint16_t OBSERVED_IDS = 64;

uint8_t sample_payload(uint8_t index) { return static_cast<uint8_t>(0x11 * (index + 1)); }

}  // namespace

// ---------------------------------------------------------------------------
// Rule engine — the per-frame decision every forwarded frame pays
// ---------------------------------------------------------------------------

BENCH(rule_match_hit_first, "8-rule table, frame matches rule 0") {
  static const RuleFixture fx(false);
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    const RuleEntry *rule = match_rule(fx.table, RULE_BASE_ID, false, false);
    sum += rule != nullptr ? 1 : 0;
  }
  return sum;
}

BENCH(rule_match_hit_last, "8-rule table, frame matches rule 7 (full scan)") {
  static const RuleFixture fx(false);
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    const RuleEntry *rule = match_rule(fx.table, RULE_BASE_ID + RULE_COUNT - 1, false, false);
    sum += rule != nullptr ? 1 : 0;
  }
  return sum;
}

BENCH(rule_match_miss, "8-rule table, no rule matches (full scan, default accept)") {
  static const RuleFixture fx(false);
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    const RuleEntry *rule = match_rule(fx.table, 0x7A0, false, false);
    sum += rule == nullptr ? 1 : 0;
  }
  return sum;
}

BENCH(process_frame_forward, "match + forward decision, no patch") {
  static const RuleFixture fx(false);
  uint8_t data[MAX_FRAME_DATA_LEN];
  for (uint8_t b = 0; b < MAX_FRAME_DATA_LEN; b++)
    data[b] = sample_payload(b);
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    uint32_t can_id = RULE_BASE_ID + (i % RULE_COUNT);
    bool extended = false;
    sum += process_frame(fx.table, can_id, extended, false, data, MAX_FRAME_DATA_LEN) == FrameAction::FORWARD ? 1 : 0;
    bench::sink(data);
  }
  return sum;
}

BENCH(process_frame_patch, "match + apply an 8-byte AND/OR patch") {
  static const RuleFixture fx(true);
  uint8_t data[MAX_FRAME_DATA_LEN];
  for (uint8_t b = 0; b < MAX_FRAME_DATA_LEN; b++)
    data[b] = sample_payload(b);
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    uint32_t can_id = RULE_BASE_ID + (i % RULE_COUNT);
    bool extended = false;
    sum += process_frame(fx.table, can_id, extended, false, data, MAX_FRAME_DATA_LEN) == FrameAction::FORWARD ? 1 : 0;
    bench::sink(data);
  }
  return sum;
}

// ---------------------------------------------------------------------------
// TX slot lifecycle
// ---------------------------------------------------------------------------

BENCH(slot_pool_acquire_release, "8 slots, pool empty — first slot is free") {
  static SlotPool<8> pool;
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    uint8_t slot = pool.acquire();
    sum += slot;
    pool.release(slot);
  }
  return sum;
}

BENCH(slot_pool_acquire_deep, "8 slots, 7 in flight — acquire scans to the last") {
  static SlotPool<8> pool;
  static bool primed = [] {
    for (uint8_t i = 0; i < 7; i++)
      (void) pool.acquire();
    return true;
  }();
  (void) primed;
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    uint8_t slot = pool.acquire();
    sum += slot;
    pool.release(slot);
  }
  return sum;
}

// ---------------------------------------------------------------------------
// Observation rings — what the datalogger tap adds to every received frame
// ---------------------------------------------------------------------------

BENCH(observe_ring_push_pop, "TapRecord through a 256-deep ring, one push + one pop") {
  static ObserveRing<256, TapRecord> ring;
  TapRecord record{};
  record.can_id = 0x123;
  record.dlc = 8;
  for (uint8_t b = 0; b < MAX_FRAME_DATA_LEN; b++)
    record.data[b] = sample_payload(b);
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    record.t_us = static_cast<uint32_t>(i);
    sum += ring.push(record) ? 1 : 0;
    TapRecord out{};
    sum += ring.pop(out) ? out.dlc : 0;
  }
  return sum;
}

BENCH(observe_ring_push, "TapRecord push into a ring with room (the ISR half alone)") {
  static ObserveRing<256, TapRecord> ring;
  TapRecord record{};
  record.can_id = 0x123;
  record.dlc = 8;
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    record.t_us = static_cast<uint32_t>(i);
    sum += ring.push(record) ? 1 : 0;
    // Drain without timing intent: keeping the ring non-full is the fixture, not the operation.
    // It is counted in the number, so read this case as an upper bound on push alone.
    TapRecord out{};
    (void) ring.pop(out);
  }
  return sum;
}

BENCH(snapshot_ring_push, "FrameSnapshot into the seqlock ring (last_frame sensor)") {
  static SnapshotRing<4> ring;
  FrameSnapshot snapshot{};
  snapshot.can_id = 0x321;
  snapshot.dlc = 8;
  for (uint8_t b = 0; b < MAX_FRAME_DATA_LEN; b++)
    snapshot.data[b] = sample_payload(b);
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    snapshot.can_id = 0x300 + (i & 0xFF);
    ring.push(snapshot);
    sum += ring.pushed();
  }
  return sum;
}

BENCH(subscribed_set_contains, "64 subscribed IDs, binary search, every probe hits") {
  static SubscribedSet<OBSERVED_IDS> set = [] {
    SubscribedSet<OBSERVED_IDS> s;
    for (uint16_t i = 0; i < OBSERVED_IDS; i++)
      s.insert(RULE_BASE_ID + i, false);
    return s;
  }();
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++)
    sum += set.contains(RULE_BASE_ID + (i % OBSERVED_IDS), false) ? 1 : 0;
  return sum;
}

BENCH(subscribed_set_contains_miss, "64 subscribed IDs, binary search, every probe misses") {
  static SubscribedSet<OBSERVED_IDS> set = [] {
    SubscribedSet<OBSERVED_IDS> s;
    for (uint16_t i = 0; i < OBSERVED_IDS; i++)
      s.insert(RULE_BASE_ID + i, false);
    return s;
  }();
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++)
    sum += set.contains(0x400 + (i % OBSERVED_IDS), false) ? 0 : 1;
  return sum;
}

BENCH(id_timing_record, "64 IDs live, linear scan — average depth 32") {
  static IdTimingTable<OBSERVED_IDS> table = [] {
    IdTimingTable<OBSERVED_IDS> t;
    for (uint16_t i = 0; i < OBSERVED_IDS; i++)
      t.record(RULE_BASE_ID + i, false, i);
    return t;
  }();
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    table.record(RULE_BASE_ID + (i % OBSERVED_IDS), false, static_cast<uint32_t>(i));
    sum += table.size();
  }
  return sum;
}

BENCH(id_timing_record_first, "64 IDs live, the ID that sits at index 0 — best case") {
  static IdTimingTable<OBSERVED_IDS> table = [] {
    IdTimingTable<OBSERVED_IDS> t;
    for (uint16_t i = 0; i < OBSERVED_IDS; i++)
      t.record(RULE_BASE_ID + i, false, i);
    return t;
  }();
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    table.record(RULE_BASE_ID, false, static_cast<uint32_t>(i));
    sum += table.size();
  }
  return sum;
}

BENCH(estimate_frame_bits, "bus-load accounting, one frame") {
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++)
    sum += estimate_frame_bits((i & 1) != 0, false, static_cast<uint8_t>(i & 7));
  return sum;
}

// ---------------------------------------------------------------------------
// Composite: everything one received frame pays before it is forwarded
// ---------------------------------------------------------------------------

BENCH(rx_frame_full, "bus-load + id_timings + subscribed + tap push + rule decision") {
  static const RuleFixture fx(false);
  static IdTimingTable<OBSERVED_IDS> timings;
  static SubscribedSet<OBSERVED_IDS> subs = [] {
    SubscribedSet<OBSERVED_IDS> s;
    for (uint16_t i = 0; i < OBSERVED_IDS; i++)
      s.insert(RULE_BASE_ID + i, false);
    return s;
  }();
  static ObserveRing<256, TapRecord> tap;

  uint8_t data[MAX_FRAME_DATA_LEN];
  for (uint8_t b = 0; b < MAX_FRAME_DATA_LEN; b++)
    data[b] = sample_payload(b);

  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    uint32_t can_id = RULE_BASE_ID + (i % OBSERVED_IDS);
    bool extended = false;
    const uint32_t now_us = static_cast<uint32_t>(i);

    sum += estimate_frame_bits(extended, false, MAX_FRAME_DATA_LEN);
    timings.record(can_id, extended, now_us);
    if (subs.contains(can_id, extended)) {
      TapRecord record{};
      record.t_us = now_us;
      record.can_id = can_id;
      record.dlc = MAX_FRAME_DATA_LEN;
      memcpy(record.data, data, MAX_FRAME_DATA_LEN);
      sum += tap.push(record) ? 1 : 0;
    }
    sum += process_frame(fx.table, can_id, extended, false, data, MAX_FRAME_DATA_LEN) == FrameAction::FORWARD ? 1 : 0;

    TapRecord out{};
    (void) tap.pop(out);
    bench::sink(data);
  }
  return sum;
}

// ---------------------------------------------------------------------------
// sd_logger line formatting — the measured writer ceiling (M2 stress run §9.6, 2026-07-26, git history)
// ---------------------------------------------------------------------------

namespace {

const sdl::SourceTable &source_fixture() {
  static const sdl::SourceTable table = [] {
    sdl::SourceTable t;
    t.add(1, sdl::KIND_CAN, "seg1");
    t.add(2, sdl::KIND_CAN, "seg2");
    t.add(3, sdl::KIND_LIN, "lin0");
    return t;
  }();
  return table;
}

sdl::LogRecord sample_record() {
  sdl::LogRecord rec{};
  rec.t_us = 0x12345678;
  rec.id = 0x1A2;
  rec.source = 1;
  rec.flags = sdl::REC_FLAG_EXTENDED;
  rec.len = 8;
  for (uint8_t b = 0; b < 8; b++)
    rec.data[b] = sample_payload(b);
  return rec;
}

}  // namespace

BENCH(format_record, "one full CSV record line, 8 payload bytes") {
  const sdl::SourceTable &sources = source_fixture();
  sdl::LogRecord rec = sample_record();
  char line[sdl::SD_LOG_MAX_LINE];
  // One chain for the whole run, exactly as the writer holds one per open file: the periodic
  // re-anchor is part of the per-record cost and a fresh clock per iteration would anchor every
  // line and price the wrong thing.
  sdl::DeltaClock clock;
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    rec.t_us = static_cast<uint32_t>(i);
    // The payload is a compile-time constant, and without this the optimiser folds hex_bytes
    // into a constant store at -O2 — which reads as a 10x win that no real frame ever gets.
    bench::sink(rec);
    sum += sdl::format_record(line, sizeof(line), clock, rec, 1'700'000'000'000'000ull + i, sources);
    bench::sink(line);
  }
  return sum;
}

BENCH(format_record_snprintf, "the same line via snprintf — what v1 replaced") {
  // No SourceTable: the M1 shape resolved the label at the call site, so the reference keeps the
  // label literal. The comparison is the formatting cost, and the lookup is three entries either way.
  sdl::LogRecord rec = sample_record();
  char line[sdl::SD_LOG_MAX_LINE];
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    rec.t_us = static_cast<uint32_t>(i);
    const uint64_t t_full = 1'700'000'000'000'000ull + i;
    // Deliberately the shape M1 had: one format-string parse for the header fields and one more
    // per payload byte. Kept as the reference the hand-rolled emitters are measured against.
    int n = snprintf(line, sizeof(line), "C,%llu,seg1,%X,x,%u,", (unsigned long long) t_full, rec.id, rec.len);
    for (uint8_t b = 0; b < rec.len && n > 0 && (size_t) n < sizeof(line); b++)
      n += snprintf(line + n, sizeof(line) - n, "%02X", rec.data[b]);
    sum += n > 0 ? (uint64_t) n : 0;
    bench::sink(line);
  }
  return sum;
}

BENCH(line_writer_hex_bytes, "8 payload bytes through the hex emitter alone") {
  uint8_t data[8];
  for (uint8_t b = 0; b < 8; b++)
    data[b] = sample_payload(b);
  char line[sdl::SD_LOG_MAX_LINE];
  uint64_t sum = 0;
  for (size_t i = 0; i < reps; i++) {
    bench::sink(data);  // see format_record: a constant payload folds away entirely at -O2
    sdl::LineWriter w(line, sizeof(line));
    w.hex_bytes(data, 8);
    sum += w.finish();
    bench::sink(line);
  }
  return sum;
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

/// Time one case: calibrate the rep count up to a target window, then take the *minimum* of five
/// trials. Minimum rather than mean on purpose — scheduler noise and migration only ever add
/// time, so the fastest trial is the closest to the code's own cost.
double run_case(const bench::Case &c, uint64_t &checksum) {
  constexpr auto TARGET = std::chrono::milliseconds(30);
  constexpr size_t MAX_REPS = 1u << 28;

  size_t reps = 1024;
  for (;;) {
    const auto start = Clock::now();
    checksum += c.fn(reps);
    if (Clock::now() - start >= TARGET || reps >= MAX_REPS)
      break;
    reps *= 4;
  }

  double best_ns = 0.0;
  for (int trial = 0; trial < 5; trial++) {
    const auto start = Clock::now();
    checksum += c.fn(reps);
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / (double) reps;
    if (trial == 0 || ns < best_ns)
      best_ns = ns;
  }
  return best_ns;
}

}  // namespace

int main(int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : nullptr;
  uint64_t checksum = 0;
  size_t ran = 0;

  printf("== hot-path microbenchmarks (host build, -O2, no sanitizers) ==\n");
  printf("%-28s %10s %12s  %s\n", "case", "ns/op", "Mops/s", "fixture");

  for (const auto &c : bench::registry()) {
    if (filter != nullptr && strstr(c.name, filter) == nullptr)
      continue;
    const double ns = run_case(c, checksum);
    printf("%-28s %10.2f %12.2f  %s\n", c.name, ns, ns > 0.0 ? 1000.0 / ns : 0.0, c.note);
    ran++;
  }

  if (ran == 0) {
    fprintf(stderr, "no case matched filter '%s'\n", filter != nullptr ? filter : "");
    return 2;
  }

  // Reading aid, not a device claim: 4131 frames/s is the saturated-bus rate measured on the
  // bench at 500 kbit/s (M2 stress run §9.4, 2026-07-26, git history), so 1 us of per-frame work is 0.41 % of one
  // core. Apply it to a *difference* between two rows, never to an absolute host number.
  printf("\nat the measured 4131 frames/s: 1 ns/frame = 0.0004 %% of a core, 1 us/frame = 0.41 %%\n");
  printf("host timings rank implementations; they do not predict C6 cost. Confirm on the bench.\n");
  // Consume the checksum so no case can be optimised away entirely.
  printf("checksum %llu\n", (unsigned long long) checksum);
  return 0;
}
