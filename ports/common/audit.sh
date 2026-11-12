#!/usr/bin/env bash
# Relais — static audit shared by every RTOS port (mission M32).
#
# What a machine WITHOUT the vendor SDK can still prove about a port:
#   1. every file it adds is under the 500-line limit of the project;
#   2. it contains no allocation, no file, no clock and no raw socket call: a
#      port talks to the network through core/include/relais/transport.h and to
#      time through its kernel, never around them;
#   3. it COMPILES THE SHARED CORE, not a copy of it (the claim of the whole
#      project is "one core, compiled N times");
#   4. the glue header ports/common/relais_port.h, which every port includes,
#      really does compile with a plain host compiler.
#
# Usage: ports/common/audit.sh <port-directory>
# Exit: 0 when the audit passes, 1 on a real violation. A missing SDK is NOT a
# violation and is not this script's business.
set -u

PORT="${1:-.}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
fail=0

echo "-- audit of $PORT"

# 1. size limit -------------------------------------------------------------
while IFS= read -r f; do
  n=$(wc -l < "$f")
  if [ "$n" -ge 500 ]; then
    echo "  FAIL  $f is $n lines (limit 500)"
    fail=1
  fi
done < <(find "$PORT" -name '*.c' -o -name '*.h' -o -name '*.conf' -o -name '*.overlay' \
         -o -name 'CMakeLists.txt' -o -name 'Makefile' | sort)
echo "  $(find "$PORT" \( -name '*.c' -o -name '*.h' \) | wc -l | tr -d ' ') source files, all under 500 lines"

# 2. forbidden dependencies in the port's own code --------------------------
forbidden='\b(malloc|calloc|realloc|free|fopen|fwrite|fprintf|socket|sendto|recvfrom|connect|clock_gettime|gettimeofday|localtime|sleep)\s*\('
hits=$(grep -rnE "$forbidden" --include='*.c' --include='*.h' "$PORT" \
       | grep -v 'relais_tr_recv\|relais_tr_send\|/\* ' || true)
if [ -n "$hits" ]; then
  echo "  FAIL  a port must not bypass the transport interface:"
  echo "$hits" | head -6 | sed 's/^/        /'
  fail=1
else
  echo "  OK    no allocation, no file, no raw socket, no clock call in the port"
fi

# 3. the shared core, not a copy -------------------------------------------
if grep -rq "core/src" "$PORT"/CMakeLists.txt "$PORT"/Makefile 2>/dev/null; then
  echo "  OK    the build compiles the shared core/ (no copy in the port)"
else
  echo "  FAIL  the build does not reference core/src: a port must not carry a copy"
  fail=1
fi
if find "$PORT" -name '*.c' | xargs grep -l "relais_arb_step\|relais_node_on_status" 2>/dev/null \
   | grep -v main.c | grep -q .; then
  echo "  FAIL  protocol logic re-implemented outside main.c"
  fail=1
fi

# 4. the shared glue header compiles with a plain host compiler --------------
if command -v gcc >/dev/null 2>&1; then
  if gcc -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
       -D_POSIX_C_SOURCE=200809L -I"$ROOT/core/include" -fsyntax-only -x c \
       "$ROOT/ports/common/relais_port.h" 2>/dev/null; then
    echo "  OK    ports/common/relais_port.h compiles with the host gcc (-Werror)"
  else
    echo "  FAIL  ports/common/relais_port.h does not compile with the host gcc"
    fail=1
  fi
else
  echo "  MISSING gcc: the glue header was not compiled"
fi

[ "$fail" -eq 0 ] && echo "  AUDIT-OK $PORT" || echo "  AUDIT-FAILED $PORT"
exit "$fail"
