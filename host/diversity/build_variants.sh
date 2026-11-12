#!/usr/bin/env bash
# Relais — build the diversity variants of the core (mission M37, docs/CCF.md).
#
# HONESTY RULE. This script never invents a diversity it does not have. Every
# compiler is probed with `command -v` and `--version` first; a missing one is
# recorded as ABSENT in the manifest together with the exact command that was
# tried. Every variant that IS built records its exact compilation line, the
# sha256 of its binary and the digest of the source it was built from, so the
# claim "N variants really built, from the same source" is checkable.
#
# Usage: host/diversity/build_variants.sh [--jobs N] [--only a,b] [--out DIR]
# Output: <out>/variants.tsv (manifest), <out>/<name>/relais-trace (binaries),
#         <out>/toolchain.tsv (what exists on this machine).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
JOBS=4
ONLY=""
OUT="build/diversity"

while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) JOBS="$2"; shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "usage: build_variants.sh [--jobs N] [--only a,b] [--out DIR]" >&2; exit 2 ;;
  esac
done

BASE_FLAGS="-std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -fno-strict-aliasing"
DEFS="-D_POSIX_C_SOURCE=200809L"
INCLUDES="-Icore/include -Ihost/simulate -Ihost/diversity"
SRC="$(printf '%s ' core/src/*.c)$(printf '%s ' host/simulate/sim.c \
  host/simulate/sim_faults.c host/diversity/trace.c host/diversity/law_alt.c)"

mkdir -p "$OUT" || exit 2
MANIFEST="$OUT/variants.tsv"
TOOLCHAIN="$OUT/toolchain.tsv"
printf 'name\tstatus\trole\tcompiler\tcompiler_version\tflags\tbinary\tsha256\tbytes\tnote\n' > "$MANIFEST"
printf 'candidate\tpath\tresolved\tversion\n' > "$TOOLCHAIN"

# Digest of everything the variants share: the core source, the simulator and
# this harness. One number that proves the variants differ only by their build.
SOURCE_DIGEST="$(cat core/src/*.c core/include/relais/*.h host/simulate/*.c \
  host/simulate/*.h host/diversity/trace.c host/diversity/law_alt.c \
  host/diversity/law_alt.h 2>/dev/null | sha256sum | cut -c1-16)"
export SOURCE_DIGEST
echo "source digest (core + sim + harness): $SOURCE_DIGEST"
echo "$SOURCE_DIGEST" > "$OUT/source-digest.txt"

# --- toolchain probe: what really exists here -------------------------------
probe_toolchain() {
  local c path resolved version
  for c in gcc cc clang clang-18 clang-17 clang-16 clang-15 clang-14 tcc icc icx \
           musl-gcc gcc-14 gcc-13 gcc-12 gcc-11 gcc-10 gcc-9 x86_64-linux-gnu-gcc \
           aarch64-linux-gnu-gcc riscv64-linux-gnu-gcc; do
    path="$(command -v "$c" 2>/dev/null || true)"
    if [ -z "$path" ]; then
      printf '%s\tABSENT\t-\tcommand -v %s\n' "$c" "$c" >> "$TOOLCHAIN"
      continue
    fi
    resolved="$(readlink -f "$path" 2>/dev/null || echo "$path")"
    version="$("$c" --version 2>&1 | head -1)"
    printf '%s\t%s\t%s\t%s\n' "$c" "$path" "$resolved" "$version" >> "$TOOLCHAIN"
  done
}

# --- variant table ----------------------------------------------------------
# role=variant  : must agree with the reference bit for bit (a divergence is a
#                 serious defect and is published as such).
# role=control  : must NOT agree; it proves the comparator has sensitivity
#                 (an injected defect, or a floating point option that is
#                 forbidden in production).
# Each entry: name|compiler|extra flags|role|note
VARIANTS=(
  "ref-O2|gcc||variant|reference build: the project CFLAGS, unchanged"
  "O0|gcc|-O0|variant|no optimisation: a different code generator, same semantics"
  "O1|gcc|-O1|variant|low optimisation"
  "O3|gcc|-O3|variant|aggressive optimisation, vectorisation enabled"
  "Os|gcc|-Os|variant|size optimisation"
  "march-native|gcc|-march=native|variant|code for THIS cpu (fma, avx2, avx512f): contraction may change the rounding"
  "fp-contract-off|gcc|-march=native -ffp-contract=off|variant|same cpu, fma contraction forbidden: isolates the rounding effect of march-native"
  "strict-aliasing|gcc|-O3 -fstrict-aliasing|variant|aliasing optimisation ON: the project builds with -fno-strict-aliasing"
  "no-builtin|gcc|-fno-builtin|variant|no libc builtin substitution: strlen/memcpy/expf really call the library"
  "stack-protector|gcc|-fstack-protector-strong|variant|hardening, no intended semantic change"
  "pic|gcc|-fPIC -pie|variant|position independent code, another code path in the compiler"
  "std-c17|gcc|-std=c17|variant|another C dialect of the same compiler"
  "ubsan|gcc|-O1 -fsanitize=undefined|variant|runtime undefined behaviour detector: a latent UB is a common cause amplifier"
  "mut1|gcc|-DRELAIS_MUTANT=1|control|injected defect M1 (actuator applies anybody order): the comparator MUST see it"
  "mut5|gcc|-DRELAIS_MUTANT=5|control|injected defect M5 (second keeps its own state): the comparator MUST see it"
  "fast-math|gcc|-O2 -ffast-math|control|reassociation and reciprocal: known to break bit identity, forbidden in production"
)

wanted() {
  [ -z "$ONLY" ] && return 0
  local needle=",$1,"
  case ",$ONLY," in *"$needle"*) return 0 ;; *) return 1 ;; esac
}

compiler_for() {
  case "$1" in
    gcc) echo "gcc" ;;
    clang) echo "clang" ;;
    *) echo "$1" ;;
  esac
}

# --- one build --------------------------------------------------------------
build_one() {
  local name="$1" cc="$2" extra="$3" role="$4" note="$5"
  local dir="$OUT/$name" bin="$OUT/$name/relais-trace" log="$OUT/$name/build.log"
  local status sha bytes cpath cversion flags cmd
  mkdir -p "$dir"
  cpath="$(command -v "$cc" 2>/dev/null || true)"
  if [ -z "$cpath" ]; then
    printf '%s\tABSENT\t%s\t%s\t-\t%s\t-\t-\t0\t%s; compiler not installed (command -v %s)\n' \
      "$name" "$role" "$cc" "$extra" "$note" "$cc" >> "$MANIFEST"
    echo "  $name: ABSENT ($cc not installed)"
    return 0
  fi
  cversion="$("$cc" -dumpversion 2>/dev/null)"
  # -std= and the optimisation level come from BASE_FLAGS; an entry that sets
  # its own replaces them, so the recorded line shows exactly one of each.
  flags="$BASE_FLAGS"
  case " $extra " in *" -std="*) flags="$(echo "$flags" | sed 's/-std=c11 //')" ;; esac
  case " $extra " in
    *" -O0 "*|*" -O1 "*|*" -O2 "*|*" -O3 "*|*" -Os "*|*" -Ofast "*)
      flags="$(echo "$flags" | sed 's/-O2 //')" ;;
  esac
  cmd="$cc $flags $extra $DEFS $INCLUDES $SRC -lm -o $bin"
  echo "$cmd" > "$dir/compile-line.txt"
  if timeout 300 bash -c "$cmd" > "$log" 2>&1; then
    status=BUILT
    sha="$(sha256sum "$bin" | cut -c1-16)"
    bytes="$(stat -c %s "$bin")"
  else
    status=FAILED
    sha="-"; bytes=0
    note="$note | build failed: $(tail -1 "$log" | cut -c1-160)"
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s %s\t%s\t%s\t%s\t%s\n' \
    "$name" "$status" "$role" "$cc" "$cversion" "$flags" "$extra" "$bin" "$sha" "$bytes" "$note" >> "$MANIFEST"
  echo "  $name: $status ($cc $cversion)"
}

# --- probe the axes that need a real check before being promised ------------
probe_optional_axes() {
  # A second compiler, if one is installed, is the only real diversity of
  # implementation available here. Probed, never assumed.
  local c
  for c in clang tcc; do
    if command -v "$c" >/dev/null 2>&1; then
      VARIANTS+=("${c}-O2|${c}|-O2|variant|second compiler actually present on this machine")
      echo "  extra compiler found: $c ($($c --version 2>&1 | head -1))"
    else
      echo "  no $c on this machine: no second compiler, no diversity of implementation"
    fi
  done
  # 32 bit ABI: another calling convention and another long double, if multilib
  # is installed. Compiling is not enough, the link must succeed.
  if timeout 60 gcc -m32 -x c -o "$OUT/.m32probe" - >/dev/null 2>&1 <<<'int main(void){return 0;}'; then
    VARIANTS+=("m32|gcc|-m32|variant|32 bit ABI: another calling convention, another long double")
    echo "  32 bit multilib usable: an m32 variant is built"
  else
    printf 'm32\tBLOCKED\tvariant\tgcc\t-\t-m32\t-\t-\t0\t32 bit multilib not installed (no 32 bit libgcc): link fails\n' >> "$MANIFEST"
    echo "  32 bit multilib missing: m32 recorded as BLOCKED, not built"
  fi
  rm -f "$OUT/.m32probe" 2>/dev/null
}

main() {
  echo "probing the toolchain..."
  probe_toolchain
  probe_optional_axes
  echo "building ${#VARIANTS[@]} variants with -j$JOBS..."
  local running=0 entry name cc extra role note
  for entry in "${VARIANTS[@]}"; do
    IFS='|' read -r name cc extra role note <<< "$entry"
    wanted "$name" || continue
    build_one "$name" "$cc" "$extra" "$role" "$note" &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running - 1)); fi
  done
  wait
  local built absent failed
  built="$(awk -F'\t' 'NR>1 && $2=="BUILT"' "$MANIFEST" | wc -l)"
  absent="$(awk -F'\t' 'NR>1 && ($2=="ABSENT" || $2=="BLOCKED")' "$MANIFEST" | wc -l)"
  failed="$(awk -F'\t' 'NR>1 && $2=="FAILED"' "$MANIFEST" | wc -l)"
  echo "manifest: $MANIFEST"
  echo "built=$built absent_or_blocked=$absent failed=$failed source_digest=$SOURCE_DIGEST"
  [ "$failed" -eq 0 ] || echo "WARNING: $failed variant(s) failed to build, see the logs"
}

main
