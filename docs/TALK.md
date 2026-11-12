# Relais — workshop / conference talk (45 minutes)

*Support for a 45-minute session. Every command here exists in the repository
and was run to produce the numbers quoted; timings assume a warm build
(`make -j$(nproc)` done beforehand). Demos run on a plain Linux PC — no QNX
licence, no root, no hardware.*

**Audience ritual:** before the first command, say out loud the standing of the
project: *"Relais is a demonstrator aligned with Annex 6 of UN Regulation No. 79.
It is not compliant, not approved and not certifiable. Every timing I quote is a
measured worst case under emulation, never a proven worst case."* Say it once,
and point to it again whenever a number is shown.

---

## Minute-by-minute plan

| Time | What | Notes |
|---|---|---|
| 0–2 | **The problem.** Fail-safe vs fail-operational: a steering column makes silence a safe state; steer-by-wire and automated driving remove that | One slide, three sentences |
| 2–5 | **The state of the art, and its two timeouts.** 100 ms (`autoware-qnx-guardian`) and 2 000 ms (`Medical_Infusion_Pump`) | `docs/SPEC.md` §2 |
| 5–7 | **The four traps.** dead-or-mute, the returning machine, the jolt, the wrongful handover | `docs/SPEC.md` §1.3 |
| 7–9 | **The idea.** The actuator is the arbiter *and* the clock. Three consequences: it hands out mandates, revokes on its own evidence, gives the tempo | `docs/SPEC.md` §0 |
| 9–11 | **The will.** Every order carries the plan for the next N ticks | `docs/SPEC.md` §4.4–4.5 |
| 11–17 | **Demo 1 — the one-command demo** | below |
| 17–22 | **Demo 2 — death of the holder, journal read** | below |
| 22–25 | **Demo 3 — the revenant (freeze and thaw)** | below |
| 25–28 | **Demo 4 — the cable cut between computers** | below |
| 28–33 | **Demo 5 — exhaustive exploration** | below |
| 33–38 | **Demo 6 — the mutants (proof of the proof)** | below |
| 38–41 | **The honest limits** — single point of failure, emulated timings, common defect, QNX never run | `docs/SAFETY.md` §7 |
| 41–45 | **Questions** — the five below, and anything else | |

---

## The six demos, in order

### Demo 1 — the whole thing in one command (11–17)

```bash
make demo
```

**What happens:** three real processes start on the PC — one actuator, two
computers — over UDP, and play the specification's demonstration: a slalom, then
the second freezes, then the holder dies. The run ends with the handover journal
and the verifier's verdict.

**What to say while it runs:** *"One command, no root, no QNX, no QEMU. Look at
the last two columns: `max_gap=0` — not one tick went by without a fresh order —
and `max_deviation=0` — the curve compared against a fault-free twin of the same
scenario did not move. That is the whole promise in two zeros."*

Expected tail (deterministic variant `--demo --sim`):

```
  tick     event                  node     mandate
  2        mandate_granted        A        1
  904      mandate_revoked        A        1
  904      mandate_auto_switch    B        2
ticks=1600 violations=0 max_gap=0 (bound 0) max_deviation=0 (bound 0.02)
```

### Demo 2 — kill the holder, then read the evidence (17–22)

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet \
    --fault 500:death:0 --journal /tmp/fault.jsonl --twin-journal /tmp/twin.jsonl
./build/relais-verify /tmp/fault.jsonl --level complet --twin /tmp/twin.jsonl
```

**What to say:** *"At tick 500 the holder dies. Open `/tmp/fault.jsonl` around
tick 500 and walk four lines: the last order of A, the first shadow of B, the
tick where the mandate moves — exactly p+1 ticks after A's last order — and the
first order of B. Then note the verdict line: `violations=0`."*

**Follow-up worth a minute:** drop `--level complet` to run the same death at
base level, and watch the journal's `"source":"testament"` records bridge the
two ticks the second needs to ask and be granted. This is the will, visible.

### Demo 3 — the machine that comes back from the dead (22–25)

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet --fault 800:freeze:0:40
```

**What to say:** *"A freezes for 200 ms — an eternity at 5 ms. Two periods later
the actuator has moved on. When A wakes, it still believes it commands and sends
an order. Watch the rejection: it does not carry the current mandate, and it does
not target the next tick. A reads the next status, sees B holding mandate 2, and
stands down as second. No timeout, no vote, no split brain."*

Expected: `ticks=2000 violations=0` with `mandats 2` (A → B, A returns as
second), stale orders rejected.

### Demo 4 — cut the cable between the two computers (25–28)

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet --fault 600:cut-peer:0
```

**What to say:** *"Now cut the link between the two computers only. Nothing
happens — and that is the point. The second no longer receives the replicated
state, but it still hears the actuator's status, so it keeps following and
rebuilds its state. No handover is triggered, and that is exactly the
'dead or just disconnected?' dilemma settled without a third computer: a machine
that cannot hear its neighbour cannot promote itself."*

Expected: `mandats 1` (no handover), `violations=0`.

### Demo 5 — exhaustive exploration (28–33)

```bash
./build/relais-explore --budget 100000 --depth 12 --faults 1
./build/relais-explore --budget 10000000 --depth 16 --faults 2
```

**What to say:** *"The protocol core is a pure state machine — no clock, no
socket, no allocation — which is exactly why it can be enumerated. The first
command explores 100 198 states at one fault; the second, 10 000 330 states at
two simultaneous faults. Zero violation in both. What is explored is the code
that runs, compiled from the same files as the QNX processes — not a model
written alongside."*

### Demo 6 — prove the proof: the mutants (33–38)

```bash
python3 host/mutants/run_mutants.py
```

**What to say:** *"The same core, recompiled eight times with one deliberate
defect each. Each mutant faces a scenario the healthy core passes first — a probe
the nominal software fails would prove nothing. If the bench does not flag a
mutant, the bench is broken. Result: 8 out of 8 flagged, each by the invariant
it was built to break."* Point at M7: a defect in a *machine*, absorbed by the
arbiter, evidenced only by the journal — the defence-in-depth case.

---

## The five hard questions, answered honestly

**Q1 — "The actuator is a single point of failure. That is worse than a
timeout-based design."**

*Honest answer:* It is, and the repository says it out loud
([`docs/SAFETY.md` §7](SAFETY.md) item 3). The mitigation is that the
arbitration is one C file under 500 lines, with no allocation, no clock, no
I/O — small enough to explore exhaustively (100 198 and 10 000 330 states, zero
violation). A dual-channel actuator is the level-X extension (ACT-10), and it is
an extension exactly because we have not built it. We claim a *verified single
arbiter*, not redundancy of the arbiter.

**Q2 — "Your 5 ms and 200 Hz figures are emulated. They prove nothing about a
real ECU."**

*Correct, and we do not claim otherwise.* Every latency is labelled "measured
under emulation", published with its bounds, never as a proven worst case
([`docs/RESULTS.md`](RESULTS.md) §1, [`docs/SAFETY.md`](SAFETY.md) §7 item 5).
The honest breaking point is published too: one tick of delay still violates no
invariant but pushes the gap to 7 and the deviation to 0.365. The safety
properties (I1, I2, I3, I6, I8, I10) do **not** depend on time — only handover
*speed* does. Time is for the speed of the handover, not for its correctness.

**Q3 — "Both computers run the same binary. A bug kills both at once."**

*True, and out of scope, stated explicitly* ([`docs/SAFETY.md` §7](SAFETY.md)
item 2). The disagreement detector (DET-09) flags a machine that computes wrong
but does not outvote it; masking a common-cause defect needs a dissimilar
back-up (DEG-08), which we list as an extension and have not written. We carry
the fallback law — reached within N+1 ticks when both machines are gone — and we
test it, but we never pretend it covers a shared software defect.

**Q4 — "You never actually compiled or ran anything on QNX."**

*Correct.* The QNX layer (`target/`, `image/`) is written and syntax-checked
under `-Werror` against stubs via `make -C target syntax` — that proves the code
is well-formed C, not that it boots ([`README.md`](README.md) "Status of this
tree"). The QEMU bench, the image fragments and the six feasibility probes E1–E6
are all in the repository, but level 3 (real kills under QEMU) needs the SDP,
which we did not have for this session. Everything claimed is level 0–2: PC
core, exploration, seeded simulation — none of which requires QNX. That is the
honest frontier, and we would rather draw it clearly than blur it.

**Q5 — "Why not just use Raft or Paxos?"**

*Because the problem is different — and one worth five minutes.* Raft and Paxos
solve consensus over an unreliable network among participants that may disagree;
relais has only two participants and cannot tolerate the latency of a quorum and
a leader election on a 5 ms control loop. Consensus answers "which value wins";
the steering problem answers "who may command *this tick*", and its failure mode
is not disputes but silence and stale orders. The lock-token idea is the same one
M. Kleppmann describes in *How to do distributed locking* (2016): a token whose
holder can lose it without noticing is useless unless the guard itself checks it.
The actuator *is* the guard, and it is the clock — which is precisely what a
Raft cluster is not.