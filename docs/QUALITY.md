# Quality policy (mission M14)

What this repository verifies automatically, with which tool, at which moment —
and, honestly, what it does **not** verify.

## Verified automatically

| Check | Tool | When | Gate |
|-------|------|------|------|
| Compiles with zero warning under the strict flag set (`-Wall -Wextra -Werror -Wshadow -Wconversion`) | `gcc -std=c11` | every `make all` (CI job `ci`) | fatal |
| File / function size budget (< 500 / < 50 lines) | `make size` | CI job `ci` | fatal |
| Unit tests (level 0, 5651 checks) | `make test` | CI job `ci` | fatal |
| Exhaustive state exploration (level 1, 100 k+ states, 0 violation) | `relais-explore` | CI job `ci` | fatal |
| Seeded simulation (level 2, 1 M ticks) + fault campaign | `relais-simulate`, `make campaign` | CI job `ci` | fatal |
| Mutant bench (8/8 mutants flagged) | `host/mutants/run_mutants.py` | CI job `ci` (sample) / nightly (full) | fatal |
| Offline invariant verification of journals | `relais-verify` | CI job `ci` | fatal |
| QNX layer syntax (no SDP needed) | `make -C target syntax` | CI job `ci` | fatal |
| CI budget: every job <= 5 min | `.github/ci-budget/check_timeouts.py` | CI job `ci-budget` | fatal |
| Scope guard (`## Scope` exact, no forbidden pattern) | `.github/couche0/check_couche0.py` | CI job `couche0` | fatal |
| Static analysis (cppcheck / clang-tidy when present, else clean skip; `gcc -fanalyzer` advisory) | `.github/workflows/static-analysis.yml` | on PR / merge_group | advisory |
| Reproducible build (two directories, `-ffile-prefix-map` + `SOURCE_DATE_EPOCH`, byte-identical `cmp`) | `.github/workflows/static-analysis.yml` | on PR / merge_group | fatal |
| Published coverage (per-file rates + uncovered-line justifications) | `make coverage` → `gcov` + `host/quality/gcov_report.py` | on demand | report |
| SBOM + anti-dependency guard (no system include outside libc/libm/POSIX/QNX) | `host/quality/sbom.py` | on demand | fatal on violation |

## The anti-dependency guard

`host/quality/sbom.py` walks every `.c`/`.h` under the tree and fails (exit 1)
if any `#include <…>` names a header outside the allowed set: the C11 standard
library, the POSIX surface, and the QNX toolchain headers (`sys/neutrino.h`,
`sys/slog2.h`, `sys/trace.h`). Introducing, say, `<openssl/ssl.h>` therefore
breaks the guard. This is the machine-checked form of the project's central
claim: **core/ is a pure C11 library with zero external dependency** (no clock,
no file, no socket, no allocation), and the SBOM lists only internal components
plus the toolchain.

```
python3 host/quality/sbom.py            # prints the CycloneDX 1.4 JSON
python3 host/quality/sbom.py --out sbom.json
python3 -m unittest discover -s host/quality -p 'test_*.py' -v
```

## Reproducible build

The `reproducible-build` job compiles the same tree twice, in two different
absolute directories, with `SOURCE_DATE_EPOCH=0` and
`-ffile-prefix-map=<dir>=.`, then `cmp`s every object file and executable. The
code uses no `__DATE__`/`__TIME__`/`assert`, so the only path-dependent bytes
are the DWARF compilation directory and `__FILE__`, both normalised by
`-ffile-prefix-map`. Verified locally: 149 artifacts byte-identical.

## Coverage

`make coverage` rebuilds `core/` and `host/` with `--coverage`, replays the
tests, the simulator over a fixed deterministic fault set (both `socle` and
`complet` levels), the offline verifier and the explorer, then runs `gcov`.
`docs/COVERAGE.md` publishes the per-file rates and, for every uncovered line of
`arbiter.c`, `node.c`, `control.c`, `frame.c` and `invariants.c`, a
justification: a named test to write, defensive/unreachable code, a
process/target boundary, or a mutant-bench-only path.

## What is NOT verified automatically

- **Real QNX 8.0 execution.** `target/` is syntax-checked on the PC
  (`make -C target syntax`) but never run on a QNX kernel here; the QNX behaviour
  is argued from the shared `core/` and the PC bench, not executed.
- **Real hardware / real network timing.** The PC bench and the QEMU layer
  (`host/bench`) approximate the bus; WCET/RTA on the target is out of scope of
  this repository.
- **100 % branch coverage of `arbiter.c`.** It is at 96.14 %; the 8 unexecuted
  branches live in two exported helpers with no in-tree caller. Every uncovered
  line is justified in `docs/COVERAGE.md` rather than silently ignored.
- **The `relais_inv_violate(…)` reporting bodies in `invariants.c`.** They are
  uncovered by `make coverage` *by design* — they fire only on a violation, and
  "0 violation" is the safety claim. They are covered (and must fire) by the
  mutant bench.
- **cppcheck / clang-tidy on this worker.** Neither is installed; the
  `static-analysis` job skips them cleanly and says so, falling back to the
  always-present `gcc -fanalyzer` advisory pass. Installing them is a runner
  provisioning decision, not a repository one.
- **A live fuzzer / sanitizer run.** Sanitizers and fuzzing are a separate
  mission (M22); they are not part of the default gate.

## Reproduce everything locally

```
make -j4 all && make test          # 5651 checks green
make coverage                       # gcov numbers + build/cov/coverage.summary.md
python3 host/quality/sbom.py        # SBOM + anti-dependency guard
python3 -m unittest discover -s host/quality -p 'test_*.py' -v
python3 .github/ci-budget/check_timeouts.py
```
