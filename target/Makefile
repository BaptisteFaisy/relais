# Relais — the QNX 8.0 target layer.
#
# Two builds, two truths:
#   make            the real one, with the SDP 8.0 (qcc -V gcc_ntox86_64).
#   make syntax     what a PC without the SDP can prove: every file compiles
#                   against the stubs of host_stub.h. This is what CI runs.
#
# All protocol logic comes from ../core: nothing here is re-implemented.

QCC      ?= qcc
QCCFLAGS ?= -V gcc_ntox86_64 -std=c11 -O2 -Wall -Wextra -Werror
CORE     := ../core
CORE_SRC := $(wildcard $(CORE)/src/*.c)
INCLUDES := -I$(CORE)/include -Iinclude
LDFLAGS  ?= -lsocket -lm

BINS := relais-net relais-core relais-ctl relaisctl relais-actuator
SRC_net       := net.c qnx_util.c
SRC_core      := core.c qnx_util.c
SRC_ctl       := ctl.c qnx_util.c
SRC_relaisctl := relaisctl.c qnx_util.c
SRC_actuator  := actuator_qnx.c qnx_util.c

STUBFLAGS := -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion \
             -Wno-sign-conversion -D_POSIX_C_SOURCE=200809L -DRELAIS_HOST_STUB \
             -I$(CORE)/include -Iinclude
SOURCES := $(wildcard *.c) $(wildcard include/*.h) host_stub.h

.PHONY: all syntax clean qnx

all: qnx

qnx: $(addprefix build/,$(BINS))

build/relais-net: $(SRC_net) $(CORE_SRC)
	@mkdir -p build
	$(QCC) $(QCCFLAGS) $(INCLUDES) $(SRC_net) $(CORE_SRC) $(LDFLAGS) -o $@

build/relais-core: $(SRC_core) $(CORE_SRC)
	@mkdir -p build
	$(QCC) $(QCCFLAGS) $(INCLUDES) $(SRC_core) $(CORE_SRC) $(LDFLAGS) -o $@

build/relais-ctl: $(SRC_ctl) $(CORE_SRC)
	@mkdir -p build
	$(QCC) $(QCCFLAGS) $(INCLUDES) $(SRC_ctl) $(CORE_SRC) $(LDFLAGS) -o $@

build/relaisctl: $(SRC_relaisctl) $(CORE_SRC)
	@mkdir -p build
	$(QCC) $(QCCFLAGS) $(INCLUDES) $(SRC_relaisctl) $(CORE_SRC) $(LDFLAGS) -o $@

build/relais-actuator: $(SRC_actuator) $(CORE_SRC)
	@mkdir -p build
	$(QCC) $(QCCFLAGS) $(INCLUDES) $(SRC_actuator) $(CORE_SRC) $(LDFLAGS) -o $@

# The only proof a machine without the SDP can produce, and the one CI runs.
syntax:
	@fail=0; for f in $(wildcard *.c); do \
	  if gcc -fsyntax-only $(STUBFLAGS) $$f; then echo "  ok   $$f"; \
	  else echo "  FAIL $$f"; fail=1; fi; done; \
	for f in $(wildcard include/*.h) host_stub.h; do \
	  if gcc -fsyntax-only $(STUBFLAGS) -x c $$f; then echo "  ok   $$f"; \
	  else echo "  FAIL $$f"; fail=1; fi; done; \
	[ $$fail -eq 0 ] || (echo "SYNTAX-FAIL"; exit 1); echo "SYNTAX-OK"

clean:
	rm -rf build
