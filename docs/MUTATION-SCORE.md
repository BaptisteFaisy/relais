# Mutation score of the core

## What a mutation score is

A **mutant** is the program with one small, mechanical fault injected: a `<`
turned into `<=`, a constant nudged by one, an `&&` swapped for an `||`. The
**mutation score** is the fraction of those faults the test suite actually
catches. It answers a question a line-coverage number cannot: *coverage says the
tests ran the code; a mutation score says the tests would notice if the code were
wrong.*

A mutant is **killed** when at least one oracle fails on it — a unit test goes
red, the simulator reports an invariant violation, or the explorer finds a
counter-example. A mutant **survives** when it changes observable behaviour but
no oracle fails: that is a real gap in the tests. A mutant is **equivalent** when
it provably (or, here, observably) cannot change behaviour at all — it carries no
information and is excluded from the score. A mutant is **invalid** when it does
not even compile.

## Why this complements the eight mutants of spec 7.6 (INJ-11)

The bench in `host/mutants/run_mutants.py` injects **eight hand-written design
defects** (spec 7.6): the actuator accepts anybody's order, ignores the tick
label, plays the will past its horizon, and so on. Each is a *conceptual* mistake
a designer could make, and the bench must flag all eight (it does — see
`docs/RESULTS.md` §5).

This document measures something different: **implementation faults**. Nobody
sits down and writes "accept any order"; a tired engineer writes `>=` where the
spec says `>`, or `n + 1` where it should be `n`. Those are the faults a mutation
score hunts. The two are complementary:

| | spec 7.6 mutants (`run_mutants.py`) | mutation score (`mutation_score.py`) |
|---|---|---|
| Nature | deliberate **design** defects | mechanical **implementation** faults |
| Count | 8, hand-chosen | generated from every line of `core/src/*.c` |
| Question | does the bench see a wrong design? | are the tests strong enough to see a typo? |
| Verdict | 8/8 flagged | killed / survived / equivalent / invalid |

## Method

`host/mutants/mutation_score.py` (operators in `host/mutants/mutation_ops.py`):

1. **Generate.** Every line of every `core/src/*.c` file is scanned. Preprocessor
   lines and anything inside a string, a character literal, or a comment are
   skipped (they are masked, not mutated). Each remaining token yields the
   mutations below. Duplicate text edits are collapsed; the most specific
   operator keeps the label.
2. **Select.** `--max-mutants N` takes a deterministic, representative sample by
   round-robin over `(file, operator)` buckets, so a capped run still touches
   every file and every operator.
3. **Build.** The repository is copied to a temporary tree, the single line is
   patched, and the tree is rebuilt with the project's own flags
   (`-std=c11 -O2 -Wall -Wextra -Werror -Wshadow -Wconversion`). No
   `-DRELAIS_MUTANT`: the fault is a source patch, not a compile-time switch.
4. **Judge.** The whole `make test` suite runs, then a reduced set of oracles:
   four seeded `relais-simulate` scenarios (nominal, level C, a death, a freeze)
   and two `relais-explore` exhaustive searches (one and two faults). A non-zero
   exit, a `violations=[1-9]`, or an `EXPLORE-ECHEC` kills the mutant.
5. **Classify.** Build failure → **invalid**. Oracle failure → **killed**. All
   oracles pass and the captured evidence (test output, scenario summaries, and
   the full actuator journals) is byte-identical to the nominal run →
   **equivalent (suspected)**. All oracles pass but the evidence differs →
   **survived**.

Builds and judging run in parallel (`multiprocessing`, bounded by `--jobs`) and
the whole pass is capped by `--time-budget` (wall clock) and `--max-mutants`.

### Operators

| Family | Edit | Example |
|---|---|---|
| `cmp` | flip a comparison | `<`↔`<=`, `>`↔`>=`, `==`↔`!=` |
| `bound` | off-by-one on a bound | `i < 8` → `i < 7` / `i < 9` |
| `logic` | swap a logical operator | `&&`↔`||` |
| `const` | nudge an integer constant | `k` → `k+1` / `k-1`, `0`↔`1` |
| `sign` | flip a unary minus | `-x` → `+x` (where unambiguously unary) |

## Result on this machine

Measured on the project PC (12-core x86-64, Linux) on **2026-11-11**, with the
verification budget:

```bash
python3 host/mutants/mutation_score.py --max-mutants 40 --time-budget 240
```

| Mutants | Killed | Survived | Equivalent (suspected) | Invalid (no build) | Score |
|---|---|---|---|---|---|
| 40 | 13 | 0 | 22 | 5 | **100.0 %** (conservative 37.1 %) |

- **13 killed** — the suite, the simulator, or the explorer caught the fault.
- **0 survived** — no selected mutant changed observable behaviour without an
  oracle noticing.
- **22 equivalent (suspected)** — byte-identical on every oracle. These are real
  no-ops: a `memset(p, 0, …)` whose every field is reassigned on the next lines,
  a buffer guard (`count >= MAX`) the tests never reach, a `crc32c_table[256]`
  size bumped to 257 (the 257th entry is never read), a defensive branch.
- **5 invalid** — did not compile. Notably, `-Werror=nonnull` and
  `-Werror=array-bounds` reject a flipped `if (p == NULL) return;` before
  `memset(p, …)` because the compiler proves the null path: the strict build
  flags are themselves a mutation-killing oracle.

The **score** is `killed / (killed + survived)` = 13/13 = 100 %, the standard
convention that excludes suspected equivalents. Because a pass/fail oracle
cannot *prove* equivalence, the **conservative** score counts every suspected
equivalent as a miss: `killed / (killed + survived + equivalent)` = 13/35 =
**37.1 %**. The honest reading sits between the two, and the 22 suspected
equivalents are the margin of doubt.

The nightly workflow (`.github/workflows/nightly.yml`, job `mutation-score`)
re-runs this every night at a larger budget and re-dates the number in
`docs/RESULTS.md`.

## Limits

- **Equivalence is suspected, not proved.** "Byte-identical on these oracles" is
  an upper bound on true equivalence: a fault could change behaviour only on an
  input the reduced scenario set never produces. Such a mutant is counted as
  equivalent here and would be a survivor under a stronger oracle. This is why
  the conservative score is published alongside the strict one.
- **The oracle is bounded.** `make test` plus four scenarios and two small
  explorations is strong but finite. Branches no test reaches cannot kill a
  mutant; a mutation there is either equivalent-by-luck or an invisible survivor.
  Raising the scenario count and the exploration budget raises the score's
  meaning, not just its value.
- **Sample, not census.** `--max-mutants 40` is a representative sample of the
  1369 generated mutants, chosen round-robin. The nightly uses a larger sample;
  a full census is a `--max-mutants 0` run away, at the cost of wall-clock time.
- **Operators are syntactic.** They model typos and off-by-ones, not semantic
  redesigns. The eight spec-7.6 mutants cover the design-defect side; neither
  subsumes the other.
