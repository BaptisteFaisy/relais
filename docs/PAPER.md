# Why the actuator must be the arbiter

*Relais — technical note. Two QNX 8.0 computers command one simulated
steer-by-wire actuator; one at a time; and the actuator, not the computers, is
the authority that decides which one.*

---

## 1. The problem: safe when stopped, safe when moving

A conventional steering assist can be *fail-safe*. If the computer fails, it
falls silent and the driver turns harder; the mechanical column still connects
the wheel to the wheels. What must be guaranteed is that the computer *stops
cleanly*.

Two evolutions break that reasoning, and they break it in the same way
([`docs/SPEC.md` §1.1](SPEC.md)):

- **Steer-by-wire.** Remove the column, and a silent computer leaves the wheels
  with no command at all.
- **Automated driving.** Remove the driver's hands, and nobody can take over
  within a second.

In both cases silence is no longer a safe state. The function must *keep
working*, at least long enough to reach a controlled stop. The system has to be
**fail-operational**, not fail-safe. The ordinary answer is to duplicate the
computer — and the entire difficulty then migrates to a single question: **who
commands, and how do you change hands without an error and without a gap?**

That question is not academic. UN Regulation No. 79, Annex 6, §3.4.3.2 asks
the manufacturer who uses a "transfer to a separate back-up system" to explain
the principle of the transfer mechanism, its logic, its level of redundancy,
its built-in controls, and the limits of the back-up's effectiveness
([`docs/SAFETY.md`](SAFETY.md) §1). Relais is, at small scale, exactly such an
answer: a transfer mechanism whose principle, logic, controls and limits are
written down and then checked by fault injection.

## 2. The state of the art, and its timeouts

Neighbouring projects solve the "who commands" question with a heartbeat and a
silence timeout. Two numbers, read from the public repositories compared in
[`docs/SPEC.md` §2](SPEC.md), frame the problem:

- `AhmedAdelWafdy7/autoware-qnx-guardian` supervises an automated-driving guest
  and reacts to **100 ms** of silence by stopping the vehicle.
- `TheUndefinedEngineer/Medical_Infusion_Pump` runs a main and a back-up sensor
  process with a **2 000 ms** silence timeout before it promotes the back-up.

Both are honest answers to a real problem. Both also illustrate the trap: the
timeout is a guess. Too short, and you depose a healthy machine that was merely
late (a *wrongful handover*). Too long, and the actuator runs without a fresh
command for the whole duration of the guess. The two projects resolve this
tension in opposite directions — one chooses 100 ms and accepts that it *stops*
rather than continues, the other chooses 2 000 ms and accepts a long command
gap — and neither has a bound measured across all the instants of the cycle.

The root cause is that the timeout belongs to a **supervisor that is not the
thing being commanded**. The decision to switch is made by a party that only
observes silence, never owns time, and cannot distinguish "the holder is dead"
from "the cable between me and the holder is cut".

## 3. The idea: the arbiter is the one that sets the tempo

Relais inverts the relationship. The **actuator itself** — the component that
applies the steering value — is the arbiter, and it is also the clock
([`docs/SPEC.md` §0](SPEC.md), ideas 1 and 2). Three consequences carry the
whole design:

1. **It hands out the mandates.** A computer never appoints itself. The mandate
   number is written by the actuator alone, increases by exactly one at every
   change of holder, and never decreases (MAN-01, MAN-03).
2. **It withdraws a mandate only on its own evidence.** p+1 consecutive ticks
   without a valid order from the holder — or an explicit cession from it
   (MAN-04). No other party may propose a handover (REL-04).
3. **It gives the tempo.** The actuator emits a status frame every period — the
   *tick*, period T = 5 ms by default — and every order names the tick it
   targets; an order for any other tick is dropped (MAN-02, ACT-09). No clocks
   are synchronised between the machines. An order computed before a freeze is
   stale *by construction*.

Because the actuator applies exactly one value per tick, chosen from a fixed
sequence (the holder's order, then the shadow of the ready second, then the
will, then a local fallback law — ACT-01), two machines that disagree cannot
both command: whatever they each believe, only the one the actuator itself
appointed is obeyed. Safety does not depend on the machines' clocks, nor on
frame delays. Time only affects how *fast* the handover is, never whether it is
correct.

The architecture diagram and the per-tick sequence are the inline diagrams of
[`docs/SPEC.md` §4.1 and §4.2](SPEC.md); standalone figures extracted from them
and from the charts of `host/report/` belong in `docs/figures/`.

## 4. Why this removes the "dead machine or cut cable?" dilemma, without a third computer

The classic two-computer deadlock is this: machine B stops hearing machine A.
Is A dead, or is only the cable between A and B cut? If B decides the answer for
itself and promotes itself, then in the cable-cut case there are now two masters
— the *double command* ([`docs/SPEC.md` §1.3](SPEC.md), SAFETY.md §2).

Relais makes the question irrelevant by moving it out of the computers. B never
decides anything: only the actuator revokes a mandate, and only on evidence *the
actuator itself* gathered — p+1 ticks with no valid order from the holder. When
A is out of touch with B but still reaches the actuator, the actuator keeps
seeing A's orders and no handover happens; the "link lost" state is signalled
within two ticks and the second keeps following the actuator's published status
without ever promoting itself (DET-06, REL-04). When A really is gone, the
actuator revokes on its own count and hands over.

The result, measured on the repository: a wrongful handover is **impossible by
construction** — the campaign swept deaths, freezes and the six half-link cuts
over all the instants of the period and recorded **zero** handover without the
actuator's own evidence ([`docs/RESULTS.md`](RESULTS.md) §1, §2, §4b). A
machine frozen for 200 ms and then thawed is disarmed twice over — its order
carries a stale mandate *and* a stale tick — and it rejoins as a second with no
intervention (REL-07, the "returning machine" of [`docs/SPEC.md` §4.6](SPEC.md)
and [`codelab/README.md`](codelab/README.md) Step 4).

This is the part that needs the least trust: it does not rely on the machines
agreeing, and it does not rely on any clock staying synchronised across a fault.

## 5. The will: a bounded plan carried inside every order

Handing the mandate over takes a few ticks even in the best case. The gap is
bridged by the **will** (*testament*): every order embeds the values the holder
plans for the following N ticks (N = 8 by default), and the actuator plays that
plan during the handover, up to the will's horizon and never beyond it (ACT-02,
I6). When the machines disagree or the holder vanishes, the physical curve shows
no hole.

Measured at the base level, the interval between two fresh orders after the
holder dies is at most **3 periods = 15 ms** at 200 Hz, bridged by the will
(REL-01). At the full level — where the second sends a shadow of its computation
every tick and the actuator adopts it from the very first miss — **no tick is
ever served without a fresh order** (REL-05). The twin-based continuity check
puts a number on the jolt: the deviation from a no-fault replay of the same
scenario stays at **0** on the no-fault run and at a measured **0.0253** of full
scale through the will-served handover ([`docs/RESULTS.md`](RESULTS.md) §1).

## 6. The evidence: three complementary proofs

Relais does not stop at asserting these properties. Three mechanisms, run from
one checkout, produce the numbers:

**Exhaustive exploration.** The protocol core is a pure C11 state machine — no
clock, no file, no socket, no allocation (ACT-06). That is what makes
exhaustive search possible: the explorer enumerates every bounded interleaving
of the three state machines, every in-flight frame, every allowed fault, and
checks the eleven invariants in every state reached. Measured on this
repository:

    ./build/relais-explore --budget 100000   --depth 12 --faults 1
    ./build/relais-explore --budget 10000000 --depth 16 --faults 2

→ **100 198 states, 0 violation** at one fault, and **10 000 330 states, 0
violation** at two simultaneous faults ([`docs/RESULTS.md`](RESULTS.md) §1).

**Differential campaign.** The same core runs in a seeded discrete-event
simulator with a fault-free *twin*: every faulted scenario is replayed without
the fault, and the deviation between the two curves quantifies the physical
effect of the fault. The campaign totals **19 659 runs, 0 failure**, including
3 972 swept deaths, 10 000 freezes up to 8 s, 5 040 link cuts, 12-hour runs
under 1 % frame loss, and a one-billion-tick no-fault run
([`docs/RESULTS.md`](RESULTS.md) §1, §2).

**Mutants, to prove the proof.** Eight deliberately defective builds of the
core — one that accepts anybody's order, one that ignores the tick label, one
that plays the will past its horizon, and so on — run the same campaign. The
bench must flag every one, because a mutant it does not flag is a defect *of the
bench*, not of the software. Result: **8 out of 8 flagged**, each by the
invariant it was meant to break ([`docs/RESULTS.md`](RESULTS.md) §5;
`python3 host/mutants/run_mutants.py`).

The unit and protocol layer underneath is **5 651 checks green** across eight
binaries (`make test`). These are the outputs of commands the jury can re-run;
nothing here is a target.

## 7. Limits, stated plainly

The design pays for its simplicity with honest limits, and it says so in the
repository rather than hiding them (SAFETY.md §7, RESULTS.md §1, §3):

- **A machine that computes wrong is detected, not outvoted.** Two computers
  give no majority. The actuator compares order and shadow over three ticks and
  raises the disagreement alert (DET-09), then keeps the holder and lights the
  red lamp. Masking the fault needs a third machine (level X, REP-08).
- **The actuator is a single point of failure.** The mitigation is the size of
  the core: one file, under 500 lines, explored in its entirety. A dual-channel
  actuator is an extension, not the delivered version (ACT-10).
- **A defect common to both copies of the same software is out of scope.** Both
  computers run the same binary; a dissimilar back-up is an extension (DEG-08).
- **Timings are emulated.** The 5 ms / 200 Hz figures are targets measured under
  QEMU/KVM, not proven worst cases; the QNX process layer compiles under
  `-Werror` against stubs but has not yet been run in a guest
  ([`README.md`](README.md) "Status of this tree").
- **The frame-authentication threat model has a boundary.** An attacker who can
  observe and inject on the actuator link in real time with the key is out of
  scope (SAFETY.md §7).

> **Standing of this document.** Relais is a demonstrator *aligned with*
> annex 6 of UN Regulation No. 79. It is **not** compliant, not approved and
> not certifiable: it is a small-scale, emulated answer to the question the
> annex asks, "explain your switch-over mechanism". Every latency quoted is a
> **measured worst case under emulation**, never a proven worst case.
> ([`docs/SAFETY.md`](SAFETY.md))

The contribution Relais makes is therefore narrow and specific: put the
arbitration where the tempo already lives, and the hardest question in a
two-computer handover — who commands, and how to change hands without a gap and
without a double command — becomes a small, exhaustively provable property of a
small state machine, instead of a timeout guess in a supervisor that is not the
thing being commanded.