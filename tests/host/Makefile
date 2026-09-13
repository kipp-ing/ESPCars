# Host unit tests for components/can_gateway/gateway_core.h.
#
# gateway_core.h is deliberately ESPHome-free and IDF-free so the exact code
# that runs in the RX interrupt also compiles and runs under a plain host
# toolchain (see the header's own comment). This target is that toolchain.
#
#   make -C tests/host            build and run everything
#   make -C tests/host FILTER=x   run only cases whose name contains x
#   make -C tests/host SAN=       build without sanitizers
#   make -C tests/host bench      run the hot-path microbenchmarks (bench.cpp)
#   make -C tests/host clean
#
# No CMake, no external test framework: the harness is harness.h, one header.
# Exit status is non-zero if any case fails, and the failing case is named on
# stdout with the file:line of every failed check.

CXX      ?= c++
CXXSTD   ?= -std=c++17
WARN     ?= -Wall -Wextra
OPT      ?= -O1 -g
# -fno-sanitize-recover matters: without it UBSan prints and continues, and CI
# would see exit 0 on a real finding.
SAN      ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
FILTER   ?=

CORE_DIR := ../../components/can_gateway
# sd_logger's tap translation (TapRecord -> LogRecord) is split into
# ESPHome-free headers for exactly this reason: it is where the two flag
# encodings collide, and a wrong bit produces a well-formed, quietly wrong log
# that a bench run cannot catch. SD_LOGGER_HOST_TEST switches its include of
# gateway_core.h from the ESPHome path to this -I path.
LOGGER_DIR := ../../components/sd_logger
# uds's core (catalog reader, decode, response validation, scheduler) is
# ESPHome-free by construction — the catalog fuzz cases in particular only mean
# something under ASan, which the device build cannot run. No define needed.
UDS_DIR  := ../../components/uds
# isotp's segmentation/reassembly core is likewise ESPHome-free. It had no host
# tier until 2026-07-30, when the bench found that no transfer needing a second
# flow-control block had ever actually run — see test_isotp_core.cpp's header.
ISOTP_DIR := ../../components/isotp
BUILD    := build
BIN      := $(BUILD)/run_tests

# RulePatch lives in can_gateway.h, which cannot be included on the host (it
# pulls in ESPHome and ESP-IDF). Its class text is extracted verbatim so the
# tests exercise the real code rather than a copy that could drift.
GEN      := $(BUILD)/rule_patch_extracted.h

SRCS     := main.cpp $(sort $(wildcard test_*.cpp))
OBJS     := $(SRCS:%.cpp=$(BUILD)/%.o)
DEPS     := $(OBJS:.o=.d)

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(SAN) -pthread -MMD -MP -I. -I$(BUILD) -I$(CORE_DIR) \
            -I$(LOGGER_DIR) -I$(UDS_DIR) -I$(ISOTP_DIR) -DSD_LOGGER_HOST_TEST
LDFLAGS  := $(SAN) -pthread

.PHONY: all test compile bench lockcheck clean
all: test

test: lockcheck $(BIN)
	@echo "== can_gateway host tests =="
	@./$(BIN) $(FILTER)

## Build without running (useful when only the compile must be checked).
compile: lockcheck $(BIN)

# The one sd_logger contract that no host case can reach: `index_mux_` is an
# `xSemaphoreCreateMutex()`, which is NOT recursive, and `IndexLock` waits
# portMAX_DELAY. So an `IndexLock` scope that grows to enclose ensure_room_(),
# commit_line_() or flush_block_() deadlocks the writer task permanently — no
# log line, no counter, no watchdog, just a component that stopped. The two are
# five lines apart today (sd_logger.cpp: the `#gap` marker takes the lock to
# read the discard window, gives it back, and only then reserves line room), so
# widening that scope is a plausible edit and not a hypothetical one.
#
# Same precedent as the RulePatch extraction below: a grep-shaped assertion over
# the component source, run before the binary is built, that fails loudly with
# the reason rather than being discovered on the bench. The scope is tracked by
# brace depth from the line the lock is declared on, with `//` comments stripped
# first; a lock declared and closed on a single line is not analysed (there are
# none, and the check errs toward silence rather than toward a false alarm).
lockcheck: $(LOGGER_DIR)/sd_logger.cpp
	@awk '\
	  { line = $$0; sub(/\/\/.*/, "", line) } \
	  line ~ /IndexLock[ \t]+[A-Za-z_]+[ \t]*\(/ { depth = 0; inlock = 1; start = FNR; next } \
	  inlock { \
	    opens = gsub(/\{/, "{", line); shuts = gsub(/\}/, "}", line); \
	    depth += opens - shuts; \
	    if (line ~ /(ensure_room_|commit_line_|flush_block_)[ \t]*\(/) { \
	      printf "%s:%d:%s   <-- inside the IndexLock taken at line %d\n", FILENAME, FNR, line, start; \
	      bad = 1; \
	    } \
	    if (depth < 0) inlock = 0; \
	  } \
	  END { exit bad ? 1 : 0 }' $< || { \
	  echo "ERROR: an IndexLock scope encloses a writer-buffer call (listed above)." >&2; \
	  echo "       index_mux_ is xSemaphoreCreateMutex() — not recursive — and IndexLock waits" >&2; \
	  echo "       portMAX_DELAY, so this deadlocks the writer task with nothing to notice it." >&2; \
	  echo "       Take the index lock, copy what you need out of it, give it back, and only then" >&2; \
	  echo "       reserve line room. Do not delete this check." >&2; exit 1; }

$(BIN): $(OBJS)
	@echo "  LD    $@"
	@$(CXX) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/%.o: %.cpp | $(GEN)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(GEN): $(CORE_DIR)/can_gateway.h | $(BUILD)
	@echo "  GEN   $@"
	@{ \
	  echo '// GENERATED by tests/host/Makefile — do not edit, do not commit.'; \
	  echo '// RulePatch extracted verbatim from components/can_gateway/can_gateway.h,'; \
	  echo '// which cannot be included on the host (ESPHome + ESP-IDF headers).'; \
	  echo '#pragma once'; \
	  echo '#include "gateway_core.h"'; \
	  echo 'namespace esphome::can_gateway {'; \
	  echo 'class GatewayRoute;  // friend target; never defined on the host'; \
	  awk '/^class RulePatch[ ]*[{]/ { p = 1 } p { print } p && /^[}];/ { exit }' $<; \
	  echo '}  // namespace esphome::can_gateway'; \
	} > $@
	@grep -q 'void set_can_id' $@ || { \
	  echo "ERROR: could not extract RulePatch from $<." >&2; \
	  echo "       The class no longer starts with a line 'class RulePatch {' or no longer" >&2; \
	  echo "       ends with a line '};' at column 0. Fix the awk rule in tests/host/Makefile" >&2; \
	  echo "       rather than dropping the RulePatch cases." >&2; \
	  rm -f $@; exit 1; }
	@grep -q 'void set_byte' $@ || { \
	  echo "ERROR: extracted RulePatch is missing set_byte; see $<." >&2; rm -f $@; exit 1; }

# Microbenchmarks. A separate binary with separate flags, on purpose:
#   -O2, because -O1 does not inline the CAN_GATEWAY_CORE_INLINE hot path the
#        device build actually gets, so an -O1 number ranks the wrong things;
#   no sanitizers, because ASan/UBSan instrument exactly the atomics and bounds
#        checks these loops consist of — the result would time the instrumentation.
# The correctness gate stays `make`, which keeps -O1 and both sanitizers.
# bench.cpp does not use RulePatch, so it needs no $(GEN).
BENCH_BIN   := $(BUILD)/run_bench
BENCH_OPT   ?= -O2 -DNDEBUG
BENCH_FLAGS := $(CXXSTD) $(WARN) $(BENCH_OPT) -I. -I$(CORE_DIR) -I$(LOGGER_DIR) -DSD_LOGGER_HOST_TEST

bench: $(BENCH_BIN)
	@./$(BENCH_BIN) $(FILTER)

$(BENCH_BIN): bench.cpp $(CORE_DIR)/gateway_core.h $(LOGGER_DIR)/log_format.h $(LOGGER_DIR)/log_record.h | $(BUILD)
	@echo "  CXX   bench.cpp ($(BENCH_OPT), no sanitizers)"
	@$(CXX) $(BENCH_FLAGS) bench.cpp -o $@

$(BUILD):
	@mkdir -p $(BUILD)

$(OBJS): | $(BUILD)

clean:
	@rm -rf $(BUILD)

-include $(DEPS)
