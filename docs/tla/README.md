# A formal model of Relais, and its cross-check against the C code

`Relais.tla` is a TLA+ specification of the actuator's arbiter and of the two
computers. It is an **independent** description of the protocol: it was written
from `docs/SPEC.md` (sections 4.3, 4.4, 4.5, 4.6) and then confronted, rule by
rule, with `core/src/arbiter.c`. Nothing in it is generated from the C code.

Two files:

| file | role |
|---|---|
| `Relais.tla` | the model: variables, actions, hypotheses, invariants |
| `MC.cfg` | the TLC configuration: small constants and the invariant list |

The cross-checker lives in `host/verify/`:

| file | role |
|---|---|
| `tla_crosscheck.py` | runs the three paths, prints the agreement table, exits non-zero on any disagreement |
| `tla_model.py` | the reduced machine, a literal transcription of `Relais.tla` (kept apart so that every file stays under 500 lines) |
| `test_tla_crosscheck.py` | 19 unit tests: agreement, caught disagreement, degenerate model |

## Running TLC

TLC is not packaged on most distributions; it is a single jar.

```sh
mkdir -p ~/.local/share/tla2tools
curl -L -o ~/.local/share/tla2tools/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
```

Then, from the repository root:

```sh
cd docs/tla
java -XX:+UseParallelGC -cp ~/.local/share/tla2tools/tla2tools.jar \
     tlc2.TLC -config MC.cfg -workers 4 Relais
```

TLC writes its working files next to the module; use `-metadir <dir>` to keep
them out of the repository.

### Status on this machine

**TLC was installed and actually run** (jar `v1.8.0` under
`~/.local/share/tla2tools/tla2tools.jar`, OpenJDK 21.0.13, 12 cores). With the
constants of `MC.cfg` (`MaxTick=3, MaxMandate=3, N=2, P=1, CertifyTicks=1`):

```
Model checking completed. No error has been found.
8800417 states generated, 1197168 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 36.
Finished in 2min 15s
```

Every invariant of `MC.cfg` holds on all 1 197 168 distinct states:
`TypeOK`, `Inv_I1`, `Inv_I2`, `Inv_I3`, `Inv_I6`, `Inv_Safety`, `Inv_Lamp`.

### The model is not vacuous

A safety invariant that holds because nothing ever happens is worth nothing, so
each behaviour the invariants talk about was probed with a deliberately false
assertion that TLC must **refute** (same constants, one probe per run). All
fifteen were refuted, i.e. all of them really occur:

| probe (asserted false) | what it proves reachable |
|---|---|
| `mandate = 0` | the mandate is granted |
| `holder = NONE` | the mandate goes vacant |
| `holder # "A"` / `holder # "B"` | both machines hold it in turn |
| `source # "holder"` | an order is applied |
| `source # "shadow"` | the substitution by the certified second happens |
| `source # "testament"` | the will is played |
| `source # "fallback"` | the fallback law takes over |
| `certified = FALSE` | the second gets certified |
| `lamp # "yellow"` / `lamp # "red"` | both degraded lamps light up |
| `cessionPending = FALSE` | a cession is recorded |
| `frozen = FALSE` on both | a machine is frozen and thaws |
| `alive` on both | a machine dies |
| all links up | a link is cut |

## Rule by rule: TLA+ ↔ C ↔ requirement

| `Relais.tla` | `core/src/arbiter.c` | requirement |
|---|---|---|
| `Init`: `mandate = 0`, `holder = NONE` | `relais_arb_init` | MAN-06 |
| `Release()` → certified second, `mandate + 1` | `release_mandate`, auto-switch branch | MAN-10, MAN-03 |
| `Release()` → vacant, mandate unchanged | `release_mandate`, vacant branch | MAN-04 |
| `ms1 == IF ord THEN 0 ELSE Cap(misses+1, P+1)` | `a->misses = s.order_valid ? 0 : …` | DET-01 |
| `IF h0 # NONE /\ ms1 > P THEN Release` | `if (holder != NONE && misses > patience) release_mandate` | MAN-04 |
| `OrderOK(f, h, m, t)`: sender, mandate, tick | `order_is_valid` (three tests, same order) | MAN-02 |
| `f.mand > m` rejected (future mandate) | `RELAIS_REJ_MANDATE_FUTURE` | SEC-04 |
| `f.mand # m` rejected (returning machine) | `RELAIS_REJ_MANDATE_STALE` | spec 4.6 |
| grant when `h1 = NONE /\ dem`, `mandate + 1` | `handle_demande`, `grant = vacant` | MAN-05 |
| A has priority among two demandes | `scan`: `if (s->demande == NULL)` on uplink A first | MAN-05 |
| `ces` + `cessionPending`, freed one tick later | `REL-06` branch at the top of `relais_arb_step` | DET-05, REL-06 |
| `pick_source`: order → shadow → will → law | `pick_value`, same four branches, same order | spec 4.4 |
| shadow taken only if `certified /\ from = cnode` | `a->certified && s->shadow->sender == a->certified_node` | REL-05 |
| `will_base = t + 1`, `will_len = N` | `a->testament_base = tick + 1` | ACT-02 |
| `certify()`: streak of agreeing ticks | `certify`, `certify_streak >= certify_ticks` | REP-05 |
| certification dropped on a handover | `switch_pending` cleared before `certify` | DEG-07 |
| `lamp_of()`: red, yellow, off by cause | `update_lamp`, same cascade of causes | DEG-01, DEG-02 |
| `newRole`, `newBelief`, `newSynced` (STATUT) | `out->statut.*`, read by `relais_node_on_status` | MAN-07 |
| `KillMachine` / `FreezeMachine` / `ThawMachine` | `EX_KILL`, `EX_FREEZE1/3`, `EX_THAW` | spec 7.3 |
| `CutLink` / `RestoreLink` | `EX_CUT`, `EX_RESTORE` | spec 7.3 |
| `DeliverFrame` / `LoseFrame` | `EX_DELAY`/delivery, `EX_DROP` | spec 3.3 |
| `WF_vars(Tick)` | the actuator is the clock | spec 4.10 |
| `Inv_I1` … `Inv_Safety` | `check_unicity`, `check_mandate`, `check_no_returning`, `check_horizon` in `core/src/invariants.c` | I1, I2, I3, I6 |

### The invariants, as stated in the model

* **I1** — after the first tick exactly one value is applied, from one of the
  four known sources; an order implies a holder, a substitution implies a
  certified second.
* **I2** — the mandate never decreases, it changes at most once per tick
  (`changesThisTick <= 1`), and no mandate number is ever recorded for two
  machines (`Cardinality(mandateHolders[k]) <= 1`).
* **I3** — when the source is the holder, the applied frame comes from the
  published holder, targets the tick being applied and carries the current
  mandate. A deposed machine's order, or one aimed at another tick, is never
  applied.
* **I6** — the will is never played past its horizon: `testamentRun <=
  testamentLen <= N` and `tick < testamentBase + testamentLen`.
* **Safety** — never two machines applied at the same tick
  (`Cardinality(appliedSet) <= 1`).
* **DEG-01** — a tick whose source is not the holder never leaves the lamp off.

### Hypotheses, written as hypotheses

These are **assumptions of the model**, guarded in the actions, not
conclusions:

* **H1, single fault** — `KillMachine` and `FreezeMachine` are enabled only
  while the other machine is alive and not frozen. The specification claims
  bounds against one fault, not two (spec 3.3, 7.3).
* **H2, loss burst ≤ P** — `LoseFrame` is enabled only while `channelLosses <
  P`; a successful delivery resets the counter. The burst window is one period.
* **H3, timing hypothesis** — one frame in flight per machine per period
  (`pending` holds at most one frame), and a frame that is not delivered inside
  its period is dropped by `Tick`. This is the model of `2δ + C + J ≤ T`.

The safety invariants do **not** depend on H2: the actuator counts its own
misses and revokes on the (P+1)-th one whatever the channel does. H2 only
bounds what is claimed about liveness.

## What the model does NOT cover

Stating this plainly matters more than the list above:

* **The control law.** `relais_fallback_step`, the damped loop, the gains: the
  model only records that the source is `fallback`. No numeric value appears.
* **The plant and the physics.** `relais_plant_step`, positions, speeds,
  disturbances, the setpoint trajectory: absent.
* **Real delays.** The model has no time: H3 says a frame either arrives in its
  period or is lost. Jitter, drift and the 5 ms period are not modelled.
* **The frames themselves.** CRC-32C, the SipHash MAC, `core/src/frame.c`: a
  frame is a 4-field record. A corrupted frame is simply not delivered.
* **The numeric agreement.** Certification in C compares `|order - shadow| <=
  epsilon`; the model abstracts this to "the shadow of the second arrived while
  the holder ordered". DET-09 (the disagreement alert after three ticks above
  epsilon) is therefore not modelled.
* **The decertification patience** (`shadow_misses > patience`), the degraded
  timer and the stop request (DEG-05), the request flood limit (SEC-06) and the
  liveness window (DEG-03/I9) are left out: they are observability and
  robustness, not the five safety invariants proved here.
* **QNX, the processes, the sockets.** The model stops at the arbiter.

## The cross-checker

```sh
python3 host/verify/tla_crosscheck.py            # model + TLC + C
python3 host/verify/tla_crosscheck.py --no-c     # model + TLC only
python3 host/verify/tla_crosscheck.py --max-tick 2
python3 -m unittest discover -s host/verify -p 'test_tla*.py' -v
```

It confronts three paths and exits non-zero on the first disagreement:

1. **model** — a breadth-first exploration of the same reduced machine, written
   in `tla_crosscheck.py` as a transcription of `Relais.tla`. The reduction is
   bounded on purpose (two machines, `MaxTick` 2 by default, small horizon and
   patience) so that it runs in seconds.
2. **TLC** — the real model checker, run on `Relais.tla` with the *same*
   constants, in a temporary directory so the repository stays clean. When the
   jar is missing the row says `not run`; it is never faked.
3. **C** — `build/relais-explore` for the verdict and the state count, and
   `build/relais-simulate` (five scenarios, with and without faults) for the
   `(mandate, holder)` pairs and the value sources the C code really produces,
   read from its JSON-lines journal.

What is compared: the violation verdicts (strict equality, all three must be
zero), the distinct state counts of the model and of TLC (strict equality: this
is what validates the transcription), the containment of every `(mandate,
holder)` pair the C produced inside the pairs the model admits, and the set of
value sources.

### What agrees, and the one gap that remains

With the default constants (`--max-tick 2`, hence `MaxTick=2, MaxMandate=2,
N=2, P=1, CertifyTicks=1`) the two independent paths agree exactly:

```
  model : 231168 distinct states, 6 (mandate, holder) pairs,
          sources ['fallback', 'holder'], 0 violation(s)
  TLC   : 231168 states, ok=True, violation=None
  C     : explorer ok=True states=60126 | simulator 3 pairs,
          sources ['fallback', 'holder', 'testament']

violations: model                      0                      0                      yes
violations: TLC                        0                      0                      yes
states: model vs TLC                   231168                 231168                 yes
violations: C explorer                 0                      0                      yes
states: C explorer                     informative            60126                  yes
violations: C simulator                0                      0                      yes
mandate/holder: C subset of model      0                      0                      yes
sources: C vs model                    ['fallback','holder']  ['fallback','holder','testament']  NO
```

Every safety verdict agrees: **zero violation on all three paths**, and every
`(mandate, holder)` pair the C code really produced is a pair the formal model
admits. The 231 168 = 231 168 equality is the strong one: the Python
transcription and TLC, reading the same `.tla`, count the same reachable states.

**The remaining disagreement is a coverage gap, not a contradiction.** At
`MaxTick=2` the reduced model never reaches the will, so it reports two sources
where the C simulator reports three. Measured, not guessed:

| constants | distinct states | sources reached |
|---|---|---|
| `MaxTick=2, N=1` | 231 168 | fallback, holder |
| `MaxTick=2, N=2` | 231 168 | fallback, holder |
| `MaxTick=3, N=1` | 400 000 (truncated) | fallback, holder, **shadow, testament** |
| `MaxTick=3, N=2` | 400 000 (truncated) | fallback, holder, **shadow, testament** |

Three ticks are needed for the substitution and the will to appear. Raising the
default to `MaxTick=3` is not free: TLC then counts 1 197 168 distinct states
while the Python breadth-first search stops at its `max_states` ceiling, so the
state-count row would disagree instead. The default stays at `MaxTick=2`, where
the two model paths can be compared exactly, and the tool reports the coverage
gap rather than hiding it -- which is what a cross-checker is for.

Reproduce every number above with:

```sh
python3 host/verify/tla_crosscheck.py                  # the table
python3 host/verify/tla_crosscheck.py --max-tick 3     # the coverage gap moves
cd docs/tla && java -XX:+UseParallelGC \
  -cp ~/.local/share/tla2tools/tla2tools.jar tlc2.TLC -config MC.cfg -workers 4 Relais
```
