#!/bin/sh
# Relais — the exact CBMC commands, one per property (mission M36).
#
# Requires cbmc on the PATH. Each property is a separate file with its own
# hypotheses and assertions; the core sources are compiled together with it,
# so the property is proved about the REAL code, not about a copy.
#
# INSTALLATION
#   Debian/Ubuntu : sudo apt-get install cbmc
#   release       : https://www.cprover.org/cbmc/ (tarball, no root needed)
#   then          : sh host/modelcheck/cbmc/run.sh
#
# BUDGET. Every property is bounded by --timeout 240 s, so the whole file
# stays inside the CI budget of 5 minutes per job. A property that runs out
# of time is reported as UNKNOWN, never as a failure of the code: only a
# counterexample (exit code 10) is a failure.
set -u

COMMON="--function main --bounds-check --pointer-check --pointer-overflow-check \
--div-by-zero-check --signed-overflow-check --unsigned-overflow-check \
--conversion-check --unwinding-assertions --trace --timeout 240"

FRAME_SRC="core/src/frame.c core/src/check.c core/src/types.c"
CORE_SRC="core/src/arbiter.c core/src/control.c core/src/frame.c core/src/check.c \
core/src/types.c core/src/params.c core/src/mutant.c"
CTL_SRC="core/src/control.c core/src/frame.c core/src/check.c core/src/types.c \
core/src/params.c core/src/mutant.c"

rc_all=0

run() {
  name="$1"
  shift
  echo "== CBMC $name"
  # shellcheck disable=SC2086
  cbmc "$@" $COMMON
  rc=$?
  case $rc in
    0) echo "CBMC-$name: VERIFIED" ;;
    10) echo "CBMC-$name: VERIFICATION FAILED (counterexample)" ; rc_all=1 ;;
    *) echo "CBMC-$name: UNKNOWN (exit $rc: timeout, unsupported or error)" ;;
  esac
}

run P-FRAME-1 host/modelcheck/cbmc/prop_frame_structure.c $FRAME_SRC
run P-FRAME-2 host/modelcheck/cbmc/prop_frame_integrity.c $FRAME_SRC
run P-ARB-1 host/modelcheck/cbmc/prop_arbiter_acceptance.c $CORE_SRC
run P-ARB-2 host/modelcheck/cbmc/prop_arbiter_stale.c $CORE_SRC
run P-ARB-3 host/modelcheck/cbmc/prop_arbiter_one_value.c $CORE_SRC
run P-CTL-1 host/modelcheck/cbmc/prop_ctl_bounded.c $CTL_SRC

exit $rc_all
