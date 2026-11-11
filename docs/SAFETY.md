# Relais — safety concept

Short safety document, laid out like the annex 6 §3.4.3.2 of UN Regulation No. 79
asks for: the principle of the switch-over mechanism, its logic, its level of
redundancy, its built-in checks, and the limits of the fallback's effectiveness.

> **Standing of this document.** Relais is a demonstrator *aligned with* annex 6
> of UN Regulation No. 79. It is **not** compliant, not approved and not
> certifiable: it is a small-scale, emulated answer to the question the annex
> asks, "explain your switch-over mechanism". Every latency quoted here is a
> **measured worst case under emulation**, never a proven worst case.

---

## 1. Hazard and safety goal

Without a steering column, an assistance computer that goes silent leaves the
wheels without a command. With no driver holding the wheel, nobody can take
over within the second. Silence is therefore not a safe state: the function has
to keep working, at least long enough to bring the vehicle to a stop.

**Safety goal.** The actuator must never stay without a fresh command for longer
than a bounded, measured interval, and it must never apply the command of a
machine that has been deposed.

**Safe state.** When no machine can command any more, the actuator runs its own
local damped loop, holds the rack where it is, lights the red lamp and asks for
a stop (DEG-05).

## 2. Principle of the switch-over

The actuator — not the computers — is the authority. Three consequences carry
the whole design:

1. **It hands out the mandates.** A machine never appoints itself. The mandate
   number is written by the actuator alone, increases by exactly one at every
   change of holder, and never decreases (MAN-01, MAN-03).
2. **It withdraws a mandate only on its own evidence**: p+1 consecutive ticks
   without a valid order from the holder, or a cession from it (MAN-04). A
   machine that cannot hear its neighbour cannot trigger a handover on its own,
   so the "is it dead or is the cable cut?" dilemma never leads to two masters
   (REL-04).
3. **It gives the tempo.** Every order names the tick it targets, and an order
   for any other tick is dropped (MAN-02, ACT-09). No clock is synchronised
   between the machines, and an order computed before a freeze is stale by
   construction — the returning machine is disarmed twice over, by its mandate
   and by its tick label (spec 4.6).

Because the actuator applies exactly one value per tick, chosen in a fixed
order, two machines that disagree cannot both command: whatever they believe,
only the appointed one is obeyed. Safety does not depend on the machines'
clocks, nor on frame delays. Time only affects how *fast* the handover is.

## 3. Logic

**One value per tick, in this order** (spec 4.4, ACT-01):

| Rank | Source | When |
|---|---|---|
| 1 | the holder's `ORDRE` for this tick | nominal service |
| 2 | the certified second's `OMBRE` for this tick | substitution, level C |
| 3 | the will (`testament`) of the last order received | handover at level S, within its horizon |
| 4 | the actuator's local fallback loop | nothing else left: safe state, red lamp |

**One tick of the arbiter**, in seven steps (CONTRACT.md): find the holder's
order → count the miss → revoke if the misses pass p → process cessions then
demandes → pick the value → update certification and signalling → publish
`STATUT` and the journal.

**Bounds that follow** (p = 1, T = 5 ms):

| Level | Worst interval between two fresh orders | Where it comes from |
|---|---|---|
| S (socle) | **3 periods = 15 ms** | the second asks on the first miss, the will covers the gap (REL-01) |
| C (complet) | **0 period** | the certified second's shadow is applied from the very first miss (REL-05) |
| both machines gone | fallback reached within **N+1 = 9 ticks = 45 ms** | I8 |

## 4. Level of redundancy

- **Two computers**, identical binaries, isolated processes per role
  (`relais-net`, `relais-core`, `relais-ctl`) so that the crash of one is seen
  by the others through the closing of its connection, and the machine cedes the
  mandate within the tick instead of waiting for a timeout (DET-05, QNX-05).
- **State replicated whole every tick** (256 bytes at most, REP-01): no session,
  no differential, no catch-up protocol. The replica is the state the holder had
  *before* computing, so the second that imports it and computes on the same
  `STATUT` produces the same bits (CMD-03).
- **One actuator, declared single point of failure.** Its arbitration logic is
  deliberately tiny (one file, under 500 lines, no allocation, no clock, no I/O)
  so that it can be explored exhaustively. Two actuator channels are the level-X
  extension (ACT-10).
- **Single-fault hypothesis**: one fault at a time, until redundancy is back.
  Double faults are not tolerated, but they must reach the safe state, and that
  is tested (F9, F10).

## 5. Built-in checks

| What is checked | By whom | Requirement |
|---|---|---|
| Order comes from the appointed holder, carries the current mandate, targets the next tick | the actuator, every frame | MAN-02 |
| Mandate never decreases, changes at most once per tick | the actuator and the verifier | MAN-03, I2 |
| Frame integrity: CRC-32C at level S, keyed SipHash-2-4 MAC at level C | the receiver | SEC-01, SEC-02, DET-04 |
| Replay: the tick label makes a captured frame stale | the actuator | SEC-03 |
| Self-appointment: an order carrying a future mandate is refused and counted | the actuator | SEC-04 |
| Order/shadow agreement within ε over 3 ticks, and the disagreement alert | the actuator | REP-05, DET-09 |
| Request flooding: at most `request_burst` demandes per window | the actuator | SEC-06 |
| Command bounded in amplitude and in rate of change, whatever its source | the emitting machine and the actuator | CMD-06, ACT-05 |
| Will never played past its horizon | the actuator and the verifier | ACT-02, I6 |
| Lamp follows the cause within two ticks and stays lit while it lasts | the actuator and the verifier | DEG-01, DEG-02, I10 |
| Degraded mode is limited in time, then a stop is requested | the actuator | DEG-05 |
| The eleven invariants, on every tick of every run | `relais-verify`, the simulator, the explorer | spec 7.2 |
| The bench itself: eight mutants must be flagged | `host/mutants/` | spec 7.6, INJ-11 |

## 6. Failure modes (FMEA, from the fault model of spec 3.3)

| ID | Failure | Detected by | Reaction | Result |
|---|---|---|---|---|
| F1 | a machine dies | p+1 ticks without a valid order | revoke, auto-switch to the certified second or grant on demande | handover, yellow lamp |
| F2 | a machine freezes, any duration | same | same; on thaw the stale orders are dropped and it stands down as second | handover, the revenant is disarmed |
| F3 | a machine runs slow, misses ticks | the miss counter, published | revoked only if the misses are consecutive | degraded, then handover |
| F4 | link between the computers cut | no `ETAT` any more | no handover; the second follows through the `STATUT` and rebuilds a state | yellow "link lost" |
| F5 | link machine–actuator cut | the actuator sees no order; the machine sees no `STATUT` | handover if it was the holder; the machine declares itself "without actuator" (MAN-09) | handover |
| F6 | lost, duplicated, reordered, late frames | tick label, mandate, CRC | dropped and counted | absorbed within the parameter bounds |
| F7 | corrupted or forged frame | CRC / MAC | dropped, counted, journaled | no effect on the service |
| F8 | a machine emits a wrong value | order/shadow comparison over 3 ticks | disagreement alert, decertification, red lamp | **detected, not arbitrated** (see §7) |
| F9 | second fault before redundancy is back | — | will, then fallback loop | safe state |
| F10 | both machines dead | — | will for N ticks, then fallback | safe state, red lamp, stop requested |
| F11 | the actuator itself fails | out of scope | — | level-X extension: two channels |

## 7. Limits of the fallback's effectiveness

Stated plainly, because annex 6 §3.4.3.2 asks for them:

1. **A machine that computes wrong is detected, not outvoted.** With two
   computers there is no majority: the actuator keeps the holder, decertifies the
   second, lights the red lamp and reports the disagreement (DET-09). Masking the
   fault needs a third machine and a vote (level X, REP-08).
2. **A defect common to both copies of the same software is out of scope.** Both
   machines run the same binary; a dissimilar backup (DEG-08) is an extension.
3. **The actuator is a single point of failure.** Mitigation: a tiny, pure,
   exhaustively explored arbitration core, and two channels at level X.
4. **The degraded mode is time-limited** (30 s of demonstration value, DEG-05),
   mirroring the rule that a vehicle must not drive indefinitely with a lit
   warning. After that, a stop is requested: the fallback holds the rack, it does
   not keep steering.
5. **Timings are emulated.** Both guests share the PC's processor; killing QEMU
   is not cutting a computer's power; the bench produces no fine hardware fault
   (a flipped bit in memory). The 5 ms / 200 Hz figures are targets to be
   measured on the QNX target under KVM, published with their full distribution,
   and marked "emulated".
6. **The threat model stops at frame authentication.** An attacker able to
   observe and inject on the actuator link in real time, with the key, is out of
   scope.

## 8. ISO 26262 vocabulary mapped on Relais

| ISO term | Meaning | In Relais |
|---|---|---|
| FTTI | fault tolerance time interval | the horizon of the will: N ticks, after which the actuator has no plan |
| FDTI | fault detection time interval | p+1 ticks, counted by the actuator itself |
| FRTI | fault reaction time interval | from the finding to the first order of the new holder: p+2 ticks at level S, 0 at level C |
| EOTTI | emergency operation time tolerance | the limited duration of the degraded mode (DEG-05) |

## 9. Traceability

Every requirement of `docs/SPEC.md` section 5 carries a level (S/C/X) and a
verification means (T test, M measurement, I inspection, D demonstration). The
mapping is kept next to the code:

| Family | Where it is proved |
|---|---|
| CMD, MAN, DET, REL, REP, ACT, DEG, SEC, ROB | `core/tests/test_*.c` (unit) and `core/tests/test_protocol.c` (protocol) |
| PERF, ROB (campaign scale) | `host/simulate/` (level 2) and `host/explore/` (level 1) |
| INJ | `host/bench/` (PC and QEMU), `scenarios/` |
| OBS | the journal (`core/src/journal.c`), `host/verify/`, `host/report/` |
| QNX | `target/` and `image/`, plus the kernel trace of QNX-04 on the target |

## 10. Validation evidence

See `docs/RESULTS.md` for the measured campaign: simulated fault runs, invariant
violations, handover bounds, deviation from the twin without fault, and the
mutant bench. Nothing in this document is claimed that the campaign has not
produced.
