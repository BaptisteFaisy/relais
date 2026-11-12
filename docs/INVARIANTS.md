# Relais — Invariants

Every invariant this project claims, where it is checked, and what its status
is. Three kinds live here:

1. the invariants of the specification (spec 7.2, I1..I11, plus DET-03 and
   DET-09);
2. the invariants discovered **automatically** by mining the journals of a
   corpus of simulated runs (M12);
3. the properties added by M12: the **non-interference** of the second
   channel, and the bounds of the **control law** proved by model checking.

Nothing here is asserted without a command that reproduces it (§6).

## 1. Status vocabulary

| Status | Meaning |
|---|---|
| **proved by exploration** | checked in *every* state of the exhaustive DFS explorer (`host/explore/`); a counter-example would be a reachable state |
| **verified by test** | checked by a dedicated harness, the seeded simulator, the offline verifier or the mutant bench |
| **proved by model checking** | checked by TLC on a TLA+ model (`docs/tla/`) |
| **candidate — confirmed** | discovered by the miner, and holds in every journal of the corpus that exercised it |
| **candidate — NOT confirmed** | discovered by the miner, and **contradicted** by at least one journal; it is *not* a claimed invariant |

A candidate is only an invariant of the specification once it has been
re-derived from the specification. Until then it stays a candidate, and this
document says so explicitly.

## 2. The invariants of spec 7.2

| ID | Invariant | Status | Checked by |
|---|---|---|---|
| I1 | **Uniqueness.** At every tick the actuator applies a single value, and its source is the holder, the certified second by substitution, the will or the fallback law. Nothing else. | proved by exploration | `core/src/invariants.c` (`check_unicity`), fed by `host/explore/world.c` |
| I2 | **Monotonicity.** The mandate number never decreases and changes at most once per tick. Two machines are never holders of the same mandate. | proved by exploration | `check_mandate`, `check_mandate_changes` (event path) |
| I3 | **No returning machine.** No order targeting another tick than the next one is applied, and no order of a deposed machine is applied after its deposition. | proved by exploration | `check_no_returning`; the tick label itself is guaranteed by the arbiter (MAN-02) and explored directly |
| I4 | **Bounded gap.** Under the single-fault hypothesis, the number of consecutive ticks without a fresh order does not exceed p+1 at base level, and is zero with a certified second. | proved by exploration, inside the timing hypothesis | `check_gap`; the bound is relaxed (gap still measured and published) on a lossy or delayed channel, and after a double fault |
| I5 | **Continuity.** The deviation between the applied value and that of the fault-free twin stays below δ. | verified by test | the simulator only: the explorer has no twin (`relais_inv_on_tick(..., NULL)`). Measured by the campaign, enforced at level C inside the timing hypothesis |
| I6 | **Will.** No will value is applied beyond its horizon. At its exhaustion, the fallback law applies at the next tick. | proved by exploration | `check_horizon` |
| I7 | **No wrongful handover.** Without an injected fault, including under lower-priority overload and with the loss of p consecutive orders, the holder does not change. | proved by exploration | the explorer runs the fault-free part of the state space with `relais_inv_fault_free(true)`; the campaign covers the injected faults |
| I8 | **Double fault.** If no machine is able, the actuator is in the fallback law at most N+1 ticks after the last fresh order. | proved by exploration | `check_horizon` (the fallback branch) |
| I9 | **Reintegration.** While a machine joins, every tick is served by the holder. | proved by exploration, inside the timing hypothesis | `check_reintegration` (the `node_back` event opens the window) |
| I10 | **Signalling.** Every loss of redundancy appears on the lamp at most 2 ticks after its detection, and stays there as long as it lasts. | proved by exploration | `check_signalling` |
| I11 | **Progress.** Every state machine processes every event in bounded time, and has no state without an exit. | proved by exploration | `relais_inv_on_tick` (contiguous tick numbering) |
| DET-03 | **Evidence.** The actuator revokes a mandate only on its own evidence, and the event carries it. | proved by exploration | `relais_inv_on_event` on `mandate_revoked` / `mandate_auto_switch` (MAN-04, MAN-05) |
| DET-09 | **Detection.** A machine that computes wrong is unmasked, not merely tolerated: the disagreement is reported within 3 ticks. | verified by test | the simulator and the campaign inject a value fault (`relais_inv_value_fault`); the explorer has no fault injection of that kind |

The explorer reaches **10 000 330 states with two faults and 0 violation**
(51 s), and 100 198 states fault-free (0.6 s). The unit tests add **5651
checks**, the campaign **19 659 runs with 0 failure**, and the mutant bench
flags **8/8 mutants**.

### 2.1 The mutants as a sensitivity check

Each invariant above is tied to a mutant that breaks it, so a green bench is
not a vacuous bench (`docs/RESULTS.md` §5).

| Mutant | Breaks | Flagged as |
|---|---|---|
| M1 | the actuator applies the order of anybody | I1 / I3 |
| M2 | the actuator ignores the tick label | I3 |
| M3 | the mandate is granted with no evidence of silence | I7 |
| M4 | the will is played past its horizon | I6 |
| M5 | the second keeps its own state | I5 |
| M6 | the mandate number restarts from zero | I2 |
| M7 | the second sends orders without being granted | MAN-08 |
| M8 | the lamp goes off while the second is uncertified | I10 |

## 3. Non-interference of the second channel (M12)

**NI-1.** The second channel can never influence the value the actuator
applies, nor the mandate, unless the actuator has delivered that second a
mandate.

**Method** (`host/verify/noninterference.c`): two worlds run in lockstep,
identical except for what the *second* sends.

* *Scenario 1 — arbitrary shadows.* The reference world feeds a shadow that
  never matches the holder's order, so the second never certifies and the
  comparison window covers the whole run. World 2 replaces it with an
  arbitrary shadow drawn at random from the extremes (`0`, `-0`, `±1`,
  `±1e6`, `±FLT_MAX`) and from `[-2, 2] × max_amplitude`.
* *Scenario 2 — a deposed machine keeps emitting.* The holder A cedes, B is
  granted the mandate, and from then on world 2 lets the former holder keep
  sending arbitrary orders, while world 1 keeps it silent.

**Claim under test.** While the second is neither holder nor certified, the
applied value and the mandate must be **identical bit for bit** in the two
worlds. A divergence is a counter-example, written in the scenario format
(`{"kind":"noninterference-counterexample", ...}`).

| | Result |
|---|---|
| Nominal build | **200 runs × 2 scenarios × 400 ticks, 0 divergence** |
| Same source, `-DRELAIS_MUTANT=1` (the actuator applies anybody's order) | **200/200 runs diverge**, exit 1, counter-example written |

The second row is the point of the exercise: a test that cannot fail proves
nothing. M1 makes the deposed machine's arbitrary order effective, and the
harness sees it at the first tick of the deposed phase
(`world1.value = 0.327424` against `world2.value = 0.0460601`, the injected
value).

**Status: verified by test** — `host/verify/noninterference.c`, built by
`make all` as `build/noninterference`, run with `./build/noninterference
--runs 200`.

## 4. The control law (M12, model checking)

`docs/tla/ControlLaw.tla` models the physical half of the system: the state
of the controller, one period of the law (`relais_ctl_step` transcribed line
by line), the clamps, and the rack integration (`relais_plant_step`). It is
the part the protocol model does not cover.

TLC cannot check real numbers, so the law is transcribed in fixed point
(SC = 1000) with integer arithmetic; the measurement grid is finite, which is
sound for the two bounds because they are **total properties of the clamps**.

| ID | Invariant | Status | Checked by |
|---|---|---|---|
| CL-1 | **CmdBounded.** The command never leaves the amplitude bound: `-max_amplitude ≤ cmd ≤ max_amplitude` (ACT-05, CMD-06). | proved by model checking | `CmdBounded` in `docs/tla/ControlLaw.tla` |
| CL-2 | **SlewBounded.** The command never moves by more than `max_slew` in one tick: `|cmd − prevCmd| ≤ max_slew` (ACT-05). | proved by model checking | `SlewBounded` |
| CL-3 | **IntegBounded.** The integral term stays inside its own limit; the anti-windup by back-calculation cannot push it out. | proved by model checking | `IntegBounded` |

TLC result: **560 distinct states, depth 10, no error** (6 s), with the
integral term initialised at `-IntegLimit`, `0` and `IntegLimit` so the whole
range of the integrator is covered.

The model is **not vacuous**: breaking `ClampSlew` in a scratch copy makes
TLC report `Invariant SlewBounded is violated` after 4 states.

**Scope note.** `prev_setpoint` and `setpoint_rate` are part of the C
controller state but feed only the testament extrapolation, never the command
of the tick; they are abstracted away from the checked state, which keeps the
model small without weakening CL-1 or CL-2. The closed loop (rack model,
trajectory, disturbance) is verified by the C simulator and by the exhaustive
explorer, not by TLC.

## 5. Invariants discovered automatically (M12)

`host/verify/invariant_mining.py` reads a corpus of journals (one JSON object
per line, `CONTRACT.md`) and evaluates a library of candidate relations —
implications between events, numeric bounds, exclusions and orderings — then
reports, for each, how many journals confirm it and with what confidence.

* **confirmed**: the relation holds in every journal that exercised it
  (support > 0).
* **candidate**: at least one journal contradicts it.
* **vacuous**: no journal exercised it; the relation says nothing.

### 5.1 The corpus

33 journals, **49 500 ticks**, produced by the seeded simulator: 10 nominal
runs (both levels, five seeds), 8 runs with the death of A swept over the
period (both levels), and 15 runs with one fault each — death of B, freeze
(A and B, with and without thaw), cut of the link to the actuator, cut of the
link between the computers, slow machine, biased value, crash of a local
process, restart, double death, lossy channel, delayed channel, random
faults, and a forged order. The corpus is regenerated by §6.4; it is not
stored in the repository (12 MB of generated journals).

### 5.2 The candidates, sorted by confidence

| ID | Relation | Kind | Confidence | Journals | Support | Status |
|---|---|---|---|---|---|---|
| AMP-BOUND | `|value| ≤ max_amplitude` at every tick | bound | 1.00 | 33/33 | 49 500 | candidate — confirmed |
| MANDATE-MONO | the mandate never decreases and rises by at most 1 | ordering | 1.00 | 33/33 | 49 467 | candidate — confirmed |
| MISSES-BOUND | holder set ⇒ `misses ≤ p+1` | bound | 1.00 | 33/33 | 48 861 | candidate — confirmed |
| HOLDER-FRESH | `source=holder ⇒ misses=0` | exclusion | 1.00 | 33/33 | 47 844 | candidate — confirmed |
| CERT-NODE | `certified=true ⇒ certified_node` is set | exclusion | 1.00 | 21/33 | 21 331 | candidate — confirmed |
| LAMPOFF-HOLDER | `lamp=off ⇒ source=holder` | exclusion | 1.00 | 21/33 | 20 901 | candidate — confirmed |
| NONHOLDER-LIT | `source ∈ {shadow, testament, fallback} ⇒ lamp ≠ off` | exclusion | 1.00 | 33/33 | 1 656 | candidate — confirmed |
| FALLBACK-RED | `source=fallback ⇒ lamp=red` | exclusion | 1.00 | 33/33 | 530 | candidate — confirmed |
| GRANT-VACANT | `mandate_granted ⇒` the mandate was vacant, or was freed earlier in the same tick | implication | 1.00 | 33/33 | 253 | candidate — confirmed |
| CERT-NEXT | a `certified` event ⇒ the next tick reports `certified` | implication | 1.00 | 21/33 | 50 | candidate — confirmed |
| AUTOSW-CERT | `mandate_auto_switch ⇒` the previous tick was certified | implication | 1.00 | 9/33 | 27 | candidate — confirmed |
| **SLEW-BOUND** | `|value[t] − value[t−1]| ≤ max_slew` | bound | **0.94** | **31/33** | 49 467 | **candidate — NOT confirmed** |

The "journals" column counts the journals that *exercised* the relation; a
relation that is never exercised in a journal (no `mandate_auto_switch`
happened there) does not count against it.

### 5.3 What the mining changed

Two relations were **refuted as first stated**, and the miner is what found
them:

* **GRANT-VACANT**, first stated as "`mandate_granted` ⇒ the previous tick
  was vacant", was contradicted by 8 of 10 journals. The counter-example is
  real and the first statement was too strong: the arbiter revokes on its own
  evidence and grants **within the same tick** (`relais_arb_step`: revoke at
  step 3, grant at step 4). The relation now reads "vacant before, or freed
  earlier in the same tick", and it is confirmed in 33/33 journals. This is
  MAN-05, now stated in the form the journal can actually show.
* **SLEW-BOUND** is the one relation that stays a candidate. It is
  contradicted in 2 of 33 journals — `28-double-death` and `31-delayed` —
  and in **16 transitions out of 49 467** (0.03 %). Every one of them is a
  **change of source**: `testament → holder` (8), `testament → fallback` (5),
  `fallback → holder` (3); the largest jump is 0.8866. Within a single
  source the bound holds.

  This is not a defect, it is the boundary of the claim. `max_slew` bounds
  the command a machine emits and the state of the fallback loop; it does
  **not** bound the applied value across a change of source at the actuator.
  The continuity that is claimed across a handover is δ (I5), and I5 is not
  enforced in exactly those two runs — a double fault (F9, F10) and a
  delayed channel, both outside the timing hypothesis. So the correct
  statement is: *SLEW-BOUND holds within a source; across a source change the
  applied value is bounded by δ, inside the timing hypothesis only.*

  **SLEW-BOUND is therefore NOT an invariant of the specification.** It
  stays a candidate, and the document says so.

### 5.4 The miner is not a rubber stamp

`host/verify/test_invariant_mining.py` checks, on synthetic journals, that a
true relation is reported confirmed, that a false one is reported candidate
(and the journal that contradicts it is named), and that a degenerate corpus
— empty, header only, malformed lines, no events — never crashes and never
promotes a vacuous relation. 13 tests.

## 6. Reproducing everything

### 6.1 Build and unit tests

```sh
cd <repo> && make -j4 all && make test      # 5651 checks
```

### 6.2 Non-interference (M12)

```sh
make all
./build/noninterference --runs 200           # expect: NONINTERFERENCE-OK, exit 0
```

Sensitivity (the mutant must be caught):

```sh
make mutants                                 # builds build/m1/librelais.a
cc -std=c11 -O2 -Wall -Wextra -Werror -Icore/include -DRELAIS_MUTANT=1 \
   host/verify/noninterference.c build/m1/librelais.a -lm -o build/noninterference-m1
./build/noninterference-m1 --runs 200        # expect: NONINTERFERENCE-FAIL, exit 1
```

### 6.3 Invariant mining (M12)

```sh
python3 -m unittest discover -s host/verify -p 'test_invariant*.py' -v
```

### 6.4 The corpus, and the mining over it

```sh
mkdir -p /tmp/corpus && cd /tmp/corpus
for s in 1 2 3 4 5; do
  /path/to/build/relais-simulate --ticks 1500 --seed $((100+s)) --level complet \
      --journal nom-s$s.json --quiet
  /path/to/build/relais-simulate --ticks 1500 --seed $((200+s)) --level socle \
      --journal nom-s$s-socle.json --quiet
done
for at in 400 401 402 403; do
  /path/to/build/relais-simulate --ticks 1500 --seed $((300+at)) --level complet \
      --journal death-A-$at.json --quiet --fault $at:death:0
  /path/to/build/relais-simulate --ticks 1500 --seed $((400+at)) --level socle \
      --journal death-A-$at-socle.json --quiet --fault $at:death:0
done
/path/to/build/relais-simulate --ticks 1500 --seed 501 --level complet --journal death-B.json --quiet --fault 600:death:1
/path/to/build/relais-simulate --ticks 1500 --seed 502 --level complet --journal freeze-A.json --quiet --fault 500:freeze:0:60
/path/to/build/relais-simulate --ticks 1500 --seed 503 --level complet --journal freeze-B.json --quiet --fault 700:freeze:1:40
/path/to/build/relais-simulate --ticks 1500 --seed 504 --level complet --journal cut-A-actuator.json --quiet --fault 500:cut-actuator:0
/path/to/build/relais-simulate --ticks 1500 --seed 505 --level complet --journal cut-A-B.json --quiet --fault 600:cut-peer:0
/path/to/build/relais-simulate --ticks 1500 --seed 506 --level complet --journal slow-A.json --quiet --fault 300:slow:0:3
/path/to/build/relais-simulate --ticks 1500 --seed 507 --level complet --journal bias-B.json --quiet --fault 800:bias:1:0:0.4
/path/to/build/relais-simulate --ticks 1500 --seed 508 --level socle  --journal crash-proc-A.json --quiet --fault 500:crash:0:10
/path/to/build/relais-simulate --ticks 1500 --seed 509 --level complet --journal restart-A.json --quiet --fault 400:death:0 --fault 900:restart:0
/path/to/build/relais-simulate --ticks 1500 --seed 510 --level complet --journal double-death.json --quiet --fault 400:death:0 --fault 1000:death:1
/path/to/build/relais-simulate --ticks 1500 --seed 511 --level socle  --journal freeze-then-thaw.json --quiet --fault 500:freeze:0:40 --fault 700:thaw:0
/path/to/build/relais-simulate --ticks 1500 --seed 512 --level complet --journal lossy.json --quiet --loss 0.05
/path/to/build/relais-simulate --ticks 1500 --seed 513 --level complet --journal delayed.json --quiet --delay 1
/path/to/build/relais-simulate --ticks 1500 --seed 514 --level socle  --journal auto-faults.json --quiet --auto-faults 6
/path/to/build/relais-simulate --ticks 1500 --seed 515 --level complet --journal forge-B.json --quiet --fault 700:forge:1

python3 /path/to/host/verify/invariant_mining.py /tmp/corpus
```

The simulator is seeded, so the corpus is reproducible bit for bit and the
numbers of §5.2 come out again.

### 6.5 The control law, model checked (M12)

```sh
cd docs/tla
java -cp /path/to/tla2tools.jar tlc2.TLC ControlLaw.tla
# expect: Model checking completed. No error has been found.
```

## 7. Limits

* The mining reads journals. What the journal cannot show is stated where it
  is checked: the tick label of an applied order is guaranteed by the arbiter
  (MAN-02) and explored directly, not inferred from the journal.
* I4, I5 and I9 are claimed **inside the timing hypothesis** and under the
  single-fault rule. Outside it (double fault, lossy or delayed channel) the
  gap and the deviation are still measured and published, but only the safety
  invariants keep being enforced — which is exactly what §5.3 observed on
  its own.
* The control-law model is a fixed-point transcription of the C law, over a
  finite measurement grid. It certifies the command bounds, which are total
  properties of the clamps; it does not replace the closed-loop simulation.
* A machine that computes wrong is **detected, not outvoted** (DET-09,
  `docs/SAFETY.md` §7). With two computers there is no majority.
