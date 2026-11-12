# Relais — open questions

*Published as a call for contribution. Each question is genuinely unsolved in
this repository, stated with the reason it is hard and what a convincing answer
would look like. The project's standing applies throughout: a demonstrator
aligned with Annex 6 of UN Regulation No. 79, with emulated measured timings —
nothing here is implied to be a proven production guarantee.*

Contributions are welcome on the questions below. A useful contribution is a
pull request that adds either a proof, a measurement, or a clearly-scoped
negative result, with the command that reproduces it.

---

## OQ1 — Two machines cannot arbitrate a wrong value

The full level *detects* a machine that computes wrong — the actuator compares
the holder's order with the ready second's shadow over 3 ticks and raises the
disagreement alert ([`docs/SPEC-2.md` DET-09](SPEC-2.md)) — but it does not
*arbitrate* it: with two machines there is no majority, so the actuator keeps
the holder and lights the red lamp ([`docs/SAFETY.md` §7 item 1](SAFETY.md)).

**The question.** Is there a two-party scheme that turns "detected" into
"masked" without a third machine? For instance, a credibility/plausibility check
against a reduced internal model (DET-10, level X), or a cross-credential where
each order must be countersigned by a shadow the sender cannot forge. What are
the precise conditions under which such a scheme can designate the faulty
machine, and what is its failure rate against a correlated value fault?

**Why it is hard.** Any discriminator that runs on a single actuator must itself
be trusted, and it must work against a fault that is *silent and correlated*
with the very state the scheme depends on.

**What an answer looks like.** A construction, its attack analysis, and a
mutant/campaign run that shows the faulty machine is both detected *and*
unmasked — or a proof that it is impossible with two parties.

## OQ2 — The arbiter itself has no software diversity

The actuator's arbitration logic is deliberately tiny and exhaustively explored
([`docs/RESULTS.md` §1](RESULTS.md), ACT-06), but it is one implementation. A
defect *in the arbiter* is the one thing no other component checks, because the
arbiter is the authority.

**The question.** How does one defend the arbiter against its own software
defect, short of a second physical channel (ACT-10)? Candidates: a simplified,
differently-written **shadow arbiter** that recomputes the decision and must
agree before it is applied; a periodic re-derivation of the arbitration decision
from first principles; or an independent specification (e.g. TLA+) against which
the C implementation is cross-checked by exploration of both.

**Why it is hard.** The existing explorer exhaustively checks the *states* of a
*confirmed* arbiter; it cannot discover a defect in the code it is already
running. Diversity must be added, and then the two must be made to disagree
visibly.

**What an answer looks like.** A second implementation of the arbitration
decision, a differential harness, and a demonstration that injecting a defect
into one of the two is caught.

## OQ3 — Scaling exploration in depth

Exploration reaches 100 198 states at one fault and 10 000 330 at two
([`docs/RESULTS.md` §1](RESULTS.md)). The memory-table of visited states is
exactly what caps the depth: each additional fault multiplies the reachable
set.

**The question.** How far can the same exhaustive guarantee be pushed? The
relevant techniques — symmetry reduction (the two machines are interchangeable
but for their role), partial-order reduction on commutable events, or
state-space compression of the in-flight frame buffers — have never been applied
to this core. Which one buys the most depth per byte of state table, and can a
proof that *scales* (rather than enumerates) replace the table for the
"no double command" property in particular?

**Why it is hard.** The interesting invariant (I2/I3: never two holders, never
a deposed machine applied) is global; local arguments about one machine tend to
miss the interleaving that breaks it.

**What an answer looks like.** A measure: states/second and peak states, before
and after the technique, with the invariant still checked in every state — or a
bounded model-checking argument that proves the property independent of the
concrete count.

## OQ4 — The control law has no functional-safety argument

The protocol is proven; the **control law** (the function the computers compute,
[`core/include/relais/control.h`](../core/include/relais/control.h)) is not.
It is a pure function, bit-identical across machines (CMD-03), but its *safety*
properties — boundedness, stability, no divergence within its parameter range —
are checked only empirically by `test_plant` (trajectories bounded over 100 000
steps, tracking error under 10 %, [`docs/RESULTS.md` §6](RESULTS.md)).

**The question.** What would a real functional-safety argument for the control
law look like — an input-space bounding argument, a Lyapunov/contraction proof,
or a checked-bounds (interval) numerical argument — and can it be carried by the existing
Python-standard-library tooling alone (the rule that forbids external numeric
dependencies)? At minimum: which properties must be proved to make the
"no jolt" and "safe stop" claims about the *physical* behaviour as strong as the
protocol claims already are?

**Why it is hard.** The plant is a second-order rack with disturbance; the
regulatory question (§5.3.3 of UN R79, translated in [`docs/SPEC.md` §1.2](SPEC.md))
is about the *behaviour* of the closed loop, not the protocol.

**What an answer looks like.** A stated property of the closed loop, a proof
sketch or a bounded exhaustive numeric argument, and the limits of what the
argument covers.

## OQ5 — What would a time-triggered bus change?

The design tolerates loss, duplication, reordering and delay *within* the
timing hypothesis `2Δ + C + J ≤ T` ([`docs/SPEC.md` §4.2](SPEC.md)); the honest
breaking point is published ([`docs/RESULTS.md` §3](RESULTS.md): 1 tick of
delay → gap 7, deviation 0.365). The whole arbitration therefore leans on the
*event* discipline of the tick.

**The question.** On a time-triggered bus (TTP, FlexRay, TTEthernet, or QNX's
time-aware networking), many of the channel faults Relais models simply cannot
occur — transmission is scheduled, reordering is eliminated, delay is bounded a
priori. What would that let the protocol *drop*? Could the tick label be
weakened, the miss-counter timeouts made exact, the mandate published on a
dedicated slot instead of piggy-backed on a status frame — and what, if
anything, would a time-triggered transport break in the current
"actuator as clock" argument, since there would then be a second source of time?

**Why it is hard.** It is a *redesign of the assumptions*, and the answer cannot
be asserted: it must be a variant of the core with the timing hypothesis
replaced, re-explored, and re-measured against the same invariants.

**What an answer looks like.** A fork of the core with the transport discipline
changed, the exploration and campaign re-run, and a diff-level statement of
which requirements became free and which got harder.

---

## How to contribute

1. Pick one question; open an issue quoting its `OQn` id.
2. A contribution is a PR with either code (a new host/ module, a core variant
   behind a compile flag), or a `docs/` note that states the negative or positive
   result and the exact command to reproduce it.
3. Do not touch `core/src/` without the mission's explicit scope: the core is
   reserved, and changes to it must go through the invariant and exploration
   gates unchanged (`make test`, `make explore`, `make campaign`).

The bar is the one the rest of the repository already sets: every claim carries
a command, and every number is the output of that command, not an estimate.