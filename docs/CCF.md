# Relais — common cause failures and the diversity that really exists

> **Standing.** This document *measures* the common causes of the two computers
> and the diversity that can be produced on this machine, on the day it was
> written. Everything in §3 and §4 is a measurement, with the command that
> produces it. The beta factors of §5 are **estimates with their hypotheses
> written down**, not measurements; §8 lists what this document does not cover.
> No diversity is claimed here that was not actually produced.

Requirement IDs used: CMD-03 (same bits on both machines), CMD-09 (the law can
be exchanged), DEG-08 (dissimilar backup), DET-09 (order/shadow disagreement),
REL-03 (the successor computes what the holder would have computed), I4/I5
(gap and continuity), SEC-01/SEC-04 (MAC and forged orders), ROB-03 (loss of p
orders).

---

## 1. The claim under test

The project describes machine B as **dissimilar** to machine A. The measurable
truth today is narrower: A and B run **the same source**, compiled by **the same
compiler**, on **the same libc** and **the same kernel**, from **one repository**
and **one build**. A defect that reaches the binary therefore reaches both
machines, and the redundancy protects against nothing of that kind. A jury of
functional safety will attack exactly this point, and it is right to.

This document answers three questions with numbers:

1. What is shared, and what is not? (§2)
2. Does the diversity we can produce change any decision? (§3, §4)
3. What would it cost to get real diversity? (§5, §7)

## 2. Inventory of the shared layers

`HOST` = the PC the demonstrator runs on; `QNX` = the two embedded images of
`target/`. "Shared" means *the same artefact, the same process, or the same
file* on both machines.

| # | Layer | Machine A | Machine B | Shared? | Evidence |
|---|---|---|---|---|---|
| 1 | **Source** (`core/`, 2 584 lines of C11) | same tree | same tree | **YES** | one digest for every variant: `b2e8240117003293` (`build/diversity/source-digest.txt`) |
| 2 | **Compiler** | gcc 13.3.0 | gcc 13.3.0 | **YES** | `cc`, `gcc`, `x86_64-linux-gnu-gcc`, `gcc-13` all resolve to `/usr/bin/x86_64-linux-gnu-gcc-13` |
| 3 | **Second compiler** | absent | absent | **YES (none)** | `command -v clang tcc` → nothing; no other gcc major; no `musl-gcc` (`build/diversity/toolchain.tsv`) |
| 4 | **Compile options** | one `Makefile`, one `CFLAGS` | same | **YES** | `Makefile` lines 9-11 |
| 5 | **libc** | glibc 2.39 | glibc 2.39 | **YES** | `ldd --version` → Ubuntu GLIBC 2.39-0ubuntu8.9 |
| 6 | **Kernel** (PC bench) | Linux 6.14.0-37 | same host | **YES** | `uname -r`; both "computers" are processes of one host |
| 7 | **Kernel** (QNX target) | QNX 8.0 | QNX 8.0 | **YES** | one image recipe, `image/` |
| 8 | **Tempo / clock** | the actuator | the actuator | **YES, by design** | the actuator is "arbitre ET horloge" (spec 1.4); both machines read the tick from STATUT |
| 9 | **Build chain** | one Makefile, one CI workflow | same | **YES** | `.github/workflows/` |
| 10 | **Repository / version** | one git history | same commit | **YES** | one commit builds both images |
| 11 | **Specification** | one `docs/SPEC.md` | same | **YES** | a design defect is common by construction |
| 12 | **Test oracle** | `core/src/invariants.c` | same checker | **YES** | the explorer, the simulator and the verifier share it |
| 13 | **Test corpus** | `scenarios/`, seeds | same | **YES** | 27 runs of §3 |
| 14 | **MAC key** | one symmetric key for A, B **and** the actuator | same | **YES** | `--key` is passed to all three; the PC bench runs with the MAC disabled (`relais_key_clear` → `enabled = false`) |
| 15 | **Clamping helpers** | `relais_clamp_value/slew` | same functions | **YES** | `core/src/control.c`; an alternative law reuses them (§4) |
| 16 | **Plant model** | `core/src/plant.c` | same | **YES** | both machines and the actuator share it |
| 17 | **Power, chassis, bus** (two real boards) | board A | board B, own cable | **NO** | `docs/TWO-BOARDS.md` |
| 18 | **Power, host, bus** (the bench of this repo) | one host, one PSU, one NVMe | same host | **YES** | the emulation cannot demonstrate any hardware diversity |
| 19 | **Mandate number** | actuator writes it | — | **NO** | `relais_arb` is the only writer |
| 20 | **Fault instance** | injected into one machine | not injected | **NO** | that is exactly what the fault model does — and it is the assumption this document questions |

Rows 1-16 are the shared layers that make a *software* common cause certain.
Rows 17-20 are the layers that are genuinely per-machine.

## 3. Measured: compilation diversity produces different code and identical decisions

`host/diversity/build_variants.sh` builds every variant of the core that this
machine can produce, and records the exact compilation line, the binary digest
and the source digest. `host/diversity/compare.py` then plays one corpus on
every variant and compares **every decision of every tick bit for bit**: the
float columns are written as raw IEEE-754 patterns, not as a rounded decimal.

```
host/diversity/build_variants.sh --jobs 3
python3 host/diversity/compare.py --ticks-cap 1500 --seeds 4
```

**Corpus** (27 runs): the 11 published scenarios of `scenarios/`, the 8 targeted
probes of `host/mutants/run_mutants.py` (the runs that make an injected defect
visible), and 8 seed sweeps (a death, and a lossy channel with a delay).

**Result** (`build/diversity/comparison.json`):

| Quantity | Value |
|---|---|
| Variants built | **16** (13 production-eligible + 3 controls) |
| Variants not built | 1 (`m32`, BLOCKED: no 32-bit multilib), plus clang/tcc absent |
| Distinct binary digests | **16 / 16** (185 784 B for `-O0` … 636 904 B for the UBSan build) |
| Tick records compared | **588 000** |
| Field comparisons | **14 112 000** (24 columns per tick) |
| **Divergences between real variants** | **0** |
| Controls detected | **3 / 3** |

The controls prove the comparator is not blind:

| Control | Detected on | First divergent tick | Differing rows | Worst gap |
|---|---|---|---|---|
| `-DRELAIS_MUTANT=1` (actuator applies anybody's order) | `probe-forge` | 600 | 901 | 0.428 (value) |
| `-DRELAIS_MUTANT=5` (second keeps its own state) | `slalom-freeze-A-200ms` | 942 | 559 | 0.0403 (shadow) |
| `-ffast-math` (reassociation, reciprocal) | `rolling-update` | 3 | 128 | 2.38e-06 (velocity) |

**Two conclusions, both measured:**

- The axes `-O0/-O1/-O2/-O3/-Os`, `-march=native` (this CPU has FMA, AVX2,
  AVX-512), `-ffp-contract=off`, strict aliasing ON, `-fno-builtin`, `-fPIC`,
  `-fstack-protector-strong`, `-std=c17` and `-fsanitize=undefined` change the
  **machine code** (16 different digests) and change **no decision** (0
  divergences in 14 112 000 comparisons). *Flag diversity is not diversity*: the
  common-cause factor for every one of those axes stays exactly where it was.
- `-ffast-math` breaks the bit identity CMD-03 relies on (worst 2.38e-06 here,
  below the 0.005 order/shadow tolerance, so the protocol survives it — but the
  two machines would no longer compute the same bits). It must stay forbidden,
  and the CI should keep the compiler version pinned.

**BLOCKED:** a 32-bit variant (another calling convention, another `long
double`) cannot be built — the multilib is not installed, so the link fails. A
second compiler cannot be built either: none is installed on this machine, and
the script records that as ABSENT instead of pretending.

## 4. Measured: design diversity changes decisions — and has a price

Compiling the same source differently changes nothing (§3). Writing a
**different control law** does change the decisions. `host/diversity/law_alt.c`
holds two laws written apart from the built-in PID and plugged through the
existing three-function interface of CMD-09 (`relais_node_set_law`); `core/` is
untouched.

* **first-order** — a first-order compensator on a PD target: no integrator, no
  anti-windup, no filtered measurement.
* **ramp** — a ramp follower: feed-forward on the measured slope of the request
  plus a weak position feedback.

```
python3 host/diversity/law_effect.py     # -> build/diversity/law-effect.{json,md}
```

Measured over 5 handover runs (death of the holder, death of the second,
voluntary cession, freeze, crash of a local process), 1 500 ticks each:

| Configuration | order/shadow gap p99 | Certified ticks | Violations | Max deviation from the twin | Tracking error (mean) |
|---|---|---|---|---|---|
| identical (both built-in) | 0.0000 | 798-1493 / 1500 | 0 | 0.0000 | 0.0150 |
| first-order on B, band **not** published | 0.0089 - 0.0146 | 341 - 717 / 1500 | **2** | 0.0253 | 0.0150 |
| first-order on B, DEG-08 band published (`deps=0.10, ddelta=0.05`) | 0.0089 - 0.0130 | 788 - 1483 / 1500 | 0 | 0.0088 | 0.0150 |
| ramp on B, band `deps=0.10` | 0.1880 - **1.4422** | 258 - 332 / 1500 | 0 - 11 | 0.1876 | 0.0084 - 0.0101 |
| ramp on B, band `deps=0.25` | 0.1880 - 1.4422 | 758 - 954 / 1500 | 0 - 7 | 0.1873 | 0.0084 - 0.0101 |
| first-order on **A** (the holder) | 0.1543 | 763 - 1458 / 1500 | 0 | 0.0106 | 0.0150 |
| first-order on **both** | 0.0000 | 798 - 1493 / 1500 | 0 | 0.0000 | 0.0150 |
| ramp on **both** | 0.0000 | 798 - 1493 / 1500 | 0 | 0.0000 | **0.0029** |

Five results, all reproducible with the command above:

1. **A dissimilar law is not certified while the band is the default one.** The
   first-order law's shadow differs from the holder's order by 0.0089 - 0.0146
   of full scale, above `epsilon` (0.005) and below the published `deps`
   (0.10). Without the DEG-08 band the second loses its certification, the
   handover falls back to the will, and **2 invariant violations** appear
   (I5: deviation 0.0253 above `delta` 0.02) plus one tick without a fresh
   order. The band is not cosmetic: without it, the level-C claims break.
2. **With the band published, the invariants hold again** (0 violations, max
   deviation 0.0088 against the published 0.05), the second is certified
   *dissimilar* (1487/1500 ticks) and the lamp stays yellow. This is the
   DEG-08 trade, now with numbers.
3. **A feed-forward design can be too different to be certified at all.** The
   ramp follower's gap reaches 1.4422 of full scale (the two commands saturate
   in opposite directions at the trajectory onset). To certify it always, the
   band would have to exceed 1.0 — the whole full scale — which makes the
   comparison meaningless. Even with `deps=0.25` it is uncertified for a third
   of the ticks and the handover still deviates by 0.1873.
4. **A different law on *both* machines is not diversity at all.** The gap is
   0, the arbiter certifies the second as *identical* (the dissimilar flag is
   never raised) and the decisions are bit-identical again. "Dissimilar" must
   mean different *implementations*, not a different constant.
5. **Design diversity can also improve the nominal behaviour**: the ramp
   follower tracks the request 5.2x better on average (0.0029 against 0.0150 of
   full scale), at the price of a slightly worse peak at the trajectory onset
   (0.0309 against 0.0283). That is an argument for a second *implementation*
   of the whole control chain, not only for a backup.

### 4.1 What is missing to go further: the will is not produced by the installed law

CMD-09 has three hooks (init, step, export/import). It has **no hook for the
will**. `make_order()` in `core/src/node.c` always calls
`relais_testament_fill(&s->ctl, ...)`, which runs the **built-in** law on the
built-in state — a state an installed law never updates — and `as_holder()`
publishes `previous = s->ctl.last_value` (stale, zero) in ETAT.

The cost is measurable at level S, where there is no shadow and the handover is
always served by the will:

| Configuration (level S) | Max deviation from the twin |
|---|---|
| built-in law (the shipped configuration) | 0.0015 - 0.0253 |
| alternative law on the holder | **0.2307** (death of the holder), 0.0332 - 0.0340 (cession, freeze, crash) |

That is 9x to 154x worse than the shipped configuration: when a machine running
an alternative law dies, the actuator plays a will that does not come from the
law that was running. To close the gap, `relais_law` needs a fourth hook (a
`predict`/`fill_testament` function) and an accessor to the last value, so that
the will and ETAT come from the installed law. **That means touching
`core/include/relais/node.h` and `core/src/node.c`, which this mission does not
do** (core/ stays pure and untouched). It is the single most valuable change
this measurement points at.

## 5. The common-cause factor, with its hypotheses

### 5.1 The model

For a 1oo2 channel with a common-cause factor beta and a per-channel dangerous
failure rate `lambda_D` with proof-test interval `T1`, the average probability
of a dangerous failure is (IEC 61508-6, Annex B, structure and constants of the
periodic test model):

```
PFD(1oo2) = beta * PFD_1 + (1 - beta)^2 * PFD_1^2 / 3,     PFD_1 = lambda_D * T1 / 2
```

The two terms are the two ways the system fails: the common cause (the first
term, proportional to beta) and the two independent failures at the same time
(the second term, quadratic). **The whole benefit of the redundancy lives in the
second term, and the second term is multiplied by `(1 - beta)^2`.** With
`beta = 1` the second term vanishes and the redundancy buys nothing at all:
`PFD(1oo2) = PFD_1`, exactly as a single channel.

Illustration with `lambda_D = 1e-6 /h` (hypothesis H3, a plausible dangerous
failure rate of a software channel) and `T1 = 720 h` (monthly proof test), so
`PFD_1 = 3.6e-4`:

| beta | PFD(1oo2) | Gain against beta = 1 |
|---|---|---|
| 1 (today: same binary) | 3.6e-4 | none — the redundancy is worth nothing here |
| 0.1 | 3.60e-5 | 10x |
| 0.01 | 3.64e-6 | 99x |
| 0 | 4.32e-8 | 8 333x |

### 5.2 Hypotheses, written down

- **H1 — beta_sw = 1 for every software failure mode, today.** Identity is not
  a probabilistic estimate: a defect in the source is present in both copies
  with probability 1, and rows 1-16 of §2 are all shared. This is also the
  position of the standards: for identical software in redundant channels no
  credit is given for the redundancy (IEC 61508-6, Annex D; ISO 26262-9, §7
  requires dependent failures of identical elements to be treated as fully
  correlated).
- **H2 — beta_hw is 0.01 to 0.1 on two physically separate boards, and 1 in the
  bench of this repository.** Separated hardware with a common environment is
  usually credited with 10^-3 to 10^-2 (NUREG/CR-5485, EPRI TR-101805) and up
  to 10^-1 when the power, the clock or the environment are shared. In the PC
  bench the two "machines" are processes of one host: rows 18 of §2 — no
  hardware diversity can be demonstrated there at all, only reasoned about.
- **H3 — lambda_D = 1e-6 /h** for the illustration above. It is a hypothesis,
  not a measurement of this project: the project measures no hardware failure
  rate.
- **H4 — the measured diversity of §3 does not change beta.** Sixteen different
  binaries, 0 divergence in 14 112 000 comparisons: the flag axis is not a
  diversity axis.
- **H5 — the design axis does change beta, by a bounded amount.** A dissimilar
  second (DEG-08 with the band published) breaks the correlation *of the control
  law*. The residual correlation is still large: same author, same language,
  same compiler, same libc, same specification, same plant model, and the same
  clamping helpers. An estimate of **beta_law = 0.1 to 0.3** is defensible for
  "same team, same toolchain, different algorithm"; it is an **estimate, not a
  measurement**. For reference, the N-version experiment of Knight and Leveson
  (IEEE TSE 12(1), 1986) measured correlated failures even between *separately
  written* versions, so "0.1 to 0.3" must not be read as "0".

### 5.3 Per-cause table

| Cause | Shared today | beta today | Mitigation | Cost | Status |
|---|---|---|---|---|---|
| Source defect | yes | 1 (H1) | write the law twice, independently; vote | weeks + a third board | **DEG-08 + §4: implemented and measured** |
| Compiler defect | yes | 1 | second compiler (clang, Ada) on the same source | ~1 week + CI matrix | **BLOCKED here** (absent) — recommended |
| libc defect | yes | 1 | musl, or a freestanding build of the core | ~2 days (core has no allocation, no file, no socket) | **BLOCKED here** — recommended |
| Kernel / scheduler | yes | 1 | a second RTOS (Zephyr port of the core) | 2-4 weeks | **BLOCKED here** — recommended |
| Design / specification | yes | 1 | two specifications, cross-checked | months | recommended (out of reach) |
| Build chain / CI | yes | 1 | two pipelines, two repositories | days | recommended |
| Tempo | yes, by design | 1 | a second time source, arbitrated | hardware | by design (spec 1.4) |
| MAC key handling | yes | 1 | one key per node | hours | recommended |
| Hardware (real boards) | no | 0.01-0.1 (H2) | separate power, separate clock | hardware | out of reach here |
| Fault model blind spot | yes | 1 | a second, independent fault model | weeks | recommended |
| Test oracle defect | yes | 1 | a second, independent checker | weeks | recommended |

**Composite estimate.** For a dangerous failure caused by the *software* of the
two computers, the effective factor today is **beta_eff = 1.0**: the two
machines are one software product. The redundancy covers the faults the fault
model declares to be per-machine (row 20 of §2), which is precisely what the
19 659-run campaign injects — and nothing else. With the design-diverse second
of §4 deployed, the factor for the control-law subsystem drops to an estimated
**0.1 - 0.3** (H5) for law defects, while every other row of the table stays
at 1.

## 6. Mitigations: applied here, recommended, impossible here

**Applied in this mission** (`host/diversity/`, no change to `core/`):

| # | Mitigation | What it buys | Cost |
|---|---|---|---|
| M1 | 16 build variants, each with its exact compile line, binary digest and source digest | makes any diversity claim checkable; shows the flag axis is worthless | 1 day |
| M2 | A bit-exact comparator with **controls** (injected defects and a forbidden FP option) | proves the comparator is not blind; gives a reusable oracle for any future compiler | 1 day |
| M3 | Two laws written apart, plugged through CMD-09, with the measured price of each | turns DEG-08 from a claim into a measurement | 1 day |
| M4 | This document | states the common causes instead of hiding them | 1 day |

**Recommended, not applied here** (each needs something this machine or this
mission does not have): a second compiler and a second libc; a second RTOS; two
independently written specifications and laws with a voter; per-node MAC keys;
a pinned compiler version in CI with `-ffast-math` refused; an independent
second invariant checker; a fault model written by somebody else.

**Impossible in this mission**: any hardware diversity. Rows 17-20 of §2 can
only be realised on two real boards (`docs/TWO-BOARDS.md`).

## 7. What real diversity would cost

Ordered by what each removes from the table of §5.3. The beta figures are the
estimates of H5 extended, not measurements.

| Option | What it removes | Residual common causes | Effort | beta (est.) |
|---|---|---|---|---|
| **Second compiler** (clang or Ada on the same source) | compiler defect, code-generation defect | source, design, libc, kernel, author | ~1 week: a CI matrix, fixing the portability warnings, re-proving the invariants on the second build | 1.0 → **0.6 - 0.8** |
| **Second libc** (musl, or freestanding for the core) | libc defect | source, design, compiler, kernel | ~2 days: `core/` allocates nothing, opens no file, uses no clock (rule 2), so it ports as is | 1.0 → **0.5 - 0.7** |
| **Second RTOS** (Zephyr port of the core) | kernel, scheduler, timer, libc | source, design, compiler, author | 2-4 weeks: drivers, the QMX-style time model, the test suite on the new target | 1.0 → **0.4 - 0.6** |
| **A second written-apart implementation** (law + arbiter, other team, other language) | source and design defects of the control chain | specification, plant model, oracle, author | 3-6 months, plus re-proving the invariants and running the two implementations against each other (differential testing over 10 M states) | 1.0 → **0.05 - 0.2** |
| **Three machines and a voter** (M05) | does *not* remove any common cause: it masks a defect that hits one machine | everything above | weeks + a third board | no change (1.0) |
| **Diverse hardware** (two real boards, separate power and clock) | hardware common causes | all software rows | hardware | 1.0 → **0.9 - 0.99** (software still 1) |

The honest conclusion of the table: **no amount of compilation or tooling
diversity reaches a factor below about 0.5**, because rows 1, 2, 11, 12, 15 and
16 of §2 are shared by construction. Only an independently written second
implementation gets close to 0.1, and it costs months.

## 8. What this document does not cover

- **No hardware.** Everything measured here runs on one host: the power, the
  bus, the clock and the kernel of the two "machines" are the host's. Rows
  17-18 of §2 are reasoned about from published ranges, not measured.
- **The fault model is itself a common cause.** `host/simulate/sim_faults.c`
  decides what a fault is; a mode it does not model cannot be found by any
  campaign, whatever the diversity.
- **The invariant checker is shared** by the explorer, the simulator and the
  verifier: a defect in `core/src/invariants.c` invalidates the three verdicts
  at once, and none of the 10 M explored states would have caught it.
- **The corpus is one corpus.** 27 runs, one set of seeds, one set of
  trajectories; a defect that needs another trajectory could still hide.
- **The beta numbers of §5 are estimates.** Only §3 (0 divergence over 14 112 000
  comparisons, 3/3 controls detected) and §4 (the table) are measurements.

## 9. Verdict

The second computer is **not dissimilar today**. It is a hot standby running the
same binary, which protects against the faults the model injects into one
machine and against nothing else. The three levers that exist without new
hardware have been measured:

1. the compilation axis is worth **nothing** (0 divergence, 16 different
   binaries): do not sell it as diversity;
2. the design axis is worth something and has a **measured price** (band to
   publish, certification to accept as "dissimilar", a will that does not
   follow the installed law: 0.2307 of full scale at the handover);
3. everything that would really reduce the factor costs weeks to months and a
   second implementation, not a second compiler flag.

Requirement coverage: CMD-03, CMD-09, DEG-08, DET-09, REL-03, I4, I5, SEC-01,
SEC-04, ROB-03, INJ-11.
