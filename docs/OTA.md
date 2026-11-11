# Rolling update without interruption (REL-10)

> Spec 12, item 6 — "update the two machines in turn without a single tick being
> served by anything else than a fresh order." Requirements: REL-10, REL-09,
> REL-06, REL-05, DET-05, DEG-03, DEG-07, MAN-03.

An automobile steering computer is updated over the air (OTA) for the whole life
of the vehicle. The two machines cannot be updated at the same time — that would
leave the rack with no pilot. They are updated **one after the other**, and the
question this document answers is the strong one:

> While the two machines are updated in turn, is there a **single tick** whose
> value comes from something older than a fresh order — the will (testament), or
> the actuator's emergency law?

The answer proved here is **no**. Every tick of the sequence is served either by
the holder's order or by the certified second's shadow (REL-05). The witness
never turns red, the emergency law never runs, and the mandate number only ever
grows, by exactly one step per change of holder (MAN-03).

## 1. The mechanism

Two pieces, both already in the core, plus one fault type in the simulator:

- **The cession (REL-09, DET-05).** A machine that is about to stop *cedes* the
  mandate first: `relais_node_on_handover` emits a `CESSION` frame in the same
  tick. The actuator records it and frees the mandate at the **next** tick
  (REL-06), appointing the certified second immediately. The order the holder
  already sent for the current tick is still applied under a known holder, so
  nothing is lost.
- **The reboot (DEG-03, DEG-07).** After the installation window the machine
  comes back through `relais_node_init`: an **empty** control state, no role, no
  mandate. It reintegrates as an unarmed second and the actuator must
  **certify it again** (REP-05, three matching ticks) before it can be trusted
  with the mandate. A rebooted machine never commands on the strength of what it
  was before the update.
- **The `update` fault (simulator).** `host/simulate/sim_faults.c` models the
  whole thing as one fault: cede (if holder), go out of service for `arg` ticks,
  then reboot through `relais_node_init`. A companion `handover` fault models the
  plain voluntary cession of a holder that stays up.

### Why the cession comes *first*

A planned absence must not pay for a silence detector. If the machine simply
stopped, the actuator could only conclude it was gone after **p+1** ticks without
a valid order (MAN-04) — the handover would depend on a timeout. By ceding
first, the machine hands the actuator a piece of evidence it can act on at the
very next tick, so the handover costs **one tick** instead of p+1, and the value
for that tick is the certified second's shadow — already fresh. `test_ota.c`
measures both paths side by side:

| way the holder is lost | ticks until the second holds |
|---|---|
| cession first (`update` / `handover`) | **1** |
| brutal stop (`death`) | **p + 1** |

## 2. The sequence

`scenarios/rolling-update.json` (level C, T = 5 ms, p = 1, N = 8). A holds the
mandate first; the instants below are the ones the deterministic run produces.

| step | tick | what happens | requirement |
|---|---|---|---|
| arm | 2 | A is granted mandate 1 | MAN-05 |
| | 5 | B certified as second | REP-05 |
| **update B** | 15 | B (the second) goes out for the installation | DEG-01 |
| | 32 | B decertified (its shadows stop) | DEG-07 |
| | 36 | B reboots, empty state, reintegrates | DEG-03 |
| | 40 | **B certified again** | DEG-07 |
| **cede A → B** | 50 | A cedes; at 51 B holds mandate 2, served by B's shadow | REL-09, REL-06, REL-05 |
| | 55 | A certified again as the second | REP-05 |
| **update A** | 65 | A (now the second) goes out | DEG-01 |
| | 90 | **A certified again** | DEG-07 |
| **cede B → A** | 100 | B cedes; at 101 A holds mandate 3 | REL-09 |
| nominal | 104 | B certified again — redundant state restored | REP-05 |

The mandate went **1 → 2 → 3**: one step per change of holder, never a jump,
never a repeat (MAN-03, I2).

### Gantt (text)

```
tick      0        20        40        60        80       100       120
          |---------|---------|---------|---------|---------|---------|
machine A  HOLDER....|.................|..cede->..|UPDATED..|..reboot.|HOLDER...
machine B  second....|UPDATED..|reboot.|certified.|HOLDER...|.........|..cede->.
mandate    1         1         1     2  2         2      3  3         3
command    fresh     fresh     fresh fresh        fresh    fresh      fresh
witness    off       yellow(B out)  off  off       yellow(A out)  off
```

Both machines are never down at the same time; the command is fresh on every
column.

## 3. The proof

`core/tests/test_ota.c` drives the very same world as the seeded simulator (one
actuator, two computers, the links, the plant) and checks, over the whole
sequence:

- **(a)** not one tick is served by anything else than a fresh order — the source
  is `holder` or `shadow` from the arming of the mandate to the end (REL-10,
  REL-05, I4 at level C);
- **(b)** the mandate grows by exactly one step per change of holder and ends
  back with A at mandate 3 (MAN-03, I2);
- **(c)** every machine that was updated is certified again **before** it takes
  the mandate back (DEG-07) — the marks `b_back < b_holds` and `a_back < a_holds`;
- **(d)** the invariant checker of spec 7.2 (`relais_inv`, the same one the
  explorer and `relais-verify` use) reports **zero violation**;
- **(e)** the witness is never red and the emergency law never runs.

A second test isolates the reason for the cession-first rule: the same instant,
two ways of losing the holder, and the handover costs 1 tick with the cession
against p+1 with a brutal stop.

```
$ ./build/test_ota
-- REL-10: two updates in a row, no tick without a fresh order
   ticks=121 violations=0 max_gap=0 (bound 0) ... level=complet
   armed=5 update(B)=15 B certified again=40 cession A->B=50 B holds=51
   A certified=55 update(A)=65 A certified again=90 cession B->A=100 A holds=101 ...
-- REL-06 / REL-10: the cession first makes the handover cost one tick
OK 28 checks
```

The same sequence replays deterministically through the simulator:

```
$ ./build/relais-simulate --ticks 200 --seed 1011 --level complet \
      --fault 30:update:1:20 --fault 60:handover:0 \
      --fault 80:update:0:20 --fault 120:handover:1
ticks=200 violations=0 max_gap=0 (bound 0) max_deviation=0 (bound 0.02) ... level=complet
```

### On a real PC

`host/bench/rolling_update.py` runs the same rolling update with three real
processes on the host (two computers + the actuator), triggers the cession, and
prints a line-by-line account of the mandate, the source and the witness. The PC
node binary has no `relaisctl handover` channel, so the bench plays the part of
the machine's maintenance tool and emits the very `CESSION` frame relais-core
sends on the QNX target.

The PC separates two kinds of claim, on purpose:

- **structural** (load independent): the mandate grew by one step per handover,
  two cessions drove the two handovers, the emergency law never ran, and the
  nominal redundant state was restored — this is what the demo proves on real
  processes;
- **real time** (load dependent): not one stale tick, the witness never red. On
  a quiet machine these hold; on a loaded shared PC the 5 ms deadline is missed
  now and then and DET-09 can latch the witness red on a start-up replication
  race. The rigorous, deterministic proof of the real-time claims is
  `test_ota.c` and the seeded simulator, not a wall-clock run — the same prudence
  the whole project states for its 5 ms / 200 Hz figures (measured on QNX under
  KVM, not on a shared PC).

## 4. The honest limit

This mechanism covers **unavailability**, not a **change of semantics**.

An update that only replaces a machine's software while the control law keeps
computing the same bits is exactly what is proved above: the machine is absent
for a bounded window, comes back empty, is recertified, and the command is never
stale.

An update that **changes the control law itself** — new gains, a new filter, a
different setpoint handling — is a different matter. Update the two machines in
turn and there is a window where A runs the *new* law and B still runs the *old*
one. They no longer compute the same bits, so:

- B's shadow stops matching A's order; the actuator sees a **disagreement**
  (DET-09) and the witness turns red — correctly, because the two machines really
  do disagree;
- B can no longer be **certified** while the laws differ, so the redundancy is
  lost for the whole window between the two updates;
- at the handover from the old law to the new one, the applied value can jump by
  whatever the two laws differ by — a **behavioural discontinuity** this
  mechanism does not and cannot smooth over, because there is no single agreed
  value to be continuous with.

In short: rolling update keeps the *service* continuous while a machine is
*down*; it says nothing about keeping the *behaviour* continuous while the two
machines run *different laws*. Covering that would require the two laws to be
behaviourally equivalent inside a tolerance for the whole overlap window (a
property of the laws, not of the protocol), or a coordinated switch of both
machines at a known point — outside the scope of REL-10.
