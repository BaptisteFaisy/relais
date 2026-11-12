# Induction: from "explored down to depth k" to "true for every k"

Mission M44, idea 2 of `docs/DEEPENING` (the list of 100 ideas). This page
documents the k-induction engine of `host/explore/induction.c`, the rule it
applies, what it now proves **without a bound on the depth**, and what stays
bounded. The command-line driver is `build/relais-induction`; the unit tests
are `build/test_induction`.

```
$ make -j4 all && make test && make explore && make induction
```

## 1. What the exhaustive exploration proves, and what it does not

`host/explore` runs a depth-first search over the world of
`host/explore/world.c`: two computers, one actuator, six half-links, the
frames in flight and the faults. It hashes every world byte for byte, never
expands one twice, and stops when the tick bound or the state budget is
reached. What it establishes is therefore:

> **no safety invariant is violated in the first k ticks** (k = 12 by default,
> 100 198 concrete states, 0 violation).

That is a bounded statement. A protocol that keeps its invariants for twelve
ticks and breaks them at the hundred-and-thirtieth is perfectly consistent
with it. The induction removes that kind of statement.

The invariants checked are the ones of the **core library**
(`core/src/invariants.c`, reached through `relais_inv_*`): the explorer, the
seeded simulator and the offline verifier judge the same way, one
implementation, three uses. The induction engine does not re-implement a
single check: its oracle is `explore_world_ok`, the core's own verdict.

## 2. The inference rule

Let

* `S` be the set of worlds (the states), `Init` the initial world, and `→`
  the transition relation of `host/explore`: a move of
  `explore_world_enumerate` applied by `explore_world_apply`. A move is
  either a **tick** (the actuator applies exactly one value, the two computers
  compute, the frames in flight move) or an **adversary move** (drop, delay,
  duplicate, cut, restore, kill, freeze, thaw, restart) that does not advance
  time;
* `P` the safety property: the conjunction of the invariants of spec 7.2,
  as the core library checks them;
* `≡` the equivalence relation "same situation", the class of
  `host/explore/induction.c`.

The engine establishes four things:

| | statement | where it is checked |
|---|---|---|
| **(R1) BASE** | every state reachable from `Init` in at most `k` moves satisfies `P` | the existing depth-first search of `search.c`, reused as it is (`explore_search`), concrete states |
| **(R2) STEP** | for every situation `s` reachable, and every move `m`, if the state of `s` satisfies `P` then so does the state of `s·m` | the quotient search of `induction.c`, over the **whole** set of situations, not only the first `k` levels |
| **(R3) CLOSURE** | the set of reachable situations is closed: the successor of a situation is a situation of the set | the quotient search ends when it finds no new situation; the engine publishes whether it closed |
| **(R4) SOUNDNESS** | `P` depends only on the situation, not on what the situation forgets | a hypothesis, stated in section 4, inherited from `host/explore/symbolic.h` |

> **Theorem.** (R1) + (R2) + (R3) + (R4) ⟹ `P` holds on every reachable state,
> at every depth, for every `k`. **The bound on the depth is gone.**

*Proof.* By (R3) the set of reachable situations contains the initial one and
is closed under the transition relation: it is an **inductive invariant** of
the transition system. Every reachable state belongs to one of its situations.
By (R2), every situation of the set satisfies `P` (the engine checks `P` on a
representative of each, and by (R4) the verdict is the same for the whole
class). Hence every reachable state satisfies `P`, whatever its depth. ∎

(R1) is not a separate obligation in the implementation: the quotient search
checks `P` on **every** world it generates, including all the states of the
first `k` levels. The bounded search of `search.c` is kept and published as an
independent, concrete confirmation of the first `k` ticks.

## 3. The situation (the equivalence class)

The situation of a world is the protocol facts the safety invariants read:
the actuator's evidence and decisions (mandate relation, holder, misses,
certification streak, shadow misses, lamp, cause, degraded timer), the two
computers' discrete state (role, believed mandate, replica age, silence
counter, ready/deposed flags), the liveness of the frames in flight (which
slot holds which kind of frame, for which mandate), the invariant accumulator
(the memory of every safety check: gap, testament run, fallback timer, lamp
delay, reintegration timer, last mandate and holder) and the fault state (dead
computers, frozen computers, cut links, burst, pending mutations).

Three things are **deliberately forgotten**, and this is where the residual
risk lives:

* **the floats** (order value, shadow value, testament contents, the plant and
  the trajectory): no invariant of the list reads a value, except the
  continuity ones I4/I5, which are out of the explorer's reach anyway. This
  is the same argument as `host/explore/symbolic.h`;
* **the absolute tick**: it is replaced by an **age** (how long ago the fact
  happened) and by the **phase** of the rate-limit window (SEC-06);
* **the absolute mandate number**: it grows with every handover (MAN-03), so it
  is replaced by its **relation** to the actuator's mandate (equal, older,
  future), which is what the acceptance rule of an order reads (MAN-02).

The ages are **bucketed at the horizons that matter**, not saturated at 255:
the liveness window of the actuator (`LIVENESS_TICKS`, core/src/arbiter.c), the
window of I8 (N+1 ticks) and the "same tick or older" distinction the
computers make. Beyond a horizon, a fact cannot change any verdict, so its
exact age is not needed.

## 4. The hypotheses, stated precisely

* **H1 — the transition model.** One tick is atomic: the actuator collects
  what arrived, decides the one value it applies, the rack moves, the STATUT
  goes out, the holder computes first and hands its state to the second, then
  the second computes (spec 4.2). The adversary moves do not advance time and
  are the ones `explore_world_enumerate` allows: the single-fault hypothesis
  (one persistent machine fault at a time), one disrupted link at a time, and
  no burst of losses longer than `p` (ROB-03, spec 3.3, 7.3 item 5).
* **H2 — the abstraction is sound for the invariants.** The verdict of the core
  checker is a function of the situation alone. This is the hypothesis of
  `symbolic.h`, extended with the tick normalisation. It is *not* proven by
  this module; it is stated, and the same residual risk is documented there.
* **H3 — the ageing coarsening.** Two situations that differ only by an age
  beyond the horizon that reads it behave identically. The horizons are the
  ones of section 3.
* **H4 — one representative per situation.** The quotient graph is computed
  from one representative of each situation. A second representative of the
  same situation could, in principle, lead to a situation outside the closed
  set. The published closure therefore certifies the *explored* representatives;
  the unexpanded ones carry the same residual risk as `symbolic.h`.

## 5. What is proved, what stays bounded

Two configurations are published by `make induction`.

### 5.1 Without a fault (`--faults 0`): no bound on the depth

```
$ ./build/relais-induction --k 12 --budget 100000 --classes 60000 --faults 0
INDUCTION property=safety level=complet faults<=0 k=12
  base case : 1977 concrete states, 739 expanded, depth 12 ticks
  step      : 9203 situations, 21473 concrete states, 21473 transitions, deepest tick 1024
  quotient closed: yes (k_close=1023)
  invariants proved inductive: 11 / 13
  VERDICT: PROVED for every depth (no bound on k)
```

With `--faults 0` the adversary may still **duplicate** any frame in flight
(the duplication move is not a fault under the budget of spec 7.3): the
nondeterminism of the channel is still explored, only the losses, the cuts and
the machine faults are not.

The set of situations is finite and **closes** at 9 203 situations, after
1 023 ticks. By the theorem of section 2, the safety invariants of spec 7.2
then hold on **every** execution of the fault-free protocol, whatever its
length. The `k` published for each invariant is `k_close = 1023`: the depth
at which the induction closes.

### 5.2 With one fault (`--faults 1`): still bounded, and said so

```
$ ./build/relais-induction --k 12 --budget 100000 --classes 100000 --faults 1
  base case : 100272 concrete states, 34673 expanded, depth 12 ticks
  step      : 100001 situations, 207213 concrete states, 207213 transitions, deepest tick 2050
  quotient closed: NO (k_close=2050)
  VERDICT: BOUNDED -- no violation in the 207213 concrete states examined, but the set of situations did not close within the budget (explored to depth 2050)
```

The single-fault situation space is far bigger than the fault-free one (the
adversary multiplies the situations by the cut links, the dead computers and
the frames in flight). The engine explores 100 001 situations and 207 213
transitions without closing the set, so **no unbounded claim is made**: the
engine prints `BOUNDED`, and the invariant table says "bounded: the
situations did not close" for every invariant instead of claiming a proof.
What remains guaranteed there is the bounded exploration of `search.c`
(100 272 concrete states, 0 violation) plus the step verified over the
situations explored.

This is a **result, not a failure**: the engine states exactly where the
bound still is, and how far it went.

### 5.3 Two invariants out of reach

`I5` (continuity against the twin without fault) needs the twin, and `DET-09`
(an unmasked wrong value) needs a value fault (INJ-10). The world of
`host/explore` has neither, so the explorer never reaches those two checks —
the same scope statement as `host/explore/symbolic.h`. The table says
"not exercised by the explorer" for them, never "proved".

## 6. The witness: the proof has to bite

An engine that proves everything proves nothing. `host/explore/test_induction.c`
feeds it two properties that are certainly false and demands that it **refuse**
them, with a counter-example:

* `witness:the-tick-is-always-even` — the tick number is not always even;
* `witness:the-holder-is-never-lost` — the actuator does lose its holder when
  the holder dies (MAN-10 hands the mandate over).

```
$ ./build/relais-induction --witness --k 8 --faults 1
WITNESS holder-never-lost: REFUSED (clause 'witness:the-holder-is-never-lost' does not hold at tick 2048)
WITNESS tick-always-even: REFUSED (clause 'witness:the-tick-is-always-even' does not hold at tick 1)
```

The driver exits non-zero if a witness is ever certified, and `make induction`
runs it.

The same check is run against the **real core**, not only against synthetic
witnesses: `make induction-mutant` builds the eight mutants of the repository
and asserts that the engine **never certifies** the property of a defective
core.

```
$ make induction-mutant
M1: not certified
...
M8: not certified
INDUCTION-MUTANT-OK
```

Two of them are refused outright, with the invariant that breaks named:
M3 (`grant without evidence`) is caught as `DET-03 violated at tick 8` and M8
(`lamp off too early`) as I10. The six others are reported as `BOUNDED`: their
defect needs a fault, and the single-fault exploration does not close its set
of situations within the budget, so the engine does not claim anything about
them either way. That is the honest reading of a bounded search. The same mechanism is available for a strengthening: a property is a
**conjunction of clauses**, so a "renfort" (an auxiliary invariant, e.g.
`no-future-mandate`: no computer believes a mandate the actuator has not
handed out yet, MAN-02/SEC-04) is one more clause. The engine then proves
`P ∧ Q`; if that conjunction is inductive, `P` holds at every depth as well.
On the nominal core no strengthening is needed: the safety property is
inductive as it stands.

## 7. The difference with the exhaustive exploration

| | `host/explore` (search.c) | `host/explore/induction.c` |
|---|---|---|
| statement | no violation in the first `k` ticks | the property holds at **every** depth (when the set of situations closes) |
| states | concrete worlds, byte for byte | the situations of the concrete worlds |
| depth | bounded by the tick bound | unbounded (the quotient closes on its own) |
| verdict | `EXPLORE-OK` / `EXPLORE-ECHEC` | `PROVED for every depth` / `BOUNDED` / `REFUSED` |
| counter-example | the path of the breach | the path of the breach, plus the invariant that broke |
| hypothesis | none beyond the model | the abstraction is sound (H2) and one representative per situation (H4) |

The two are complementary: the bounded exploration is concrete and blind to
no abstraction, the induction is unbounded but rests on the abstraction. The
bounded exploration is the empirical check of the abstraction: it explores
the very situations the induction reasons about, and it finds nothing.

## 8. Reproduce

```
make -j4 all && make test          # the unit tests, including the witness
make explore                        # the bounded exploration: 0 violation
make induction                      # the table of invariants x k, the witness
```

Files: `host/explore/induction.h` (the API and the rule), `host/explore/induction.c`
(the engine), `host/explore/induction_report.c` (the table and the report),
`host/explore/induction_main.c` (the driver), `host/explore/test_induction.c`
(the tests and the witness).
