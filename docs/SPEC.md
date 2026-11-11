# Relais — handover between two computers on QNX 8.0 — specification

**Working name: "Relais"** (in a relay race, one runner carries the baton at a
time and passes it without stopping).
Version 0.1 — 11 November 2026 — candidate project for the QNX Global
Hackathon, Automotive & Mobility category.
Submission deadline: Tuesday 15 December 2026 at 20:00, Paris time.

> This is the English translation of the original French specification
> ([docs/spec-fr.md](spec-fr.md)), which remains the reference of record for
> the jury. It is split for readability:
> **[SPEC.md](SPEC.md) — sections 0–4** ·
> **[SPEC-2.md](SPEC-2.md) — sections 5–8** ·
> **[SPEC-ANNEX.md](SPEC-ANNEX.md) — sections 9–16**.

---

## 0. Quick read

**What it is.** Two QNX 8.0 machines run the same control loop of a simulated
steering actuator. Only one command at a time. When it fails, the other takes
over within a bounded and measured delay, without a jump. The evicted machine
can no longer impose anything, even if it wakes up believing it is still in
command.

**The promise, in one sentence.** After any single fault (machine dead,
machine frozen, cable cut), the actuator is never left more than three periods
without a fresh order at base level (15 ms at 200 Hz), never a single one at
full level, and it never applies the order of a deposed machine. This is
demonstrated by exploration, not merely asserted.

**Four ideas carry the project.**

1. **The actuator is the arbiter.** It hands out mandate numbers, and it
   revokes a mandate only on its own evidence: the holder no longer sends it
   anything. Two machines that no longer agree with each other therefore
   cannot both command. The "dead machine or cut cable?" dilemma is settled
   without a third computer.
2. **The actuator gives the tempo.** It emits a state every period (the
   *tick*), and every order is valid only for the tick it names. No clocks to
   synchronise between the machines, and an order computed before a freeze is
   stale by construction.
3. **The will (testament).** Every order embeds the values planned for the
   following ticks. During the handover, the actuator plays the plan of the
   missing holder: the physical curve shows no hole.
4. **Proof by exploration, and proof of the proof.** The core of the protocol
   is a pure state machine, without clock or network. It is explored
   exhaustively on the PC, then machines are really killed, frozen and
   unplugged under QEMU, at every instant of the cycle. Deliberately
   defective versions of the software (the *mutants*) check that the test
   bench really sees faults.

**Three ambition levels.** Every requirement carries a level: **S** (base,
indispensable for a submission), **C** (full, the targeted version), **X**
(extreme, what pushes the idea to its limit).

**To be validated before writing the first line of product code** (section 10,
week 1): six feasibility trials. The most important one checks that two QNX
machines under QEMU exchange frames without administrator rights on the PC,
and at which latency.

**Status of the claims.** Section 14 separates what has been verified (text of
the regulation, QEMU options tried on this PC, neighbouring repositories read)
from what remains a design hypothesis.

---

## 1. Context

### 1.1 Fail-safe, or fail-operational

As long as a steering column connects the wheel to the wheels, the failure of
the assist computer is bearable: the assist stops and the driver turns harder.
It is enough that the computer falls silent cleanly. This is called
*fail-safe*.

Two evolutions break this reasoning:

- **Steer-by-wire.** Without a column, a computer that falls silent leaves the
  wheels without command.
- **Automated driving.** When nobody holds the wheel, nobody can take over
  within the second.

In both cases, falling silent is no longer a safe state. The function has to
continue, at least long enough to stop properly. The system must be
*fail-operational*. The usual answer is to duplicate the computer, and the
whole difficulty shifts to one question: **who commands, and how do you change
hands without error and without delay?**

### 1.2 Regulatory and normative framework

| Text | What has to be taken from it |
|---|---|
| **UN Regulation No. 79** (steering equipment), revision 4, series 03 of amendments | Admitts steering whose effort comes from a single energy source (*full-power steering*, §2.5.2.5), the framework for steer-by-wire. Detail below. |
| **UN Regulation No. 79, Annex 6** | Requirements for "complex electronic control systems": the manufacturer documents its safety concept, its strategy in case of defect, and the analysis that justifies it. |
| **ISO 26262:2018** (functional safety of road vehicles) | Provides the vocabulary of the time intervals: fault tolerant time interval, detection interval, reaction interval, and permitted duration of the back-up operation. Taken from secondary sources, the standard being paywalled. |

**Useful points of Regulation No. 79 for the design** (paragraph numbers read
on revision 4 of 7 November 2018; free translations, to be aligned with the
official French version and with the text in force):

- **§2.3.2.1 — command transmission**: all components through which the
  steering command signals pass. The two computers of this project are part of
  it.
- **§5.3.1.3**: any failure of a transmission other than a purely mechanical
  one is clearly signalled to the driver.
- **§5.3.3.1**: the vehicle must not be able to drive indefinitely above
  10 km/h with a fault that lights the warning signal.
- **§5.3.3.2**: in case of failure in the command transmission, it must remain
  possible to steer the vehicle **with the performance intended for the intact
  system**. This is the continuity requirement.
- **§5.3.3.3**: in case of failure of the energy source of the command
  transmission, at least 24 "figure-of-eight" manoeuvres (loops of 40 m
  diameter, at 10 km/h) remain possible at the performance level of the intact
  system.
- **§5.3.3.4**: in case of failure of the power transmission, no immediate
  change of the steering angle. This is the no-jump requirement.
- **§5.4.2.1.1 and §5.4.2.1.2**: a red signal for failures of the main
  equipment, a yellow signal for a fault detected electrically that does not
  light the red one.
- **Annex 6, §3.4.3**: among the possible provisions in case of defect, (b)
  "transfer to a separate back-up system". **§3.4.3.2**: in that case, the
  manufacturer explains the principles of the transfer mechanism, its logic, its
  level of redundancy, its built-in controls, and defines the limits of
  effectiveness of the back-up.
- **Annex 6, §3.4.4**: the analysis relies on a failure mode study or a fault
  tree, and on a validation, for example by tests with hardware in the loop.
- **Annex 6, §4.1.2**: the reaction of the system is verified under the effect
  of a failure.

This project is, at small scale, the answer to §3.4.3.2: a transfer mechanism
whose principle, logic, controls and limits are written down, then verified by
fault injection.

**ISO 26262 vocabulary reused in this document** (from secondary sources):

| ISO term | Meaning | Equivalent in Relais |
|---|---|---|
| Fault tolerant time interval (FTTI) | Time between the occurrence of a fault and a possible hazardous event, if nothing is done | Horizon of the will: beyond it, the actuator has no plan left |
| Fault detection interval (FDTI) | Time between the fault and its detection | p+1 ticks |
| Fault reaction interval (FRTI) | Time between detection and the controlled state | From the evidence to the first order of the new holder |
| Emergency operation time interval (EOTTI) | Time during which one may drive in back-up mode without unreasonable risk | Bounded duration of the degraded mode (DEG-05) |

**Production examples** (from the specialised press and Wikipedia, not
verified against manufacturer documents): the 2013 Infiniti Q50, the first
production car with permanent steer-by-wire, carried three computers and kept a
clutch that restored a mechanical link in case of failure. The Tesla
Cybertruck has no mechanical back-up and relies on duplicated motors and
triplicated sensors.

### 1.3 Four traps

1. **Dead or mute?** A machine that no longer hears its neighbour does not
   know whether the neighbour is dead or the cable is cut. If it takes over in
   the second case, two machines command. This is the *double command*.
2. **The returning machine.** A machine frozen for a few hundred milliseconds
   (overload, transient hardware fault, freeze of a virtual machine) resumes
   exactly where it stopped. It ignores that time has passed and sends an
   order computed for a situation that no longer exists.
3. **The jolt.** If the replacement starts from a different state
   (integrator, filter, position in the trajectory), the order jumps at the
   moment of the change.
4. **The wrongful handover.** A silence timeout that is too short deposes a
   healthy machine that was merely late. A timeout that is too long leaves the
   actuator without command.

### 1.4 Why a real-time microkernel

- **Detecting a fault by silence only works if the delays are bounded.** The
  more uncertain the latency of a system, the longer the silence timeout must
  be to avoid deposing a healthy machine. The two neighbouring projects read
  set that timeout at 100 ms and at 2 000 ms (section 2). Relais targets two
  periods, i.e. 10 ms. That gap is only tenable if the worst wake-up latency
  of a task is small and known.
- **Each role is an isolated process.** Network, protocol and control law are
  separated. The crash of one is known to the others by the closing of its
  connection, and the machine cedes the hand within the period instead of
  waiting for a silence timeout.
- **The critical path stays prioritised end to end.** From the received frame
  to the emitted order, the exchanges between processes go through message
  passing with priority inheritance. This shows up in a kernel trace.
- **The network stack is also a process** (`io-sock`). The priority at which
  it handles the protocol frames is a hypothesis to be measured (trial E5).

### 1.5 Hackathon framework

- **Category targeted**: Automotive & Mobility. Of the 133 public repositories
  visible on 11 November 2026, none describes a handover between two machines.
- **Jury criteria**, of equal weight: technical achievement, complete and
  coherent product, potential impact (a real problem in a QNX market, and why
  real time), innovation and creativity, including in the use of AI.
- **To be provided**: a new public repository under a free licence, a
  description, build instructions specifying the QNX version, the target and
  the toolchain, a video of less than three minutes on YouTube, all in English.
- **Available prizes**: category, grand prize, and two cumulative bonuses: best
  use of QNX-specific technologies, best tutorial.

| Jury criterion | The project's answer |
|---|---|
| Technical achievement | Arbitration by the actuator, orders addressed to a tick, state replication without a jump, purely core explored exhaustively, campaign of real faults |
| Complete product | From one-command launch to the campaign report, with a service tool, a dashboard and a tutorial |
| Impact | Steer-by-wire and automated driving: the function must survive the failure of one computer, and the regulation asks for the transfer mechanism to be explained |
| Innovation | The actuator as arbiter and as clock; the will; the mutants that prove the bench; at extreme level, the automatic search for the worst fault scenario |

---

## 2. Positioning against the existing state

Repositories read on 11 November 2026. None does a handover between two
machines.

| Project (public GitHub) | What it does | What it does not do |
|---|---|---|
| `TheUndefinedEngineer/Medical_Infusion_Pump` (April 2026) | Infusion pump on a Raspberry Pi 5 card: six processes, including a main sensor and a back-up sensor. Beat every 200 ms, silence timeout of 2 000 ms, promotion of the back-up announced in less than a millisecond. Demonstrations by forcibly stopping processes. | Redundancy between processes of the same machine. No protection described against two active at once. No automated test bench. |
| `AhmedAdelWafdy7/autoware-qnx-guardian` (hackathon) | A QNX guest supervises a Linux guest running automated driving. Silence timeout of 100 ms. On failure, it stops the vehicle with a minimal-risk manoeuvre. | It stops, it does not continue: that is *fail-safe*. No delay measurement nor fault injection to date, according to its README. |
| `zuhairhalawa/faultline` (hackathon) | Fault injection bench on a target: crash, freeze, processor steal, slow or stuck sensor. A supervisor restarts the processes. Verdict per step, report. | A single machine. No handover, no second. |
| `Lainholic/qnx-safepulse-lab` (hackathon) | A watchdog restarts a crashed driver (61 to 89 ms measured over 50 crashes) and goes into emergency stop after 50 ms of bus silence. | A single machine. |
| `atss0/qnx-hotswap` (hackathon) | Updates a driver while running, without its clients noticing (according to its description, repository not read in detail). | Continuity during a deliberate update, not during a failure. |

**Outside QNX**, cited from memory as landmarks and not re-verified: server
cluster managers, which isolate the suspicious machine by cutting its power;
floating IP addresses, whose failover is counted in seconds; QEMU virtual
machine replication (COLO); time-triggered automotive and aeronautical buses;
the command-monitor pairs of flight computers.

**What must distinguish Relais, without ambiguity:**

1. Two machines, not two processes: the whole machine can die, freeze or lose a
   cable.
2. Continue, not stop: the command is not interrupted.
3. The double command made impossible by construction, then verified by
   exhaustive exploration.
4. A bound measured at every instant of the cycle, not on a few trials.
5. A bench that tests itself with mutants.

**Rule of prudence.** The project presents itself as a demonstrator "aligned
with" Annex 6 of Regulation No. 79. It never claims to be compliant or
approvable. The delays are worst cases measured under emulation, never proven
worst cases.

---

## 3. Scope

### 3.1 Objectives

- Drive a simulated actuator from two QNX machines, one at a time.
- Hold a measured bound on the interval between two fresh orders, whatever the
  single fault and whatever its instant.
- Make the double command impossible, including against a machine that wakes
  up after a freeze.
- Demonstrate all of this with automated tests, reproducible by the jury on a
  PC, without hardware and without administrator rights.

### 3.2 Out of scope

- Real actuator, power electronics, duplicated power supply.
- Tolerance to a machine that computes wrongly: the project detects the
  disagreement (full level), it does not guarantee arbitrating it between two
  machines.
- A common defect in both copies of the same software. A dissimilar back-up is
  an extension (section 12).
- Attacker on the network beyond the authentication of frames.
- Sensor redundancy: the position and the setpoint come from the simulated
  actuator.
- Functional safety certification and type approval.

### 3.3 Fault model

| Code | Fault | Handling |
|---|---|---|
| F1 | Death of one machine (clean stop) | Tolerated |
| F2 | Freeze then resumption, any duration | Tolerated |
| F3 | Slow machine, missing ticks | Tolerated; deposed if the misses follow each other |
| F4 | Cut of the link between computers, one way or both | Tolerated without handover; redundancy reported as degraded |
| F5 | Cut of the link between one machine and the actuator, one way or both | Tolerated; handover if it was the holder |
| F6 | Loss, duplication, reordering, delay of frames | Tolerated within the bounds of the parameters |
| F7 | Corrupted or forged frame | Detected, treated as a loss |
| F8 | Wrong value emitted by a machine | Detected at full level, not tolerated |
| F9 | Second fault before the redundancy is restored | Safe state of the actuator |
| F10 | Death of both machines | Safe state of the actuator |
| F11 | Failure of the actuator itself | Out of scope (extension: two channels) |

**Single-fault hypothesis.** One fault at a time, until the redundancy is
restored. Double faults are not tolerated, but they must lead to the safe
state, and that is tested.

### 3.4 Technical assumptions

- QNX SDP 8.0 obtained through QNX Everywhere, image built with `mkqnximage`,
  QEMU x86_64 target. Both machines boot the same image and tell themselves
  apart by the MAC address of their network card.
- QEMU 8.2 or newer on a Linux PC, with hardware acceleration (KVM) for the
  timing measurements. No administrator rights required.
- Timing assumptions, to be measured and not assumed: a frame delay Δ of at
  most 1 ms on a healthy link, a computation time C of at most 0.5 ms, a jitter
  J of at most 1 ms.
- The simulated actuator is deemed reliable. Its arbitration logic is
  deliberately tiny so that it can be explored in its entirety.

---

## 4. Proposed design

This section describes a solution. It has no value as a requirement: only
sections 5 to 7 commit.

### 4.1 The actors and the bench

```
                Linux PC, no administrator rights

   ┌──────────────┐     inter-computer link      ┌──────────────┐
   │   QEMU  A    │                              │   QEMU  B    │
   │   QNX 8.0    │◄───────►┌───────────┐◄──────►│   QNX 8.0    │
   │   holder     │         │ programmed│        │   second     │
   └──────┬───────┘         │   cable   │        └───────┬──────┘
          │ link A          └───────────┘                │ link B
          ▼                                              ▼
   ┌─────────────────────────────────────────────────────────────┐
   │  simulated actuator: arbiter + rack model + tempo (tick)    │
   └──────────────────────────────┬──────────────────────────────┘
                                  │ journal, one record per tick
   ┌──────────────┐               ▼
   │ orchestrator │──► QEMU: death, freeze, thaw, cable unplugged
   └──────────────┘──► invariant checker ──► report
```

| Actor | Role |
|---|---|
| Machines A and B | Two identical QNX machines. Each computes the control law. One is the mandate holder, the other is the second. |
| Actuator | Simulates the rack, gives the tick, arbitrates the mandate, applies one value per tick, keeps the journal that is authoritative. On the PC at base level, in a third QNX machine at full level. |
| Programmed cable | Small relay on the PC between the two QEMU. It passes, drops, delays, duplicates or reorders frames, per message type and per direction. |
| Orchestrator | Launches the machines, runs a fault plan, drives QEMU (death, freeze, thaw, cable unplugged), collects journals and captures. |
| Verifier | Reads the journal of the actuator and checks the invariants. |

### 4.2 The tick: the actuator gives the tempo

```
   tick n                              tick n+1
     │ the actuator applies the value   │ the actuator applies the value
     │ intended for tick n               │ intended for tick n+1
     │ STATUT(n) ───────────► A and B    │ STATUT(n+1) ───────────► A and B
     │       A computes
     │       A ── ORDRE(n+1) ──► actuator
     │       A ── ETAT(n+1) ───► B
     │       B ── OMBRE(n+1) ──► actuator   (full level)
```

- The machines do not wake on their own clock but on the arrival of the
  `STATUT`. No clock synchronisation is necessary.
- An order names the tick it targets. The actuator discards any order that
  targets another tick than the next one.
- **Fresh order**: an order computed from the `STATUT` of the previous tick.
  It is the unit of measurement of the project.
- Cycle constraint: the round trip, the computation and the return must fit
  in the period, that is 2Δ + C + J ≤ T. With the assumptions of section 3.4:
  3.5 ms for a 5 ms period.

### 4.3 The mandate

The actuator holds two variables that only it modifies: the **mandate number**
and the **holder**.

| Rule | Statement |
|---|---|
| Acceptance | An `ORDRE` is applied only if it comes from the holder, carries the current mandate and targets the coming tick. |
| Revocation | The mandate is revoked only on the actuator's own evidence: p+1 consecutive ticks without a valid order from the holder (p = 1 by default), or a `CESSION` received from it. |
| Assignment, base | A non-holder sends a `DEMANDE`. The actuator grants it if the mandate is vacant, refuses it otherwise. The request may carry a first order. |
| Assignment, full | The second sends an `OMBRE` at every tick. The actuator *certifies it ready* when its shadows agree with the holder's orders. On revocation of the mandate, it switches by itself to the certified second. |
| Numbering | The number increases by 1 at each change of holder and never decreases. A machine cannot assign one to itself. |
| Publication | Every `STATUT` announces the mandate, the holder and the source of the applied value. A deposed machine learns it there. |

**Why the double command is impossible.** Two machines may *believe* they are
holders for an instant. That has no effect: the actuator applies only the
order of the holder it designated itself, for the coming tick. Safety depends
neither on the clocks of the machines nor on the frame delays. Time only plays
on the speed of the handover.

### 4.4 What the actuator applies at each tick

A single value, chosen in this order:

1. the `ORDRE` of the holder for this tick;
2. failing that, the `OMBRE` of the certified second for this tick (the
   *substitution*, full level);
3. failing that, the value planned by the will of the last order received, if
   its horizon is not exceeded;
4. failing that, the *fallback law*: a damped local loop that holds the
   position, red lamp lit.

### 4.5 Death of the holder, tick by tick

Worst case: A dies just after having emitted its order for tick n. Default
parameters (p = 1).

| Tick | Received by the actuator | Base: value applied | Full: value applied |
|---|---|---|---|
| n | `ORDRE` from A | A's order (fresh) | A's order (fresh) |
| n+1 | Nothing from A | A's will, first miss | B's `OMBRE` (fresh), first substitution |
| n+2 | Nothing from A; B's `DEMANDE` at base level | A's will, mandate declared vacant then granted to B | B's `OMBRE` (fresh); the mandate passes to B |
| n+3 | `ORDRE` from B | B's order (fresh), next mandate | B's order (fresh) |

- **Base**: at most 3 periods between two fresh orders, i.e. 15 ms, bridged by
  the will. A variant where the second asks at the first miss brings this delay
  down to 2 periods.
- **Full**: no tick without a fresh order. The mandate change becomes a
  formality, carried out 2 periods after the last order of A.
- **Double fault**: if B also dies, the fallback law applies at most N+1 ticks
  after the last fresh order (N = 8, i.e. 45 ms).

### 4.6 The returning machine

```
  time ────────────────────────────────────────────────────────────────►
  A   holder, mandate 7 │ frozen ░░░░░░░░░░░░░░░░░ │ thawed: sends ORDRE(mandate 7, old tick)
  B   second ────────────┴ substitutes, then holder of mandate 8 ────────────────────────────────
  ACT applies A          │ applies B; mandate 7 → 8            │ drops A; A reads the STATUT, becomes second
```

At thaw, A does not necessarily know that time has passed. Its order is
dropped twice over: it no longer comes from the holder, and it targets a past
tick. A discovers in the next `STATUT` that B holds mandate 8. It stops
emitting orders and resynchronises as second.

A freeze shorter than p+1 ticks changes nothing: the substitution or the will
bridges, and A stays holder.

### 4.7 Fault truth table

| Fault | What the actuator sees | What the other machine sees | Expected outcome |
|---|---|---|---|
| A (holder) dies | No more order from A | No more `ETAT` | Handover to B; yellow lamp |
| A freezes, then comes back | No order, then stale orders | No more `ETAT` | Handover to B; A's orders dropped; A becomes second |
| A freezes for less than p+1 ticks | An isolated miss | A missing `ETAT` | No handover; nothing to report |
| B (second) dies | No more `OMBRE` | No more replica acknowledgement | No handover; yellow lamp "second absent" |
| Link between computers cut | Nothing abnormal | Each stops hearing the other | No handover; B follows the `STATUT`s; yellow lamp "link lost" |
| A–actuator link cut, both directions | No more order from A | `STATUT`: holder's misses | Handover to B; A declares itself "without actuator" |
| A is no longer heard by the actuator | No more order from A | Same | Handover to B; A learns it from the `STATUT` |
| A no longer hears the actuator | No more order from A (it has no tempo) | Frozen `ETAT` | Handover to B; A deposes itself |
| B–actuator link cut | No more `OMBRE` | `STATUT`: second not certified | No handover; yellow lamp |
| A slow, isolated misses | Non-consecutive misses | — | No handover; counter published |
| A emits wrong values | `ORDRE` different from the `OMBRE` | — | "Disagreement" alert, red lamp |
| A and B die | Nothing | — | Will, then fallback law; red lamp |
| Corrupted or forged frame | Invalid check | — | Dropped and counted |

### 4.8 State replication and absence of a jump

- The state of the control law is small (integrator, filters, position in the
  trajectory: a few dozen bytes). The holder sends it **entirely at every
  tick** in an `ETAT`. There is no differential, no session, no catch-up.
- The second replaces its state with the received one, then computes on the
  same measurements as the holder. Same binary, same inputs: the same value
  is expected bit for bit, which allows the actuator to compare order and
  shadow.
- If the link between computers is cut, the second rebuilds a jump-free state
  from the applied value and the position published by the actuator.
- The measurement of continuity is a **fault-free twin**: the same scenario,
  replayed without fault by the simulator, gives tick by tick the reference
  value and position. The deviation to this twin quantifies the physical
  effect of each fault.

### 4.9 Software architecture of one machine

```
          UDP frames                  QNX messages              QNX messages
  links ◄──────────► ┌────────────┐ ───────────► ┌─────────────┐ ───────────► ┌────────────┐
                     │ relais-net │              │ relais-core │              │ relais-ctl │
                     │ (prio 62)  │ ◄─────────── │ (prio 61)   │ ◄─────────── │ (prio 60)  │
                     └────────────┘              └──────┬──────┘              └────────────┘
                                                        │ /dev/relais/status, temoin, ctl
                                                 ┌──────┴──────┐
                                                 │  relaisctl  │
                                                 └─────────────┘
```

| Process | Role | Priority | May access |
|---|---|---|---|
| `relais-net` | Sole owner of the sockets. Checks the shape and the authenticator of frames, hands them to the core. | 62 | The network, the core's channel |
| `relais-core` | Resource manager. Runs the protocol state machine, holds the role and the state replica. | 61 | The channels of `relais-net` and `relais-ctl` |
| `relais-ctl` | Control law: pure function of the state and the measurements. Knows neither the network nor the role. | 60 | The core's channel |
| `relaisctl` | Service tool: state, lamp, on-demand handover, test commands. | 10 | `/dev/relais` |

**Reaction scale.** A process that crashes is restarted in place, and the
machine cedes the mandate within the tick. A whole machine that disappears is
replaced by the handover. The two mechanisms complement each other: the first
is faster, the second covers everything.

### 4.10 A pure core, three uses

The protocol logic (the actuator's arbiter, a computer's state machine, the
frame encoding) is a portable C library. It reads no clock, opens no file and
no socket, allocates nothing: it takes an event and returns a new state and a
set of actions.

The same files are compiled three times: in the QNX processes, in the
simulator on the PC, and in the exhaustive explorer. What is explored is
therefore the code that runs, not a model written alongside.

### 4.11 Means of injection under QEMU

| Fault | Means | Status on this PC |
|---|---|---|
| Death of a machine | Brutal stop of the QEMU process, or the `quit` command | Command present |
| Death at a precise point of the code | Write to the `isa-debug-exit` device from the guest | Device accepted; access from QNX to be tried (E4) |
| Freeze and thaw | `stop` and `cont` commands | Commands present |
| Cable unplugged | `set_link` command | Executed successfully on a stopped machine |
| Silence without loss of carrier, loss, delay, reordering | Programmed cable between two `-netdev dgram` links | Syntax accepted; frame exchange to be tried (E1) |
| Warm restart | `system_reset` command | Command present |
| Proof | Capture of each link by `filter-dump` | Capture file created |

---

*Continued in [SPEC-2.md](SPEC-2.md): section 5 (requirements CMD…QNX-09),
section 6 (protocol and formats), section 7 (validation plan, invariants,
mutants) and section 8 (the demo).*
