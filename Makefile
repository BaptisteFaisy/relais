# Relais — one build for the PC: core library, unit tests, explorer, simulator,
# actuator, verifier, mutants. The QNX build lives in target/ and actuator/qnx.
#
# Budget: every CI job stays under 5 minutes (user rule).

CC ?= gcc
AR ?= ar
BUILD := build
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
         -fno-strict-aliasing
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Icore/include -Icore/tests -Iactuator -MMD -MP
LDLIBS += -lm

# Coverage build: slowest machine code, instrumentation for gcov. Used only by
# the `coverage` target (mission M14); `make all` stays -O2 and clang-free.
COVERAGE_CFLAGS := -std=c11 -O0 -g --coverage -Wall -Wextra -Werror -Wshadow \
                   -Wconversion -Wno-sign-conversion -fno-strict-aliasing

CORE_SRC := $(sort $(wildcard core/src/*.c))
CORE_OBJ := $(patsubst core/src/%.c,$(BUILD)/core/%.o,$(CORE_SRC))
TEST_SRC := $(filter-out core/tests/test_util.c,$(sort $(wildcard core/tests/test_*.c)))
TEST_FIXTURES := $(sort $(wildcard core/tests/fixtures_*.c))
TEST_BIN := $(patsubst core/tests/%.c,$(BUILD)/%,$(TEST_SRC))
# M19: the lifecycle/time fault world lives in host/faultlife and reuses the
# same core; it is built like a unit test so `make test` runs it too.
TEST_BIN += $(BUILD)/test_faultlife
# host/explore holds the explorer (explore.c), the quotient abstraction
# (symbolic.c), the counter-example reducer (shrink.c), the shared world
# (world.c) and two test drivers. The library sources are linked into every
# explore binary; the mains are kept apart so `make all` builds them all.
EXPLORE_LIB_SRC := host/explore/world.c host/explore/symbolic.c host/explore/shrink.c \
                   host/explore/search.c
EXPLORE_SRC := host/explore/explore.c $(EXPLORE_LIB_SRC)
# The explorer is only built once host/explore holds sources: until then the
# proof of level 1 lives in the seeded simulator, and `make all` stays green.
EXPLORE_BIN := $(if $(EXPLORE_SRC),$(BUILD)/relais-explore)
EXPLORE_TEST_SRC := $(sort $(wildcard host/explore/test_*.c))
EXPLORE_TEST_BIN := $(patsubst host/explore/%.c,$(BUILD)/%,$(EXPLORE_TEST_SRC))
ALL_TEST_BIN := $(TEST_BIN) $(EXPLORE_TEST_BIN)
SIMULATE_SRC := $(sort $(wildcard host/simulate/*.c))
ACTUATOR_SRC := $(sort $(wildcard actuator/*.c actuator/pc/*.c))
VERIFY_SRC := $(sort $(filter-out host/verify/noninterference.c,$(wildcard host/verify/*.c)))
# The non-interference harness is a stand-alone proof-by-test binary; it shares
# the core but not the verifier's main, so it is built apart (M12).
NONINTERF_BIN := $(BUILD)/noninterference
NODE_SRC := $(sort $(wildcard host/node/*.c))
# Fault injection (M18): bit flips, corruption, resource exhaustion, and the
# campaign driver that measures them against the same sim world as the bench.
FI_LIB_SRC := host/faultinject/bitflip.c host/faultinject/corrupt.c host/faultinject/resource.c
FI_DRIVER := host/faultinject/campaign.c
BRIDGE_SRC := host/difftest/bridge.c
LIB := $(BUILD)/librelais.a

MUTANTS := 1 2 3 4 5 6 7 8

.PHONY: all test explore simulate verify mutants report demo campaign clean size coverage \
     mutant-explore noninterference
all: $(LIB) $(TEST_BIN) $(EXPLORE_BIN) $(BUILD)/relais-simulate $(BUILD)/relais-actuator \
     $(BUILD)/relais-verify $(BUILD)/relais-node $(BUILD)/relais-bridge $(ALL_TEST_BIN) \
     $(BUILD)/relais-faultinject $(NONINTERF_BIN)

$(LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/core/%.o: core/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# The protocol-level tests drive the simulator world: they link the same
# pipeline instead of re-writing one (no duplication between test and bench).
SIM_SRC := host/simulate/sim.c host/simulate/sim_faults.c
$(BUILD)/test_protocol $(BUILD)/test_law $(BUILD)/test_ota $(BUILD)/test_dissimilar: $(BUILD)/test_%: \
     core/tests/test_%.c core/tests/test_util.c $(SIM_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/simulate $< core/tests/test_util.c \
	  $(SIM_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/test_%: core/tests/test_%.c core/tests/test_util.c $(TEST_FIXTURES) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< core/tests/test_util.c $(TEST_FIXTURES) $(LIB) $(LDLIBS) -o $@

# test_faultinject links the fault-injection sources on top of the core lib.
$(BUILD)/test_faultinject: core/tests/test_faultinject.c core/tests/test_util.c \
    $(TEST_FIXTURES) $(FI_LIB_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/faultinject $< core/tests/test_util.c \
	  $(TEST_FIXTURES) $(FI_LIB_SRC) $(LIB) $(LDLIBS) -o $@

# M19: lifecycle/time fault world. Explicit rule (wins over the pattern above)
# because its sources live in host/faultlife, not core/tests.
$(BUILD)/test_faultlife: host/faultlife/test_faultlife.c host/faultlife/faultlife.c \
    core/tests/test_util.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) host/faultlife/test_faultlife.c host/faultlife/faultlife.c \
	  core/tests/test_util.c $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-explore: $(EXPLORE_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(EXPLORE_SRC) $(LIB) $(LDLIBS) -o $@

# The two explore test drivers link the shared world/abstraction/reducer and the
# common test harness (no duplicated check counting).
$(BUILD)/test_symbolic $(BUILD)/test_shrink: $(BUILD)/test_%: \
    host/explore/test_%.c core/tests/test_util.c $(EXPLORE_LIB_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/explore $< core/tests/test_util.c \
	  $(EXPLORE_LIB_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-simulate: $(SIMULATE_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SIMULATE_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-actuator: $(ACTUATOR_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(ACTUATOR_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-verify: $(VERIFY_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(VERIFY_SRC) $(LIB) $(LDLIBS) -o $@

$(NONINTERF_BIN): host/verify/noninterference.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) host/verify/noninterference.c $(LIB) $(LDLIBS) -o $@

noninterference: $(NONINTERF_BIN)
	./$(NONINTERF_BIN) --runs 200

$(BUILD)/relais-node: $(NODE_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(NODE_SRC) $(LIB) $(LDLIBS) -o $@

# The fault-injection campaign: drives the same sim world, injects bit flips,
# and emits one JSON line per run that host/faultinject/campaign.py aggregates.
$(BUILD)/relais-faultinject: $(FI_DRIVER) $(FI_LIB_SRC) $(SIM_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/faultinject -Ihost/simulate $(FI_DRIVER) \
	  $(FI_LIB_SRC) $(SIM_SRC) $(LIB) $(LDLIBS) -o $@

# Differential-test bridge: exposes the C arbiter over JSON lines to arbiter_py.py.
$(BUILD)/relais-bridge: $(BRIDGE_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(BRIDGE_SRC) $(LIB) $(LDLIBS) -o $@

test: $(TEST_BIN) $(ALL_TEST_BIN)
	@fail=0; for b in $(ALL_TEST_BIN); do \
	  printf '%-28s ' "$$(basename $$b)"; \
	  if ./$$b > $(BUILD)/$$(basename $$b).log 2>&1; then \
	    tail -1 $(BUILD)/$$(basename $$b).log; \
	  else tail -3 $(BUILD)/$$(basename $$b).log; fail=1; fi; \
	done; [ $$fail -eq 0 ] || (echo "TESTS-EN-ECHEC"; exit 1)

explore: $(EXPLORE_BIN)
	@if [ -x $(BUILD)/relais-explore ]; then \
	  ./$(BUILD)/relais-explore --budget 100000 --faults 1; \
	else echo "explorateur absent de cet arbre : niveau 1 non joue"; fi

simulate: $(BUILD)/relais-simulate
	./$(BUILD)/relais-simulate --ticks 1000000 --seed 1

verify: $(BUILD)/relais-verify

# Each mutant is the same source with -DRELAIS_MUTANT=n; the bench must flag it.
mutants: $(LIB)
	@for m in $(MUTANTS); do \
	  mkdir -p $(BUILD)/m$$m; \
	  for s in $(CORE_SRC); do \
	    $(CC) $(CFLAGS) $(CPPFLAGS) -DRELAIS_MUTANT=$$m -c $$s \
	      -o $(BUILD)/m$$m/$$(basename $$s .c).o || exit 1; \
	  done; \
	  $(AR) rcs $(BUILD)/m$$m/librelais.a $(BUILD)/m$$m/*.o || exit 1; \
	  echo "MUTANT-$$m built"; \
	done

# The quotient explorer and the reducer, faced with the mutants: each must still
# catch the defect, and the reduced counter-example must be shorter than the raw
# one. Reuses the mutant libraries built by `mutants` (no second core build).
EXPLORE_MUTS := 3 4 6 8
mutant-explore: mutants
	@fail=0; for m in $(EXPLORE_MUTS); do \
	  for t in test_symbolic test_shrink; do \
	    $(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/explore host/explore/$$t.c \
	      core/tests/test_util.c $(EXPLORE_LIB_SRC) $(BUILD)/m$$m/librelais.a $(LDLIBS) \
	      -o $(BUILD)/m$$m/$$t || fail=1; \
	    ./$(BUILD)/m$$m/$$t || fail=1; \
	  done; \
	done; [ $$fail -eq 0 ] && echo "MUTANT-EXPLORE-OK" || (echo "MUTANT-EXPLORE-ECHEC"; exit 1)

# One command, no root, no QNX SDP: the demonstration of spec 8 on a plain PC.
demo: $(BUILD)/relais-actuator $(BUILD)/relais-node $(BUILD)/relais-verify
	python3 host/bench/relais_bench.py --demo

# A short campaign: 20 instants of death swept over the period, both levels.
campaign: $(BUILD)/relais-simulate $(BUILD)/relais-verify
	@fail=0; for seed in 1 2 3 4 5; do \
	  for at in 200 201 202 203; do \
	    ./$(BUILD)/relais-simulate --ticks 2000 --seed $$seed --fault $$at:death:0 --quiet || fail=1; \
	    ./$(BUILD)/relais-simulate --ticks 2000 --seed $$seed --level complet --fault $$at:death:0 --quiet || fail=1; \
	  done; \
	done; [ $$fail -eq 0 ] && echo "CAMPAGNE-OK" || (echo "CAMPAGNE-ECHEC"; exit 1)

size:
	@for f in $(shell find core actuator host target -name '*.c' -o -name '*.h' -o -name '*.py' 2>/dev/null); do \
	  n=$$(wc -l < $$f); [ $$n -lt 500 ] || echo "TOO LONG: $$f ($$n)"; done; echo "size check done"

# --- Coverage (mission M14) ---
# Rebuilds core/ and host/ with --coverage, replays the unit tests, the seeded
# simulator over a fixed fault set, the offline verifier and the explorer, then
# runs gcov and emits a per-file summary. The curated docs/COVERAGE.md holds the
# uncovered-line justifications; this target regenerates the raw numbers.
coverage:
	$(MAKE) BUILD=$(BUILD)/cov CFLAGS="$(COVERAGE_CFLAGS)" clean all
	python3 host/quality/coverage_driver.py --build $(BUILD)/cov
	python3 host/quality/gcov_report.py --root . --objects $(BUILD)/cov/core \
	  --src-dir core/src --out $(BUILD)/cov/coverage.summary.md
	@cat $(BUILD)/cov/coverage.summary.md

clean:
	rm -rf $(BUILD)

# --- Portability and transports (mission M32) -------------------------------
# APPENDED ONLY: nothing above is modified. The transport interface lives in
# core/include/relais/transport.h, its implementations in host/transport/, and
# the ports to other RTOS, another language and other compilers in ports/.
#
# What is proved here, for real:
#   build/test_transport        the interface over a memory transport (unit)
#   build/test_transport_can    the same 200-tick scenario over fragmented CAN
#   build/transport-udp-check   loopback UDP + the unmodified relais-node
#   make ports                  Zephyr / FreeRTOS / Rust / other compilers,
#                               each one honest about what is installed here.

TRANSPORT_INC := -Ihost/transport
# SocketCAN is compiled when the host really has <linux/can.h>; running it needs
# a CAN interface, which needs privileges this checkout does not have.
SOCKETCAN_FLAG := $(shell echo | $(CC) -E -include linux/can.h -x c - >/dev/null 2>&1 \
                   && echo -DRELAIS_HAVE_SOCKETCAN)
TRANSPORT_OBJ := $(BUILD)/transport/can_socketcan.o
CAN_TEST_BIN := $(BUILD)/test_transport_can
UDP_CHECK_BIN := $(BUILD)/transport-udp-check

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# --- M31: command-robustness studies (APPEND-ONLY; ideas 61,62,66,68,69,70,71) ---
# Python studies of host/study/ + physics mirrors of host/physics/. They reuse
# the built binaries (relais-simulate, relais-verify, relais-bridge): run
# `make all` first. Fixed seeds: every table is reproducible.
M31_STUDY := host/study

.PHONY: m31 m31-test m31-brake m31-infeasible m31-sensitivity m31-testament m31-dataset transports \
     ports ports-zephyr ports-freertos ports-rust ports-compilers

all: $(CAN_TEST_BIN) $(UDP_CHECK_BIN) $(TRANSPORT_OBJ)

$(BUILD)/transport/can_socketcan.o: host/transport/can_socketcan.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TRANSPORT_INC) $(SOCKETCAN_FLAG) -c $< -o $@
	@if [ -n "$(SOCKETCAN_FLAG)" ]; then \
	  echo "  socketcan backend compiled against <linux/can.h>"; \
	else echo "  socketcan backend skipped: <linux/can.h> absent"; fi

$(CAN_TEST_BIN): host/transport/test_can.c host/transport/can.c \
    core/tests/test_util.c $(TEST_FIXTURES) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Icore/tests $(TRANSPORT_INC) \
	  host/transport/test_can.c host/transport/can.c core/tests/test_util.c \
	  $(TEST_FIXTURES) $(LIB) $(LDLIBS) -o $@

$(UDP_CHECK_BIN): host/transport/udp_check.c host/transport/udp.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TRANSPORT_INC) host/transport/udp_check.c \
	  host/transport/udp.c $(LIB) $(LDLIBS) -o $@

# The three transport proofs, in one command. The UDP one binds ports 7930-7932
# and starts build/relais-node as a child process.
transports: $(BUILD)/test_transport $(CAN_TEST_BIN) $(UDP_CHECK_BIN) $(BUILD)/relais-node
	./$(BUILD)/test_transport
	./$(CAN_TEST_BIN)
	./$(UDP_CHECK_BIN)

ports-zephyr:
	$(MAKE) -C ports/zephyr check

ports-freertos:
	$(MAKE) -C ports/freertos check

ports-rust:
	$(MAKE) -C ports/rust check

ports-compilers:
	ports/compilers/check.sh

ports: ports-zephyr ports-freertos ports-rust ports-compilers
m31: m31-test m31-brake m31-infeasible m31-sensitivity m31-testament m31-dataset
	@echo "M31-STUDIES-OK"

m31-test:
	python3 -m unittest discover -s $(M31_STUDY) -p 'test_*.py'

m31-brake:
	python3 $(M31_STUDY)/brake_campaign.py

m31-infeasible:
	python3 $(M31_STUDY)/infeasible.py

m31-sensitivity:
	python3 $(M31_STUDY)/sensitivity.py

m31-testament:
	python3 $(M31_STUDY)/testament_opt.py

m31-dataset:
	python3 $(M31_STUDY)/dataset_replay.py --all
