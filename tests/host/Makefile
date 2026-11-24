# Host unit tests for components/can_gateway/gateway_core.h.
#
# gateway_core.h is deliberately ESPHome-free and IDF-free so the exact code
# that runs in the RX interrupt also compiles and runs under a plain host
# toolchain (see the header's own comment). This target is that toolchain.
# Runtime regression cases extract only our own component code using Python's
# standard library. The host build does not need the ESPHome package.
#
# Isolation rule: component glue compiled against host stubs MUST live in
# test_glue_<name>.cpp. Each gets build/glue_<name>, sharing main.cpp/harness.h
# but never another glue test's objects or run_tests. Otherwise incompatible
# esphome:: types/inline functions violate the ODR and silently run wrong code.
# Freestanding core tests alone belong in run_tests. gluecheck rejects stub
# headers and extracted glue runtime includes in its compiler dependencies.
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
.DEFAULT_GOAL := all

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
CCP_DIR := ../../components/ccp
# portal's core (resolver, envelope emitter, action parser) is ESPHome-free by construction.
PORTAL_DIR := ../../components/portal
BUILD    := build
BIN      := $(BUILD)/run_tests

# RulePatch lives in can_gateway.h, which cannot be included on the host (it
# pulls in ESPHome and ESP-IDF). Its class text is extracted verbatim so the
# tests exercise the real code rather than a copy that could drift.
GEN      := $(BUILD)/rule_patch_extracted.h
RUNTIME_GEN := $(BUILD)/gateway_runtime.stamp

GLUE_SRCS := $(sort $(wildcard test_glue_*.cpp))
GLUE_OBJS := $(GLUE_SRCS:%.cpp=$(BUILD)/%.o)
GLUE_BINS := $(GLUE_SRCS:test_glue_%.cpp=$(BUILD)/glue_%)
SRCS     := main.cpp $(filter-out $(GLUE_SRCS),$(sort $(wildcard test_*.cpp)))
LINBUS_DIR := ../../components/linbus
LINBUS_OBJS := $(BUILD)/lin_communication.o $(BUILD)/lin_protocol_handler.o
LINBUS_RUNTIME_OBJS := $(BUILD)/linbus.o $(BUILD)/lin_serial.o
CORE_OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)
OBJS     := $(CORE_OBJS) $(GLUE_OBJS) $(LINBUS_OBJS) $(LINBUS_RUNTIME_OBJS)
DEPS     := $(OBJS:.o=.d)
BINS     := $(BIN) $(GLUE_BINS)

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(SAN) -pthread -MMD -MP -I. -I$(BUILD) -I$(CORE_DIR) \
            -I$(LOGGER_DIR) -I$(UDS_DIR) -I$(ISOTP_DIR) -I$(CCP_DIR) -I$(PORTAL_DIR) -DSD_LOGGER_HOST_TEST
LDFLAGS  := $(SAN) -pthread

# Exercise the filter-enabled decoder branch with protected entity filter storage.
$(BUILD)/test_glue_gateway_decode.o: CXXFLAGS += -DUSE_SENSOR_FILTER -DUSE_BINARY_SENSOR_FILTER -DUSE_TEXT_SENSOR_FILTER

# Execute the real UdsHub glue against a controlled ESPHome/transport boundary.
$(BUILD)/test_glue_uds_hub.o: CXXFLAGS += -Iuds_stubs -DUSE_SENSOR -DUSE_TEXT_SENSOR

.PHONY: all test compile bench lockcheck gluecheck clean
all: test

# Harness exit 2 means no match; tolerate it only with an explicit filter,
# and only when at least one binary matches. Preserve every other failure.
test: lockcheck gluecheck $(BINS)
	@matched=0; failed=0; \
	for bin in $(BINS); do \
	  if output=$$(./$$bin "$(FILTER)" 2>&1); then status=0; else status=$$?; fi; \
	  if [ $$status -eq 2 ] && [ -n "$(FILTER)" ]; then \
	    echo "== $$bin (no matching cases) =="; \
	  else \
	    echo "== $$bin =="; printf '%s\n' "$$output"; \
	    matched=1; \
	    if [ $$status -ne 0 ]; then failed=1; fi; \
	  fi; \
	done; \
	if [ $$matched -eq 0 ]; then echo "ERROR: no case matched the filter in any binary" >&2; exit 2; fi; \
	exit $$failed

## Build without running (useful when only the compile must be checked).
compile: lockcheck gluecheck $(BINS)

# Inspect transitive includes, not just direct #includes: a component header can
# pull in a stub via -Iuds_stubs/-Ilinbus_stubs. Also catch our generated glue
# headers, whose fakes can be inline rather than under a *_stubs directory.
gluecheck: $(CORE_OBJS)
	@awk '\
	  $$0 ~ "(^|[ /])([^ /]+_)?stubs([/.]|$$)|build/(transport_runtime[.]h|portal_runtime[.]h|gateway_[^ /]*[.]inc)" { \
	    printf "%s:%d: %s\n", FILENAME, FNR, $$0; bad = 1; \
	  } \
	  END { exit bad ? 1 : 0 }' $(CORE_OBJS:.o=.d) || { \
	  echo "ERROR: stub/glue include in run_tests; name the test test_glue_<name>.cpp" >&2; \
	  echo "       so it runs in its own executable. Do not link stubbed types together." >&2; exit 1; }

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

$(BIN): $(CORE_OBJS) Makefile
	@echo "  LD    $@"
	@$(CXX) $(CORE_OBJS) $(LDFLAGS) -o $@

$(GLUE_BINS): $(BUILD)/glue_%: $(BUILD)/main.o $(BUILD)/test_glue_%.o Makefile
	@echo "  LD    $@"
	@$(CXX) $(filter %.o,$^) $(LDFLAGS) -o $@

$(BUILD)/glue_linbus_self_test: $(LINBUS_OBJS)
$(BUILD)/glue_linbus_runtime: $(LINBUS_OBJS) $(LINBUS_RUNTIME_OBJS)

# Compile the production LIN tables/sender against host RTOS/UART adapters.
$(BUILD)/test_glue_linbus_runtime.o $(BUILD)/test_glue_linbus_self_test.o $(LINBUS_OBJS) $(LINBUS_RUNTIME_OBJS): CXXFLAGS += -Ilinbus_stubs -I$(LINBUS_DIR)

$(LINBUS_OBJS) $(LINBUS_RUNTIME_OBJS): $(BUILD)/%.o: $(LINBUS_DIR)/%.cpp | $(BUILD)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp | $(GEN) $(RUNTIME_GEN)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test_glue_portal.o: $(BUILD)/portal_runtime.h

$(BUILD)/portal_runtime.h: generate_portal_runtime.py $(PORTAL_DIR)/portal.h $(PORTAL_DIR)/portal.cpp | $(BUILD)
	@python3 generate_portal_runtime.py

$(BUILD)/test_glue_transport_runtime.o: $(BUILD)/transport_runtime.h

$(BUILD)/transport_runtime.h: generate_transport_runtime.py $(CCP_DIR)/ccp.h $(CCP_DIR)/ccp.cpp $(ISOTP_DIR)/isotp.h $(ISOTP_DIR)/isotp.cpp | $(BUILD)
	@python3 generate_transport_runtime.py

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

$(BUILD)/test_glue_gateway_decode.o $(BUILD)/test_glue_gateway_port.o $(BUILD)/test_glue_gateway_rx.o $(BUILD)/test_glue_gateway_switch.o $(BUILD)/test_glue_gateway_frame_data.o: $(RUNTIME_GEN)

$(RUNTIME_GEN): extract_gateway_runtime.py $(CORE_DIR)/can_gateway.h $(CORE_DIR)/can_gateway.cpp | $(BUILD)
	@echo "  GEN   gateway runtime (repository code only)"
	@python3 extract_gateway_runtime.py
	@touch $@

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
