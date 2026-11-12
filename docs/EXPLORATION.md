# Exploration: brute force, symbolic quotient, and minimal counter-examples

Level 1 of the proof (spec 7.3) is an exhaustive search of the core. This page
documents the two techniques that extend it, what each one buys, what each one
can hide, and the numbers measured on this tree.

All commands below run from the repository root after `make -j4 all`.

## 1. The brute-force search (baseline)

`host/explore/search.c` runs a bounded depth-first search over the world of
`host/explore/world.c`: two computers, one actuator, six half-links, the frames
in flight and the faults. At every state the adversary may deliver, drop, delay
or duplicate a frame, kill, freeze, thaw or restart a machine, cut or restore a
link, or let the tick beat. Each resulting world is hashed **byte for byte**
(`explore_world_hash`) and never expanded twice. The invariants checked are the
very ones the simulator and the verifier use (`relais_inv_*`), so what is
explored is what runs.

```
$ make explore
explored: 100198 states, 35764 expanded, deepest tick 12, 1.0 s, faults <= 1
EXPLORE-OK 100198 states, deepest tick 12, 0 violation
```

The whole world is hashed, so a state is a point in a very high-dimensional
space: the plant position and velocity, the control-law filter taps of both
computers, and the exact bytes of every frame in flight (an `ETAT` carries up to
256 bytes of filter state). Two points that differ only in a float that **no
safety invariant ever reads** are explored as two states. That is what caps the
depth: at a budget of 100 000 states the search reaches tick 12 and no further.

## 2. Symbolic exploration — the quotient (idea 6)

`host/explore/symbolic.c` projects a world onto a small **discrete signature**,
its equivalence class, and the search deduplicates on that signature instead of
on the whole world (`relais-explore --symbolic`). The class keeps exactly the
protocol facts the safety invariants read:

| kept | why |
|---|---|
| `relais_inv` (whole) | it *is* the memory of every safety check: counters, last mandate/holder/lamp, testament run, fallback timers. A state that already breached is never collapsed onto a clean one. |
| `relais_arb` (whole) | the actuator is the only writer of the mandate and the only judge of the evidence (MAN-02/MAN-04). Mandate, holder, misses, certification streak, testament horizon, lamp, liveness — the next decision, hence the next tick record, is a function of it. |
| `tick` | I11 (contiguity) and every "last tick seen" comparison are absolute. |
| discrete projection of each computer | role, believed mandate, last mandate/holder seen, when it last heard the actuator and its peer, replication age, silence counter, ready/deposed flags: these gate *which* frame is emitted with *which* tick and mandate — everything the arbiter uses to decide a miss, a grant or a revocation (DET-03, I2, I3). |
| fault and channel state | dead, frozen_until, cut, channel_link, burst, pending_mutations: they decide which moves the adversary may still play. |
| frame liveness | which slot is busy and for which tick it delivers: whether an order reaches the actuator for tick *n+1* is discrete, and I1/I6/I8/I10 read exactly that. |

**Deliberately forgotten** (and why it cannot hide a breach of I1, I2, I3, I6,
I8, I10, I11, DET-03):

- the **plant** (pos/vel) and the **trajectory** — they only ever reach the listed
  invariants through the *value* a machine computes, and none of the listed
  invariants reads a value (the value-continuity ones, I4/I5, are out of scope
  here); the setpoint is a pure function of the tick, identical in every state;
- the **float payload of the frames in flight** (order value, testament contents,
  STATUT measurement, ETAT filter bytes) — same reason; the arbiter already
  reduces the order/shadow comparison to the discrete `certify_streak` /
  `disagreement` / `shadow_misses`, all kept inside `relais_arb`;
- the **control-law filter taps and diagnostic counters** of the computers — they
  move future values, never a future verdict of a listed invariant.

### What it gains

Same budget, strictly fewer distinct situations, so the search reaches further.
Measured on this tree (one fault, `--faults 1`):

| config | brute states | symbolic classes | reduction |
|---|---|---|---|
| socle, depth 12, budget 100 000 | 100 198 | 49 306 | 2.03× |
| socle, depth 20, budget 100 000 | 100 340 | 49 452 | 2.03× |
| complet, depth 14, budget 60 000 | 60 323 | 28 262 | 2.13× |

```
$ ./build/relais-explore --budget 100000 --depth 12 --faults 1 --symbolic
explored: 100201 states, 49306 classes, 35224 expanded, deepest tick 12, 0.0 s, faults <= 1
EXPLORE-OK 100201 states, 49306 classes, deepest tick 12, 0 violation
```

### What it can mask (residual risk, stated honestly)

Dropping the exact floats means two worlds of one class could, **several ticks
later**, disagree about whether an order matched its shadow and so about a
*future* certification. The projection keeps the certification state as the
actuator holds it **after every tick**, so the risk is confined to a divergence
that has not yet happened — it is a forward-looking coarsening, not a present
one. Two guards:

1. the abstraction keeps `relais_inv` and `relais_arb` **whole**, so any breach
   that is already latent in the accumulator or in the actuator's evidence is
   preserved, never merged away;
2. `host/explore/test_symbolic.c` runs the quotient and the brute-force search on
   the **same budget** and asserts they reach the **same verdict**. Against a
   mutant build this is exercised on a real breach; `make mutant-explore` runs it
   on mutants 3, 4, 6 and 8.

If a future invariant were ever added that reads a raw value (as I4/I5 do), the
projection would have to be widened to keep that value — this is the single
condition under which the quotient could mask a breach.

## 3. Counter-example reduction — delta-debugging (idea 7)

When the search stops on a breach it hands back the whole path from the root,
which is rarely the shortest witness: it carries the frames and faults the DFS
happened to try first. `host/explore/shrink.c` reduces that path by
delta-debugging: it deletes events one at a time, then by blocks of decreasing
size, keeping a deletion only when the **same** breach survives a replay from a
fresh world. The oracle is the explorer's own world (`shrink_violates`), so
reduction can neither invent a violation nor keep a trace that no longer has one.

`relais-explore --shrink` reduces the counter-example before writing it, and
writes the result as a replayable scenario in the format of `CONTRACT.md`
(`shrink_write_scenario`).

### Measured on a real mutant

Mutant M3 (`-DRELAIS_MUTANT=3`, "mandate granted with no evidence of silence")
breaches DET-03. The raw counter-example the search finds, and its reduction:

```
$ ./build/m3/relais-explore --budget 20000 --depth 14 --faults 1 --level complet --shrink
EXPLORE-ECHEC after 3246 states, tick 14
first: DET-03 violated at tick 14: mandate granted while a holder was in place
shrink: 55 events -> 5 events (minimal counter-example)
```

**55 events → 5 events**, still breaching the same DET-03. The same breach is
found by the quotient search in fewer states (2 139 states / 978 classes against
3 246 states brute-force), and `test_shrink` asserts the reduced trace is strictly
shorter and violates the same invariant id.

## 4. How to reproduce

```
make -j4 all && make test          # 5665 checks, including test_symbolic/test_shrink
make explore                       # brute-force level 1: 100198 states, 0 violation
./build/relais-explore --symbolic  # the quotient, classes vs states
make mutant-explore                # quotient + reducer faced with mutants 3/4/6/8
python3 host/mutants/run_mutants.py  # the eight mutants, 8/8 flagged
```

`test_symbolic` proves the quotient is smaller than the brute-force search and
masks nothing on the same budget; `test_shrink` proves the reducer returns a
strictly shorter trace that still breaches the same invariant. Both run green on
the nominal core (no breach to find, so the properties hold vacuously) and on a
mutant build (a real breach to minimise).
