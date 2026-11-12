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
TEST_BIN += $(BUILD)/test_faultlife $(BUILD)/test_latency
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
     $(BUILD)/relais-faultinject $(NONINTERF_BIN) $(BUILD)/relais-intruder

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

# The intruder (idea 35): an adversarial process, run on purpose. It includes
# the QNX header for the real device paths, hence the host stub.
INTRUDER_SRC := host/bench/intruder.c
$(BUILD)/relais-intruder: $(INTRUDER_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itarget/include -DRELAIS_HOST_STUB $< $(LIB) $(LDLIBS) -o $@

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

# Fuzzing and sanitizers (ROB-09): the real targets, the real sanitizers, a
# bounded budget for CI and `make fuzz-long` for the 24 h run. Everything lives
# in host/fuzz, which picks libFuzzer when clang is there and falls back to its
# own seeded generator under gcc + ASAN/UBSAN when it is not.
FUZZ_BUDGET ?= 60
FUZZ_LONG   ?= 86400
fuzz:
	@$(MAKE) --no-print-directory -C host/fuzz fuzz FUZZ_BUDGET=$(FUZZ_BUDGET)

fuzz-long:
	@$(MAKE) --no-print-directory -C host/fuzz fuzz-long FUZZ_LONG=$(FUZZ_LONG)

fuzz-report:
	@$(MAKE) --no-print-directory -C host/fuzz report

# Idea 35: the opposed policy. The intruder attacks, the system refuses, and the
# refusal is the real output of the real system call (docs/SECURITY-POLICY-DEMO.md).
demo-policy: $(BUILD)/relais-intruder
	./$(BUILD)/relais-intruder --port 7100

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

# --- k-induction (mission M44) ---
# The induction engine reuses the world, the quotient projection and the
# depth-first search of host/explore; only induction.c is new.
$(BUILD)/relais-induction: host/explore/induction_main.c host/explore/induction.c \
    host/explore/induction_report.c $(EXPLORE_LIB_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/explore $< host/explore/induction.c \
	  host/explore/induction_report.c $(EXPLORE_LIB_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/test_induction: host/explore/test_induction.c core/tests/test_util.c \
    host/explore/induction.c host/explore/induction_report.c $(EXPLORE_LIB_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/explore $< core/tests/test_util.c \
	  host/explore/induction.c host/explore/induction_report.c \
	  $(EXPLORE_LIB_SRC) $(LIB) $(LDLIBS) -o $@

# --- M36: bounded exhaustion of the C core, and the tooled half ---
# The harness in host/modelcheck/ drives the REAL core functions over a
# finite, published input space and checks the safety properties on every
# case. Nothing here is added to `all`: the exhaustion is its own target, and
# the CBMC / Frama-C harnesses are checked for syntax even where the tools are
# absent (an absent tool is never a red build).
MODELCHECK_SRC := $(sort $(wildcard host/modelcheck/*.c))
MODELCHECK_BIN := $(BUILD)/relais-exhaustive
MODELCHECK_SAN := $(BUILD)/relais-exhaustive-san
MODELCHECK_CBMC_SRC := $(sort $(wildcard host/modelcheck/cbmc/*.c))
MODELCHECK_WP_SRC := $(sort $(wildcard host/modelcheck/wp/*.c))
MODELCHECK_WP_HDR := host/modelcheck/wp/relais_core_spec.h
MC_CFLAGS := $(CFLAGS) $(CPPFLAGS) -Ihost/modelcheck -Icore/tests
MC_SYNTAX := -std=c11 -fsyntax-only -Wall -Wextra -Werror -Wshadow -Wconversion \
             -Wno-sign-conversion -D_POSIX_C_SOURCE=200809L -Icore/include

$(MODELCHECK_BIN): $(MODELCHECK_SRC) core/tests/test_util.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(MC_CFLAGS) $^ $(LDLIBS) -o $@

# The same exhaustion under AddressSanitizer + UndefinedBehaviorSanitizer: the
# parser and the arbiter must not read or write outside their buffers and must
# not trigger any undefined behaviour. If the sanitizer runtime is missing the
# pass is skipped and says so.
exhaustive: $(MODELCHECK_BIN)
	./$(MODELCHECK_BIN)
	@if printf 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined \
	    -x c - -o $(BUILD)/.sanprobe 2>/dev/null; then \
	  echo "== M36: the same exhaustion under ASan + UBSan =="; \
	  $(CC) $(MC_CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
	    $(MODELCHECK_SRC) core/tests/test_util.c $(LIB) $(LDLIBS) -o $(MODELCHECK_SAN) \
	    && ./$(MODELCHECK_SAN); \
	else echo "MC-SANITIZER absent on this machine: the ASan/UBSan pass is skipped"; fi

# The tooled half: the CBMC harnesses (one file per property) and the
# Frama-C/WP contracts. The harnesses are always compiled here, so a machine
# without the tools still proves that they are valid C11; when a tool is
# present it is run and its verdict published, when it is absent the target
# says exactly what is missing and how to install it.
modelcheck:
	@echo "== M36: syntax of the CBMC harnesses =="
	@for f in $(MODELCHECK_CBMC_SRC); do \
	  $(CC) $(MC_SYNTAX) -Ihost/modelcheck/cbmc $$f || exit 1; \
	  echo "  syntax OK $$f"; \
	done
	@echo "== M36: syntax of the Frama-C/WP harnesses and contracts =="
	@for f in $(MODELCHECK_WP_SRC) $(MODELCHECK_WP_HDR); do \
	  $(CC) $(MC_SYNTAX) -Wno-unused-parameter -Ihost/modelcheck/wp $$f || exit 1; \
	  echo "  syntax OK $$f"; \
	done
	@echo "== M36: CBMC =="
	@if command -v cbmc >/dev/null 2>&1; then \
	  sh host/modelcheck/cbmc/run.sh; \
	else \
	  echo "CBMC ABSENT on this machine: the harnesses are ready, nothing is proved here."; \
	  echo "  install: sudo apt-get install cbmc  (or a release of https://www.cprover.org/cbmc/)"; \
	  echo "  then   : sh host/modelcheck/cbmc/run.sh"; \
	fi
	@echo "== M36: Frama-C / WP =="
	@if command -v frama-c >/dev/null 2>&1; then \
	  sh host/modelcheck/wp/run.sh; \
	else \
	  echo "frama-c ABSENT on this machine: the contracts and harnesses are ready."; \
	  echo "  install: sudo apt-get install frama-c-base  (or opam install frama-c)"; \
	  echo "  then   : sh host/modelcheck/wp/run.sh"; \
	fi
	@echo "MC-MODELCHECK-OK"

# --- WCET measured on a target-like machine (mission M40) -------------------
# APPEND-ONLY block: every line below was added at the end of this Makefile, no
# existing line was rewritten. host/wcet/probe.c is the portable in-process
# measurement of the critical path "STATUT received -> ORDRE emitted"
# (clock_gettime CLOCK_MONOTONIC, same source for gcc and qcc); measure.py
# drives it, host/timing/rta.py is reused for the response-time analysis and
# target-report.py publishes one status per target, BLOCKED when not measured.
WCET_DIR := host/wcet
WCET_SRC := $(WCET_DIR)/probe.c $(WCET_DIR)/wcet_stats.c
WCET_TEST_SRC := $(WCET_DIR)/test_wcet_stats.c
WCET_SAMPLES ?= 20000
WCET_WARMUP ?= 500
WCET_RUNS ?= 2
WCET_MODES ?= none,cpu,journal,net
WCET_CPU ?=
WCET_OUT ?= runs/wcet
# Extra flags for host/wcet/measure.py, e.g. WCET_EXTRA=--no-keep-samples
WCET_EXTRA ?=

# --- Golden traces (mission M35) ---
# The behavioural fingerprint of the core: a FROZEN game of scenarios is
# replayed by the seeded simulator and every normalised journal is hashed
# (SHA-256). `golden-verify` is the guard: it must stay green on every pull
# request. `golden` and `golden-update` REWRITE the versioned reference
# tools/golden/traces.json: they are deliberate acts (docs/GOLDEN.md).
golden: $(BUILD)/relais-simulate
	@echo "capture: the reference tools/golden/traces.json is rewritten"
	python3 tools/golden/capture.py

golden-verify: $(BUILD)/relais-simulate
	python3 tools/golden/verify.py

golden-update: $(BUILD)/relais-simulate
	@echo "deliberate refresh: review the diff of tools/golden/traces.json"
	python3 tools/golden/verify.py --update

# --- Diversity bench (mission M37): common causes, real build variants, design diversity ---
# Builds every variant of the core this machine can produce (each with its exact
# compile line and its binary digest), plays one corpus on all of them and compares
# every decision of every tick bit for bit, then measures what a design-diverse
# control law costs at the handover (CMD-09, DEG-08). Curated numbers: docs/CCF.md;
# raw artefacts: build/diversity/. Manual or nightly target: it is NOT a PR job, so
# the 5 minute CI budget of the other jobs is untouched.
DIVERSITY_DIR := $(BUILD)/diversity
DIVERSITY_JOBS ?= 3

# --- Mission M30: the faithful rack model, the observer, the adaptive law ---
# APPEND-ONLY block: nothing above this line was modified, so the thirteen
# missions that integrate one after another do not conflict here. The new
# files in core/src/ and core/tests/ are picked up by the wildcards of CORE_SRC
# and TEST_SRC above; only the host bench needs a rule of its own.
PHYSICS_SRC := $(sort $(wildcard host/physics/*.c))
PHYSICS_BIN := $(BUILD)/relais-physics

# --- M21: keys, anti-replay and the chained journal (SEC-07, ideas 31-34) ---
# Appended at the end and never edited above: thirteen missions integrate one
# after another, so an existing line is never rewritten here.
#
# The simulator and the protocol-level tests write their journal through the
# shared chained writer of actuator/journal_writer.c, so one implementation
# serves the PC actuator, the QNX actuator and the simulator. The explicit
# prerequisites keep make aware of a source the wildcards above do not list.
SIM_SRC += actuator/journal_writer.c
SIMULATE_SRC += actuator/journal_writer.c
$(BUILD)/relais-simulate $(BUILD)/test_protocol $(BUILD)/test_law $(BUILD)/test_ota: \
    actuator/journal_writer.c actuator/journal_writer.h

# --- M20: byzantine frame injector and attack bench (host/byzantine) ---
# Append-only block: the unit test core/tests/test_byzantine.c is already picked
# up by the TEST_BIN wildcard above, so only the standalone bench is added here.
BYZ_SRC := host/byzantine/inject.c host/byzantine/run_byzantine.c

# --- M02: latency measurement (host/measure) — APPENDED, nothing above is
# rewritten. Prerequisite lists are expanded when make reads them, so the new
# binaries are hooked onto the existing targets by appended prerequisite lines;
# the recipes below see the accumulated NODE_SRC / CPPFLAGS / TEST_BIN.
LATENCY_SRC      := host/measure/latency.c host/measure/latency_json.c
LATENCY_TEST_SRC := host/measure/test_latency.c
LATENCY_TOOL_SRC := host/measure/relais-latency.c
LATENCY_CAMPAIGN := host/measure/run_latency_campaign.py
NODE_SRC  += $(LATENCY_SRC)
CPPFLAGS  += -Ihost/measure

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
     ports ports-zephyr ports-freertos ports-rust ports-compilers fuzz fuzz-long fuzz-report \
     demo-policy latency byzantine physics diversity diversity-variants diversity-compare \
     diversity-laws impair golden golden-verify golden-update wcet wcet-probe wcet-test wcet-isolate \
     wcet-report wcet-portable fmeda induction

# The induction engine, faced with the mutants: it must NEVER certify the
# property of a defective core. The proof has to bite on the real core, not
# only on the synthetic witnesses of test_induction.
induction-mutant: mutants
	@fail=0; for m in 1 2 3 4 5 6 7 8; do \
	  $(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/explore host/explore/induction_main.c \
	    host/explore/induction.c host/explore/induction_report.c $(EXPLORE_LIB_SRC) \
	    $(BUILD)/m$$m/librelais.a $(LDLIBS) -o $(BUILD)/m$$m/relais-induction || fail=1; \
	  out=$$(./$(BUILD)/m$$m/relais-induction --k 10 --budget 50000 --classes 100000 \
	    --faults 1 2>/dev/null); \
	  case "$$out" in \
	    *"PROVED for every depth"*) echo "M$$m: CERTIFIED (BUG!)"; fail=1;; \
	    *) echo "M$$m: not certified";; esac; \
	done; [ $$fail -eq 0 ] && echo "INDUCTION-MUTANT-OK" || \
	  (echo "INDUCTION-MUTANT-ECHEC"; exit 1)

.PHONY: induction-mutant
fmeda:
	python3 host/fmeda/fmeda.py --out docs/FMEDA.md --csv docs/FMEDA.csv
	python3 -m unittest discover -s host/fmeda -p 'test_*.py'
impair: $(BUILD)/relais-actuator $(BUILD)/relais-node $(BUILD)/relais-verify
	python3 host/impair/run_impaired.py --out $(BUILD)/impair
diversity: diversity-variants diversity-compare diversity-laws

diversity-variants:
	host/diversity/build_variants.sh --jobs $(DIVERSITY_JOBS) --out $(DIVERSITY_DIR)

diversity-compare: diversity-variants
	python3 host/diversity/compare.py --out-dir $(DIVERSITY_DIR)

diversity-laws: diversity-variants
	python3 host/diversity/law_effect.py --out-dir $(DIVERSITY_DIR)
physics: $(PHYSICS_BIN)
	python3 host/physics/epsilon_study.py
byzantine: $(BUILD)/run_byzantine
	./$(BUILD)/run_byzantine
latency: $(BUILD)/relais-actuator $(BUILD)/relais-node $(BUILD)/relais-latency
	python3 $(LATENCY_CAMPAIGN) --out runs/latency

all: $(CAN_TEST_BIN) $(UDP_CHECK_BIN) $(TRANSPORT_OBJ) $(BUILD)/test_latency $(BUILD)/relais-latency \
     $(BUILD)/run_byzantine $(PHYSICS_BIN) $(BUILD)/relais-wcet-probe $(BUILD)/test_wcet_stats \
     $(BUILD)/relais-induction $(BUILD)/test_induction

induction: $(BUILD)/relais-induction $(BUILD)/test_induction
	@./$(BUILD)/test_induction
	@./$(BUILD)/relais-induction --k 12 --budget 100000 --classes 60000 --faults 0
	@./$(BUILD)/relais-induction --k 12 --budget 100000 --classes 100000 --faults 1
	@./$(BUILD)/relais-induction --witness --k 8 --faults 1

$(PHYSICS_BIN): $(PHYSICS_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PHYSICS_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/run_byzantine: $(BYZ_SRC) host/byzantine/inject.h $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/byzantine $(BYZ_SRC) $(LIB) $(LDLIBS) -o $@
test: $(BUILD)/test_latency $(BUILD)/test_wcet_stats
ALL_TEST_BIN += $(BUILD)/test_wcet_stats

$(BUILD)/relais-wcet-probe: $(WCET_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(WCET_DIR) $(WCET_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/test_wcet_stats: $(WCET_TEST_SRC) $(WCET_DIR)/wcet_stats.c \
    core/tests/test_util.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(WCET_DIR) $^ $(LDLIBS) -o $@

wcet-probe: $(BUILD)/relais-wcet-probe

# The machine preparation, on its own: prints what it could do without root and
# what stays BLOCKED (governor, turbo, isolcpus, huge pages, SCHED_FIFO).
wcet-isolate:
	sh $(WCET_DIR)/isolate.sh $(if $(WCET_CPU),--cpu $(WCET_CPU))

# The strongest portability proof available with no QNX SDP and no Pi: the probe
# is C11 with -pedantic and _POSIX_C_SOURCE only (no GNU extension, no Linux or
# QNX header), so the SAME source is what qcc and the Pi's gcc compile. This
# proves the source is portable; it does NOT prove a timing number on the target.
wcet-portable: $(WCET_SRC) $(WCET_TEST_SRC)
	$(CC) -std=c11 -pedantic -Wall -Wextra -Werror -Wshadow -Wconversion \
	  -Wno-sign-conversion -O2 -D_POSIX_C_SOURCE=200809L \
	  -Icore/include -Icore/tests -I$(WCET_DIR) -fsyntax-only $^
	@echo "WCET-PORTABLE-OK c11 -pedantic, POSIX 200809 only (no GNU extension)"

wcet-test: $(BUILD)/test_wcet_stats
	./$(BUILD)/test_wcet_stats
	python3 -m unittest discover -s $(WCET_DIR) -p 'test_*.py'

wcet-report:
	@mkdir -p $(WCET_OUT)
	python3 $(WCET_DIR)/target-report.py --out $(WCET_OUT)/TARGETS.md

wcet: wcet-probe wcet-portable wcet-test
	python3 $(WCET_DIR)/measure.py --samples $(WCET_SAMPLES) --warmup $(WCET_WARMUP) \
	  --runs $(WCET_RUNS) --interference $(WCET_MODES) --lock \
	  $(if $(WCET_CPU),--cpu $(WCET_CPU)) $(WCET_EXTRA)
	python3 $(WCET_DIR)/rta.py --sweep
	$(MAKE) wcet-report
$(BUILD)/relais-node: $(LATENCY_SRC)

$(BUILD)/test_latency: $(LATENCY_TEST_SRC) $(LATENCY_SRC) core/tests/test_util.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LATENCY_TEST_SRC) $(LATENCY_SRC) \
	  core/tests/test_util.c $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-latency: $(LATENCY_TOOL_SRC) $(LATENCY_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LATENCY_TOOL_SRC) $(LATENCY_SRC) $(LIB) \
	  $(LDLIBS) -o $@

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
