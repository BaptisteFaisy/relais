# Relais — 3-hour practical session (TP) for embedded-systems students

*Transformation of [`codelab/README.md`](README.md) into a self-contained 3-hour
practical. Everything runs on a plain Linux PC: no QNX licence, no root, no
hardware. The numbers quoted are the real outputs of the commands. State the
standing once, at the start: Relais is a demonstrator aligned with Annex 6 of
UN Regulation No. 79 — not compliant, not approved, not certifiable — and every
latency is an emulated measured worst case, never a proven worst case.*

## Learning objectives

After this session the student can:

1. Explain why a steering column makes *fail-safe* valid, and why steer-by-wire
   and automated driving force *fail-operational*.
2. State the four traps of a two-computer handover (dead-or-mute, the returning
   machine, the jolt, the wrongful handover) and how Relais addresses each.
3. Run the demonstration, read the actuator's journal, and locate the four lines
   that describe a handover tick by tick.
4. Distinguish the two handover levels (base: the will bridges; full: the ready
   second substitutes from the first miss).
5. Trigger a freeze, a cable cut and a death, and predict the outcome from the
   truth table.
6. Explain the difference between level 1 (exploration), level 2 (simulation)
   and the mutant bench, and say what each proves that the others cannot.

## Prerequisites

- A Linux PC with `gcc`, `make`, `python3` (standard library only). Nothing else.
- Basic C reading (the students will read ~30 lines of `core/src/arbiter.c`).
- No prior QNX knowledge: the QNX layer is only inspected, not built.

## Session plan (3 h)

| Block | Minutes | What |
|---|---|---|
| TD1 | 25 | Build and run the demonstration |
| TD2 | 20 | Read one journal line |
| TD3 | 35 | Kill the holder; base vs full level |
| TD4 | 20 | The returning machine (freeze/thaw) |
| TD5 | 20 | Cut the cable between computers |
| TD6 | 45 | Run the proof, then read the core |
| Open exercise | 15 | Add a fault to the model and prove it |

---

## TD1 — build and run the demonstration (25 min)

**Énoncé.** Clone, build and run the one-command demonstration. Read the final
line and say what each field means.

**Commande.**

```bash
git clone https://github.com/BaptisteFaisy/relais.git && cd relais
make -j$(nproc)
make demo          # one command, no root, no QNX, no QEMU
```

**Résultat attendu.** The demo starts three real processes (one actuator, two
computers) over UDP, plays a slalom, freezes the second, kills the holder, and
prints the handover journal and the verifier's verdict. The tail must show:

```
  tick     event                  node     mandate
  904      mandate_revoked        A        1
  904      mandate_auto_switch    B        2
ticks=1600 violations=0 max_gap=0 (bound 0) max_deviation=0 (bound 0.02)
```

`max_gap=0` is the promise: no tick without a fresh order. `max_deviation=0`
compares every applied value with the fault-free twin. On a busy machine, use
the deterministic mode instead: `python3 host/bench/relais_bench.py --demo --sim`.

---

## TD2 — read one journal line (20 min)

**Énoncé.** The actuator writes one JSON record per tick; that file is the
authority. Read one line and explain, in your own words, why the value `-0.031`
was applied and who computed it.

**Commande.** (journal is shown in TD1 output; the canonical example is)

```json
{"kind":"tick","tick":902,"mandate":1,"holder":"A","source":"shadow","value":-0.031,
 "pos":0.598,"vel":-0.02,"setpoint":0.599,"misses":1,"lamp":"yellow",
 "cause":"node_missing","stop":false}
```

**Résultat attendu.** At tick 902 the holder A sent nothing (`misses: 1`), so
the actuator applied the **shadow** of B, its ready second: the command stayed
fresh, the lamp went yellow, and the record says exactly why.

---

## TD3 — kill the holder: base vs full level (35 min)

**Énoncé.** Kill the holder at tick 500 and verify the handover offline. Then
run the same death at base level and observe the will.

**Commande.**

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet \
    --fault 500:death:0 --journal /tmp/fault.jsonl --twin-journal /tmp/twin.jsonl
./build/relais-verify /tmp/fault.jsonl --level complet --twin /tmp/twin.jsonl

./build/relais-simulate --ticks 2000 --seed 7 --level socle \
    --fault 500:death:0 --journal /tmp/faults.jsonl --twin-journal /tmp/twins.jsonl
./build/relais-verify /tmp/faults.jsonl --level socle --twin /tmp/twins.jsonl
```

**Résultat attendu.** Open `/tmp/fault.jsonl` around tick 500 and find the four
lines that matter: A's last order, B's first shadow, the tick where the mandate
moves (exactly `p+1` ticks after A's last order), and B's first order. At full
level `max_gap=0`. At base level the journal shows `"source":"testament"`
records bridging the two ticks the second needs to ask and be granted;
`max_gap=1` (bound 2), the will serving the gap.

---

## TD4 — the returning machine (20 min)

**Énoncé.** Freeze A for 200 ms, then thaw it. Explain the double rejection of
its first order after the thaw.

**Commande.**

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet --fault 800:freeze:0:40
```

**Résultat attendu.** Two periods after the freeze the actuator has moved on:
A's order on thaw is rejected **twice over** — it does not carry the current
mandate, and it does not target the next tick. A reads the next status, sees B
holding mandate 2, and stands down as second. No timeout, no vote, no split
brain. The run ends `violations=0`.

---

## TD5 — cut the cable between the two computers (20 min)

**Énoncé.** Cut only the link between A and B. Predict what happens, using the
truth table (`docs/TRUTH-TABLE.md`, case F4), then check.

**Commande.**

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet --fault 600:cut-peer:0
```

**Résultat attendu.** Nothing happens, and that is the point. The second no
longer receives the replicated state, but it still hears the actuator's status,
so it keeps following and rebuilds a state from what the actuator publishes.
**No handover is triggered** (`mandats 1`). A machine that cannot hear its
neighbour cannot promote itself: the "dead or just disconnected?" dilemma,
settled without a third computer.

---

## TD6 — run the proof, then read the core (45 min)

**Énoncé.** Run the three levels of proof and the mutant bench. Then open the
core and read `relais_arb_step` (seven steps).

**Commande.**

```bash
make test                        # unit + protocol checks
make campaign                    # 40 fault runs, both levels
./build/relais-explore --budget 100000 --depth 12 --faults 1    # level 1
python3 host/mutants/run_mutants.py
```

**Résultat attendu.** `make test` prints green lines ending in a checks count
per binary; `make campaign` prints `CAMPAGNE-OK`; the explorer prints `100198
states … 0 violation`; the mutant bench prints a table where each of M1–M8 is
`FLAGGED`. Then read `core/src/arbiter.c` and identify the seven steps of
`relais_arb_step`: find the holder's order → count the miss → revoke past `p` →
process cessions then demandes → pick the one value (order → shadow → will →
fallback) → update readiness and the lamp → publish the status and the journal.
Note the absence of clock, file, socket and allocation — the same file compiles
into the QNX processes, the simulator and the explorer.

---

## Open exercise — add a fault to the model and prove it (15 min, plus homework)

**Énoncé.** The fault model lives in `host/simulate/sim_faults.c`. It already
registers `death`, `freeze`, `thaw`, `cut-actuator`, `cut-peer`, `restore`,
`slow` and `bias` (the name table is at the top of the file). Add **one** new
fault kind that is not there yet — a sensible choice is *frame duplication* (a
package is delivered twice) — and prove two things: (a) the fault is actually
exercised, and (b) the safety invariants still hold under it.

**Guidance (what "prove it" means here).**

1. Add an enum value and a name entry, implement the effect (in the
   `deliver`/`apply` path, where the existing `bias` and `cut_*` effects are
   applied), and expose it through the `--fault at:kind:node[:arg[:value]]`
   parser.
2. Exercise it: `./build/relais-simulate --ticks 2000 --seed 7 --level complet
   --fault 500:yourkind:0` and confirm the run reports `violations=0` **and**
   that the fault actually fired (look at the mandate/counter output, or add a
   journal-visible marker by analogy with the existing kinds).
3. Prove no regression: `make test` and `./build/relais-explore --budget 100000
   --depth 12 --faults 1` must still be green after your change.

**Correction sketch (read only after trying).** See §Corrections below.

---

## Corrections

**TD1.** `max_gap`: largest number of consecutive ticks without a fresh order.
`max_deviation`: largest difference from the fault-free twin replay. A value of
0 on both is the full-level promise.

**TD2.** `source:"shadow"` means the holder's order was missing and the second's
shadow was applied; the second is marked ready (the actuator accepted its
shadow as matching after 3 ticks).

**TD3.** The four lines: A's `ORDRE` (mandate 1, tick n), B's `OMBRE` (first
substitution), the tick record where `mandate: 2` appears (p+1 after A's last
order), B's first `ORDRE`. Base level: the will (`source:"testament"`) covers
the gap; `max_gap=1` is inside the `p+1=2` bound.

**TD4.** The thawed machine is disarmed by `MAN-02` (not the holder) and by
`ACT-09` (order targets a past tick). It rejoins as second by `REL-07`.

**TD5.** `cut-peer` is fault F4: tolerated **without** handover; the second
recomputes a bump-free state from the published status (`REP-04`); the lamp
signals "link lost" (`DET-06`).

**TD6.** Exploration is exhaustive and unbeatable for the arbiter's *own*
decisions (M3, M8 fall almost immediately); the seeded campaign is indispensable
for adversarial frames and long double faults (M1, M2, M4). Neither replaces the
other — the table in [`docs/RESULTS.md` §5](../docs/RESULTS.md) says which proof
catches which mutant.

**Open exercise.** The minimal correct change touches three places: the
`sim_fault_kind` enum in `host/simulate/sim.h`, the `fault_names` table and the
`sim_apply_fault` switch in `host/simulate/sim_faults.c`, and the `--fault`
parser path that already accepts `kind` strings. A duplication fault makes a
frame appear twice; the arbiter drops the duplicate by its tick label and
mandate (no effect on the command, `ROB-02`), so `violations=0` is the expected
verdict — and the *proof* that the fault fired is that you put a counter or a
journal event in your new path and saw it move. If the explorer or `make test`
turns red after your edit, the fault is touching a path the invariants cover,
and you must either constrain it to the delivery path or reconsider whether your
"fault" is really a silent, benign duplicate.