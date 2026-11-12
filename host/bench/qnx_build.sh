#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# qnx_build.sh -- the complete QNX 8.0 bring-up receipt, idempotent (mission M01).
#
# One script for the whole path from a PC to two QNX guests and an actuator
# steering the simulated steering gear: source the SDP, cross-compile the
# target, build the bootable image with mkqnximage, verify it, then run the
# bench. Every step prints what it is about to do; a missing prerequisite
# stops the step with an actionable message, never a Python traceback.
#
# Requirement IDs exercised here: QNX-01..QNX-08 (resource manager, pulses,
# fixed priority, kernel trace, death-by-connection-close, supervision,
# security policy, boot-to-certified delay), SEC-05, PERF-01..PERF-08, OBS-04.
#
# Usage:
#   qnx_build.sh [all|env|build|image|verify|run]     (default: all)
#
# Environment overrides:
#   RELAIS_QNX_SDP     path of the SDP root (dir holding qnxsdp-env.sh)
#   RELAIS_QNX_IMAGE_OUT  mkqnximage output dir (default build/qnx-image)
#   RELAIS_SCENARIO    scenario file for the run step (default slalom-nominal)
#   FORCE=1            rebuild the image even when it is up to date
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH="$ROOT/host/bench"
PYTHON="${PYTHON:-python3}"
QNX_ENV="$BENCH/qnx_env.py"
TARGET="$ROOT/target"
IMAGE_OUT="${RELAIS_QNX_IMAGE_OUT:-$ROOT/build/qnx-vm}"
IMAGE_FINAL="$ROOT/image/qnx-ifs.img"
SCENARIO="${RELAIS_SCENARIO:-scenarios/slalom-nominal.json}"
MIN_IMAGE_BYTES=$((1024 * 1024))

step()  { printf '\n== %s ==\n' "$*"; }
ok()    { printf '   ok: %s\n' "$*"; }
hint()  { printf '   -> %s\n' "$*"; }
die()   { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# -- environment -----------------------------------------------------------
# Runs the detection module and returns its verdict (0/1/2). When a key is
# given, asks the module to print a single value instead (for shell reuse).
env_report() {
    "$PYTHON" "$QNX_ENV" "$@"
}

require_sdp() {
    step "QNX SDP 8.0 environment"
    verdict="$(env_report --print verdict || true)"
    case "$verdict" in
        0) ok "SDP and toolchain present (verdict $verdict)" ;;
        1) hint "SDP found but incomplete (verdict $verdict): checking each tool below."
           env_report || true ;;
        *) env_report || true
           die "No QNX SDP on this host. Get it through QNX Everywhere "
               "(https://qnx.com/everywhere, myQNX account), install it, and "
               "either put it in ~/qnx800 or set RELAIS_QNX_SDP. Until then, "
               "only `make -C target syntax` and PC/sim modes are available." ;;
    esac
}

sdp_env_script() {
    env_report --print env-script || die "qnxsdp-env.sh not found (no SDP)."
}

# -- build -----------------------------------------------------------------
build_target() {
    require_sdp
    step "Cross-compiling the target (qcc -V gcc_ntox86_64)"
    SDP_ENV="$(sdp_env_script)"
    # shellcheck disable=SC1090
    source "$SDP_ENV"
    if ! command -v qcc >/dev/null 2>&1; then
        die "qcc is not on PATH after sourcing $SDP_ENV -- is the SDP host "
            "tools install complete?"
    fi
    ok "qcc: $(qcc -V gcc_ntox86_64 -dumpversion 2>&1)"
    make -C "$TARGET"
    ok "target binaries built in $TARGET/build"
}

# -- image -----------------------------------------------------------------
# Real mkqnximage 8.0 flow (verified on the SDP, mission M46): long options
# only, a VM directory holding local/ + output/, and file injection through
# local/snippets/ifs_files.custom. The old -B/-S/-f/-t/-o flags never existed
# in this tool. host/bench/qnx_vm.py drives it; see docs/QNX-RUN.md.
image_needs_build() {
    [ "${FORCE:-0}" = "1" ] && { hint "FORCE=1: rebuilding"; return 0; }
    [ -f "$IMAGE_FINAL" ] || return 0
    for newer in image/fragments/* "$TARGET"/build/*; do
        [ -e "$newer" ] || continue
        [ "$newer" -nt "$IMAGE_FINAL" ] && return 0
    done
    return 1
}

build_image() {
    require_sdp
    step "Building the bootable image with mkqnximage (QNX-07, QNX-08)"
    if ! image_needs_build; then
        ok "image $IMAGE_FINAL is up to date; run with FORCE=1 to rebuild"
        return 0
    fi
    # Inject the target/ binaries listed in the mapping, when they exist
    # (missing ones are M45's business: the base image still builds).
    adds=()
    while IFS= read -r line; do
        line="${line%%#*}"
        case "$line" in *=*) ;; *) continue ;; esac
        name="${line%%=*}"; host="${line#*=}"
        [ -e "$ROOT/$host" ] && adds+=(--add "$name=$ROOT/$host")
    done < "$ROOT/image/fragments/vm_files.relais"
    [ ${#adds[@]} -gt 0 ] || hint "no target/ binaries yet (see M45): " \
        "building the base image only"
    "$PYTHON" "$BENCH/qnx_vm.py" --vm-dir "$IMAGE_OUT" build "${adds[@]}" \
        || die "qnx_vm.py build failed (mkqnximage output above)"
    [ -f "$IMAGE_OUT/output/ifs.bin" ] || die "no ifs.bin under $IMAGE_OUT"
    install -m 0644 "$IMAGE_OUT/output/ifs.bin" "$IMAGE_FINAL"
    ok "image written to $IMAGE_FINAL (from $IMAGE_OUT/output/ifs.bin)"
}

# -- verify ----------------------------------------------------------------
verify_image() {
    step "Verifying the image"
    if [ ! -f "$IMAGE_FINAL" ]; then
        hint "no image at $IMAGE_FINAL yet"
        build_image
    fi
    size="$(stat -c '%s' "$IMAGE_FINAL")"
    ok "$IMAGE_FINAL is ${size} bytes"
    if [ "$size" -lt "$MIN_IMAGE_BYTES" ]; then
        die "image is suspiciously small (< 1 MiB): the IFS is incomplete. "
            "Rebuild and check the mkqnximage output."
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        ok "sha256: $(sha256sum "$IMAGE_FINAL" | cut -d' ' -f1)"
    fi
}

# -- run -------------------------------------------------------------------
run_bench() {
    step "Launching the two guests and the actuator (mode qemu)"
    if [ ! -f "$IMAGE_FINAL" ]; then
        die "no image at $IMAGE_FINAL: run the build then the image step "
            "first (or just 'all')."
    fi
    if ! env_report --print verdict | grep -qE '^[01]$'; then
        env_report
        die "environment not ready to run QEMU guests."
    fi
    hint "the bench starts the PC actuator (level S), the programmable cable "
         "and the two QNX guests, then verifies the journal."
    "$PYTHON" "$BENCH/relais_bench.py" --mode qemu --scenario "$SCENARIO"
    ok "bench finished; verify the journal with relais_bench.py --verify"
}

# -- dispatcher ------------------------------------------------------------
cmd="${1:-all}"
case "$cmd" in
    env)    require_sdp ;;
    build)  build_target ;;
    image)  build_image ;;
    verify) verify_image ;;
    run)    run_bench ;;
    all)
        build_target
        build_image
        verify_image
        run_bench
        ;;
    *)  printf 'usage: %s [all|env|build|image|verify|run]\n' "$0" >&2
        exit 2 ;;
esac