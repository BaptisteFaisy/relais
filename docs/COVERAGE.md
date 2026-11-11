# Coverage (mission M14)

This document publishes the **measured** code coverage of `core/` and lists, for
every uncovered line of the five protocol-critical files, a justification: either
*a test to write* (named) or *defensive / unreachable code* (explained).

## How to reproduce

```
make coverage          # rebuilds with --coverage, replays the suite, runs gcov
```

`make coverage` (see the `coverage` target in the Makefile) does, in order:

1. `make BUILD=build/cov CFLAGS="<-O0 -g --coverage …>" clean all` — recompiles
   `core/` and `host/` with gcov instrumentation (the normal `make all` stays
   `-O2` and instrumentation-free).
2. `host/quality/coverage_driver.py` — replays the unit tests (level 0), the
   seeded simulator over a fixed, deterministic fault set (level 2) at both the
   `socle` and `complet` levels, the offline verifier on every journal (at the
   level it was produced), and the exhaustive explorer (level 1).
3. `host/quality/gcov_report.py` — re-runs gcov and writes the per-file summary
   to `build/cov/coverage.summary.md` (the raw, regenerable numbers).

`lcov`/`genhtml` are **not** installed on the worker, so the summary is produced
from `gcov` directly by a stdlib-only Python script (project rule 6). The figures
below are the `gcov` "Lines executed" / "Branches executed" percentages.

> A stress scenario (both machines down at once) legitimately breaks invariant
> I4; that is fine for *coverage* (the code still ran) but is **not** a safety
> claim. The safety proof lives in the explorer (10 M states, 0 violation) and
> the campaign, not in `make coverage`. The driver therefore tolerates a
> simulator/verifier exit code of 1 (violation detected) and fails only on a
> real error (exit >= 2); tests and the explorer stay strict.

## Per-file coverage (core/src)

| file | lines | executed | branches | uncovered lines |
|------|------:|---------:|---------:|----------------:|
| arbiter.c    | 313 | 94.89 % | 96.14 % | 16 |
| check.c      | 104 | 100.00 % | 96.15 % | 0 |
| control.c    | 107 | 100.00 % | 100.00 % | 0 |
| frame.c      | 245 | 96.73 % | 97.22 % | 8 |
| invariants.c | 197 | 85.28 % | 93.43 % | 29 |
| journal.c    | 147 | 97.96 % | 98.96 % | 3 |
| mutant.c     | 3   | 33.33 % | n/a | 2 |
| node.c       | 250 | 90.40 % | 88.10 % | 24 |
| params.c     | 82  | 98.78 % | 100.00 % | 1 |
| plant.c      | 104 | 96.15 % | 91.67 % | 4 |
| types.c      | 76  | 56.58 % | 71.21 % | 33 |

**arbiter.c branch coverage is 96.14 %**; the 8 unexecuted branches all sit in
two exported helpers (`relais_arb_vacant`, `relais_arb_force_link`) that have no
in-tree caller — the arbiter's *protocol* logic is fully branch-covered.

## Uncovered lines and justifications

Legend — **[T]** a test to write (named), **[D]** defensive / unreachable,
**[P]** process/target boundary (exercised by `host/node`, `target/` or the PC
demo, not by the simulator-based `make coverage`), **[M]** exercised by the
mutant bench `host/mutants/run_mutants.py`.

### core/src/arbiter.c (16 lines)

| line | code | class | justification |
|------|------|:-----:|---------------|
| 71–72 | `*why = RELAIS_REJ_MANDATE_FUTURE; return false;` | **[D]/[T]** | Rejects a frame whose mandate is *ahead* of the arbiter's (SEC-04: nobody appoints itself). The arbiter is the sole writer of mandate numbers (MAN-06); the `forge` fault reuses the *current* mandate, so no in-tree path can present a future one. **Test:** a white-box unit test calling `relais_arb_step` with a crafted ORDRE frame carrying `mandate = a->mandate + 1`. |
| 75–76 | `*why = RELAIS_REJ_MANDATE_STALE; return false;` | **[D]/[T]** | The returning machine (spec 4.6): holder presents a mandate *older* than the arbiter's. Reachable only in a partition where the arbiter granted a new mandate without the old holder observing it; `forge-stale` uses a stale *tick*, not a stale mandate. **Test:** white-box unit test with `sender = holder, mandate = a->mandate - 1`. |
| 117–118 | `default: break;` | **[D]** | STATUT, ETAT and SUIVI never reach the actuator's scan (peer/actuator-emitted). Unreachable by construction; self-documenting arm. |
| 269–271 | `a->certified = false; … push_event(RELAIS_EV_DECERTIFIED …)` | **[T]** | De-certification of a trusted second. Reachable but not produced by the current scenario set: it needs the second certified over N agreeing cushions and *then* biased so its shadow deviates > epsilon. **Test/scenario:** `certify-then-bias-second`, e.g. `--fault 400:bias:1:0:0.60`. |
| 379–380 | `memset(&local,…); meas = &local;` | **[D]/[T]** | Null-measurement fallback. Every in-tree caller of `relais_arb_step` passes a non-NULL `meas`. **Test:** unit test calling `relais_arb_step(a, tick, NULL, 0, NULL, &out)`. |
| 482–483 | `relais_arb_vacant` | **[T]** | Exported helper, no in-tree caller (reserved for the actuator vacancy check). **Test:** trivial unit test (`NULL` and `holder == NONE` → true). |
| 486–488 | `relais_arb_force_link` | **[T]** | Exported helper, no in-tree caller (reserved for forcing link hearing). **Test:** trivial unit test. |

### core/src/node.c (24 lines) — all class **[P]**

The node state machine is exercised by the simulator through `relais_node_init /
on_etat / on_suivi / on_silence / on_status / on_local_fault` (the `crash` fault
drives `on_local_fault`). The lines below belong to the **node-process / QNX
target** side and are reached only via `host/node/relais-node` (a real UDP
process) or `target/core.c`, not by `make coverage`:

| line | code | justification |
|------|------|---------------|
| 22–25 | `die_here` death execution (`o->died`, `o->at`) | The axis-of-death: armed by `relais-node --die` / `target` RL_CMD_ARM_DEATH; the simulator's `death` fault sets `nd->dead` directly instead. |
| 328–330 | `relais_node_on_handover` | REL-09 maintenance handover, called only by `target/core.c` (RL_CMD_HANDOVER, QNX). |
| 346–349 | `relais_node_arm_death` | Called by `relais-node --die` and `target/core.c`. |
| 352–367 | `relais_node_status_text` | Status dump used by `relais-node` and `target/core.c`. |

**Gap-closer:** extend coverage with the PC demo (`host/bench/relais_bench.py
--demo`, which launches `relais-node --die/--bias`) or a unit test driving the
node SM directly.

### core/src/control.c — 100 % lines, 100 % branches. No uncovered line.

### core/src/frame.c (8 lines)

| line | code | class | justification |
|------|------|:-----:|---------------|
| 102 | `relais_frame_write_u64` | **[T]** | Public helper with no in-tree caller (the sibling `write_u32/write_f32/read_*` are used). **Test:** unit test asserting the exact 8-byte big-endian output. |
| 113–114 | `relais_msg_to_peer` | **[P]/[T]** | Called only by `host/node/relais-node.c` and `target/net.c`. **Test:** unit test (ETAT/SUIVI → true, else false). |
| 196–198 | encode `default: c->ok = false;` | **[D]** | The switch covers every `RELAIS_MSG_*` value; default is unreachable. |
| 276–277 | decode `default: return false;` | **[D]** | Same — unreachable defensive arm. |

### core/src/invariants.c (29 lines)

| line | code | class | justification |
|------|------|:-----:|---------------|
| 21 | `else relais_params_default(&v->params);` | **[D]/[T]** | Init branch when `params == NULL`; every in-tree caller passes params. **Test:** `relais_inv_init(&v, NULL)`. |
| 73,74,77,79 | `relais_inv_violate(…, "I1", …)` | **[M]** | I1 reporting bodies — fire only when no value is applied / order with no holder / uncertified substitution. Never taken in a correct run (that *is* the safety claim). Exercised by the mutant bench. |
| 87,90,93 | `relais_inv_violate(…, "I2", …)` | **[M]** | I2 (mandate monotonic / single holder / one change per tick) reporting bodies. |
| 101 | `relais_inv_violate(…, "I3", …)` | **[M]** | I3 reporting body. |
| 121 | `relais_inv_violate(…, "I4", …)` | **[M]** | I4 (fresh-order gap) reporting body. |
| 145 | `relais_inv_violate(…, "I5", …)` | **[M]** | I5 (twin deviation) reporting body. |
| 154 | `relais_inv_violate(…, "I6", …)` | **[M]** | I6 (will past horizon) reporting body. |
| 167 | `relais_inv_violate(…, "I8", …)` | **[M]** | I8 (late fallback) reporting body. |
| 196 | `relais_inv_violate(…, "I10", …)` | **[M]** | I10 (lamp) reporting body. |
| 204 | `relais_inv_violate(…, "I11", …)` | **[M]** | I11 (tick contiguity) reporting body. |
| 222 | `relais_inv_violate(…, "I9", …)` | **[M]** | I9 (reintegration) reporting body. |
| 229–232 | DET-09 detection + report | **[M]** | Fires only if a wrong value went unreported as a disagreement; silent when detection works. Exercised by the value-fault mutants. |
| 250,252,259,262,270,272 | DET-03 / I7 reporting bodies | **[M]** | Mandate-revoked-without-evidence and handover-without-fault reporting; fire only on a broken run. |
| 279–281 | `case RELAIS_EV_DISAGREEMENT:` | **[M]** | Sets `disagreement_seen` for DET-09; reached only when a disagreement event is journaled (value-fault mutants). |

> Every `relais_inv_violate(...)` line is, by construction, a branch that must
> stay silent on a correct run. Their non-coverage in `make coverage` **is** the
> statement "0 violation"; they are covered (and must fire) by
> `host/mutants/run_mutants.py`, which flags 8/8 mutants through exactly these
> calls.

### Out of the required five, for honesty

- **types.c (56.58 %)** — mostly `enum → string` / conversion helpers for event
  and source kinds that the fixed scenario set does not all produce; each missing
  string is a one-line unit-test away.
- **mutant.c (33.33 %)** — `relais_mutant_name` and the `default: "nominal"` arm
  are only meaningful in a `-DRELAIS_MUTANT=n` build; the nominal build compiles
  the non-mutant path, so the mutant-name table is exercised by the mutant bench,
  not by `make coverage`.
- **journal.c (3 lines)** / **params.c (1 line)** / **plant.c (4 lines)** —
  defensive `return false / RELAIS_SRC_NONE / RELAIS_EV_NONE` arms on malformed
  input and a couple of rarely-hit numeric branches.
