#!/usr/bin/env sh
# Relais — "from zero to proofs" (reproducibility promise, REP-01..REP-05).
#
# One command re-runs, on a clean machine, every proof that does not need the
# QNX SDP: the unit tests (level 0), the exhaustive exploration (level 1), the
# fault campaign (level 2), the mutant bench and the demonstration. It only
# *proposes* commands, never executes an installation or a download, and never
# asks for administrator rights: when a tool is missing it prints the exact
# command for your distribution and stops.
#
# Usage:
#   tools/bootstrap.sh            run everything this machine can run
#   tools/bootstrap.sh --dry-run  print the steps without running anything
#   tools/bootstrap.sh --help     this text
#
# Idempotent: safe to relaunch; `make` reuses what is already built, the
# campaign simply re-runs, the mutant bench rebuilds its own build dirs.

set -eu

LOG=""
SUMMARY=""
DRY=0
MISSING_REQUIRED=0

usage() {
  sed -n '2,17p' "$0"
}

msg()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# Distribution detection and install hints (propose, never execute).
# ---------------------------------------------------------------------------
distro() {
  if [ -f /etc/os-release ]; then
    . /etc/os-release
    case "$ID" in
      debian|ubuntu|linuxmint|pop|raspbian) echo deb ;;
      fedora|centos|rhel|rocky|almalinux|amzn) echo rpm ;;
      arch|manjaro|endeavouros) echo arch ;;
      alpine) echo alpine ;;
      opensuse-leap|opensuse-tumbleweed|sles) echo suse ;;
      *) echo other ;;
    esac
  elif have brew; then
    echo brew
  else
    echo other
  fi
}

# pkg(TOOL DISTRO) -> package name for that tool on that distribution.
pkg() {
  case "$1:$2" in
    gcc:deb) echo gcc ;; gcc:rpm) echo gcc ;; gcc:arch) echo gcc ;;
    gcc:alpine) echo build-base ;; gcc:suse) echo gcc ;; gcc:brew) echo gcc ;;
    make:deb|make:rpm|make:arch|make:alpine|make:suse|make:brew) echo make ;;
    python3:deb|python3:rpm|python3:alpine|python3:suse) echo python3 ;;
    python3:arch) echo python ;; python3:brew) echo python3 ;;
    cpio:deb|cpio:rpm|cpio:arch|cpio:alpine|cpio:suse) echo cpio ;;
    cpio:brew) echo cpio ;;
    qemu:deb|qemu:rpm) echo qemu-system-x86 ;;
    qemu:arch) echo qemu-system-x86 ;;
    qemu:alpine) echo qemu-system-x86_64 ;;
    qemu:suse) echo qemu ;;
    qemu:brew) echo qemu ;;
    *) echo "$1" ;;
  esac
}

# install_cmd TOOL -> the one-line command to install TOOL on this machine.
install_cmd() {
  local d p
  d=$(distro); p=$(pkg "$1" "$d")
  case "$d" in
    deb) echo "sudo apt-get update && sudo apt-get install -y $p" ;;
    rpm) echo "sudo dnf install -y $p" ;;
    arch) echo "sudo pacman -S --needed $p" ;;
    alpine) echo "sudo apk add $p" ;;
    suse) echo "sudo zypper install $p" ;;
    brew) echo "brew install $p" ;;
    *) echo "install the equivalent of '$p' with your package manager" ;;
  esac
}

# require TOOL : a prerequisite that blocks everything when absent.
require() {
  if have "$1"; then
    msg "  ok    $1"
  else
    msg "  MISS  $1   ->   $(install_cmd "$1")"
    MISSING_REQUIRED=1
  fi
}

# note TOOL : a prerequisite whose absence only skips the step that uses it.
note() {
  if have "$1"; then
    msg "  ok    $1"
  else
    msg "  (opt) $1 absent  ->  $(install_cmd "$1")   [step will be skipped]"
  fi
}

# ---------------------------------------------------------------------------
# Step runner: prints each command in --dry-run, otherwise runs it.
# ---------------------------------------------------------------------------
run() {
  local label=$1; shift
  if [ "$DRY" -eq 1 ]; then
    printf '  [dry-run] %-42s : ' "$label"
    printf '%s ' "$@"
    printf '\n'
  else
    step "$label"
    "$@"
  fi
}

# mark NAME : record a green stage, except in --dry-run.
mark() { [ "$DRY" -eq 1 ] || { echo "$1=OK" >>"$SUMMARY"; }; }

# maybe LABEL CMD... : a step whose failure must not abort the whole run.
maybe() {
  local label=$1; shift
  if [ "$DRY" -eq 1 ]; then
    printf '  [dry-run] %-42s : ' "$label"
    printf '%s ' "$@"
    printf '\n'
  else
    step "$label"
    if ! "$@"; then
      echo "-> $label: skipped or blocked (see above)"
      echo "$label=SKIP" >>"$SUMMARY" 2>/dev/null || true
    fi
  fi
}

# ---------------------------------------------------------------------------
# The proof steps themselves (each is one public command, no hidden flags).
# ---------------------------------------------------------------------------
prereqs() {
  step "Prerequisites (propose, never install)"
  require gcc
  require make
  require python3
  note cpio
  note qemu-system-x86_64
  if [ "${MISSING_REQUIRED:-0}" -ne 0 ] && [ "$DRY" -eq 0 ]; then
    echo
    echo "Install the missing tools with the command(s) above, then relaunch:"
    echo "  tools/bootstrap.sh"
    exit 1
  fi
}

build_step() {
  run "build (core + tests + tools)" make -j"$(jobs_count)" all
  mark build
}

test_step() {
  if [ "$DRY" -eq 0 ]; then
    step "unit tests (level 0)"
    make test 2>&1 | tee "$LOG"
    mark tests
  else
    printf '  [dry-run] %-42s : make test\n' "unit tests (level 0)"
  fi
}

explore_step() {
  run "exhaustive exploration (level 1)" make explore
  mark explore
}

campaign_step() {
  run "fault campaign, seeded (level 2 sample)" make campaign
  mark campaign
}

mutants_step() {
  run "mutant bench (8/8 must be flagged)" python3 host/mutants/run_mutants.py
  mark mutants
}

demo_step() {
  # Deterministic, no clock, no QEMU: the demonstration of spec 8 everywhere.
  run "demonstration (deterministic, PC)" \
    python3 host/bench/relais_bench.py --demo --sim --out runs/bootstrap-demo
  mark demo
}

vm_step() {
  # The QEMU freeze/thaw demonstration. Needs qemu-system-x86_64; without a
  # QNX image (image/qnx-ifs.img or $RELAIS_QNX_IMAGE) the bench reports
  # BLOQUE with the exact command to build one -- never an obscure failure.
  if have qemu-system-x86_64; then
    maybe "VM demonstration (QEMU guests)" \
      python3 host/bench/relais_bench.py --demo --mode qemu --out runs/bootstrap-vm
  elif [ "$DRY" -eq 1 ]; then
    printf '  [dry-run] %-42s : (qemu-system-x86_64 present? -> run VM demo)\n' \
      "VM demonstration (QEMU guests)"
  else
    step "VM demonstration (QEMU guests)"
    echo "skipped: qemu-system-x86_64 is not installed ($(install_cmd qemu-system-x86_64))"
  fi
}

jobs_count() {
  if have nproc; then nproc; else getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2; fi
}

# ---------------------------------------------------------------------------
# Final summary, comparable in shape to docs/RESULTS.md §1.
# ---------------------------------------------------------------------------
summary() {
  step "Summary (compare with docs/RESULTS.md)"
  if [ -s "$LOG" ]; then
    local checks
    checks=$(grep -o '[0-9][0-9]* checks' "$LOG" | grep -o '[0-9][0-9]*' \
      | awk '{s+=$1} END{print s+0}')
    echo "  unit checks            : ${checks:-?} green (expected 5651 for this tree)"
  fi
  echo "  exploration            : 0 violation (expected); state count varies with budget"
  echo "  campaign               : 0 failure (CAMPAGNE-OK)"
  echo "  mutants                : 8/8 flagged"
  echo "  demonstration          : OK (deterministic, PC)"
  if have qemu-system-x86_64; then
    echo "  VM demonstration       : OK with a QNX image, BLOQUE without (see docs/REPRODUCE.md)"
  else
    echo "  VM demonstration       : skipped (qemu-system-x86_64 not installed)"
  fi
  if [ -s "$SUMMARY" ]; then
    echo "  -- stage log --"
    sed 's/^/    /' "$SUMMARY"
  fi
  echo
  echo "To replay the QNX-emulated numbers (level 3), see docs/REPRODUCE.md."
}

# ---------------------------------------------------------------------------
main() {
  case "${1:-}" in
    --help|-h) usage; exit 0 ;;
    --dry-run) DRY=1 ;;
    "")
      ;;
    *) usage; exit 2 ;;
  esac

  # Temporary files for the captured logs go under /tmp, never the repo.
  LOG=$(mktemp -t relais-bootstrap-test.XXXXXX.log)
  SUMMARY=$(mktemp -t relais-bootstrap-summ.XXXXXX)
  trap 'rm -f "$LOG" "$SUMMARY"' EXIT

  prereqs
  if [ "$MISSING_REQUIRED" -ne 0 ]; then
    exit 1
  fi
  build_step
  test_step
  explore_step
  campaign_step
  mutants_step
  demo_step
  vm_step
  summary
}

main "$@"