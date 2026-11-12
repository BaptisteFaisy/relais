# Relais — the FreeRTOS port (mission M32, idea 73).
#
#   make check    honest verdict: configures and builds when a kernel checkout
#                 and a cross toolchain are present, otherwise reports what is
#                 missing and exits 0
#   make audit    the checks that need no kernel at all
#
# The real build entry point is CMakeLists.txt; this Makefile exists so that the
# port answers the same `make check` as every other port.

ROOT := ../..

.PHONY: check audit build clean

check:
	./check.sh

audit:
	$(ROOT)/ports/common/audit.sh .

build:
	cmake -B build -DFREERTOS_KERNEL_PATH=$(FREERTOS_KERNEL_PATH)
	cmake --build build -j 4

clean:
	rm -rf build
