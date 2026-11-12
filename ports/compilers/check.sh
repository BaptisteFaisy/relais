#!/usr/bin/env bash
# Relais — portability of the core across compilers and language standards
# (mission M32, ideas 73/74/75).
#
# The claim under test: "core/ is a portable C11 library with no clock, no file,
# no socket and no allocation, compiled three times". This script proves as much
# of it as THIS machine can prove, and says plainly what it cannot:
#
#   * every C compiler that is installed compiles every file of core/ with the
#     project's strict flags (-Wall -Wextra -Werror -Wshadow -Wconversion);
#   * the same sources compile as strict ISO C11 (-pedantic-errors), as C99,
#     C17 and C2x, and as freestanding (-ffreestanding -nostdlib), which is what
#     an RTOS build without a libc starts from;
#   * g++ accepts them as C++17, so a dissimilar C++ backup can link the core;
#   * core/ is grepped for the four forbidden dependencies.
#
# A compiler that is NOT installed is reported as MISSING and does not fail the
# run: the verdict is honest, not red. Exit status is 0 unless a compiler that
# IS installed produces a warning or an error.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="${OUT:-build/ports-compilers}"
mkdir -p "$OUT"

STRICT="-Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -fno-strict-aliasing"
INC="-Icore/include"
DEFS="-D_POSIX_C_SOURCE=200809L"
CORE_SRC=$(ls core/src/*.c)
N_SRC=$(echo "$CORE_SRC" | wc -l | tr -d ' ')

total_ok=0
total_fail=0
missing=0

# probe <compiler> <label> <extra flags...>
probe() {
  local cc="$1"; shift
  local label="$1"; shift
  if ! command -v "$cc" >/dev/null 2>&1; then
    printf '  MISSING  %-34s (%s not installed here)\n' "$label" "$cc"
    missing=$((missing + 1))
    return 0
  fi
  local ok=0 fail=0 log="$OUT/$(echo "$label" | tr ' /+=' '____').log"
  : > "$log"
  for f in $CORE_SRC; do
    if "$cc" "$@" $STRICT $DEFS $INC -c "$f" -o "$OUT/probe.o" >>"$log" 2>&1; then
      ok=$((ok + 1))
    else
      fail=$((fail + 1))
      echo "    first error in $f:" >>"$log"
    fi
  done
  if [ "$fail" -eq 0 ]; then
    printf '  OK       %-34s %d/%d objects, 0 warning\n' "$label" "$ok" "$N_SRC"
    total_ok=$((total_ok + ok))
  else
    printf '  FAIL     %-34s %d/%d objects (see %s)\n' "$label" "$ok" "$N_SRC" "$log"
    head -12 "$log" | sed 's/^/      /'
    total_fail=$((total_fail + fail))
  fi
  rm -f "$OUT/probe.o"
}

echo "== Relais core: $N_SRC sources, compiler and standard matrix =="

echo "-- gcc, the reference build of the project"
probe gcc "gcc -std=c11 -O2 (reference)" -std=c11 -O2
probe gcc "gcc -std=c11 -pedantic-errors" -std=c11 -O2 -pedantic-errors
probe gcc "gcc -std=c99" -std=c99 -O2
probe gcc "gcc -std=c17" -std=c17 -O2
probe gcc "gcc -std=c2x" -std=c2x -O2
probe gcc "gcc -ffreestanding -nostdlib (no libc)" -std=c11 -O2 -ffreestanding -nostdlib
probe gcc "gcc -O0 -fno-inline" -std=c11 -O0 -fno-inline
probe gcc "gcc -Os (size, embedded default)" -std=c11 -Os

echo "-- other compilers, when they are installed"
probe clang "clang -std=c11" -std=c11 -O2
probe tcc "tcc -std=c11" -std=c11
probe arm-none-eabi-gcc "arm-none-eabi-gcc -std=c11 -mcpu=cortex-m4" \
      -std=c11 -O2 -mcpu=cortex-m4 -ffreestanding
probe riscv64-unknown-elf-gcc "riscv64-unknown-elf-gcc -std=c11" -std=c11 -O2
probe zig "zig cc -std=c11 -target thumb-freestanding-none" \
      cc -std=c11 -O2 -target thumb-freestanding-none

echo "-- the same sources as C++ (a dissimilar backup may be C++)"
if command -v g++ >/dev/null 2>&1; then
  ok=0; fail=0; log="$OUT/cxx17.log"; : > "$log"
  for f in $CORE_SRC; do
    if g++ -x c++ -std=c++17 -O2 $STRICT $DEFS $INC -c "$f" -o "$OUT/probe.o" >>"$log" 2>&1; then
      ok=$((ok + 1)); else fail=$((fail + 1)); fi
  done
  if [ "$fail" -eq 0 ]; then
    printf '  OK       %-34s %d/%d objects, 0 warning\n' "g++ -std=c++17" "$ok" "$N_SRC"
    total_ok=$((total_ok + ok))
  else
    printf '  FAIL     %-34s %d/%d objects (see %s)\n' "g++ -std=c++17" "$ok" "$N_SRC" "$log"
    head -12 "$log" | sed 's/^/      /'
    total_fail=$((total_fail + fail))
  fi
  rm -f "$OUT/probe.o"
else
  printf '  MISSING  %-34s (g++ not installed here)\n' "g++ -std=c++17"
  missing=$((missing + 1))
fi

echo "-- purity of core/: the four forbidden dependencies"
# Word boundaries matter: relais_inv_fault_free() is not free(), and runtime()
# is not time(). Without them this check cries wolf on the core's own names.
forbidden='\b(malloc|calloc|realloc|free|fopen|fclose|fwrite|fprintf|socket|sendto|recvfrom|bind|time|clock_gettime|gettimeofday|localtime|rand)[[:space:]]*\('
hits=$(grep -nE "$forbidden" core/src/*.c core/include/relais/*.h | grep -v '^\s*\*' | wc -l | tr -d ' ')
if [ "$hits" -eq 0 ]; then
  echo "  OK       no allocation, no file, no socket, no clock in core/ (0 hit)"
else
  echo "  FAIL     $hits hit(s) of a forbidden dependency in core/:"
  grep -nE "$forbidden" core/src/*.c core/include/relais/*.h | head -8 | sed 's/^/      /'
  total_fail=$((total_fail + hits))
fi

echo "-- what this machine cannot prove"
echo "  QNX 8.0 (qcc)          : $(command -v qcc >/dev/null 2>&1 && echo present || echo 'MISSING - target/Makefile syntax mode is the fallback')"
echo "  Zephyr SDK / west      : $(command -v west >/dev/null 2>&1 && echo present || echo 'MISSING - see ports/zephyr and docs/PORT-ZEPHYR.md')"
echo "  FreeRTOS kernel source : $([ -d ports/freertos/FreeRTOS-Kernel ] && echo present || echo 'MISSING - see ports/freertos and docs/PORT-FREERTOS.md')"
echo "  CAN interface (vcan0)  : $(ip link add dev rl_probe type vcan 2>/dev/null && ip link del dev rl_probe 2>/dev/null && echo present || echo 'MISSING - needs CAP_NET_ADMIN; SocketCAN backend compiled, not run')"
echo "  cargo / rustc          : $(command -v cargo >/dev/null 2>&1 && cargo --version || echo 'MISSING - see ports/rust and docs/PORT-RUST.md')"

echo "== summary: $total_ok object compilations OK, $total_fail failed, $missing tool(s) missing =="
if [ "$total_fail" -ne 0 ]; then
  echo "COMPILERS-FAILED"
  exit 1
fi
echo "COMPILERS-OK (missing tools reported above, not counted as failures)"
exit 0
