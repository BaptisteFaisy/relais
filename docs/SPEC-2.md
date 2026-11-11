# Relais — specification, sections 5 to 8

Continuation of [SPEC.md](SPEC.md) (sections 0–4). The last part, sections 9
to 16, is in [SPEC-ANNEX.md](SPEC-ANNEX.md).

---

## 5. Requirements

Level: **S** base, **C** full, **X** extreme. Verification: **T** automated
test, **M** measurement, **I** code inspection, **D** demonstration.

### 5.1 Control loop and model

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| CMD-01 | The actuator paces the system: it emits a `STATUT` at every tick of period T (5 ms by default, adjustable from 1 to 50 ms). | S | T |
| CMD-02 | On receipt of the `STATUT` of tick n, each machine computes the value intended for tick n+1 and emits it before the end of the period. | S | T |
| CMD-03 | The control law is a pure function of the state and the measurements: same inputs, same outputs bit for bit on the two machines. | S | T, I |
| CMD-04 | The path from the received `STATUT` to the emitted order performs no memory allocation, no page fault, no disk access, and no wait on a less prioritised process. | S | I, M |
| CMD-05 | The actuator model (second-order rack with disturbance) and the reference trajectories (slalom, lane change, step) are provided and configurable by file. | S | D |
| CMD-06 | The order is bounded in amplitude and in rate of change by the emitting machine. | S | T |
| CMD-07 | A "slow motion" mode multiplies T (by 10 by default) without changing the logic: all protocol delays are expressed in ticks. | C | T |
| CMD-08 | A second demonstration profile drives a braking manoeuvre, with its own fallback law. | C | D |
| CMD-09 | The control law can be exchanged without touching the protocol, through a three-function interface: initialise, compute, export the state. | X | T |

### 5.2 Mandate and arbitration

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| MAN-01 | The actuator holds a 32-bit mandate number and the identity of the holder. Only it modifies them. | S | T, I |
| MAN-02 | The actuator applies an `ORDRE` only if it comes from the holder, carries the current mandate and targets the coming tick. Any other order is dropped and counted. | S | T |
| MAN-03 | The mandate number never decreases and increases by exactly 1 at each change of holder. | S | T |
| MAN-04 | The actuator revokes the mandate only on its own evidence: p+1 consecutive ticks without a valid order from the holder (p = 1 by default), or a `CESSION` received from it. | S | T |
| MAN-05 | A non-holder obtains the mandate through a `DEMANDE`. The actuator grants it if the mandate is vacant and refuses it otherwise. The request may carry a first order. | S | T |
| MAN-06 | At start-up, the mandate is vacant. The first valid request obtains it; the other machine becomes second. | S | T |
| MAN-07 | Every `STATUT` publishes the mandate, the holder, the source of the applied value and the number of consecutive misses. | S | T |
| MAN-08 | A machine that sees itself deposed in a `STATUT`, or refused, stops emitting `ORDRE`s before the next tick. | S | T |
| MAN-09 | A machine deprived of `STATUT` for 4 ticks considers itself deposed. Safety does not depend on this rule (MAN-02 suffices); it serves diagnostics. | C | T |
| MAN-10 | The certified second emits an `OMBRE` at every tick. The actuator switches the mandate to it by itself after p+1 misses of the holder, without any additional exchange. | C | T |
| MAN-11 | The mandate number survives the restart of the actuator. | C | T |
| MAN-12 | Every external effect of a machine other than the order (lamp, shared journal, message to a third party) carries the mandate number, so that its recipient drops the one of a deposed machine. | C | T, I |
| MAN-13 | With two actuator channels, the two mandates go to the same machine, or the divergence is signalled within one tick. | X | T |

### 5.3 Detection

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| DET-01 | The actuator notes the absence of order tick by tick. No detection delay depends on the clock of a machine. | S | T, I |
| DET-02 | The second suspects the holder when it receives no `ETAT` for one tick, or when a `STATUT` announces a miss. A suspicion has no effect without the actuator's agreement. | S | T |
| DET-03 | The loss of p consecutive orders does not cause any change of holder. | S | T |
| DET-04 | A frame with an invalid check, of unexpected size or version, is treated as lost and counted. | S | T |
| DET-05 | A machine that notices the death of one of its critical processes emits a `CESSION` within the tick, without waiting for a silence timeout. | C | T |
| DET-06 | Cut of the only link between computers: no handover; the "link lost" state is signalled within 2 ticks. | C | T |
| DET-07 | Cut of the only link between the holder and the actuator: handover to the second; the former holder declares itself "without actuator". | C | T |
| DET-08 | Cut in one direction only: the outcome is the one of the table in 4.7, verified for the six half-links. | C | T |
| DET-09 | At every tick the actuator compares the holder's `ORDRE` with the second's `OMBRE`. A deviation greater than ε for 3 ticks raises the "disagreement" alert. | C | T, M |
| DET-10 | On disagreement, a plausibility check by a reduced model designates the faulty machine when possible. Otherwise the actuator keeps the holder and lights the red lamp. | X | T |

### 5.4 Handover

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| REL-01 | After the death of the holder, the first fresh order of the new holder is applied at most p+2 ticks after the last fresh order of the old one (3 ticks, i.e. 15 ms at 200 Hz). | S | T, M |
| REL-02 | In the meantime, the actuator applies the will of the missing holder. | S | T |
| REL-03 | The first order of the new holder starts from the replicated state: its deviation from the value of the will at the same tick does not exceed δ (2 % of full scale by default). | S | T |
| REL-04 | A machine cannot trigger a handover on its own: the actuator's evidence is required. | S | T |
| REL-05 | With a certified second, no tick is served without a fresh order: the `OMBRE` substitutes from the first miss, and the mandate switches at tick p+1. | C | T, M |
| REL-06 | Consented handover: on `CESSION`, the mandate passes to the certified second at the next tick. | C | T |
| REL-07 | A former holder that reappears sees its orders dropped and ranges itself as second, without intervention. | C | T |
| REL-08 | Two successive handovers (from A to B, then from B to A after reintegration) happen without restarting the actuator and without a lost tick. | C | T |
| REL-09 | `relaisctl handover` triggers a consented handover, for maintenance. | C | D |
| REL-10 | Update without interruption: update the second, hand it the mandate, update the other one. No tick without a fresh order during the operation. | X | T |

### 5.5 State replication and continuity

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| REP-01 | The holder sends the second, at every tick, a complete `ETAT` of at most 256 bytes. | S | T |
| REP-02 | The second replaces its state with the received one, then computes. An `ETAT` older than the last received one is dropped. | S | T |
| REP-03 | The age of the replica, in ticks, appears in every `OMBRE` and every `DEMANDE`. | S | T |
| REP-04 | Without `ETAT`, the second rebuilds a jump-free state from the applied value and the position published by the actuator. | C | T |
| REP-05 | The actuator certifies a second "ready" after 3 consecutive ticks during which its `OMBRE` equals the `ORDRE` within ε. It withdraws the certification at the first deviation, or after more than p missing shadows. | C | T |
| REP-06 | Over the whole campaign, the deviation between the applied value and that of the fault-free twin stays below δ, and the position deviation below a bound fixed after the feasibility trials. | C | T, M |
| REP-07 | When two links between computers exist, the `ETAT` goes out on both; the second keeps the first valid copy. | C | T |
| REP-08 | With three machines, the actuator applies the value on which two machines agree. | X | T |

### 5.6 Actuator and safe state

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| ACT-01 | At every tick, the actuator applies exactly one value, chosen in the order of section 4.4, and journals its source. | S | T |
| ACT-02 | Every `ORDRE` embeds a will of N values (N = 8). The actuator never plays a value beyond the horizon of the most recent will. | S | T |
| ACT-03 | When the will is exhausted, the fallback law applies at the next tick and the red lamp lights. | S | T |
| ACT-04 | The exit from the fallback law is rate-limited, on a valid order of a holder. | S | T |
| ACT-05 | The actuator bounds every value in amplitude and in rate of change, whatever its source. | S | T |
| ACT-06 | The arbitration logic is a portable C library of less than 500 lines, without allocation, without clock and without input/output, identical on PC and on QNX. | S | I |
| ACT-07 | The actuator writes a journal with one record per tick and one record per event (section 6.4). | S | T |
| ACT-08 | The actuator also runs in a third QNX machine, so that the tempo is given by a real-time system. | C | D |
| ACT-09 | An order delayed by the network beyond its tick is dropped even if it comes from the holder. | C | T |
| ACT-10 | Two actuator channels, each with its own arbiter: the loss of one channel is tolerated. | X | T |

### 5.7 Degraded mode, signalling and reintegration

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| DEG-01 | Three lamp states, published in the `STATUT` and under `/dev/relais/temoin`: off (full redundancy), yellow (redundancy lost, command assured), red (fallback law or disagreement). | S | T |
| DEG-02 | The lamp stays lit as long as the cause lasts, and the cause is readable: machine absent, link lost, second not certified, disagreement, fallback law. | S | T |
| DEG-03 | A restarted machine joins as second without disturbing the command: during its reintegration, every tick is served by the holder. | S | T |
| DEG-04 | A machine becomes a certified second less than 2 s after the end of its start-up (target). | C | M |
| DEG-05 | Bounded duration of the degraded mode: after 30 s without redundancy (demonstration value), the system raises "stop requested", mirroring the rule that forbids driving indefinitely with a signalled fault. | C | T |
| DEG-06 | Second fault in degraded mode: will, then fallback law within at most N+1 ticks. | C | T |
| DEG-07 | A deposed machine becomes eligible again only after a new certification: the mandate does not bounce back and forth at every tick. | C | T |
| DEG-08 | A dissimilar back-up (simplified control law, written separately) can hold the role of second, to cover a defect common to both copies of the software. | X | T |

### 5.8 Real time

The values are initial targets, to be adjusted after the feasibility trials.
They are to be read as **measured worst cases**, not proven worst cases.

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| PERF-01 | From the `STATUT` received by the machine to the order emitted: 1 ms at the 99.9th percentile, 2 ms in the worst case, under a processor load of lower priority. | S | M |
| PERF-02 | Outside an injected fault, fewer than one tick in 10 000 is served otherwise than by the holder's order, over 30 minutes. | S | M |
| PERF-03 | Over 200 deaths of the holder, the interval between two fresh orders never exceeds p+2 ticks. | S | M |
| PERF-04 | With a certified second: zero tick without a fresh order over 2 000 deaths and 10 000 freezes. | C | M |
| PERF-05 | The mandate change happens exactly p+1 ticks after the last order of the holder. | C | M |
| PERF-06 | The same measurements hold under three loads: processor saturated by less prioritised tasks, burst of frames on the links, memory pressure. | C | M |
| PERF-07 | The three worst cases of PERF-01 are explained by a kernel trace showing where the time went. | C | D |
| PERF-08 | The period is lowered to the breaking point. The smallest period for which PERF-02 holds is published. | C | M |
| PERF-09 | The same measurements are redone on two real boards linked by Ethernet. | X | M |

Under QEMU, the two machines share the PC's processor: the durations do not
represent real hardware. They are presented as emulated, with their full
distribution.

### 5.9 Robustness

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| ROB-01 | The invariants of section 7.2 hold for any single fault of the model, at any instant. | S | T |
| ROB-02 | Duplicated, reordered or delayed frames (by less than one tick) have no effect on the command. | S | T |
| ROB-03 | The loss of 10 % of the frames of a link, without a burst of more than p, does not change the holder. | S | T |
| ROB-04 | A fault during a handover, a reintegration or a cession respects the invariants (at most two nested faults). | C | T |
| ROB-05 | A freeze of any duration, from one tick to ten minutes, of either machine in either role, respects the invariants. | C | T |
| ROB-06 | A warm restart of the holder is treated as its death, then as a reintegration. | C | T |
| ROB-07 | The crash of every process of every machine, at random instants, leads to a cession or to a handover, then to a local restart. | C | T |
| ROB-08 | A slow machine (processor stolen one tick out of three) is deposed if it misses p+1 consecutive ticks. Never a double command. | C | T |
| ROB-09 | No frame parser ever crashes, whatever the input: 24 h of random data testing. | X | T |
| ROB-10 | The two machines dead then restarted together: exit from the fallback law without a jump. | X | T |

### 5.10 Security

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| SEC-01 | A malformed frame, of unknown version or from an unknown sender, is rejected and counted, without effect on the service. | S | T |
| SEC-02 | Every frame carries an authenticator computed with a key specific to the link. A frame forged without the key is rejected. | C | T |
| SEC-03 | The replay of an authentic captured frame is rejected by the tick tag. | C | T |
| SEC-04 | No machine can assign itself a mandate. An `ORDRE` carrying a future mandate is rejected and reported. | C | T |
| SEC-05 | A QNX security policy assigns a type to each process: only `relais-net` opens sockets, only `relais-core` serves `/dev/relais`. The test commands are absent from the production image. | C | T |
| SEC-06 | Mandate requests are rate limited. A flood of requests does not delay the processing of orders. | C | T |
| SEC-07 | Keys are renewed at every start-up by an authenticated exchange. | X | T |

### 5.11 Observability

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| OBS-01 | The processes write to the QNX system log (slog2). | S | D |
| OBS-02 | `/dev/relais/status` gives the state as text: role, mandate, replica age, counters, lamp. | S | D |
| OBS-03 | User events are inserted in the kernel trace: `STATUT` received, order emitted, role change, cession. | S | M |
| OBS-04 | The bench captures each link in a pcap file. | S | T |
| OBS-05 | A campaign report is generated as a self-contained HTML: table per fault type, histograms, worst cases, curves superimposed on the fault-free twin. | S | D |
| OBS-06 | A live dashboard shows who holds the mandate, the source of each tick, the command and the position. | C | D |
| OBS-07 | `relais-verify` reads an actuator journal and checks all the invariants offline. | C | T |
| OBS-08 | The journal is exported in MCAP format, for reading in a standard viewer. | X | D |

### 5.12 Fault injection bench

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| INJ-01 | A single command launches the two QNX machines and the actuator, without administrator rights on the PC. | S | D |
| INJ-02 | Death: brutal stop of one machine at an instant chosen with respect to the tick. It is the bench, which knows the tempo, that triggers it. | S | T |
| INJ-03 | Freeze and thaw of a machine, configurable duration. | S | T |
| INJ-04 | Cut and restoration of a link, with and without loss of carrier. | S | T |
| INJ-05 | Programmed cable between computers: loss, delay, duplication, reordering, per frame, per message type and per direction. | S | T |
| INJ-06 | Each test is described by a file (scenario, faults, instants, seed) and is replayable. The report cites this file. | S | T |
| INJ-07 | Death points: the machine powers off at a precise point of the code (section 7.5) at the requested tick. | C | T |
| INJ-08 | Sweep: 100 instants per period, for each fault type, each role and each phase of the protocol. | C | T |
| INJ-09 | Slow machine: processor steal by repeated short freezes, and by a higher-priority task inside the guest. | C | T |
| INJ-10 | Value fault: a test command adds a bias to the orders of one machine. | C | T |
| INJ-11 | Mutants: variants of the software with a deliberate defect (section 7.6) run the same campaign, which must flag each of them. | C | T |
| INJ-12 | Automatic search for the worst case: an optimiser chooses fault instants and durations to maximise the deviation from the fault-free twin. | X | T |
| INJ-13 | Three benches run in parallel on the same PC, and their results are merged. | X | T |

### 5.13 Use of QNX-specific technologies

| ID | Requirement | Level | Verif. |
|---|---|---|---|
| QNX-01 | `relais-core` is a resource manager; its interface is reduced to `open`, `read` and `devctl`. | S | I |
| QNX-02 | Timers and notifications between processes use pulses. No active waiting. | S | I |
| QNX-03 | The critical processes run at fixed priority and lock their memory at start-up. | S | I |
| QNX-04 | A kernel trace shows the path from the `STATUT` to the order and the effective priority of each step, network stack included. | S | D |
| QNX-05 | The death of a process is known to its peers by the closing of its connection, without periodic polling. | C | T |
| QNX-06 | The processes are launched and restarted by the QNX supervision service. | C | D |
| QNX-07 | The security policy is applied, and a refusal is shown. | C | D |
| QNX-08 | The delay between the kernel start-up and the certified-second state is measured and published. | C | M |
| QNX-09 | The two computers are two guests of the QNX hypervisor on the same board, or two real boards. | X | D |

---

## 6. Protocol and formats

### 6.1 Frame

All frames are fixed-size UDP datagrams per type, 512 bytes at most.

| Field | Size | Role |
|---|---|---|
| Magic number | 4 | Recognise the protocol |
| Version | 1 | Refuse an unknown version |
| Type | 1 | See 6.2 |
| Sender | 1 | A, B, third machine, actuator |
| Flags | 1 | Ready, cession, test frame |
| Tick | 8 | Emitted tick (`STATUT`) or targeted tick (orders) |
| Mandate | 4 | Current mandate (`STATUT`) or mandate believed by the sender |
| Payload length | 2 | Size of the content |
| Reserved | 2 | Zero |
| Content | variable | According to the type |
| Check | 8 | CRC-32C at base level; keyed authenticator at full level |

### 6.2 Messages

| Type | Direction | When | Useful content |
|---|---|---|---|
| `STATUT` | Actuator → A and B | Every tick | Holder, source of the applied value, consecutive misses, second certified or not, lamp and cause, measured position and velocity, applied value, simulated wheel setpoint |
| `ORDRE` | Holder → actuator | Every tick | Value for the targeted tick, will of N values |
| `OMBRE` | Second → actuator | Every tick (full level) | Value for the targeted tick, replica age |
| `DEMANDE` | Non-holder → actuator | On suspicion (base level) | Last mandate seen, reason, optional first order |
| `CESSION` | Holder → actuator and second | Local fault, maintenance | Mandate ceded, reason |
| `ETAT` | Holder → second | Every tick | Complete state of the control law, last emitted value |
| `SUIVI` | Second → holder | Every tick | Replica age, ready flag |

### 6.3 Parameters

| Parameter | Default | Meaning |
|---|---|---|
| T | 5 ms | Tick period |
| p | 1 | Consecutive misses tolerated without deposition |
| N | 8 ticks | Horizon of the will |
| Certification | 3 ticks | Matching ticks to certify a second |
| Bail | 4 ticks | Silence of the actuator after which a machine deposes itself |
| ε | 0.5 % of full scale | Deviation tolerated between order and shadow |
| δ | 2 % of full scale | Deviation tolerated at handover |
| Degraded duration | 30 s | Time admitted without redundancy before "stop requested" |

Consistency rule, checked at start-up: N ≥ p + 2 + 3, so that the will covers
the handover even if three requests are lost.

### 6.4 Actuator journal

The journal of the actuator is authoritative: it is what the verifier reads.

- **One record per tick**: tick number, timestamp, mandate, holder, source of
  the value (holder, substitution, will, fallback), applied value, position,
  consecutive misses, lamp.
- **One record per event**: mandate change, certification granted or
  withdrawn, request granted or refused, order dropped with its reason
  (sender, mandate, tick, check, plausibility), disagreement alert, entry into
  and exit from the fallback law.
- **Format**: one JSON line per record at base level; fixed-size binary at
  full level, with conversion.

---

## 7. Validation plan

### 7.1 Five test levels

| Level | Where | What is tested |
|---|---|---|
| 0 — unit | PC | Frames, arbiter, state machine, control law |
| 1 — exhaustive exploration | PC | All bounded interleavings of the core, faults included |
| 2 — seeded simulation | PC | Long random sequences, with a timing model |
| 3 — real faults | QEMU | The full chain: machines killed, frozen, cables cut |
| 4 — hardware | Two boards | Real latencies (extreme level) |

### 7.2 Invariants

| ID | Invariant |
|---|---|
| I1 | **Uniqueness.** At every tick, the actuator applies a single value, and its source is the holder, the certified second by substitution, the will or the fallback law. Nothing else. |
| I2 | **Monotonicity.** The mandate number never decreases and changes at most once per tick. Two machines are never holders of the same mandate. |
| I3 | **No returning machine.** No order targeting another tick than the next one is applied, and no order of a deposed machine is applied after its deposition. |
| I4 | **Bounded gap.** Under the single-fault hypothesis, the number of consecutive ticks without a fresh order does not exceed p+1 at base level, and is zero with a certified second. |
| I5 | **Continuity.** The deviation between the applied value and that of the fault-free twin stays below δ. |
| I6 | **Will.** No will value is applied beyond its horizon. At its exhaustion, the fallback law applies at the next tick. |
| I7 | **No wrongful handover.** Without an injected fault, including under lower-priority overload and with the loss of p consecutive orders, the holder does not change. |
| I8 | **Double fault.** If no machine is able, the actuator is in the fallback law at most N+1 ticks after the last fresh order. |
| I9 | **Reintegration.** While a machine joins, every tick is served by the holder. |
| I10 | **Signalling.** Every loss of redundancy appears on the lamp at most 2 ticks after its detection, and stays there as long as it lasts. |
| I11 | **Progress.** Every state machine processes every event in bounded time, and has no state without an exit. |

### 7.3 Exhaustive exploration of the core

1. The three actors (A, B, actuator) are three pure state machines, compiled
   for the PC from the target's files.
2. The global state gathers their three states, the frames in flight on each
   link and the faults in progress.
3. In each state, the explorer enumerates all possible events: deliver an
   in-flight frame, lose it, duplicate it, beat the tick, kill a machine, freeze
   it, thaw it, restart it, cut or restore a half-link.
4. The traversal keeps the table of states already seen and stops at a depth of
   a few ticks. Fault budget: one at base level, two at full level.
5. The safety invariants (I1, I2, I3, I6) are checked in every state. The
   bounded gap (I4) is checked on every path where the adversary respects the
   single-fault hypothesis.
6. Any counter-example is written as a scenario that the simulator and the
   QEMU bench know how to replay.

Order of magnitude targeted, to be confirmed by the first version: 100 000
distinct states at base level, ten million at full level.

### 7.4 Seeded simulation

The same core runs in a discrete-event simulator. A seed fixes every draw:
frame delays (in fractions of a tick), losses, fault instants and durations.
The simulator runs several million ticks per second. A seed that violates an
invariant is reduced to the smallest faulty scenario, then kept as a
non-regression test.

The simulator also produces the **fault-free twin** of each scenario.

### 7.5 Campaign of real faults under QEMU

- **Sweep of the instant.** For each fault (death, freeze, cut), the instant is
  moved from 0 to T in steps of T/100 with respect to the tick.
- **Death points.** The machine powers off at a precise point of the code:

| Point | Position |
|---|---|
| P1 | `STATUT` received, before the computation |
| P2 | Computation done, before any send |
| P3 | `ORDRE` sent, `ETAT` not yet |
| P4 | `ETAT` sent on one link, not on the other |
| P5 | Holder: `CESSION` sent to the actuator only, or to the second only |
| P6 | Second: suspicion raised, `DEMANDE` not yet sent |
| P7 | Second: mandate received, first `ORDRE` not yet emitted |
| P8 | Deposed machine: while it switches to the role of second |
| P9 | Reintegration: before the certification |

- **Freezes.** Durations from one tick to ten minutes, in geometric
  progression, in both roles.
- **Cables.** Each of the six half-links alone, then by pairs at full level.
- **Nested faults.** A second fault while the first is being handled: the
  outcome must be a foreseen state, or the fallback law.
- **Overload.** Thirty minutes to twelve hours without fault, under load, for
  I7.

**Known limits.** The two machines share the PC: a host load spike touches
both. Killing QEMU is not cutting the power of an ECU. The bench produces no
fine-grained hardware fault (bit flipped in memory).

### 7.6 The mutants: proving that the bench sees faults

Each mutant is a variant of the software with a deliberate defect. It runs the
same campaign. If the bench does not flag it, it is the bench that is at
fault.

| Mutant | Defect introduced | What the bench must report |
|---|---|---|
| M1 | The actuator accepts the order of any machine | I1 |
| M2 | The actuator ignores the tick tag | I3 |
| M3 | The actuator grants the mandate without noticing the silence | I7 |
| M4 | The will is played beyond its horizon | I6 |
| M5 | The second does not replace its state with the received one | I5 |
| M6 | The mandate number restarts from zero on restart of the actuator | I2 |
| M7 | The second emits `ORDRE`s on mere suspicion, without waiting for the grant | Orders of a non-holder found in the journal (MAN-08); the actuator, on the other hand, must hold |
| M8 | The lamp goes off when a link returns although the second is not certified | I10 |

Mutants M1 to M4 only touch base-level requirements. M7 checks the defence in
depth: the defect is in a machine, and the arbiter must absorb it without any
actuator invariant falling.

### 7.7 Measurements

- **Interval between fresh orders**: read from the actuator's journal, in ticks
  and in time.
- **Deviation from the fault-free twin**: applied value and position, tick by
  tick.
- **Processing time inside a machine**: from the kernel trace events (OBS-03).
- **Published results**: histogram, median, 99th and 99.9th percentiles,
  maximum, and the three worst cases commented.

### 7.8 Success thresholds per level

| Measure | Base | Full | Extreme |
|---|---|---|---|
| Core states explored | 100 000, one fault | 10 000 000, two faults | Same, plus an independent concordant specification |
| Ticks simulated with a seed | 1 million | 1 billion | — |
| Real deaths | 200 | 2 000 swept, plus all death points | 10 000 |
| Freezes and thaws | 2 000 | 10 000 | 100 000 |
| Link cuts | 500 | 5 000, including one-way cuts | — |
| Mutants flagged | 4 out of 4 (M1 to M4) | 8 out of 8 | — |
| Duration without wrongful handover | 30 min | 12 h under load | 72 h |
| Invariant violations tolerated | 0 | 0 | 0 |

Every violation met during development is kept as a non-regression test, with
the scenario that reproduces it.

---

## 8. Demonstration (video of less than three minutes)

| Time | On screen | Message |
|---|---|---|
| 0:00 – 0:20 | One sentence and a diagram | Without a steering column, a computer that falls silent is no longer a safe state. Two are needed, and only one may command. |
| 0:20 – 0:45 | Architecture diagram | Two QNX, one actuator arbiter that gives the tempo, one will inside every order. |
| 0:45 – 1:20 | Live curve and terminal | Slalom running. Machine A is killed. The curve does not move. Banner: "mandate 1 → 2, zero tick without a fresh order". Yellow lamp. |
| 1:20 – 1:45 | Same | A restarts and becomes a certified second again: lamp off. Machine B is frozen for 200 ms then thawed: "order dropped: stale mandate, stale tick". B ranges itself as second. |
| 1:45 – 2:05 | Same | The cable between the computers is cut: no handover, yellow lamp "link lost". |
| 2:05 – 2:30 | Campaign report | Number of faults swept over all the instants of the cycle, zero violation; states explored; mutants: 8 out of 8 flagged. |
| 2:30 – 2:45 | Kernel trace and terminal | The path from the `STATUT` to the order, with the priorities. An intrusive process is refused access to `/dev/relais`. |
| 2:45 – 3:00 | Repository | Licence, single command to replay the demo, tutorial. |

**Scenarios.** Three sources, in fallback order:

1. A fixed-seed generator: slalom, lane change, step, with road disturbance.
   Always available.
2. A braking profile (CMD-08).
3. Real steering-angle recordings taken from a public driving dataset (licence
   to be confirmed).

---

*Continued in [SPEC-ANNEX.md](SPEC-ANNEX.md): section 9 (deliverables and
repository), section 10 (schedule), section 11 (risks), section 12 (pushing the
idea further), section 13 (acceptance criteria), section 14 (what is verified
and what remains to be validated), section 15 (glossary) and section 16
(sources).*
