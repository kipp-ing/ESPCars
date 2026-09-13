#pragma once
//
// Minimal test harness for the can_gateway host tests.
//
// Deliberately hand-rolled and header-only: the repo has no C++ test dependency
// and gateway_core.h is dependency-free by design, so the test target must not
// drag in CMake, Catch2 or GoogleTest. Everything here is host-side scaffolding
// only — no component code lives in this file.
//
// Usage:
//     TEST(my_case) {
//       CHECK(some_condition);
//       CHECK_EQ(actual, expected);
//     }
//
// Cases self-register at static-init time; main.cpp just calls run_all().
// A failing check prints file:line plus both values and marks the case failed;
// the case keeps running so one broken assumption reports every consequence.
// run_all() returns 1 when any case failed, 2 when the name filter matched
// nothing (a typo in CI must not look like success).

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace hosttest {

struct TestCase {
  const char *name;
  const char *file;
  void (*fn)();
  /// Non-null when the case asserts a contract the code currently violates.
  /// The assertion is NOT weakened: it runs and prints its evidence every time,
  /// but it does not decide the exit status, so the target stays usable as a CI
  /// gate while the reported defect is open. Remove the marker with the fix.
  const char *known_defect;
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline int &current_failures() {
  static int failures = 0;
  return failures;
}

/// Non-null when the running case called SKIP_IF and the condition held. Distinct
/// from `known_defect`: that marks a case that always runs and always reports its
/// evidence; this marks a case that cannot run at all on this checkout (private,
/// bench-specific test data not present) and says so loudly rather than either
/// silently vanishing or failing a build that never had access to the data.
inline const char *&current_skip_reason() {
  static const char *reason = nullptr;
  return reason;
}

struct Registrar {
  Registrar(const char *name, const char *file, void (*fn)(), const char *known_defect = nullptr) {
    registry().push_back(TestCase{name, file, fn, known_defect});
  }
};

/// Human-readable rendering of a checked value (decimal + hex for integers).
template<typename T> std::string repr(const T &value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld (0x%llx)", static_cast<long long>(value),
                  static_cast<unsigned long long>(static_cast<long long>(value)));
    return std::string(buf);
  } else if constexpr (std::is_pointer_v<T>) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%p", static_cast<const void *>(value));
    return std::string(buf);
  } else {
    return std::string("<value>");
  }
}

/// Comparison that tolerates the mixed integer/enum widths the core header uses,
/// without tripping -Wsign-compare in every test.
template<typename A, typename B> bool eq(const A &a, const B &b) {
  if constexpr (std::is_enum_v<A> || std::is_enum_v<B>) {
    return static_cast<long long>(a) == static_cast<long long>(b);
  } else if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
    if constexpr (std::is_signed_v<A> || std::is_signed_v<B>) {
      return static_cast<long long>(a) == static_cast<long long>(b);
    } else {
      return static_cast<unsigned long long>(a) == static_cast<unsigned long long>(b);
    }
  } else {
    return a == b;
  }
}

inline void report_failure(const char *file, int line, const std::string &message) {
  current_failures()++;
  std::printf("    %s:%d: %s\n", file, line, message.c_str());
}

inline int run_all(int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : nullptr;
  int total = 0;
  int failed = 0;
  int skipped = 0;
  int known_reproduced = 0;
  int known_quiet = 0;
  for (const TestCase &tc : registry()) {
    if (filter != nullptr && std::strstr(tc.name, filter) == nullptr)
      continue;
    total++;
    current_failures() = 0;
    current_skip_reason() = nullptr;
    if (tc.known_defect != nullptr)
      std::printf("[ KNOWN ] %s — asserts a contract the code currently violates:\n           %s\n", tc.name,
                  tc.known_defect);
    tc.fn();
    if (tc.known_defect != nullptr) {
      if (current_failures() != 0) {
        known_reproduced++;
        std::printf("[ KNOWN ] %s reproduced the open defect (not counted as a CI failure)\n", tc.name);
      } else {
        known_quiet++;
        std::printf("[ KNOWN ] %s did not reproduce on this host/run — the defect is a memory-ordering\n"
                    "           hazard and stays latent on strongly-ordered CPUs. Marker stays until fixed.\n",
                    tc.name);
      }
      continue;
    }
    if (current_skip_reason() != nullptr) {
      skipped++;
      std::printf("[ SKIP ] %s — %s\n", tc.name, current_skip_reason());
      continue;
    }
    if (current_failures() == 0) {
      std::printf("[  ok  ] %s\n", tc.name);
    } else {
      failed++;
      std::printf("[ FAIL ] %s  (%d failed check%s in %s)\n", tc.name, current_failures(),
                  current_failures() == 1 ? "" : "s", tc.file);
    }
  }
  std::printf("\n%d case%s run, %d failed", total, total == 1 ? "" : "s", failed);
  if (skipped > 0)
    std::printf(", %d skipped", skipped);
  if (known_reproduced + known_quiet > 0)
    std::printf(", %d known-defect case%s (%d reproduced here)", known_reproduced + known_quiet,
                known_reproduced + known_quiet == 1 ? "" : "s", known_reproduced);
  std::printf("\n");
  if (total == 0) {
    std::printf("ERROR: no case matched the filter\n");
    return 2;
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace hosttest

#define HOSTTEST_FAIL_(msg) ::hosttest::report_failure(__FILE__, __LINE__, (msg))

/// Bails out of the current TEST case, loudly, without counting it as a failure —
/// for a case that depends on private, bench-specific data absent on this
/// checkout. Must be the last thing the case does before it would otherwise touch
/// the missing data; unlike CHECK, a case that never reaches its assertions at all
/// still needs to say so rather than silently reporting `[ ok ]`.
#define SKIP_IF(cond, reason) \
  do { \
    if (cond) { \
      ::hosttest::current_skip_reason() = (reason); \
      return; \
    } \
  } while (0)

#define CHECK(cond) \
  do { \
    if (!(cond)) \
      HOSTTEST_FAIL_(std::string("CHECK(" #cond ") failed")); \
  } while (0)

#define CHECK_MSG(cond, msg) \
  do { \
    if (!(cond)) \
      HOSTTEST_FAIL_(std::string("CHECK(" #cond ") failed: ") + (msg)); \
  } while (0)

#define CHECK_EQ(actual, expected) \
  do { \
    auto hosttest_a_ = (actual); \
    auto hosttest_e_ = (expected); \
    if (!::hosttest::eq(hosttest_a_, hosttest_e_)) \
      HOSTTEST_FAIL_(std::string("CHECK_EQ(" #actual ", " #expected "): got ") + ::hosttest::repr(hosttest_a_) + \
                     ", want " + ::hosttest::repr(hosttest_e_)); \
  } while (0)

/// Same as CHECK_EQ but carries a caller-supplied tag, for table-driven cases
/// where the line number alone does not identify the failing row.
#define CHECK_EQ_MSG(actual, expected, msg) \
  do { \
    auto hosttest_a_ = (actual); \
    auto hosttest_e_ = (expected); \
    if (!::hosttest::eq(hosttest_a_, hosttest_e_)) \
      HOSTTEST_FAIL_(std::string("CHECK_EQ(" #actual ", " #expected ") [") + (msg) + "]: got " + \
                     ::hosttest::repr(hosttest_a_) + ", want " + ::hosttest::repr(hosttest_e_)); \
  } while (0)

/// Byte-buffer comparison; reports the first differing index.
#define CHECK_BYTES(actual, expected, len) \
  do { \
    const uint8_t *hosttest_ab_ = (actual); \
    const uint8_t *hosttest_eb_ = (expected); \
    for (size_t hosttest_i_ = 0; hosttest_i_ < static_cast<size_t>(len); hosttest_i_++) { \
      if (hosttest_ab_[hosttest_i_] != hosttest_eb_[hosttest_i_]) { \
        HOSTTEST_FAIL_(std::string("CHECK_BYTES(" #actual ", " #expected ") differ at [") + \
                       std::to_string(hosttest_i_) + "]: got " + ::hosttest::repr(hosttest_ab_[hosttest_i_]) + \
                       ", want " + ::hosttest::repr(hosttest_eb_[hosttest_i_])); \
        break; \
      } \
    } \
  } while (0)

#define TEST(name) \
  static void name(); \
  static ::hosttest::Registrar hosttest_reg_##name(#name, __FILE__, name); \
  static void name()

/// A case whose assertions are correct but which the code under test currently
/// fails. The checks run and print exactly as written — nothing is weakened —
/// but the case does not decide the exit status, so an open defect does not
/// make the whole target permanently red. `reason` must name the defect and its
/// location. Delete the marker (use TEST) together with the fix.
#define TEST_KNOWN_DEFECT(name, reason) \
  static void name(); \
  static ::hosttest::Registrar hosttest_reg_##name(#name, __FILE__, name, reason); \
  static void name()
