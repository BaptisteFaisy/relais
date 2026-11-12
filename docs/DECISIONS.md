# Relais — design decision log

One entry per design decision actually taken in this repository, recovered from
`git log` and from the comments next to the code (each entry cites both). The
format is: context, decision, alternatives rejected and why, consequences, date.
All dates are `2026-11-11`, the day every commit below landed.

This document records *why* the design looks the way it does. It does not
restate the protocol: for that see [`docs/PROTOCOL.md`](PROTOCOL.md) and
[`docs/SPEC.md`](SPEC.md).

---

## D1. The actuator is the arbiter and the clock

**Context.** Two computers command the same steering actuator. Whoever decides
which one is in charge, and who sets the tempo, becomes the authority the whole
safety argument rests on.

**Decision.** The actuator — not the computers — is both the arbiter and the
clock. It hands out mandate numbers, withdraws them only on its own evidence
(p+1 ticks of silence, or a cession), and applies exactly one value per tick.

**Alternatives rejected.**
- *The machines elect the holder* (a distributed vote): rejected because a
  machine that cannot hear its neighbour cannot tell "it is dead" from "the
  cable is cut", and both would then believe they hold the mandate. The actuator
  alone removes the ambiguity: it revokes only on evidence it can see itself.
- *A synchronised clock across machines*: rejected because the core is pure C11
  with no clock (`core/include/relais/types.h`), and synchronising clocks is a
  whole unsolved problem; the actuator's tick label is the only tempo.

**Consequences.** Double command is impossible by construction (I1–I3); safety
does not depend on the machines' agreement. The actuator becomes a single point
of failure (F11, out of scope — see `docs/SAFETY.md` §7 and `docs/DEBT.md`).

**Evidence.** `git b816451` — "the actuator is the arbiter and the clock. It
hands out mandate numbers, revokes one only on its own evidence…"; code comment
`core/src/arbiter.c:3-7` — "The actuator is the only writer of the mandate. It
revokes a mandate only on its own evidence… Two machines that no longer agree
therefore cannot both command".

---

## D2. The testament (will) is a prediction, not a hold

**Context.** When the holder goes silent, the actuator must keep serving a value
until a successor takes over. The naive answer is to hold the last order frozen.

**Decision.** The will is a *model-based prediction*: the holder fills it from
the state it had just computed with, so it extrapolates the manoeuvre instead of
freezing it. The successor that imports the replicated state computes what the
holder would have computed.

**Alternatives rejected.**
- *Freeze the last value*: rejected because a frozen command during a slalom
  produces a physical jump the successor cannot match; the will follows the
  manoeuvre so the substitution is bump-free (REL-03).

**Consequences.** The handover at level S is continuous (zero deviation from the
twin); the will is bounded by its horizon N (ACT-02, I6), after which the local
fallback loop takes over.

**Evidence.** Code comment `core/src/control.c:8-10` — "the testament is a
model-based prediction, so the successor that takes over from a replicated state
computes what the holder would have computed (REL-03)"; `core/src/control.c:59`
— "the slope of the steering request… is what lets the will follow the manoeuvre
instead of freezing it."

---

## D3. State is exported BEFORE the computation

**Context.** The holder replicates its control-law state to the second every
tick. The question is whether to snapshot the state before or after computing
this tick's order.

**Decision.** The state carried by `ETAT` is the one the holder had **before**
computing this tick's order. The second imports it, computes on the same
`STATUT`, and gets the same value bit for bit (CMD-03, DET-09).

**Alternatives rejected.**
- *Export after computing*: rejected because it would put the replica one tick
  ahead of its holder; the second would then compute a value for tick n+1 from a
  state that already includes tick n's effect, and the two would diverge — the
  actuator could no longer compare order and shadow.

**Consequences.** Order and shadow are bit-identical, which is what makes the
order/shadow disagreement check (DET-09) meaningful and keeps the handover
bump-free.

**Evidence.** Code comment `core/src/node.c:40-43` — "REP-01: the state carried
by ETAT is the one the holder had BEFORE computing this tick's order… Exporting
the state after the computation would put the replica one tick ahead of its
holder."

---

## D4. `testament_base = tick + 1`

**Context.** The will covers the ticks after the order it travels with. The base
tick — the first tick the will serves — has to be pinned down exactly.

**Decision.** The will carried by an order for `tick` starts at `tick + 1`,
because the holder filled it from the state it had just computed with for
`tick`. So `a->testament_base = tick + 1` (`core/src/arbiter.c:202`).

**Alternatives rejected.**
- *Start the will at `tick`*: rejected because the order for `tick` is already
  the fresh value for `tick`; overlapping the will onto `tick` would serve the
  same tick twice and make the horizon count wrong (I6).

**Consequences.** The horizon N is counted from `tick+1`; the verifier's
`testament_run` and the `N+1` bound (I8) line up exactly, and mutant M4
("testament past horizon") is caught.

**Evidence.** Code comment `core/src/arbiter.c:200-201` — "ACT-02: this order
covers `tick`; the will it carries starts at tick+1, because the holder filled it
from the state it had just computed with"; assignment at `core/src/arbiter.c:202`.

---

## D5. A cession takes effect at the next tick

**Context.** A machine may give up the mandate (cession, DET-05). The question
is whether the actuator frees the mandate in the same tick it records the
cession, or in the next one.

**Decision.** The actuator records the cession in the tick it arrives and frees
the mandate at the **next** tick (REL-06), so the order already received for the
current tick is still applied under a known holder, and the successor can be
granted in that same next tick.

**Alternatives rejected.**
- *Free the mandate immediately*: rejected because the order for the current
tick was computed under the ceding holder; applying it after the mandate has
already moved would serve a value from a machine that is no longer appointed.

**Consequences.** No value is served by a deposed holder; the cession and the
grant can happen back-to-back without a gap, which is why the demande may carry
its first order for the very tick it is granted (MAN-05).

**Evidence.** Code comment `core/src/arbiter.c:107` — "DET-05: the machine gives
up in the same tick; the actuator records it now and frees the mandate at the
next tick (REL-06)"; `core/src/arbiter.c:383-385` — "REL-06: a cession recorded
at the previous tick frees the mandate here, so the order already received for
that tick was applied under a known holder and the new one can be granted in the
same tick."

---

## D6. Bounds are claimed only under the timing hypothesis

**Context.** The handover bound "3 periods at level S" (REL-01) assumes frames
arrive within a budget. The simulator and the bench must not silently claim that
bound when the timing hypothesis is violated.

**Decision.** The bound is claimed only while the timing hypothesis
`2*delta + C + J <= T` holds (delta = link delay, C = compute time, J = jitter).
A channel that delays a frame by a whole tick is outside that hypothesis; the
verifier marks the run as faulted and I7 ("no wrongful handover") no longer
applies to it. Losses *inside* the hypothesis still count as normal operation.

**Alternatives rejected.**
- *Claim the bound unconditionally*: rejected because a whole-tick delay is not a
  lossy link — it is a different operating regime, and pretending the 3-period
bound still holds there would be the one claim in the repository that nothing
backs (the prudence rule of `docs/SAFETY.md`).

**Consequences.** Every latency figure published is a measured worst case under
the stated hypothesis, never a proven worst case; `relais_inv_fault_free` and
`relais_inv_lossy_channel` carry the distinction into the invariants.

**Evidence.** Code comment `host/simulate/main.c:117-123` — "A channel that
delays a frame by a whole tick is outside the timing hypothesis 2*delta + C + J
<= T: it IS a fault, so I7 no longer applies to it"; `git a1ff050` body — "a
channel delayed by a whole tick is outside the timing hypothesis 2d+C+J <= T, so
it is treated as a fault and I7 no longer applies to it; losses inside the
hypothesis still do."

---

## D7. The plant model is integrated exactly, and shared

**Context.** The simulator needs a rack model so that a fault's physical effect
can be measured. The twin without fault and the faulted run must be comparable.

**Decision.** One second-order rack model (semi-implicit Euler, NaN-clamped)
lives in `core/src/plant.c` and is shared by both machines, the actuator, the
simulator and the explorer. Nothing is drawn at random in the physics; the same
reference trajectory is followed tick by tick.

**Alternatives rejected.**
- *A separate model per process*: rejected because then the fault-free twin and
  the faulted run would not share a reference, and the deviation measurement
  (spec 4.8) would be meaningless.
- *Letting a divergent step produce NaN*: rejected because a run could silently
  diverge into NaN and hide a violation; every non-finite value is clamped
  (spec 7.4).

**Consequences.** The fault-free twin is a real reference: deviation from it
quantifies the physical effect of each fault, and the explorer runs the very same
plant as the simulator (`host/explore/world.c`).

**Evidence.** Code comment `core/src/plant.c:1-6` — "Both machines and the
actuator share it, so the twin without fault and the faulted run follow the very
same reference (spec 4.8)"; `core/src/plant.c:30-31` — "One semi-implicit Euler
step. Anything non finite is clamped, so a run can never silently diverge into
NaN and hide a violation (spec 7.4)."

---

## D8. The witness (lamp) follows the source, not the presence

**Context.** The lamp must report the state of the redundancy. The naive signal
is "is the second machine present?" — but presence is not the same as the command
being served by the holder.

**Decision.** The lamp follows the **source** of the value applied this tick
(`out->source`: holder / shadow / testament / fallback), not merely the presence
of a machine. When the source is not `HOLDER`, the redundancy is lost even while
the substitution keeps the command fresh, and the lamp goes yellow (DEG-01, I10).

**Alternatives rejected.**
- *Lamp = "second present"*: rejected because the second can be present yet
  uncertified, or the holder can be silent while the will serves — presence does
  not capture whether the command is actually fresh from the holder.

**Consequences.** The lamp is a faithful witness of the command path; mutant M8
("lamp out early while a link comes back, second uncertified") is caught because
the lamp must stay lit while the source is not the holder.

**Evidence.** Code comment `core/src/arbiter.c:305` — "DEG-01, DEG-02 and I10:
the lamp follows the cause and stays lit while it lasts"; `core/src/arbiter.c:318-320`
— "DEG-01: the holder is not delivering, the redundancy is lost even while the
substitution keeps the command fresh"; the lamp branches on `out->source`
(`core/src/arbiter.c:312-321`).

---

## D9. A disturbed channel is a fault

**Context.** The simulator can inject frame loss and delay. The question is where
the line is between "a lossy link the protocol absorbs" and "a fault".

**Decision.** A channel that delays a frame by a whole tick is classified as a
**fault**, not as normal lossy operation: it is outside the timing hypothesis
(`2*delta + C + J <= T`), so I7 no longer applies to it and the run is marked
faulted. Losses and delays *inside* the hypothesis are absorbed and I7 still
holds.

**Alternatives rejected.**
- *Treat every delay as a loss*: rejected because a whole-tick delay changes the
  operating regime — the order arrives for a tick that has already passed, which
  is exactly the stale-order case the tick label is designed to drop, not a
  benign loss.

**Consequences.** The fault model of spec 3.3 (F6, losses within bounds) stays
honest: the bench never claims a bound against a regime it does not cover, and
`relais_inv_lossy_channel` records the distinction for the verifier.

**Evidence.** Code comment `host/simulate/main.c:117-123` (same passage as D6,
read from the classification side); `git a1ff050` body — "it is treated as a
fault and I7 no longer applies to it; losses inside the hypothesis still do."

---

## D10. The explorer enumerates the tick last

**Context.** The exhaustive explorer (`host/explore/`) enumerates, at every
state, every choice the adversary could make: deliver/drop/delay/duplicate a
frame, kill/freeze/thaw/restart a machine, cut/restore any of the six half-links,
or let the tick beat. The order in which it enumerates these choices shapes the
depth-first search.

**Decision.** The tick is enumerated **last**. The DFS then dives first into the
paths where a fault is injected before the next tick beats — which is where the
interesting interleavings live — and still explores the pure-tick path, one level
later.

**Alternatives rejected.**
- *Enumerate the tick first*: rejected because the search would spend its depth
  budget on the boring "nothing happens" branch and reach the fault-injection
  interleavings only at greater depth, missing the interesting states within the
  bounded depth.

**Consequences.** Within the same depth budget the explorer reaches far more
interesting interleavings: 100 198 states / 0 violation with one fault, and
10 000 330 states / 0 violation with two faults, in under a minute.

**Evidence.** Code comment `host/explore/world.c:269-272` — "The tick is
enumerated LAST on purpose: the depth-first search then dives into the paths
where a fault is injected before the next tick beats, which is where the
interesting interleavings live. The pure-tick path is still explored, one level
later."; `git edb3ad7` — "The explorer enumerates what the seeded simulator draws
at random… Depth first, iterative, bounded."

---

## How to verify this log

Every decision above is cited against a commit subject/body and a code comment
that a reader can open:

```
git show b816451   # D1 (actuator as arbiter and clock)
git show a1ff050   # D6, D9 (timing hypothesis, disturbed channel)
git show edb3ad7   # D10 (explorer enumerates tick last)
sed -n '1,16p'   core/src/arbiter.c   # D1
sed -n '1,10p'   core/src/control.c   # D2
sed -n '40,43p'  core/src/node.c      # D3
sed -n '200,202p' core/src/arbiter.c  # D4
sed -n '107,110p;383,387p' core/src/arbiter.c  # D5
sed -n '117,123p' host/simulate/main.c         # D6, D9
sed -n '1,6p;30,31p' core/src/plant.c          # D7
sed -n '305,321p' core/src/arbiter.c           # D8
sed -n '269,272p' host/explore/world.c         # D10
```