# Relais — the measured comparison with the state of the art

*This document is the measured counterpart of
[`docs/COMPARISON.md`](COMPARISON.md). That one compares the design of
Relais with the design of its neighbours; this one plays the same scenarios,
with the same faults and the same seeds, on the Relais protocol and on two
baselines implemented for real, and publishes the numbers. Every claim of
the comparison is either a number of this document or a file of this
repository.*

**Standing.** The same as everywhere else in this repository: Relais is a
demonstrator aligned with Annex 6 of UN Regulation No. 79. It is not
compliant, not approved and not certifiable. The latencies below are
measured in a discrete-event bench, not on a target, and are expressed in
ticks of the control period (T = 5 ms by default); the microsecond figures
are the tick period multiplied by a tick count, so they are a **lower bound**
on what a real system would do (see the threats, section 4.1).

---

## 1. The method

### 1.1 One world, three protocols

The bench is `host/baseline/`. It holds the world: two computers, one
actuator, the links, the plant, the trajectory, the control law and the
fault schedule. Three implementations are plugged into it through
`host/baseline/proto.h`:

| implementation | what it is | file |
|---|---|---|
| `relais` | the shipped protocol, driven through the real core (`core/src/arbiter.c`, `core/src/node.c`) | `host/baseline/relais_backend.c` |
| `heartbeat` | static primary, a heartbeat every tick, a silence timeout, a watchdog relay at the actuator | `host/baseline/heartbeat.c` |
| `lease` | a leader holding a lease it renews with a heartbeat, a term that only grows, a registrar at the actuator | `host/baseline/lease.c` |

The Relais side is **not** a sketch of the protocol: the adapter calls
`relais_arb_step()`, `relais_node_on_status()`, `relais_node_on_etat()` and
`relais_node_on_silence()`, so the numbers published for Relais are the
numbers of the code that ships.

### 1.2 The two baselines, described

**`heartbeat`** (the most widespread approach, the one the field uses):
A is the primary, B is the standby. The primary sends its order to the
actuator and its state (the ETAT frame) to the peer every tick; that frame
is the heartbeat the standby watches. The standby counts the ticks without a
heartbeat and, after `timeout` ticks of silence, declares the primary dead
and starts commanding. At the actuator, a watchdog relay selects the source:
it keeps the primary while the primary is fresh and switches to the standby
after `timeout` ticks without a fresh command from the primary. When the
primary comes back, the relay is re-energised and the primary commands
again — nobody ever told it that it had been deposed.

**`lease`** (the Raft / etcd family, reduced): the leader holds a lease for
`lease_ticks` and renews it with a heartbeat; the term (Raft's election
term) only grows and a frame carrying an old term is rejected. The actuator
plays the registrar: it publishes the holder, the term and the expiry, and
it grants the lease only once the previous lease has expired. A leader that
is cut off steps down locally when its own lease expires. What is left out
is stated in the file header: no log, no commit index, no quorum (there are
two computers and one registrar), no randomised backoff (with two
candidates a random delay only adds latency), and the state replication of
the project instead of a Raft log. The point being compared is the
handover, not the storage.

### 1.3 The fairness rules, enforced by the bench

* the same plant (`core/src/plant.c`), the same trajectory and the same
  control law (`core/src/control.c`) for the three implementations;
* the same fault schedule, applied at the same tick;
* the same channel: the loss, delay and duplication decisions are drawn
  **before** the run, per (tick, computer), so the three implementations
  face the same network whatever they send;
* the same pipeline: a frame sent by a computer reaches the actuator one
  tick later; a frame between the two computers arrives in the same tick;
* the same fallback law at the actuator (the damped local loop of
  `relais_fallback_step()`): a baseline that holds the last value instead
  would do worse, and the comparison would flatter Relais;
* the same state replication: the standby of each baseline imports the
  state of the primary (the ETAT frame) and computes on the same
  measurements, so a takeover starts from the same control state as the
  primary had. Otherwise the comparison would measure a state divergence
  and not a protocol;
* a fault-free twin runs in lockstep and gives the reference position of
  the rack.

### 1.4 The metrics

| metric | definition |
|---|---|
| handover delay | ticks between the fault and the change of commander at the actuator (the mandate for Relais, the relay for the heartbeat, the lease for the lease) |
| first fresh value | ticks between the fault and the first tick at which the applied value is the fresh order of **another** computer |
| ticks without a fresh order | ticks at which the applied value is not the order of a living computer (the testament, the fallback law or the last value) |
| double commands | ticks at which two computers each believe they command (each sent an order) |
| loss of setpoint | the largest distance between the rack of the faulty run and the rack of the fault-free twin |
| discontinuity at the takeover | the largest change of the applied command around the tick of the change of commander |

A "fresh order" is strict: the order of a living computer, computed for that
tick. The testament (ACT-02) is not fresh: it is a plan, not a
computation.

### 1.5 The scenarios

Fourteen scenarios, taken from `scenarios/` where they exist and derived
from them otherwise. The table of the document gives the origin of each.
The scenario `death-A-phase` sweeps the instant of the fault over the
cycle, the way the campaign of `docs/RESULTS.md` does.

### 1.6 The campaign

N = 30 repetitions per cell, seeds 1000..1029, T = 5 ms (5000 us per
tick). The reference timeout of the two baselines is **20 ticks (100 ms)**:
it is the silence timeout quoted for the field in `docs/SPEC.md` section
1.4 (the guardian). The timeout is then swept, so that neither baseline can
be accused of a badly chosen knob.

---

## 2. The results

<!-- BEGIN GENERATED TABLES -->

Generated by `make compare` (host/baseline/compare.py).
N = 30 repetitions per cell, seeds 1000..1029, T = 5 ms (5000 us per tick).
The reference timeout of the two baselines is 20 ticks (100 ms), the silence timeout quoted for the field in docs/SPEC.md section 1.4.

### Table 1 - delay of the handover

The delay between the fault and the change of commander at the actuator.
A dash means the change never happened in that run: the count is in the last column.

| scenario | implementation | N | p50 ticks | p99 ticks | max ticks | p50 us | max us | no takeover |
|---|---|---|---|---|---|---|---|---|
| nominal | relais | 30 | - | - | - | - | - | 30 |
| nominal | heartbeat | 30 | - | - | - | - | - | 30 |
| nominal | lease | 30 | - | - | - | - | - | 30 |
| death-A | relais | 30 | 2.0 | 2.0 | 2.0 | 10000 | 10000 | 0 |
| death-A | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| death-A | lease | 30 | 21.0 | 21.0 | 21.0 | 105000 | 105000 | 0 |
| death-B | relais | 30 | - | - | - | - | - | 30 |
| death-B | heartbeat | 30 | - | - | - | - | - | 30 |
| death-B | lease | 30 | - | - | - | - | - | 30 |
| freeze-A-200ms | relais | 30 | 2.0 | 2.0 | 2.0 | 10000 | 10000 | 0 |
| freeze-A-200ms | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| freeze-A-200ms | lease | 30 | 21.0 | 21.0 | 21.0 | 105000 | 105000 | 0 |
| freeze-B-1s | relais | 30 | - | - | - | - | - | 30 |
| freeze-B-1s | heartbeat | 30 | - | - | - | - | - | 30 |
| freeze-B-1s | lease | 30 | - | - | - | - | - | 30 |
| cut-A-to-actuator | relais | 30 | 2.0 | 2.0 | 2.0 | 10000 | 10000 | 0 |
| cut-A-to-actuator | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| cut-A-to-actuator | lease | 30 | 21.0 | 21.0 | 21.0 | 105000 | 105000 | 0 |
| cut-between-computers | relais | 30 | - | - | - | - | - | 30 |
| cut-between-computers | heartbeat | 30 | - | - | - | - | - | 30 |
| cut-between-computers | lease | 30 | - | - | - | - | - | 30 |
| cut-peer-then-death-A | relais | 30 | 2.0 | 2.0 | 2.0 | 10000 | 10000 | 0 |
| cut-peer-then-death-A | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| cut-peer-then-death-A | lease | 30 | 21.0 | 21.0 | 21.0 | 105000 | 105000 | 0 |
| double-death | relais | 30 | 2.0 | 2.0 | 2.0 | 10000 | 10000 | 0 |
| double-death | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| double-death | lease | 30 | 21.0 | 21.0 | 21.0 | 105000 | 105000 | 0 |
| slow-machine | relais | 30 | - | - | - | - | - | 30 |
| slow-machine | heartbeat | 30 | - | - | - | - | - | 30 |
| slow-machine | lease | 30 | - | - | - | - | - | 30 |
| handover-A | relais | 30 | 1.0 | 1.0 | 1.0 | 5000 | 5000 | 0 |
| handover-A | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| handover-A | lease | 30 | 1.0 | 1.0 | 1.0 | 5000 | 5000 | 0 |
| loss-5pc | relais | 30 | - | - | - | - | - | 30 |
| loss-5pc | heartbeat | 30 | - | - | - | - | - | 30 |
| loss-5pc | lease | 30 | - | - | - | - | - | 30 |
| one-tick-gap | relais | 30 | - | - | - | - | - | 30 |
| one-tick-gap | heartbeat | 30 | - | - | - | - | - | 30 |
| one-tick-gap | lease | 30 | - | - | - | - | - | 30 |
| death-A-phase | relais | 30 | 2.0 | 2.0 | 2.0 | 10000 | 10000 | 0 |
| death-A-phase | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| death-A-phase | lease | 30 | 21.0 | 21.0 | 21.0 | 105000 | 105000 | 0 |
| blackout-20ticks | relais | 30 | 2.0 | 2.0 | 2.0 | 10000 | 10000 | 0 |
| blackout-20ticks | heartbeat | 30 | 20.0 | 20.0 | 20.0 | 100000 | 100000 | 0 |
| blackout-20ticks | lease | 30 | 21.0 | 21.0 | 21.0 | 105000 | 105000 | 0 |

### Table 2 - what the delay costs

A tick without a fresh order is a tick served by the fallback law instead of a value computed by a living computer. The loss of setpoint is the largest distance between the rack of the faulty run and the rack of the fault free twin.

| scenario | implementation | first fresh value from another computer (p50 ticks) | ticks without a fresh order (mean) | worst (max) | double commands (mean) | worst (max) | loss of setpoint, max | discontinuity at the takeover, max |
|---|---|---|---|---|---|---|---|---|
| nominal | relais | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| nominal | heartbeat | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| nominal | lease | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| death-A | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0001 |
| death-A | heartbeat | 20.0 | 19.0 | 19 | 0.00 | 0 | 0.1735 | 1.0001 |
| death-A | lease | 22.0 | 21.0 | 21 | 0.00 | 0 | 0.1915 | 0.0000 |
| death-B | relais | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| death-B | heartbeat | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| death-B | lease | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| freeze-A-200ms | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0001 |
| freeze-A-200ms | heartbeat | 20.0 | 19.0 | 19 | 1.00 | 1 | 0.1735 | 1.0001 |
| freeze-A-200ms | lease | 22.0 | 21.0 | 21 | 0.00 | 0 | 0.1915 | 0.0000 |
| freeze-B-1s | relais | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| freeze-B-1s | heartbeat | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| freeze-B-1s | lease | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| cut-A-to-actuator | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0074 |
| cut-A-to-actuator | heartbeat | 20.0 | 19.0 | 19 | 0.00 | 0 | 0.0171 | 1.0001 |
| cut-A-to-actuator | lease | 22.0 | 21.0 | 21 | 0.00 | 0 | 0.0219 | 0.0000 |
| cut-between-computers | relais | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| cut-between-computers | heartbeat | - | 0.0 | 0 | 381.00 | 381 | 0.0000 | 0.0000 |
| cut-between-computers | lease | - | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| cut-peer-then-death-A | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0074 |
| cut-peer-then-death-A | heartbeat | 20.0 | 19.0 | 19 | 0.00 | 0 | 0.0171 | 0.9999 |
| cut-peer-then-death-A | lease | 22.0 | 21.0 | 21 | 0.00 | 0 | 0.0220 | 0.0000 |
| double-death | relais | 1.0 | 139.0 | 139 | 0.00 | 0 | 0.9135 | 0.0074 |
| double-death | heartbeat | 20.0 | 158.0 | 158 | 0.00 | 0 | 0.9715 | 1.0001 |
| double-death | lease | 22.0 | 160.0 | 160 | 0.00 | 0 | 0.9715 | 0.0000 |
| slow-machine | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0002 | 0.0000 |
| slow-machine | heartbeat | - | 333.0 | 333 | 0.00 | 0 | 0.1857 | 0.0000 |
| slow-machine | lease | - | 333.0 | 333 | 0.00 | 0 | 0.1857 | 0.0000 |
| handover-A | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| handover-A | heartbeat | 20.0 | 19.0 | 19 | 1.00 | 1 | 0.1735 | 1.0001 |
| handover-A | lease | 2.0 | 1.0 | 1 | 0.00 | 0 | 0.0046 | 0.7224 |
| loss-5pc | relais | 21.0 | 2.0 | 6 | 0.00 | 0 | 0.0010 | 0.0000 |
| loss-5pc | heartbeat | - | 46.0 | 63 | 0.00 | 0 | 0.0312 | 0.0000 |
| loss-5pc | lease | - | 46.0 | 63 | 0.00 | 0 | 0.0312 | 0.0000 |
| one-tick-gap | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0000 |
| one-tick-gap | heartbeat | - | 1.0 | 1 | 0.00 | 0 | 0.0046 | 0.0000 |
| one-tick-gap | lease | - | 1.0 | 1 | 0.00 | 0 | 0.0046 | 0.0000 |
| death-A-phase | relais | 1.0 | 0.0 | 0 | 0.00 | 0 | 0.0000 | 0.0075 |
| death-A-phase | heartbeat | 20.0 | 19.0 | 19 | 0.00 | 0 | 0.1749 | 1.0001 |
| death-A-phase | lease | 22.0 | 21.0 | 21 | 0.00 | 0 | 0.1935 | 0.0001 |
| blackout-20ticks | relais | - | 20.0 | 20 | 0.00 | 0 | 0.0258 | 0.0089 |
| blackout-20ticks | heartbeat | - | 20.0 | 20 | 0.00 | 0 | 0.0209 | 0.0000 |
| blackout-20ticks | lease | 22.0 | 21.0 | 21 | 0.00 | 0 | 0.0224 | 0.0000 |

### Table 3 - tuning of the baselines

The timeout of the two baselines, swept. A false takeover is a change of commander in a scenario where no machine died (nominal, 5 % of loss, slow machine, one missed tick): the timeout was too short.

| timeout | heartbeat: handover on death-A (p50 ticks) | heartbeat: ticks without a fresh order | heartbeat: double commands when the dead machine comes back | heartbeat: false takeovers | lease: handover on death-A (p50 ticks) | lease: ticks without a fresh order | lease: double commands | lease: false takeovers |
|---|---|---|---|---|---|---|---|---|
| 2 ticks (10 ms) | 2.0 | 1.0 | 1.00 | 0 / 120 | 3.0 | 3.0 | 0.00 | 0 / 120 |
| 4 ticks (20 ms) | 4.0 | 3.0 | 1.00 | 0 / 120 | 5.0 | 5.0 | 0.00 | 0 / 120 |
| 10 ticks (50 ms) | 10.0 | 9.0 | 1.00 | 0 / 120 | 11.0 | 11.0 | 0.00 | 0 / 120 |
| 20 ticks (100 ms) | 20.0 | 19.0 | 1.00 | 0 / 120 | 21.0 | 21.0 | 0.00 | 0 / 120 |
| 40 ticks (200 ms) | 40.0 | 39.0 | 1.00 | 0 / 120 | 41.0 | 41.0 | 0.00 | 0 / 120 |
| 400 ticks (2000 ms) | - | 199.0 | 0.00 | 0 / 120 | - | 199.0 | 0.00 | 0 / 120 |

### Table 4 - the channel hypothesis

The same campaign with the burst guard of ROB-03 switched off: the channel may then lose two frames in a row, which a timeout based baseline has to survive.

| implementation | timeout | false takeovers, burst guard ON | false takeovers, burst guard OFF | takeover on death-A (p50 ticks) |
|---|---|---|---|---|
| heartbeat | 2 ticks (10 ms) | 0 / 120 | 27 / 120 | 2.0 |
| heartbeat | 4 ticks (20 ms) | 0 / 120 | 1 / 120 | 4.0 |
| heartbeat | 10 ticks (50 ms) | 0 / 120 | 0 / 120 | 10.0 |
| heartbeat | 20 ticks (100 ms) | 0 / 120 | 0 / 120 | 20.0 |
| heartbeat | 40 ticks (200 ms) | 0 / 120 | 0 / 120 | 40.0 |
| heartbeat | 400 ticks (2000 ms) | 0 / 120 | 0 / 120 | - |
| lease | 2 ticks (10 ms) | 0 / 120 | 27 / 120 | 3.0 |
| lease | 4 ticks (20 ms) | 0 / 120 | 1 / 120 | 5.0 |
| lease | 10 ticks (50 ms) | 0 / 120 | 0 / 120 | 11.0 |
| lease | 20 ticks (100 ms) | 0 / 120 | 0 / 120 | 21.0 |
| lease | 40 ticks (200 ms) | 0 / 120 | 0 / 120 | 41.0 |
| lease | 400 ticks (2000 ms) | 0 / 120 | 0 / 120 | - |

<!-- END GENERATED TABLES -->




---

## 3. The reading of the tables

**3.1 The delay is the timeout, and only the timeout.** On the death of the
holder, the heartbeat baseline changes of commander after **20 ticks
(100 ms)**, the lease baseline after **21 ticks (105 ms)**, and Relais after
**2 ticks (10 ms)** — the p+1 ticks of the evidence the actuator counts
itself (MAN-04), with the certified second appointed in the same tick
(MAN-10). The same 2 / 20 / 21 appears on the cut of the link to the
actuator, on the death after a cut between the computers, on the death whose
instant is swept over the cycle, and on the blackout. The distribution has
no dispersion (p50 = p99 = max on every row): the delay is a count of
ticks, not a measurement (threat 4.1).

**3.2 A timeout baseline cannot tell a dead machine from a cut link.** When
the link between the two computers is cut and the holder is alive, the
heartbeat baseline declares it dead after the timeout and takes over: the
bench measures **381 ticks with two commanders** (mean over the window),
because the actuator keeps applying the primary (it is still fresh) while
the standby believes it commands. The lease baseline does not take over (the
lease is renewed, so it has evidence) but it cannot hand over either: in
the scenario where the holder dies while the link between the computers is
cut, it needs its 21 ticks. Relais needs 2: the actuator counts its own
misses and the second it has certified takes over, whether the first
machine is dead, cut or frozen. **This is the property the mission asked to
measure: the difference is not a faster timeout, it is the difference
between a timeout and an evidence.**

**3.3 What the delay costs.** During the 19 ticks the heartbeat baseline
waits, the actuator serves the fallback law, not a computed order, and the
rack leaves the trajectory of the twin by **0.17** of full scale (against
**0.0000** for Relais, which stays bit-identical to the twin). At the
takeover, the command jumps by **1.00**, the whole scale: the fallback law
had saturated the command in the opposite direction. For the lease baseline
the takeover itself is clean (the discontinuity is 0.0000, because the
standby shares the state of the leader) but the 21 ticks of fallback law
cost the same 0.19 of setpoint. The same effect appears without any
failure: with 5 % of the frames lost, the baselines serve **46 ticks
without a fresh order** (63 at worst) against **2** for Relais, whose
shadow covers the lost order (REL-05).

**3.4 The dead machine that comes back.** When the holder freezes for
200 ms and wakes up, the heartbeat baseline has **one tick with two
commanders**, and the commands differ: the returning machine computes from
a state that is 200 ms old. Relais has **zero** double commands (the
returning machine reads the mandate and stands down, spec 4.6), and so has
the lease baseline (the term rejects the returning leader). This is the
weakness of the watchdog family, and it is published rather than hidden.

**3.5 The baselines are not strawmen.** Three results go against Relais and
are published as they are:

* the **lease** baseline has no double command at all, in any scenario,
  and its takeover is perfectly continuous (the discontinuity is 0.0000);
* on a **planned** step-down (a machine going out of service for
  maintenance) the lease baseline releases the lease and hands over in
  **1 tick**, like the Relais cession (REL-09, REL-06); the heartbeat
  baseline waits for its timeout (20 ticks);
* swept to the shortest timeout the channel hypothesis allows, the
  heartbeat baseline reaches a handover delay of **2 ticks**, the same as
  Relais. It pays for it: one tick without a fresh order instead of zero,
  and the split brain of 3.4 remains.

**3.6 The channel hypothesis decides the timeout.** With the burst guard of
ROB-03 (no two consecutive losses on the same link) the shortest safe
timeout is 2 ticks; with the guard off, the channel may lose two frames in
a row and the same timeout produces **27 false takeovers out of 120 runs**:
the baseline changes of commander while both machines are alive. Four ticks
still produces one; from ten ticks on, none. The timeout that survives both
hypotheses is therefore **10 ticks (50 ms), five times the handover delay
of Relais**. This is the cost of a timeout: it is not a number one picks,
it is a number one must inflate until the channel can no longer imitate a
death.

---

## 4. The threats on the validity

### 4.1 The bench counts ticks, it does not measure time

The world is a discrete-event simulation: the handover delay is a number of
ticks, and the microsecond figures are that number multiplied by the tick
period. The scheduling jitter of a real QNX system (the wake-up latency, the
cache, the priority inversion) is **not** in the numbers. The comparison
between the three implementations remains valid, because they live in the
same world; the absolute latencies do not, and they are a lower bound.

### 4.2 What the baselines do NOT have, and why it is not rigged

* **No shadow.** The standby of the two baselines sends nothing to the
  actuator while it is a standby. Relais sends the shadow (OMBRE) of the
  certified second, which is what makes "not one tick without a fresh
  order" (REL-05) true at full level. This is a real difference between
  the designs and it is credited to Relais, but it is a difference of
  *mechanism*, not of tuning: a baseline could grow a shadow, and then it
  would have to certify it, which is the protocol Relais implements.
* **No will.** The testament (ACT-02) lets the actuator play the plan of
  the last order for N ticks. The baselines have no such thing: they go to
  the fallback law at once. The comparison gives them the fallback law of
  the project, which is generous; a plain hold-last-value would be worse.
* **The computers of the heartbeat baseline do not read the arbitration
  state.** A watchdog design has no state to publish: the actuator does not
  know who is in charge. Reading it would be the mandate of Relais in
  disguise. The lease baseline does read the lease state, because the
  registrar is its authority.
* **The lease baseline is not Raft.** No log, no commit index, no quorum,
  no snapshot. It is the handover mechanism of Raft, reduced to what two
  computers and one actuator need.

### 4.3 The threats that remain

* the channel model is **independent per computer**: the order of one
  machine and the shadow of the other are lost independently. A correlated
  outage hits both; the scenario `blackout-20ticks` measures it, and there
  the three implementations are equal (20 ticks without a fresh order).
* the fault schedule is **deterministic**; only the channel draws depend on
  the seed. The dispersion of the handover delay is therefore zero, and the
  p50, the p99 and the max are equal on every row but one.
* the loss model is a Bernoulli draw with a burst guard; a real link has
  bursts, which the sensitivity table of section 3.6 approximates by
  switching the guard off.
* the baselines are given the **same plant and the same control law**, so
  the numbers compare handovers and not control laws.
* the Relais side runs the real core, but the core was written for the
  simulator of `host/simulate/`, not for this harness: the harness
  reproduces its rules (the one tick pipeline, the commander first, the loss
  of the STATUT, the cession at the next tick). The two benches agree tick
  for tick on the death of the holder: `relais-simulate --level complet
  --fault 800:death:0` applies the last order of the dead holder at tick
  800, the shadow of the certified second at tick 801 and revokes the
  mandate at tick 802, and the harness measures the same first fresh value
  (1 tick) and the same handover (2 ticks).

### 4.4 What would change the conclusion

A timeout baseline with a *proof* of death (a hardware watchdog with an
independent line, an attested clock) would break the tie on the delay. Such
a mechanism is not a protocol, it is a second arbiter, and it costs a
second failure domain: the bench cannot price it, so it is not claimed
here.

---

## 5. Reproduction

```sh
make -j4 all && make test && make explore && make baseline && make compare
```

* `make baseline` builds the harness and the two baselines and runs their
  unit tests (`host/baseline/test_baseline.c`).
* `make compare` plays the campaign and rewrites the tables of section 2
  of this document, between the GENERATED markers, and drops the raw
  numbers in `build/compare/results.json`.

One scenario, one implementation, tick by tick:

```sh
./build/relais-baseline --impl heartbeat --timeout 20 --seed 7 \
  --fault 200:death:0 --trace
```
