# Relais — fuzzing and sanitizers (ROB-09, SEC-01, SEC-02, MAN-11).
#
# Three targets: the wire parser, the journal parsers/formatters, and the
# arbiter driven by a script of event choices. With clang they are built for
# libFuzzer (coverage guided); without clang — the usual case on this machine —
# the same LLVMFuzzerTestOneInput entry points are driven by host/fuzz's own
# seeded generator under gcc + ASAN/UBSAN, and the Makefile says so loudly.
#
# make fuzz       : bounded budget per target (FUZZ_BUDGET, default 60 s for CI)
# make fuzz-long  : the 24 h of ROB-09 (FUZZ_LONG, default 86400 s), out of CI
# make corpus     : regenerate the seeds from the real encoder and simulator
# make report     : aggregate the runs into docs/RESULTS.md (tools/fuzz-report.py)

SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c

ROOT        := ../..
CC          ?= gcc
BUILD       := $(ROOT)/build/fuzz
LIB         := $(ROOT)/build/librelais.a
SIMULATE    := $(ROOT)/build/relais-simulate
CORPUS      := corpus
CRASHES     := $(CORPUS)/crashes
LOGS        := logs
LDLIBS      := -lm

CFLAGS   := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion \
            -Wno-sign-conversion -fno-strict-aliasing
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -I$(ROOT)/core/include -I.
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=undefined

TARGETS  := fuzz_frame fuzz_journal fuzz_arbiter
BINS     := $(addprefix $(BUILD)/,$(TARGETS))

FUZZ_BUDGET ?= 60
FUZZ_LONG   ?= 86400
SEED        ?= 20261111
JOBS        ?= 1

# Per target: how long an input may be, and where its seeds live.
MAXLEN_fuzz_frame    := 1024
MAXLEN_fuzz_journal  := 1024
MAXLEN_fuzz_arbiter  := 2048
CORPUSDIR_fuzz_frame   := $(CORPUS)/frame
CORPUSDIR_fuzz_journal := $(CORPUS)/journal
CORPUSDIR_fuzz_arbiter := $(CORPUS)/arbiter

# Is clang with libFuzzer available? One probe compile decides, no guess.
CLANG := $(shell command -v clang 2>/dev/null)
HAVE_LIBFUZZER := $(shell \
  mkdir -p $(BUILD) 2>/dev/null; \
  if [ -n "$(CLANG)" ]; then \
    printf 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n){(void)d;(void)n;return 0;}\n' \
      > $(BUILD)/.probe.c; \
    if $(CLANG) -fsanitize=fuzzer,address,undefined $(BUILD)/.probe.c \
         -o $(BUILD)/.probe >/dev/null 2>&1; then echo yes; else echo no; fi; \
  else echo no; fi)

ifeq ($(HAVE_LIBFUZZER),yes)
  FUZZ_CC  := $(CLANG)
  FUZZ_SAN := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
  DRIVER   :=
  BACKEND  := libfuzzer
else
  FUZZ_CC  := $(CC)
  FUZZ_SAN := $(SAN)
  DRIVER   := fuzz_driver.c
  BACKEND  := homegrown
endif

.PHONY: all fuzz fuzz-long corpus report clean banner
all: $(BINS) $(BUILD)/gen_corpus

banner:
	@echo "fuzz backend: $(BACKEND) (clang=$(if $(CLANG),$(CLANG),absent), libfuzzer=$(HAVE_LIBFUZZER))"
	@if [ "$(BACKEND)" = "homegrown" ]; then \
	  echo "NOTE: clang/libFuzzer is not available here — falling back to the"; \
	  echo "      home-grown seeded generator with $(CC) $(SAN)."; \
	  echo "      Install clang to get coverage-guided fuzzing (docs/FUZZING.md)."; \
	fi

$(LIB):
	@$(MAKE) --no-print-directory -C $(ROOT) build/librelais.a

$(SIMULATE):
	@$(MAKE) --no-print-directory -C $(ROOT) build/relais-simulate

$(BUILD)/gen_corpus: gen_corpus.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/fuzz_%: fuzz_%.c $(DRIVER) fuzz_util.h fuzz_script.h $(LIB)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(CFLAGS) $(CPPFLAGS) $(FUZZ_SAN) $< $(DRIVER) $(LIB) $(LDLIBS) -o $@

# Seeds: real frames from the shared encoder, real journal lines from a real
# simulator run, and arbiter scripts in the documented format.
corpus: $(BUILD)/gen_corpus $(SIMULATE)
	@mkdir -p $(BUILD) $(CRASHES)
	$(SIMULATE) --ticks 400 --seed 1007 --journal $(BUILD)/sim-journal.jsonl --quiet
	$(BUILD)/gen_corpus $(CORPUS) --journal $(BUILD)/sim-journal.jsonl
	@echo "corpus: $$(find $(CORPUS) -type f -not -path '$(CRASHES)*' | wc -l) seeds"

# One run of one target. A crash keeps its input under corpus/crashes/ and fails
# the build, so CI cannot miss it.
define run_target
	@mkdir -p $(LOGS) $(CRASHES) $(BUILD)
	@echo "--- $(1): budget $(2) s, max_len $(MAXLEN_$(1)), corpus $(CORPUSDIR_$(1))"
	@rc=0; \
	if [ "$(BACKEND)" = "libfuzzer" ]; then \
	  $(BUILD)/$(1) -max_total_time=$(2) -max_len=$(MAXLEN_$(1)) \
	    -dict=$(CURDIR)/frame.dict -artifact_prefix=$(CRASHES)/ -print_final_stats=1 \
	    $(CORPUSDIR_$(1)) > $(LOGS)/$(1).log 2>&1 || rc=$$?; \
	else \
	  $(BUILD)/$(1) --budget $(2) --seed $(SEED) --max-len $(MAXLEN_$(1)) \
	    --corpus $(CORPUSDIR_$(1)) --crash-file $(BUILD)/$(1)-crash.bin \
	    > $(LOGS)/$(1).log 2>&1 || rc=$$?; \
	fi; \
	tail -n 12 $(LOGS)/$(1).log; \
	if [ $$rc -ne 0 ]; then \
	  stamp=$$(date +%Y%m%d-%H%M%S); \
	  if [ -s $(BUILD)/$(1)-crash.bin ]; then \
	    cp $(BUILD)/$(1)-crash.bin $(CRASHES)/crash-$(1)-$$stamp.bin; \
	  fi; \
	  for f in $(CRASHES)/crash-* $(CRASHES)/leak-* $(CRASHES)/oom-* $(CRASHES)/timeout-*; do \
	    [ -e "$$f" ] && echo "kept case: $$f"; done 2>/dev/null; \
	  echo "FUZZ-CRASH target=$(1) exit=$$rc log=$(LOGS)/$(1).log"; \
	  exit 1; \
	fi; \
	echo "fuzz $(1): no crash (log $(LOGS)/$(1).log)"
endef

fuzz: banner all corpus
	$(call run_target,fuzz_frame,$(FUZZ_BUDGET))
	$(call run_target,fuzz_journal,$(FUZZ_BUDGET))
	$(call run_target,fuzz_arbiter,$(FUZZ_BUDGET))
	@echo "FUZZ-OK backend=$(BACKEND) budget=$(FUZZ_BUDGET)s targets=3 crashes=0"

# ROB-09: 24 h of hostile data, out of CI. Parallel jobs use distinct seeds.
fuzz-long: banner all corpus
	@mkdir -p $(LOGS) $(CRASHES) $(BUILD)
	@echo "fuzz-long: $(FUZZ_LONG) s per target, backend $(BACKEND), jobs $(JOBS)"
	@for t in $(TARGETS); do \
	  case $$t in \
	    fuzz_frame)   cdir=$(CORPUSDIR_fuzz_frame);   mlen=$(MAXLEN_fuzz_frame);; \
	    fuzz_journal) cdir=$(CORPUSDIR_fuzz_journal); mlen=$(MAXLEN_fuzz_journal);; \
	    fuzz_arbiter) cdir=$(CORPUSDIR_fuzz_arbiter); mlen=$(MAXLEN_fuzz_arbiter);; \
	  esac; \
	  if [ "$(BACKEND)" = "libfuzzer" ]; then \
	    $(BUILD)/$$t -max_total_time=$(FUZZ_LONG) -max_len=$$mlen -dict=frame.dict \
	      -artifact_prefix=$(CRASHES)/ -jobs=$(JOBS) $$cdir \
	      > $(LOGS)/$$t-long.log 2>&1 & \
	  else \
	    $(BUILD)/$$t --budget $(FUZZ_LONG) --seed $$(( $(SEED) + 1 )) \
	      --max-len $$mlen --corpus $$cdir \
	      --crash-file $(BUILD)/$$t-crash.bin > $(LOGS)/$$t-long.log 2>&1 & \
	  fi; \
	done; \
	fail=0; for j in $$(jobs -p); do wait $$j || fail=1; done; \
	grep -h FUZZ-SUMMARY $(LOGS)/*-long.log 2>/dev/null || true; \
	[ $$fail -eq 0 ] && echo "FUZZ-LONG-OK backend=$(BACKEND) budget=$(FUZZ_LONG)s" || \
	  { echo "FUZZ-LONG-CRASH: see $(LOGS) and $(CRASHES)"; exit 1; }

report:
	python3 $(ROOT)/tools/fuzz-report.py --logs $(LOGS) --crashes $(CRASHES) \
	  --corpus $(CORPUS) --bin $(BUILD) --backend $(BACKEND) \
	  --results $(ROOT)/docs/RESULTS.md

clean:
	rm -rf $(BUILD) $(LOGS)
