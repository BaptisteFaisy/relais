# Relais — measured campaign

Everything below was produced on this repository, on a 12-core x86-64 PC running
Linux, by `build/relais-simulate`, `build/relais-verify`, `make test`,
`make campaign` and `python3 host/mutants/run_mutants.py`. Nothing here is a
target or an estimate: these are the outputs of the commands you can re-run.

Timings are **emulated**. Both computers are two state machines of the same
core, driven by the same discrete-event loop; the numbers say nothing about a
real ECU. They are published with their bounds, not as proven worst cases.

---

## 1. Headline results

| Promise | Measured | Where |
|---|---|---|
| Level C: no tick without a fresh order after the holder dies | **max gap 0 ticks** (bound 0) | `--level complet --fault 500:death:0` |
| Level C: continuity against the twin without fault | **max deviation 0.000000** (bound δ = 0.02) | same run |
| Level S: bounded hole during the handover | **max gap 1 tick** (bound p+1 = 2) | `--fault 500:death:0` |
| Level S: deviation during the handover, served by the will | **0.0253** of full scale (measured; δ is a level-C requirement, REP-06) | same run |
| Mandate change exactly p+1 ticks after the holder's last order (PERF-05) | **2 ticks = p+1** | `core/tests/test_protocol.c` |
| Both machines gone: safe state reached | fallback loop entered within **N+1 = 9 ticks = 45 ms** | `test_protocol.c`, campaign §5 |
| No wrongful handover, 5000 ticks, no fault (I7) | **0 handovers, mandate stays 1** | `test_protocol.c` |
| Level 1: exhaustive exploration, one fault | **100 198 states, depth 12, 0 violation, 0.6 s** | `make explore` |
| Level 1, **two simultaneous faults** | **10 000 330 states, depth 16, 0 violation, 51 s** | `relais-explore --budget 10000000 --depth 16 --faults 2` |
| **12 hours** without a wrongful handover (8 640 000 ticks) under 1 % frame loss | **0 violation**, worst gap 1 tick (bound p+1) | `relais-simulate --ticks 8640000 --loss 0.01` |
| The bench sees its own defects | **8 mutants out of 8 flagged** | `host/mutants/run_mutants.py` |

## 2. Scale of the campaign

| Campaign | Runs | Failures |
|---|---|---|
| One million ticks, no fault, both levels (I7) | 6 | 0 |
| **Deaths swept** over 1 986 instants of the period, both roles, level C | 3 972 | 0 |
| **Freezes**, 1 tick to 1 597 ticks (8 s), both roles, both levels | 10 000 | 0 |
| **Cuts** of the six half-links, one way and both ways, swept instants, both levels | 5 040 | 0 |
| Cuts and freezes at Fibonacci-scaled durations, both roles, both levels | 64 | 0 |
| Nested faults (two faults, one before the other is over) | 50 | 0 |
| Actuator restarts, mandate must survive (MAN-11) | 15 | 0 |
| Forged orders, replayed stale orders, slow machines, process crashes, restarts, value faults | 510 | 0 |
| **12-hour runs** (8 640 000 ticks) under 1 % frame loss, both levels | 2 | 0 |
| **Total** | **19 659** | **0** |

Throughput: one million ticks in 2.0 to 4.0 s, i.e. 250 000 to 500 000 ticks per
second per core; 8 640 000 ticks — twelve hours of emulated service — in 10 to
15 s. Ten million explored states take 51 s. The pull-request CI keeps a 40-run
sample of this campaign inside its five-minute budget; the full scale is run on
demand.

## 3. Channel model: what is tolerated, and the breaking point

| Channel | Result (2000 ticks, level C) |
|---|---|
| No loss, no delay | 0 violations, max gap 0 |
| 1 % loss, no burst longer than p | 0 violations, max gap 0, deviation 0 |
| 5 % loss | 0 violations, max gap 0, deviation 0.0034 |
| 10 % loss | 0 violations, max gap 1 (bound p+1 = 2), deviation 0.0057 |
| **1 tick of delay** | no invariant violated, but **max gap 7** and **deviation 0.365** — measured and published |
| 2 ticks of delay | max gap 16, deviation 0.86 |

The last two rows are the honest breaking point. The protocol assumes
`2Δ + C + J ≤ T` (spec 4.2): a frame delayed by a whole period misses the tick it
targets, and the actuator drops it — which is exactly what it must do (ACT-09).
The miss counter then does its job and the mandate churns. Beyond its timing
hypothesis the system degrades; it does not silently pretend. Losses inside the
hypothesis, with no burst longer than p, are absorbed with no effect at all
(ROB-02, ROB-03).

## 4. Hostile frames

| Attack | What happens |
|---|---|
| Order forged by a machine that is not the holder, carrying the current mandate and the current tick | rejected, counted `not_holder`, journaled; the applied value is unchanged (MAN-02, I3) |
| Order replayed by the holder itself for a tick three periods old | rejected, counted `tick_missed` (ACT-09, SEC-03) |
| Order carrying a future mandate | rejected, counted `mandate_future` (SEC-04) |
| Frame with one flipped byte, or without the link key | rejected, counted `check` (SEC-01, SEC-02, DET-04) |
| 200 000 random buffers thrown at the parser | no crash, no overrun (sentinel-guarded), never accepted with a bad control (ROB-09) |
| Value fault: a machine adds a 0.3 bias to its orders | the actuator reports `disagreement` after 3 ticks, decertifies the second, lights the red lamp (DET-09) |

## 4b. What is claimed where

The bounds (I4 gap, I5 continuity, I9 reintegration) are claimed **inside the
timing hypothesis** `2Δ + C + J ≤ T`, with no burst of losses longer than p —
which is what the specification itself states (ROB-03, spec 7.3 item 5). Outside
it, the gap and the deviation are still measured and published, and the **safety**
invariants keep being enforced everywhere, at every level, on every path:

| Invariant | Claimed | Where |
|---|---|---|
| I1 one value per tick, from a known source | always | explorer, simulator, verifier |
| I2 mandate monotonic, one change per tick | always | idem |
| I3 no order of a deposed machine | always | idem |
| I6 the will never plays past its horizon | always | idem |
| I8 the safe state is reached within N+1 ticks | always | idem |
| I10 the lamp follows the cause within two ticks | always | idem |
| I11 ticks are contiguous, every event is processed | always | idem |
| DET-03 no handover without the actuator's own evidence | always | idem |
| I4 bounded gap, I5 continuity, I9 reintegration | inside the timing hypothesis, single fault | idem |

## 5. The mutant bench (spec 7.6)

Each mutant is the same core compiled with `-DRELAIS_MUTANT=n`. Each one faces a
scenario that the **healthy core passes first** — a probe the nominal software
fails would prove nothing, and the bench rejects it as `BAD-PROBE`.

| Mutant | Defect | Must report | Verdict | Evidence |
|---|---|---|---|---|
| M1 | the actuator applies an order from anybody | I1 / I3 | FLAGGED | I5 at tick 600: the forged value was applied |
| M2 | the actuator ignores the tick label | I3 / I5 | FLAGGED | I5 at tick 600: a stale order was applied |
| M3 | the mandate is granted with no evidence of silence | DET-03 | FLAGGED | DET-03 at tick 602: granted while a holder was in place |
| M4 | the will is played past its horizon | I6 / I8 | FLAGGED | I6 at tick 439: will played past its horizon |
| M5 | the second keeps its own state instead of the replica | I4 / DET-09 | FLAGGED | I4 at tick 651: a tick without a fresh order, second certified |
| M6 | the mandate restarts at zero when the actuator restarts | I2 | FLAGGED | I2 at tick 800: mandate went backwards |
| M7 | the second orders on a mere suspicion, without the grant | MAN-08 (journal) | FLAGGED | 3 orders from a non-holder in the journal, **and the arbiter held**: 0 violations |
| M8 | the lamp goes out while redundancy is still lost | I10 | FLAGGED | I10 at tick 4: redundancy lost and the lamp stayed off |

M7 is the defence-in-depth case: the defect is in a machine, the arbiter absorbs
it, and the journal is the evidence.

**Which proof catches which mutant.** The two levels are complementary, and the
table says so rather than pretending either one is enough:

| Mutant | Level 1 (explorer, 400 000 states, 2 faults, level C) | Level 2 (campaign) |
|---|---|---|
| M1 accepts anybody's order | not caught — the explorer's machines only ever send legitimate frames | **caught** (a forged order is injected) |
| M2 ignores the tick label | not caught — no frame is stale in the explored world | **caught** (a stale order is replayed) |
| M3 grants without evidence | **caught**, DET-03 after 4 630 states | **caught** |
| M4 will past its horizon | not caught at this budget — needs both machines dead for N+1 ticks | **caught**, I6 |
| M5 second keeps its own state | — | **caught**, I4 |
| M6 mandate resets | — | **caught**, I2 |
| M7 orders on suspicion | — | **caught** through the journal |
| M8 lamp off too early | **caught**, I10 after 17 states | **caught** |

Read honestly: exhaustive exploration is unbeatable for the *arbiter's own*
decisions (M3, M8 fall almost immediately), and the seeded campaign is
indispensable for everything that needs an adversarial frame or a long double
fault (M1, M2, M4). Neither replaces the other.

## 6. Unit and protocol tests

`make test` — seven binaries, **5630 checks**, all green:

| Binary | Checks | Covers |
|---|---|---|
| `test_protocol` | 5026 | the handover tables of spec 4.5 tick by tick, the returning machine, the safe state, 5000 ticks without a wrongful handover, bit-identical replication, the mandate across an actuator restart, the degraded-mode time limit |
| `test_frame` | 226 | the seven message types round-tripped field by field, the eight reject codes, the 512-byte bound |
| `test_check` | 133 | CRC-32C and SipHash-2-4 against their published vectors, one flipped byte over 1000 positions |
| `test_plant` | 93 | trajectories bounded, continuous, deterministic; rack stable over 100 000 steps; tracking error under 10 % |
| `test_params` | 89 | defaults, `N ≥ p + 2 + 3`, every out-of-range bound, the parser, CMD-07 read in ticks |
| `test_control` | 56 | bit-exact determinism, command bounds, exact replication, no bump on rebuild, will window, successor within δ |
| `test_frame_fuzz` | 7 | 200 000 random buffers |

Three defects were found by these checks while they were being written, and are
fixed in the code rather than worked around in the tests:

1. the will was based one tick off, so the fallback loop was reached one tick
   late (I8);
2. the replicated state was exported *after* the computation, which put the
   second one tick ahead of its holder and broke the shadow comparison;
3. the lamp stayed off during a substitution, hiding a loss of redundancy (I10).

## 7. Continuous integration

Every pull request runs, on self-hosted runners, in **2 min 32 s** (budget: 5 min
per job, user rule): the docs sanity check, the CI guards' own tests (couche0,
InferHub), the C build with `-Werror`, the file-size guard (no file over 500
lines), the unit tests, the seeded simulation of one million ticks, a 40-run
fault campaign, the M1/M4 mutant probes, the deterministic one-command demo, and
the syntax check of the QNX layer.

## 7b. Reproducing the scale

Every number above comes from a command you can run. The pull-request CI keeps a
40-run sample inside its five-minute budget; this is the full scale.

```bash
make -j$(nproc)                                   # build everything

# level 1 — exhaustive exploration
./build/relais-explore --budget 100000   --depth 12 --faults 1     #    0.6 s
./build/relais-explore --budget 10000000 --depth 16 --faults 2     #     51 s

# level 2 — long runs and sweeps
./build/relais-simulate --ticks 1000000 --seed 1 --level complet   #   ~3 s
./build/relais-simulate --ticks 8640000 --seed 99 --loss 0.01      # 12 h, ~15 s
for at in $(seq 100 7 14000); do                                   # swept deaths
  ./build/relais-simulate --ticks 2500 --seed $at --level complet \
      --fault $at:death:0 --quiet || echo "FAIL at $at"
done

# the bench that proves the bench
python3 host/mutants/run_mutants.py                                # 8/8
```

A run exits non-zero when an invariant is violated, which is what makes the
loops above a verdict rather than a log.

## 8. Acceptance criteria (spec section 13)

**Socle — submittable**

- [x] the level-S requirements are covered by tests (`core/tests/`)
- [x] one command replays the demonstration, without root: `make demo`
- [x] **100 000 core states explored, zero violation** — `make explore` (100 198 states, depth 12, 0.6 s)
- [x] 3 972 swept deaths, 10 000 freezes up to 8 s, 5 040 link cuts, zero violation (simulated; the QEMU campaign is `host/bench/`)
- [x] after a death, never more than p+2 ticks between two fresh orders (measured: 1)
- [x] mutants M1 to M4 flagged
- [x] thirty minutes under load without a wrongful handover — 360 000 ticks (30 min emulated) x 6 runs, zero violation, zero handover
- [x] public repository, Apache-2.0, English README
- [ ] video under three minutes (to be recorded)

**Complet — the target version**

- [x] the level-C requirements are covered by tests
- [x] **ten million states explored with two faults** — 10 000 330 states, depth 16, 51 s, zero violation
- [x] 2 000 swept deaths, 10 000 freezes, 5 000 cuts, zero tick without a fresh order with a certified second — 3 972 / 10 000 / 5 040 runs, zero violation
- [x] the eight mutants are flagged
- [x] twelve hours under load without a wrongful handover — 8 640 000 ticks, 1 % frame loss, both levels, zero violation
- [ ] three worst cases explained by a kernel trace — needs the QNX target and `tracer`
- [ ] an intruding process is refused by the security policy — the policy is written (`image/fragments/security_policy.xml`) but has never been compiled or enforced: it needs the SDP
- [x] safety document (`docs/SAFETY.md`) and tutorial (`codelab/`)
