# Relais — the Zephyr port (mission M32, idea 73).
#
#   make check          honest verdict: builds with west when the SDK is there,
#                       otherwise reports what is missing and exits 0
#   make check BOARD=qemu_x86
#   make audit          the checks that need no SDK at all
#
# The real build entry point is CMakeLists.txt (west/cmake); this Makefile exists
# so that the port answers the same `make check` as every other port.

BOARD ?= native_posix
ROOT  := ../..

.PHONY: check audit build clean

check:
	./check.sh $(BOARD)

audit:
	$(ROOT)/ports/common/audit.sh .

build:
	west build -b $(BOARD) . --pristine

clean:
	rm -rf build
