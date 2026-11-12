#!/usr/bin/env bash
# Relais — conditional build check of the Zephyr port (mission M32, idea 73).
#
# HONEST VERDICT: when the Zephyr SDK is installed, this really builds the image
# for the requested board and reports the result. When it is not — which is the
# case on the machine this port was written on — it says exactly what is missing,
# runs the static audit that does not need the SDK, and exits 0. A missing
# toolchain is not a red build.
#
# Usage: ports/zephyr/check.sh [board]     (default board: native_posix)
set -u

BOARD="${1:-${BOARD:-native_posix}}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

echo "== Zephyr port check (board: $BOARD) =="

missing=""
command -v west >/dev/null 2>&1 || missing="$missing west(zephyr build tool)"
[ -n "${ZEPHYR_BASE:-}" ] || missing="$missing ZEPHYR_BASE"
command -v cmake >/dev/null 2>&1 || missing="$missing cmake"
command -v ninja >/dev/null 2>&1 || missing="$missing ninja"
command -v dtc >/dev/null 2>&1 || missing="$missing dtc(device tree compiler)"
if [ -n "${ZEPHYR_SDK_INSTALL_DIR:-}" ]; then
  echo "  found  ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR"
else
  missing="$missing ZEPHYR_SDK_INSTALL_DIR(the Zephyr SDK itself)"
fi

# What can always be checked, SDK or not.
"$ROOT/ports/common/audit.sh" "$HERE" || exit 1

if [ -n "$missing" ]; then
  echo "  ZEPHYR-BUILD-SKIPPED: missing$missing"
  echo "  To build for real (docs/PORT-ZEPHYR.md has the full procedure):"
  echo "    pip install west && west init -l zephyrproject && west update"
  echo "    west build -b $BOARD $HERE --pristine"
  echo "  Delivered but NOT compiled here: src/main.c, CMakeLists.txt, prj.conf,"
  echo "  Kconfig, app.overlay, boards/qemu_x86.overlay, boards/native_posix.overlay"
  exit 0
fi

echo "-- building with west for $BOARD"
if west build -b "$BOARD" "$HERE" --pristine -- -DCMAKE_VERBOSE_MAKEFILE=ON; then
  size=$(stat -c %s build/zephyr/zephyr.elf 2>/dev/null || echo "?")
  echo "  ZEPHYR-BUILD-OK ($BOARD, zephyr.elf $size bytes)"
  exit 0
fi
echo "  ZEPHYR-BUILD-FAILED for $BOARD: the log above is the real error"
exit 1
