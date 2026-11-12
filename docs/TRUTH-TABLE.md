# Relais — fault truth table

Expected behaviour of the system, fault by fault. Translation and completion of
[SPEC.md §4.7](SPEC.md), cross-referenced with the fault model
([SPEC.md §3.3](SPEC.md), codes F1–F11), the invariants and requirements
([SPEC-2.md §5 and §7.2](SPEC-2.md)).

**How to read this table.** Default parameters: T = 5 ms (200 Hz), p = 1,
N = 8 ticks. *Fresh order*: an order computed from the `STATUT` of the previous
tick. The **single-fault hypothesis** applies: one fault at a time until
redundancy is restored; double faults are not tolerated but must reach the
safe state. "Covered by" names the verification means of spec §7: unit tests
(`make test`), exhaustive explorer (level 1, `make explore`), seeded simulator
(level 2, `make simulate`), QEMU bench (level 3: death/freeze/cut injection
INJ-02…INJ-05, instant sweep INJ-08, death points P1–P9 INJ-07), mutants M1–M8
(§7.6).

## Detailed cases (spec §4.7)

| # | Fault (model code) | What the actuator sees | What the other machine sees | Expected outcome | Invariants | Requirements | Covered by |
|---|---|---|---|---|---|---|---|
| 1 | A (holder) dies (F1) | No more order from A | No more `ETAT` | Handover to B within p+2 ticks; will bridges the gap; yellow lamp "second/holder state" per DEG-02 | I1, I2, I3, I4, I9, I10 | REL-01, REL-02, MAN-04, MAN-10, DEG-01 | Explorer; simulator; QEMU bench (INJ-02, sweep INJ-08); mutant M3 must be flagged |
| 2 | A freezes, then comes back — the returning machine (F2) | No order, then stale orders (old mandate, old tick) | No more `ETAT` | Handover to B; A's orders dropped twice over (not holder, past tick); A reads the `STATUT`, stops emitting, rejoins as second | I3, I2, I4, I9 | MAN-02, MAN-08, REL-07, DEG-07, ACT-09 | Explorer; QEMU bench (`stop`/`cont`, INJ-03); mutants M2, M7 |
| 3 | A freezes for less than p+1 ticks (F2) | An isolated miss | A missing `ETAT` | No handover; will or shadow bridges; A stays holder; nothing to report | I4, I7 | DET-03, MAN-04 | Explorer; simulator; QEMU bench (short freezes, ROB-05) |
| 4 | B (second) dies (F1 on the second) | No more `OMBRE` | No more replica acknowledgement (holder's view) | No handover; yellow lamp "second absent"; holder keeps commanding | I1, I7, I10 | DEG-01, DEG-02, REP-05 (decertification) | Explorer; simulator; QEMU bench |
| 5 | Link between computers cut, one or both ways (F4) | Nothing abnormal | Each stops hearing the other | No handover; B follows the `STATUT`s and rebuilds a bump-free state from what the actuator publishes; yellow lamp "link lost" within 2 ticks | I1, I5, I7, I10 | DET-06, REP-04, REP-07, DEG-01 | Explorer (half-link cuts); QEMU bench (`set_link`, INJ-04/05) |
| 6 | A–actuator link cut, both directions (F5) | No more order from A | `STATUT`: holder's misses | Handover to B; A declares itself "without actuator" and deposes itself after `bail_ticks` of silence | I2, I3, I4, I10 | DET-07, MAN-09, REL-01 | Explorer; QEMU bench (INJ-04) |
| 7 | A no longer heard by the actuator (F5, one way A→ACT) | No more order from A | Same as case 6 | Handover to B; A learns its deposition from the `STATUT` it still receives | I3, I4 | DET-08, MAN-08 | Explorer (six half-links, DET-08); QEMU bench |
| 8 | A no longer hears the actuator (F5, one way ACT→A) | No more order from A (A has no tempo) | Frozen `ETAT` | Handover to B; A deposes itself after 4 ticks without `STATUT` (bail) | I3, I4 | MAN-09, DET-07, DET-08 | Explorer; QEMU bench |
| 9 | B–actuator link cut (F5) | No more `OMBRE` | `STATUT`: second not certified | No handover; yellow lamp; B loses certification after more than p missing shadows | I7, I10 | REP-05, DEG-01, DET-08 | Explorer; QEMU bench |
| 10 | A slow, isolated misses (F3; also F6 losses) | Non-consecutive misses | Intermittent `ETAT` | No handover (loss of p consecutive orders changes nothing); miss counter published in every `STATUT` | I4, I7 | DET-03, ROB-03, ROB-08, MAN-07 | Explorer; simulator (loss/delay model); QEMU bench (INJ-09 slow machine) |
| 11 | A emits wrong values (F8) | `ORDRE` different from the `OMBRE` beyond ε for 3 ticks | — (actuator-side detection) | "Disagreement" alert, red lamp; full level: plausibility check designates the faulty machine when possible, otherwise the holder is kept | I1, I10 | DET-09, DET-10, DEG-01 | Simulator (bias fault); QEMU bench (INJ-10); mutant M5 |
| 12 | A and B die (F10; F9 if sequential) | Nothing | — | Will plays out to its horizon, then fallback law at most N+1 ticks after the last fresh order; red lamp; "stop requested" after the bounded degraded duration | I6, I8, I10 | ACT-02, ACT-03, DEG-05, DEG-06 | Explorer (two faults, level C); simulator; QEMU bench |
| 13 | Corrupted or forged frame (F7) | Invalid check (CRC-32C or MAC) | — | Frame dropped and counted, treated as a loss; no effect on the service | I1, I3 | DET-04, SEC-01, SEC-02, SEC-03 | Unit tests (fuzzing ROB-09 at level X); explorer |

## Fault-model summary (F1–F11, spec §3.3)

| Code | Fault | Handling | Detailed case(s) above |
|---|---|---|---|
| F1 | Death of one machine | Tolerated (handover if holder) | 1, 4 |
| F2 | Freeze then resumption, any duration | Tolerated | 2, 3 |
| F3 | Slow machine, missing ticks | Tolerated; deposed if misses are consecutive (p+1) | 10 |
| F4 | Inter-computer link cut, one or both ways | Tolerated without handover; redundancy reported degraded | 5 |
| F5 | Machine–actuator link cut, one or both ways | Tolerated; handover if it was the holder | 6, 7, 8, 9 |
| F6 | Loss, duplication, reordering, delay of frames | Tolerated within the parameter bounds | 10 (and ROB-02 unit coverage) |
| F7 | Corrupted or forged frame | Detected, treated as a loss | 13 |
| F8 | Wrong value emitted by a machine | Detected at full level, not tolerated | 11 |
| F9 | Second fault before redundancy is restored | Safe state of the actuator | 12 (and ROB-04: nested faults respect the invariants) |
| F10 | Death of both machines | Safe state of the actuator | 12 |
| F11 | Failure of the actuator itself | Out of scope (extension: two channels, ACT-10 / MAN-13) | — |

## Lifecycle and time faults (mission M19, `host/faultlife/`)

Faults of the machine's life cycle and of time, modelled in `host/faultlife/`
(a world that drives the same pure core with a per-machine clock, an in-flight
byte stream, a boot cadence, a demand storm and an OTA life cycle). Each is
exercised by `host/faultlife/test_faultlife.c` (`make test`).

| # | Fault (idea) | What the actuator sees | What the other machine sees | Expected outcome | Invariants | Requirements | Covered by |
|---|---|---|---|---|---|---|---|
| 14 | Machine clock skewed: 2x fast, 2x slow, or jumps after a gel (13) | Valid orders still stamped with the actuator's tick; nothing abnormal | Nothing | The actuator's decision (mandate, holder, source) is **identical** in all four clock scenarios — 0 diverging ticks over 90; only the node's bail diagnostic (MAN-09) fires earlier/later. The protocol reads **no machine clock**: `relais_arb_step` takes only `(tick, frames, meas)`. | I1, I2, I3, I7 | MAN-02, MAN-04, MAN-09, DET-07, ACT-09 | `test_faultlife` (`test_clock_skew`) |
| 15 | Brutal power loss, possibly mid-send: the frame in flight is truncated (14) | A partial frame (bad length / CRC) or nothing | The holder vanishes without cession | Every truncated prefix is rejected (69/69 over a 69-byte order, DET-04) and never applied; the loss mid-send still hands over to the certified second within p+2 (measured: 1 tick), no double command, 0 invariant violations against the twin. | I1, I2, I3, I4, I5 | DET-04, SEC-01, REL-01, MAN-04, ROB-09 | `test_faultlife` (`test_frame_truncation`, `test_power_loss_mid_send`) |
| 16 | Boot loop: a machine restarts every k periods, k = 2..50 (15) | The looping machine's orders/shadows stop during each reboot | The peer keeps commanding | The mandate does **not** oscillate with the reboots: second loops -> 0 changes (holder A stays); holder loops -> exactly 1 change (A->B), then stable on the peer for 1000 ticks. The looping machine is re-eligible only after re-certification (DEG-07). | I1, I2, I3 | DEG-07, MAN-10, REP-05, ROB-06 | `test_faultlife` (`test_boot_loop`) |
| 17 | Demand storm: a machine demands every tick, both machines demand (19) | A flood of DEMANDE frames | The holder is unaffected | The holder's orders keep being served every tick (500/500, source=holder, no induced delay); the arbiter scans one demande per tick so the queue never overflows (0 drops); refused vs granted measured (499 refused / 1 granted). The SEC-06 burst cap is defence-in-depth (exercised by mutant M3 and the 3-node case). | I1, I7 | SEC-06, MAN-05, REL-04 | `test_faultlife` (`test_demand_storm`) |
| 18 | OTA update (cession, shutdown, restart) interrupted at each step (20) | A cession, then the machine dies mid-send | The peer takes over | At every one of the five interruption points the outcome is a foreseen state: one machine commands (B), never a double command, never a lost mandate while a machine is alive and certified; the mandate number never goes backwards. | I1, I2, I3 | REL-06, REL-09, MAN-11, DET-05, DEG-07 | `test_faultlife` (`test_ota_interrupted`) |

## Notes

- **The outcome never depends on the machines' agreement.** In every case
  above, the mandate changes only on the actuator's own evidence (MAN-04,
  REL-04): double command is impossible by construction, whatever the fault
  combination seen by A and B (I2, I3).
- **Timing** only affects how fast the handover happens, never its safety
  (spec §4.3). Bounds (3 periods at base level, zero tick without a fresh
  order at full level) are targets measured under emulation — see the
  prudence rule in [SPEC.md §2](SPEC.md) and [SAFETY.md](SAFETY.md).
- **Nested faults** (a second fault during a handover, reintegration or
  cession) are exercised at full level (ROB-04) and must land in a foreseen
  state or in the fallback law.
