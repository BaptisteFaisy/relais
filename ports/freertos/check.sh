#!/usr/bin/env bash
# Relais — conditional build check of the FreeRTOS port (mission M32, idea 73).
#
# HONEST VERDICT: when a FreeRTOS kernel checkout and a cross toolchain are
# available, this really configures and builds the image. When they are not —
# which is the case on the machine this port was written on — it says exactly
# what is missing, runs the static audit that needs no kernel, and exits 0.
#
# Usage: ports/freertos/check.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
KERNEL="${FREERTOS_KERNEL_PATH:-}"

echo "== FreeRTOS port check =="

# A kernel checkout next to the repository is accepted, so the common developer
# layout works without an environment variable.
if [ -z "$KERNEL" ] && [ -d "$ROOT/../FreeRTOS-Kernel/tasks.c" -o -f "$ROOT/../FreeRTOS-Kernel/tasks.c" ]; then
  KERNEL="$(cd "$ROOT/../FreeRTOS-Kernel" && pwd)"
fi

missing=""
[ -n "$KERNEL" ] || missing="$missing FreeRTOS-Kernel(FREERTOS_KERNEL_PATH)"
command -v cmake >/dev/null 2>&1 || missing="$missing cmake"
command -v arm-none-eabi-gcc >/dev/null 2>&1 || missing="$missing arm-none-eabi-gcc"

# What can always be checked, kernel or not.
"$ROOT/ports/common/audit.sh" "$HERE" || exit 1

if [ -n "$missing" ]; then
  echo "  FREERTOS-BUILD-SKIPPED: missing$missing"
  echo "  To build for real (docs/PORT-FREERTOS.md has the full procedure):"
  echo "    git clone https://github.com/FreeRTOS/FreeRTOS-Kernel"
  echo "    cmake -B build -DFREERTOS_KERNEL_PATH=\$PWD/FreeRTOS-Kernel \\"
  echo "          -DFREERTOS_PORT=GCC_ARM_CM4F -DCMAKE_TOOLCHAIN_FILE=<your-arm.cmake>"
  echo "    cmake --build build"
  echo "  Delivered but NOT compiled here: src/main.c, src/assert.c, FreeRTOSConfig.h,"
  echo "  CMakeLists.txt"
  exit 0
fi

echo "-- configuring with the kernel at $KERNEL"
if cmake -B "$HERE/build" -S "$HERE" -DFREERTOS_KERNEL_PATH="$KERNEL" >/dev/null \
   && cmake --build "$HERE/build" -j 4; then
  echo "  FREERTOS-BUILD-OK"
  exit 0
fi
echo "  FREERTOS-BUILD-FAILED: the log above is the real error"
exit 1
