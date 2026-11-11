# Relais — one build for the PC: core library, unit tests, explorer, simulator,
# actuator, verifier, mutants. The QNX build lives in target/ and actuator/qnx.
#
# Budget: every CI job stays under 5 minutes (user rule).

CC       ?= gcc
AR       ?= ar
BUILD    := build
CFLAGS   ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion \
            -Wno-sign-conversion -fno-strict-aliasing
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Icore/include -Icore/tests -MMD -MP
LDLIBS   += -lm

CORE_SRC := $(sort $(wildcard core/src/*.c))
CORE_OBJ := $(patsubst core/src/%.c,$(BUILD)/core/%.o,$(CORE_SRC))
TEST_SRC := $(filter-out core/tests/test_util.c,$(sort $(wildcard core/tests/test_*.c)))
TEST_FIXTURES := $(sort $(wildcard core/tests/fixtures_*.c))
TEST_BIN := $(patsubst core/tests/%.c,$(BUILD)/%,$(TEST_SRC))
EXPLORE_SRC := $(sort $(wildcard host/explore/*.c))
# The explorer is only built once host/explore holds sources: until then the
# proof of level 1 lives in the seeded simulator, and `make all` stays green.
EXPLORE_BIN := $(if $(EXPLORE_SRC),$(BUILD)/relais-explore)
SIMULATE_SRC := $(sort $(wildcard host/simulate/*.c))
ACTUATOR_SRC := $(sort $(wildcard actuator/pc/*.c))
VERIFY_SRC := $(sort $(wildcard host/verify/*.c))
NODE_SRC := $(sort $(wildcard host/node/*.c))
LIB := $(BUILD)/librelais.a

MUTANTS := 1 2 3 4 5 6 7 8

.PHONY: all test explore simulate verify mutants report demo campaign clean size
all: $(LIB) $(TEST_BIN) $(EXPLORE_BIN) $(BUILD)/relais-simulate \
     $(BUILD)/relais-actuator $(BUILD)/relais-verify $(BUILD)/relais-node

$(LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/core/%.o: core/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# The protocol-level tests drive the simulator world: they link the same
# pipeline instead of re-writing one (no duplication between test and bench).
SIM_SRC := host/simulate/sim.c host/simulate/sim_faults.c
$(BUILD)/test_protocol $(BUILD)/test_law: $(BUILD)/test_%: \
    core/tests/test_%.c core/tests/test_util.c $(SIM_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ihost/simulate $< core/tests/test_util.c \
	  $(SIM_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/test_%: core/tests/test_%.c core/tests/test_util.c $(TEST_FIXTURES) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< core/tests/test_util.c $(TEST_FIXTURES) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-explore: $(EXPLORE_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(EXPLORE_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-simulate: $(SIMULATE_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SIMULATE_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-actuator: $(ACTUATOR_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(ACTUATOR_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-verify: $(VERIFY_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(VERIFY_SRC) $(LIB) $(LDLIBS) -o $@

$(BUILD)/relais-node: $(NODE_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(NODE_SRC) $(LIB) $(LDLIBS) -o $@

test: $(TEST_BIN)
	@fail=0; for b in $(TEST_BIN); do \
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

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
