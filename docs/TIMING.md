# Relais — the temporal argument (WCET and response-time analysis)

This document turns the measured delay of M02 into an *argument that the delay
is bounded*, which is what distinguishes a demonstrator from a concept that can
be defended in ISO 26262 terms. It answers trials **E2** and **E5** of the
specification. It is written in English because the repository and the jury are
English-speaking (rule 4).

Scope of proof. Everything here is a *calculation* of the upper bound, at fixed
priority, from per-stage estimates. It is runnable and testable without
hardware (`python3 host/timing/rta.py`, `python3 host/timing/wcet.py`). It does
**not** replace the on-target measurement: the numbers labelled *to measure on
target* are hypotheses until the QNX procedures in
`host/timing/procedures-qnx.md` have been executed.

---

## 1. Task model

One machine's critical path is a chain of three QNX processes, released once per
tick and carrying the mandate end to end (spec 4.9):

| Stage | Process | Priority | What it does | Requirement |
|---|---|---|---|---|
| receive + validate + emit | `relais-net` | **62** | owns the sockets (SEC-05); decodes and authenticates the `STATUT` frame, hands it to the core; puts the order back on the wire | SEC-05, DET-04, CMD-04 |
| protocol state machine | `relais-core` | **61** | runs `relais_node_on_status` on the `STATUT`, asks the law, produces the order and the replica | QNX-01, MAN-*, REP-*, OBS-02 |
| control law | `relais-ctl` | **60** | pure function of state + measurements; no socket, no clock, no device | CMD-09, CMD-03 |

The chain obeys a **fixed-priority** schedule (QNX-03): `relais-net` is strictly
higher priority than `relais-core`, which is strictly higher priority than
`relais-ctl`. Each stage is a separate process so a crash is *known*, not
guessed: when `relais-net` dies, `relais-core` sees its connection close and
cedes the mandate within the tick (DET-05, QNX-05).

The period T and the patience p come from `core/relais/params.h`; the default
is `T = 5 ms`, `p = 1`, `N = 8` (spec 6.3).

## 2. Assumptions and how they are verified

The argument rests on six assumptions. Each is either measured, to be measured
on the QNX target, or deliberately assumed; the final section tabulates them.

1. **Fixed priority, end to end.** `relais-net`, `relais-core` and `relais-ctl`
   run at 62/61/60 and never see a page fault on the critical path. *Assumed;
   the priority is set by `setprio(0, prio)` and the pages are locked by
   `mlockall(MCL_NOWAIT)` in `rl_realtime()` (QNX-03). *
2. **Priority inheritance of the message passing.** A QNX server answering a
   client through `MsgSend`/`MsgReply` runs at the client's priority, so
   `relais-ctl` (60) serving `relais-core` (61) does not wait behind lower
   urgency work. *Assumed for the analysis; the RTA is deliberately computed
   **without** inheritance and is therefore conservative.*
3. **The network stack (`io-sock`) treats the protocol frames at a priority not
   lower than the critical path.** *To measure on target (trial E5).*
4. **One-way frame delay Δ ≤ 1 ms on a healthy link.** *To measure (trial E2,
   `--probe E1`/`E2` of the bench).*
5. **Machine-side worst-case computation C ≤ 0.5 ms.** *Nominal value from spec
   3.4; refined here by the RTA to 1.05 ms on the example configuration.*
6. **Release jitter J ≤ 1 ms.** *Upper bound of the polling slack; refined by
   M02's latency distribution.*

## 3. Response-time analysis (RTA)

The RTA tool `host/timing/rta.py` computes, for each stage `i`, the response
time `R_i` — the worst-case completion measured from the chain release — with
Audsley's fixed-point iteration:

```
R_i = C_i + Σ_{j in hp(i)} ⌈ R_i / T ⌉ · C_j        (until fixed point)
```

All stages share the same period T because they are released together, one per
tick. The **chain response time** is `Σ R_i`; it deliberately ignores priority
inheritance, so it is an upper bound. The verdict *tenable* requires both that
every `R_i ≤ T` and that the chain response `≤ T`.

Example configuration (the built-in `rta.py` example), with `C_net = 150 µs`,
`C_core = 250 µs`, `C_ctl = 100 µs`, `T = 5000 µs`:

```
fixed-priority RTA, T=5000 us
  relais-net   prio 62  C= 150 us  R=   150 us  margin=  4850 us
  relais-core  prio 61  C= 250 us  R=   400 us  margin=  4600 us
  relais-ctl   prio 60  C= 100 us  R=   500 us  margin=  4500 us
  chain response = 1050 us  margin = 3950 us
verdict: TENABLE
```

Step by step: `R_net = 150`. `R_core = 250 + ⌈R/T⌉·150 = 400` (the only
higher-priority stage is `relais-net`). `R_ctl = 100 + ⌈R/T⌉·150 + ⌈R/T⌉·250
= 500`. The chain response is `150 + 400 + 500 = 1050 µs`, comfortably below
`T = 5 ms`, with a margin of `3.95 ms`.

This 1.05 ms is the *machine-side* worst-case: it is the quantity that PERF-01
budgets at "1 ms at the 99.9th percentile, 2 ms worst case". The nominal
`C = 0.5 ms` of spec 3.4 is the idealised computation without scheduling
interference; the RTA refines it to 1.05 ms when interference is included, and
both fall under the 2 ms worst-case budget. Both must be confirmed on target.

## 4. The cycle rule 2Δ + C + J ≤ T

Spec 4.2 states the round trip, the computation and the return must fit in one
period: **2Δ + C + J ≤ T**, where Δ is the one-way frame delay, C the
computation time, and J the release jitter.

Numeric check on the assumptions of spec 3.4 (Δ = 1 ms, C = 0.5 ms, J = 1 ms,
T = 5 ms):

```
2·Δ + C + J = 2·1.0 + 0.5 + 1.0 = 3.5 ms ≤ 5 ms      margin = 1.5 ms
```

Numeric check with the RTA-refined computation `C = 1.05 ms` (the chain
response from section 3):

```
2·Δ + C + J = 2·1.0 + 1.05 + 1.0 = 4.05 ms ≤ 5 ms    margin = 0.95 ms
```

Both are verified numerically by this repository: the first is the spec's own
arithmetic, the second uses `host/timing/rta.py` for C. The rule holds in both
cases, which is the argument that a fresh order — computed from the `STATUT` of
the previous tick — reaches the actuator before the next tick (PERF-03,
PERF-05). The honest counter-part is that the margins shrink as C approaches
2 ms and as Δ and J approach 1.5 ms combined; section 6 lists exactly which
terms are not yet measured.

## 5. Consequence for p and T

The choice of p and T is governed by the same bound. The actuator arbitrates on
its own proof: it removes a mandate after `p+1` ticks without a valid order
(ACT-08, MAN-11). If `2Δ + C + J ≤ T` holds, a machine that is alive always
answers within the tick, so `p` is only ever exhausted by a genuine fault or
link loss, never by scheduler latency.

- **T = 5 ms** is tenable only if the target measures Δ, C, J inside the budgets
  above. If `io-sock`'s priority or the KVM guest round trip (E2) exceed the
  assumption, the period must be lengthened to 10 or 20 ms — which is exactly
  the escape hatch the spec already names (SPEC-ANNEX E2/E5). PERF-08 asks to
  lower T to the breaking point; the RTA gives the tool to find it: the chain
  response of 1.05 ms is the floor below which T cannot go without margin.
- **p = 1** means a single tick without a fresh order already begins the
  mandate removal. Any scheduler margin consumed (Δ, J) shortens the margin
  available for genuine link loss; hence the analysis must be *measured*, not
  assumed, before p = 1 is claimed at 5 ms.

## 6. What cannot be done here

Four parts of the argument require the QNX SDP and the target; they are
**not** silently claimed. Each has an exact measurement procedure in
`host/timing/procedures-qnx.md`, ready to run:

1. Priority inheritance inside `io-sock` — the effective priority at which the
   network stack handles the protocol frames (assumption 3).
2. The effect of CPU partitioning on the three processes — whether the guest's
   vCPUs keep the chain isolated from lower-priority load (PERF-06).
3. `SCHED_SPORADIC` behaviour of the chain under burst release.
4. Residual page faults after `mlockall` — that the critical path really never
   faults (assumption 1).

## 7. Assumption → verification → status

| Assumption | How it is verified | Status |
|---|---|---|
| Fixed priority 62/61/60, no page fault | `setprio` + `mlockall(MCL_NOWAIT)` in `rl_realtime()`; `target/` syntax-checked in CI | assumed (QNX-03) |
| Priority inheritance of `MsgSend`/`MsgReply` | kernel trace, `TR_SEND`/`TR_REPLY` with effective priority | to measure on target (E5) |
| `io-sock` priority ≥ critical path | kernel trace of the receive path | to measure on target (E5) |
| Δ ≤ 1 ms one-way frame delay | bench `--probe E2`, UDP round trip under KVM | to measure on target (E2) |
| C ≤ 0.5 ms (nominal) / 1.05 ms (RTA) | `host/timing/rta.py` example; per-stage WCET from `trace` | partially measured (PC), to measure on target |
| J ≤ 1 ms release jitter | M02 latency distribution (`wcet.py`: p99.9, max, EVT bound) | measured on PC, to measure on target |
| 2Δ + C + J ≤ T | arithmetic in section 4, re-checked by the tools | verified numerically (this doc) |
| CPU partitioning isolates the chain | `SCHED_SPORADIC` + partition trace | to measure on target |
| No residual page fault after `mlockall` | `trace` fault count on the critical path | to measure on target |