# Relais — WCET on the targets: the method, the numbers measured here, and what stays BLOCKED

Mission M40. This document publishes a **portable measurement method** for the
critical path, the **numbers it produced on the machine that exists here**, their
**dispersion**, the **margin** to every published deadline, and the **exact
protocol** for the two targets that are not connected to this workstation.

Read it with `docs/TIMING.md` (the temporal argument: task model, RTA, the
`2Δ + C + J ≤ T` rule) and `host/timing/procedures-qnx.md` (the four kernel-trace
procedures P1..P4). This file adds what was missing: a measurement harness that
is not tied to the development PC, and a per-target status that never invents a
number.

Requirements exercised: **PERF-01** (1 ms at p99.9, 2 ms worst case, under a
lower-priority load), **PERF-06** (isolation from lower-priority load),
**PERF-07** (the worst cases are explained), **PERF-08** (the period lowered to
the breaking point), **QNX-03** (fixed priority, locked memory), **QNX-04** (the
same measurement on the target), **OBS-03**, **SEC-02**, **CMD-03**, **CMD-09**.

---

## 0. Status of the three targets

| Target | Status | Evidence | Path p50 | p99.9 | max | Regenerate with |
|---|---|---|---|---|---|---|
| Development PC (`baptiste-MS-7D18`, Linux 6.14, i5-11600K, 12 cores) | **MEASURED** | `runs/wcet/linux-x86_64/` | **0.68-1.01 µs** | **1.8-51.6 µs** | **26.6 µs - 64.4 ms** | `make wcet` |
| Raspberry Pi (aarch64 Linux) | **BLOCKED(no Pi bench)** | none — no number published | — | — | — | `python3 host/wcet/target-report.py` |
| QNX 8.0 target | **BLOCKED(no QNX SDP)** | none — no number published | — | — | — | `python3 host/wcet/target-report.py` |

Detection actually run on this machine (`python3 host/wcet/target-report.py`
prints it with every output):

```
ls /dev/mmcblk*                       -> (empty)      [rc=2]
ls /dev/ttyUSB* /dev/ttyACM*          -> (empty)      [rc=2]
command -v qemu-system-aarch64        -> (empty)      [rc=127]
command -v qcc                        -> (empty)      [rc=127]
ls -d ~/qnx800 /opt/qnx*              -> /home/baptiste/qnx800   (a sibling task
                                       started an SDP download at 13:20; it holds
                                       no host/usr/bin/qcc, so nothing compiles)
cat /proc/device-tree/model           -> (empty)      [rc=1]   (not a Pi)
uname -srm                            -> Linux 6.14.0-37-generic x86_64
```

Consequence: **no Pi number and no QNX number appears anywhere in this
repository.** Everything below section 4 is a measurement on an x86-64 Linux PC
shared with other work, and section 5 explains why it cannot be transposed.

---

## 1. What is measured, and what is not

The measured window is one machine's critical path for one tick, in process
(spec 4.2, 4.9):

```
STATUT bytes received ──────────────────────────────► ORDRE bytes emitted
  decode + CRC-32C/SipHash verify    relais-net  prio 62   (rx)
  protocol state machine             relais-core prio 61
     control law, timed separately   relais-ctl  prio 60
  encode + MAC of every frame out    relais-net  prio 62   (tx)
```

* Stamped with `clock_gettime(CLOCK_MONOTONIC)` around exactly that code
  (`host/wcet/probe.c`): five numbers per tick — `path`, `decode`, `node`, `law`,
  `encode` — with `path = decode + node + encode` and `law ⊂ node`.
* The frames are **real**: the `STATUT` comes from the real arbiter
  (`core/src/arbiter.c`) and the real rack model, and the emitted `ORDRE` is fed
  back to it, so the loop is the nominal tick, not a synthetic call sequence.
  The actuator side and the second computer run **outside** the window; the
  measured machine is the holder on every recorded tick, and the probe exits
  non-zero if one tick was not on the holder path (`orders=0`, `non_holder≠0`).
* The law is timed by a `relais_law` wrapper (**CMD-09**: the law may live in
  another process) calling the same `relais_ctl_step` on the same state: nothing
  in `core/` is modified and the computed bits are unchanged (**CMD-03**).
* Integrity is level **C** by default (16-byte key, SipHash-2-4 MAC, **SEC-02**);
  `--no-mac` measures level S (CRC-32C) and the summary says which was used.

**Outside the window, deliberately:** `recvfrom`/`sendto`, the kernel network
stack, `io-sock` on QNX, and any inter-process message passing. Those are the
parts that differ most between Linux and QNX, and they are measured elsewhere:
by M02's socket-level campaign (`host/measure/`, delivered in its own worktree —
**not present in `origin/main` at 98af2ae**, so this harness does not depend on
it) and, on the target, by `host/timing/procedures-qnx.md` P1 (`io-sock`
priority, `TR_SEND`/`TR_REPLY` effective priorities).

This is why the in-process number is the portable one: **the same C, the same
core, the same window** on a PC, on a Pi and on QNX. It isolates the cost of the
protocol code from the cost of the operating system around it — and §4.6 shows
that on this PC the second is 3 to 4 orders of magnitude larger in the tail.

## 2. The harness (what was added, what was reused)

| File | Lines | Role |
|---|---:|---|
| `host/wcet/probe.c` | 439 | the portable C probe: the measured window, the real tick loop, JSONL + JSON output, `--burn` interference generator, best-effort `mlockall`/`SCHED_FIFO` |
| `host/wcet/wcet_stats.c` / `.h` | 96 / 68 | sample sets and descriptors (n, min, mean, stdev, p50, p90, p99, p99.9, max) — the percentile formula of `host/timing/wcet.py`, in C |
| `host/wcet/test_wcet_stats.c` | 132 | 159 unit checks: exact percentiles, edge cases, JSON keys |
| `host/wcet/measure.py` | 359 | the driver: environment, runs, interference, aggregation, dispersion, cross-check, report |
| `host/wcet/isolate.sh` | 228 | machine preparation **without root**; prints `[DONE]`/`[SKIP]`/`[BLOCKED]` per knob, with the root command that would fix it |
| `host/wcet/interference.py` | 238 | the three rate-limited pressures: `cpu`, `journal`, `net` |
| `host/wcet/rta.py` | 306 | margins to the deadlines, from **measured** stage costs |
| `host/wcet/target-report.py` | 270 | per-target status: MEASURED or BLOCKED(reason) with the detection output and the protocol |
| `host/wcet/wcet_lib.py` | 179 | one place for the root, the percentile import, the budgets, the table format |
| `host/wcet/test_wcet.py` | 273 | 22 Python unit tests of the harness |
| `host/wcet/compare.py` | 144 | dispersion between two successive campaigns |
| `Makefile` | +65 | **append-only** block at the end (the first 181 lines are byte-identical to `origin/main`, md5 `5a75d575e3b6`): `wcet`, `wcet-probe`, `wcet-test`, `wcet-isolate`, `wcet-portable`, `wcet-report` |

**Reused, not duplicated** (rule 3):

* `host/timing/wcet.py` is imported by path (`wcet_lib.load_timing_wcet`): the
  percentiles, the descriptors and the Gumbel-POT extrapolation published here
  are computed by that code, the same one `host/timing/test_timing.py` pins down.
  The probe's JSONL is in **its** format, so the two tools read the same file:

  ```
  $ python3 host/timing/wcet.py runs/wcet/linux-x86_64/samples-none-run1.jsonl
  latency samples: n=20000  min=0.6  mean=0.7  stdev=0.2 us
    p50=0.7  p90=0.7  p99=0.9  p99.9=2.2  p99.99=4.5  max=28.0 us
  empirical bound: observed max is an upper bound of the true 99.99% quantile at
                   confidence 86.468% (20000 samples)
  to affirm that same bound at 95% confidence: 29956 samples
  extrapolated Gumbel-POT bound at survival 1.0e-06: 5.2 us [95% CI 4.6 .. 5.8]
  ```

* `host/timing/rta.py` is imported by path (`wcet_lib.load_timing_rta`): Audsley's
  fixed point, the stage model, the `TENABLE` verdict and its renderer are that
  module's. `host/wcet/rta.py` only substitutes **measured** costs for the
  150/250/100 µs example of `docs/TIMING.md` §3, folds in the measured
  interference, and publishes the margins.
* `host/timing/procedures-qnx.md` P1..P4 stays the reference for everything the
  kernel trace must show on the target; section 6 below points at it instead of
  restating it.

**Portability proof available here** (the strongest one without an SDP or a Pi):
`make wcet-portable` compiles the three C files with `-std=c11 -pedantic -Werror
-Wshadow -Wconversion -D_POSIX_C_SOURCE=200809L`. It passes, and the only
headers used are `sched.h stdio.h stdlib.h string.h sys/mman.h sys/utsname.h
time.h unistd.h math.h` — ISO C11 + POSIX 200809, no Linux header, no QNX
header, no GNU extension. That proves the **source** is portable; it does not
prove a timing number on the target, and it is not presented as one.

## 3. Reproduce it

```sh
cd <repo>
make -j4 all && make test && make explore && make wcet     # 4 suites, ~10 s
# a longer campaign, pinned, with the raw samples kept:
make wcet WCET_SAMPLES=50000 WCET_RUNS=3 WCET_CPU=5
# two successive executions, then their dispersion:
python3 host/wcet/compare.py runs/wcet/A/measure.json runs/wcet/B/measure.json
# per-target status, with the detection evidence:
python3 host/wcet/target-report.py --out runs/wcet/TARGETS.md
```

Every artefact lands in `runs/wcet/<host>/` (git-ignored): `env.json` and
`isolate.log` (the machine as prepared), `samples-<mode>-run<k>.jsonl` (raw
samples, in `host/timing/wcet.py`'s format), `summary-<mode>-run<k>.json` (the
probe's C summary), `measure.json` (the campaign), `report.txt`, `rta.json`,
`TARGETS.md`.

## 4. What was measured here

### 4.1 The machine as prepared (`host/wcet/isolate.sh`, no root)

```
[DONE   ] affinity        taskset -c N (own process, no root needed)
[DONE   ] memory lock     RLIMIT_MEMLOCK=4095036 KiB -> the probe's mlockall() succeeds
                          (probe summary: "memory_locked": true)
[DONE   ] cache           L2=512K; the probe warms every path for 500 ticks before recording
[BLOCKED] rt priority     SCHED_FIFO refused: RLIMIT_RTPRIO=0 -> "sched_fifo": false.
                          ROOT: ulimit -r 99 (or rtprio in /etc/security/limits.conf)
[BLOCKED] governor        powersave, sysfs read-only; 4.30-4.60 GHz observed.
                          ROOT: cpupower frequency-set -g performance
[BLOCKED] turbo/boost     no_turbo=0.  ROOT: echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
[BLOCKED] isolcpus        absent from /proc/cmdline.  ROOT + reboot: isolcpus=nohz,domain,N quiet
[BLOCKED] huge pages      always [madvise] never.  ROOT: echo never > /sys/kernel/mm/transparent_hugepage/enabled
```

Ambient: `loadavg` between 9.07 and 34.41 during the campaigns, 5-22 running
processes at the isolation probe: **this workstation is shared with dozens of
concurrent jobs**, and the load is recorded with every run
(`"loadavg_before"` in `measure.json`).

### 4.2 The default campaign — `make wcet`

20 000 samples × 2 runs × 4 interference modes = **160 000 measured ticks**, MAC
level C, `mlockall` on, no pinning, ~2 s of wall clock.

Path (STATUT received → ORDRE emitted), microseconds:

| mode | n | p50 | p90 | p99 | p99.9 | max | stdev |
|---|---:|---:|---:|---:|---:|---:|---:|
| `none` | 40 000 | **0.677** | 0.712 | **0.938** | **2.234** | 27.955 | 0.223 |
| `cpu` (12 busy processes, nice 19) | 40 000 | 1.057 | 1.091 | 1.137 | 3.689 | **3 009.205** | 22.356 |
| `journal` (5 000 JSON lines/s) | 40 000 | 0.679 | 0.714 | 0.983 | 1.276 | 11.646 | 0.122 |
| `net` (2 000 UDP datagrams/s) | 40 000 | 0.683 | 0.719 | 1.066 | 7.920 | 63.489 | 0.652 |

The five segments, mode `none` (the protocol code alone):

| segment | p50 | p90 | p99 | p99.9 | max | stdev |
|---|---:|---:|---:|---:|---:|---:|
| `decode` (frame + CRC-32C/SipHash verify) | 0.096 | 0.104 | 0.139 | 0.192 | 27.299 | 0.194 |
| `node` (protocol state machine) | 0.327 | 0.330 | 0.428 | 0.537 | 15.113 | 0.113 |
| `law` (`relais_ctl_step`) | 0.023 | 0.024 | 0.028 | 0.035 | 1.749 | 0.012 |
| `encode` (frame + MAC out) | 0.255 | 0.285 | 0.357 | 0.454 | 10.983 | 0.089 |

Extrapolated bound (Gumbel-POT, survival 1e-6, `host/timing/wcet.py`): `none`
**5.190 µs**, `journal` 3.951, `net` 22.046, `cpu` 211.716.

The probe's own C summary and the recomputation by `host/timing/wcet.py` agree
to **0.002 %** (the harness refuses to publish a run where they differ by more
than 1 %: `status = MISMATCH`).

### 4.3 A longer, pinned campaign — `make wcet WCET_SAMPLES=50000 WCET_RUNS=3 WCET_CPU=5`

50 000 samples × 3 runs × 4 modes = **600 000 measured ticks**, pinned to CPU 5.

| mode | n | p50 | p90 | p99 | p99.9 | max | stdev |
|---|---:|---:|---:|---:|---:|---:|---:|
| `none` | 150 000 | **0.679** | 0.713 | 1.037 | **1.573** | 17.774 | 0.132 |
| `cpu` | 150 000 | 1.053 | 1.089 | 1.334 | 5.733 | 931.986 | 4.177 |
| `journal` | 150 000 | 0.682 | 0.777 | 1.470 | 4.621 | 512.122 | 2.297 |
| `net` | 150 000 | 0.700 | 1.164 | 1.229 | 3.989 | 42.852 | 0.313 |

### 4.4 Dispersion

*Run to run, inside one campaign* (path, two runs of the default campaign):

| mode | p50 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|
| `none` | 0.4 % | 4.5 % | 61.1 % | 57.3 % |
| `cpu` | 0.2 % | 0.6 % | 75.0 % | 195.9 % |
| `journal` | 0.1 % | 1.6 % | 4.6 % | 91.4 % |
| `net` | 0.6 % | 18.9 % | 138.8 % | 167.4 % |

*Campaign to campaign* (`host/wcet/compare.py`, two successive executions of the
whole campaign, same command):

| statistic | unpinned (A vs B) | pinned (C vs D) |
|---|---:|---:|
| p50 | 22.1 % | **8.5 %** (0.3 % for mode `none`) |
| p90 | 42.5 % | 45.4 % |
| p99 | 20.6 % | 16.3 % |
| p99.9 | 141.0 % | 96.1 % |
| max | 191.6 % | 164.8 % |

**Reading:** the body (the cost of the code) repeats within a few percent once
the measurement is pinned and the machine is not saturated; the tail does not
repeat at all, because on a general-purpose kernel the tail is the cost of the
*host*, not of the protocol. Pinning alone moved the p99.9 of the `none` mode
from 5.803/8.661 µs (unpinned A/B) to 1.573/4.480 µs (pinned C/D).

### 4.5 What the three interferences cost

| mode | p50 | Δ p50 vs `none` | p99.9 | max |
|---|---:|---:|---:|---:|
| `none` | 0.677 | — | 2.234 | 27.955 |
| `cpu` (12 busy processes) | 1.057 | **+56 %** | 3.689 | 3 009.205 |
| `journal` (5 000 lines/s) | 0.679 | +0.3 % | 1.276 | 11.646 |
| `net` (2 000 datagrams/s) | 0.683 | +0.9 % | 7.920 | 63.489 |

At the stage level (p99.9, worst mode over the four): `relais-net`
(decode+encode) **1.103 µs**, `relais-core` (node−law) **4.029 µs**,
`relais-ctl` (law) **0.113 µs**. Their sum is the measured path — the split is
exact, not an attribution of convenience.

### 4.6 Margins to the published deadlines (`host/wcet/rta.py`, T = 5 000 µs, Δ = J = 1 000 µs)

| budget | default campaign | pinned campaign C | pinned campaign D |
|---|---:|---:|---:|
| PERF-01 p99.9 ≤ 1 000 µs | **+992.080** TENABLE | +994.267 TENABLE | +995.520 TENABLE |
| PERF-01 worst (measured max) ≤ 2 000 µs | **−1 009.205 EXCEEDED** | +1 068.014 TENABLE | −1 007.540 EXCEEDED |
| PERF-01 worst (EVT bound 1e-6) ≤ 2 000 µs | +1 788.284 TENABLE | +1 948.965 TENABLE | +1 772.281 TENABLE |
| RTA chain response ≤ T | +4 983.000 TENABLE | +4 989.000 TENABLE | +4 986.000 TENABLE |
| cycle rule 2Δ + C + J ≤ T | +1 983.000 TENABLE | +1 989.000 TENABLE | +1 986.000 TENABLE |
| PERF-08 breaking point (smallest tenable T) | 18 µs | 12 µs | 15 µs |

The RTA chain is **17 µs** (default campaign) against the 5 ms period: the
machine-side computation is 0.34 % of the tick. The margins above are the
numbers the spec asks for; the two `EXCEEDED` lines are the *worst sample*, not
the distribution.

### 4.7 PERF-07 — the worst cases, explained

In the 160 000 samples of the default campaign there is **exactly one sample
above 2 ms**: 3 009.2 µs, in `cpu-run1` (four samples above 100 µs in that run;
the seven other runs peak at 63.5 µs). Its decomposition is published by the
probe itself: `node` = 3 008.4 µs of the 3 009.2 µs, `encode` = 156.2 µs,
`decode` = 5.5 µs, `law` = 2.7 µs — **the whole overshoot is inside one
segment**, the process was preempted inside `relais_node_on_status`.

Independent campaigns show the same signature: 3 006.6 and 3 007.5 µs inside
`node` (campaign D), 34 681.6 of 34 682.2 µs inside `encode` (campaign B,
preempted while computing the MAC), 757.951 of 760.243 µs inside `decode`.
The *interpretation* (a scheduler quantum, a throttling window, a host with 50
jobs) is a **hypothesis**, not a measurement, and it is falsifiable:
`perf sched timehist` on Linux (blocked here, `perf_event_paranoid=4`; ROOT:
`sysctl -w kernel.perf_event_paranoid=1`) or the kernel trace of
`host/timing/procedures-qnx.md` P1 on the target.

### 4.8 What the numbers say

* The **microsecond** promise of the spec holds by three orders of magnitude
  for the body of the distribution: p50 ≈ 0.68 µs, p99 ≈ 1.0 µs, i.e. 0.09 % of
  the 1 ms budget at p99.9. The cost of the protocol code on the critical path
  is **≈ 0.7 µs**, of which 0.10 µs is the MAC-verified decode and 0.26 µs the
  MAC'd encode.
* The **tail** is set by the host kernel: 1 sample in 160 000 exceeded 2 ms,
  always by a preemption of ≈3 ms, on a machine shared with dozens of other
  jobs, with the governor in `powersave`, turbo on, no CPU isolation and no
  real-time priority.
* These are therefore a valid statement about the **cost of the code** and an
  invalid statement about the **target**.

### 4.9 The verification run, from a clean tree — twice

`rm -rf build runs && make -j4 all && make test && make explore && make wcet`,
executed twice on the same machine (loadavg 26.5 for run A, 28-33 for run B).
Path segment, microseconds:

| mode | n | p50 A | p99.9 A | max A | p50 B | p99.9 B | max B |
|---|---:|---:|---:|---:|---:|---:|---:|
| `none` | 40 000 | 0.681 | 1.829 | 26.557 | 1.009 | 51.566 | 32 164.868 |
| `cpu` | 40 000 | 1.041 | 4.551 | 760.243 | 1.038 | 11.416 | 26 019.755 |
| `journal` | 40 000 | 0.689 | 1.601 | 14.384 | 1.046 | 18.984 | 64 379.388 |
| `net` | 40 000 | 0.680 | 1.593 | 14.091 | 1.015 | 35.876 | 21 028.120 |

Run A: every margin TENABLE (p99.9 +995.449, worst +1 239.757, EVT +1 934.037,
chain +4 991, cycle +1 991; breaking point 9 µs). Run B: p99.9 +948.434
TENABLE, worst −62 379.388 and EVT −4 220.070 EXCEEDED, chain +4 937, cycle
+1 937, breaking point 69 µs.
**Between two identical executions the body moved by 48 %** (p50 0.681 →
1.009 µs, the machine being busier) **and the worst sample by three orders of
magnitude** (26.6 µs → 64.4 ms); the p99.9 budget was met in both. On a shared
general-purpose workstation the tail is a property of the neighbours, not of the
protocol: this is the measurement that must not be transposed, and the reason the
target must be measured with its own priorities and its own isolation.

## 5. Why these numbers are not transposable as they are

1. **Scheduler.** Linux 6.14, `CONFIG_PREEMPT_DYNAMIC=y`, no `isolcpus`, no
   SCHED_FIFO (`RLIMIT_RTPRIO=0`). A `nice 19` load can therefore preempt the
   measuring process, and it does (the 3 ms samples). On QNX 8.0 the chain runs
   at fixed priority 62/61/60 on a partitioned CPU (QNX-03), where a
   lower-priority load **cannot** preempt it. The same interference therefore
   produces a fundamentally different tail: PERF-01's "under a lower-priority
   load" is a *much* stronger requirement on QNX than on this PC.
2. **Clock.** Governor `powersave`, turbo enabled, 4.30-4.60 GHz observed: the
   cost of the code moves with the frequency, so a 10 % frequency change is a
   10 % change of every number here (measured: 48 % between the two runs of
   §4.9).
3. **Isolation and neighbours.** No CPU isolation, THP `madvise`, a shared L2/L3
   with dozens of other jobs: the `cpu` mode inflates the median by 56 %, which
   is cache and memory-bandwidth contention, not protocol work.
4. **Architecture.** x86-64 at 3.9 GHz here; the Pi is an aarch64 SoC at
   ~1.5-2.4 GHz, the QNX target runs on its own board. The same C is 2-3×
   slower there. The absolute microseconds do not travel; the **method** does.
5. **Scope of the window.** The socket syscalls and the kernel stack are
   *excluded* here. They are the dominant part of the real end-to-end latency
   and are measured elsewhere: M02's socket-level campaign
   (`host/measure/`, not present in `origin/main` at 98af2ae) and, on the
   target, `host/timing/procedures-qnx.md` P1. What is published here is the
   machine-side computation **C** of the cycle rule `2Δ + C + J ≤ T`, not the
   end-to-end latency.
6. **Statistical floor.** Affirming p99.99 at 95 % confidence needs 29 956
   samples (`host/timing/wcet.py`'s own formula), the EVT fit ≥ 100 samples and
   ≥ 20 above the threshold. Both campaigns here satisfy that; a target
   campaign must use the same count before a p99.9 is published.
7. **Conclusion.** These numbers are a **lower bound** on the target's
   computation cost (same code, same algorithm) and **no information** about the
   target's tail. Publishing them as "the WCET of Relais" would be wrong; the
   deliverable for the target is the **harness**, and the protocol below.

## 6. The protocol for the real target

### 6.1 Prepare the machine (no root needed for most of it)

```sh
sh host/wcet/isolate.sh --cpu 2 --json runs/wcet/env-<target>.json
```

The script prints one line per knob and states what it could not do. The
root-only items (governor, turbo, `isolcpus`, huge pages, `SCHED_FIFO`) are the
operator's decision, and the exact command is printed next to each `BLOCKED`
line. Nothing is bypassed.

### 6.2 Affinity, isolation, priority

* Pin: `taskset -c 2` (Linux) or `on -C 0` (QNX) for the whole campaign.
* QNX: run the chain at the priorities of `docs/TIMING.md` §1 — `relais-net` 62,
  `relais-core` 61, `relais-ctl` 60 — with `mlockall` (QNX-03), and raise
  `io-sock` to ≥ 62 (`on -p 62 io-sock`), which is procedure P1.
* QNX: declare the sporadic budgets `sched_ctl relais-net SPORADIC 62 150 5000`
  (and the two others) — procedure P3.
* Linux: `SCHED_FIFO` for the measuring process if the operator grants
  `RLIMIT_RTPRIO`; without it, say so in the report (this PC says
  `"sched_fifo": false`).

### 6.3 Number of samples and stopping criterion

* **≥ 29 956 samples per mode** to affirm p99.99 at 95 % confidence, and
  **≥ 4 603** for p99.9 at 99 % (`host/timing/wcet.py`); the EVT fit needs ≥ 20
  samples above the p99 threshold (`wcet.py`'s `MIN_EXCEED`). Use 3 × 50 000 =
  150 000 per mode.
* **Stop when** (a) the run-to-run dispersion of p50 is < 5 % and of p99 < 10 %,
  (b) **no** sample exceeds the 2 ms budget in any of the three runs, and
  (c) the four modes agree on the body within 10 %. If (b) fails, the
  isolation is not sufficient: fix the priorities and the partitioning **before**
  publishing a number, and do not publish the tail. The harness prints
  `VERDICT: measured, but N published budget(s) EXCEEDED` in that case and
  `host/wcet/rta.py --strict` turns it into a non-zero exit for a CI gate.

### 6.4 The exact commands

The same text is printed by `python3 host/wcet/target-report.py`, per target.

**Raspberry Pi (64-bit OS):**

```sh
make -j4 all wcet-probe
sh host/wcet/isolate.sh --cpu 2 --json runs/wcet/env-pi.json
# ROOT, operator's call: fix the clock before believing a tail
#   sudo cpupower frequency-set -g performance
#   sudo raspi-config nonint do governor performance
python3 host/wcet/measure.py --samples 50000 --runs 3 --cpu 2 --lock \
    --interference none,cpu,journal,net
python3 host/wcet/rta.py --sweep
scp -r runs/wcet/linux-aarch64 <pc>:<repo>/runs/wcet/
```

**QNX 8.0 target:**

```sh
source ~/qnx800/qnxsdp-env.sh
cd target && qcc -V gcc_ntox86_64 -std=c11 -O2 -Wall -Wextra -Werror \
    -D_POSIX_C_SOURCE=200809L -I../core/include -I../host/wcet \
    ../host/wcet/probe.c ../host/wcet/wcet_stats.c ../core/src/*.c \
    -lm -o relais-wcet-probe && cd ..
# on the target (or in the QEMU guest built from image/), pinned, at prio 61:
on -C 0 ./target/relais-wcet-probe --samples 50000 --warmup 500 \
    --lock --prio 61 --host qnx800-x86_64 \
    --out /tmp/samples-qnx.jsonl --summary /tmp/summary-qnx.json
# the probe alone is enough: it prints the percentiles and writes the JSONL that
# host/timing/wcet.py analyses (no python needed on the target):
python3 host/timing/wcet.py /tmp/samples-qnx.jsonl
# the kernel-side evidence: host/timing/procedures-qnx.md P1..P4, then
#   traceprinter > trace.txt
# copy runs/wcet/qnx800-x86_64/ back and re-run the target report.
```

### 6.5 Acceptance criteria to publish a target number

1. an evidence file `runs/wcet/<target-host>/measure.json` exists;
2. the isolation table has **no** `BLOCKED` on the knobs that matter (governor,
   real-time priority, CPU isolation) — or the blocked ones are named in the
   published number;
3. the C summary and `host/timing/wcet.py` agree within 1 % (the harness refuses
   otherwise);
4. the margins are published, including the ones that are negative;
5. the three worst samples are explained by the kernel trace (PERF-07).

## 7. Register of what is BLOCKED

| Item | Detection (run here) | Result | Unblocked by |
|---|---|---|---|
| Raspberry Pi bench | `ls /dev/mmcblk*`; `ls /dev/ttyUSB* /dev/ttyACM*`; `command -v qemu-system-aarch64`; `cat /proc/device-tree/model` | `(empty)`, `(empty)`, `(empty)`, `(empty)` | plugging a Pi (or booting a Pi image) and running §6.4 |
| QNX SDP / target | `command -v qcc`; `ls ~/qnx800/host/usr/bin/qcc` | `(empty)`, `(empty)` (the `~/qnx800` tree exists but holds no compiler) | installing SDP 8.0 and sourcing `qnxsdp-env.sh`, then §6.4 |
| SCHED_FIFO on the PC | `ulimit -r` | `0` | ROOT: `ulimit -r 99` or `rtprio` in `limits.conf` |
| Fixed frequency | `cat .../scaling_governor` | `powersave` (read-only) | ROOT: `cpupower frequency-set -g performance` |
| Turbo off | `cat .../intel_pstate/no_turbo` | `0` | ROOT: `echo 1 > .../no_turbo` |
| CPU isolation | `cat /proc/cmdline` | no `isolcpus` | ROOT + reboot: `isolcpus=nohz,domain,N quiet` |
| Huge pages off | `cat .../transparent_hugepage/enabled` | `always [madvise] never` | ROOT: `echo never > .../enabled` |
| Scheduler trace | `cat /proc/sys/kernel/perf_event_paranoid` | `4` | ROOT: `sysctl -w kernel.perf_event_paranoid=1` |
| End-to-end (socket) latency | `ls host/measure` | absent in `origin/main` at 98af2ae | M02's layer merged; the probe deliberately measures the in-process window only |

## 8. Files added and verification

Added by this mission, all under `host/wcet/` and `docs/WCET-TARGETS.md`, plus an
**append-only** block at the end of the `Makefile` (65 lines; the first 181 lines are byte-identical to `origin/main`: md5 `5a75d575e3b663fd90d5be4e89fc5c58` — `wcet`,
`wcet-probe`, `wcet-test`, `wcet-isolate`, `wcet-portable`, `wcet-report`).
`core/`, `host/timing/`, `host/measure/` and the CI workflows were **not**
modified.

```sh
cd /tmp/rl-w40 && make -j4 all && make test && make explore && make wcet
```

* `make -j4 all` — rc 0, **0 warning** with `-Wall -Wextra -Werror -Wshadow
  -Wconversion`; it builds `build/relais-wcet-probe` and `build/test_wcet_stats`.
* `make test` — rc 0, **13 suites, 394 434 checks** green, including the new
  `test_wcet_stats` (159 checks).
* `make explore` — `EXPLORE-OK 100198 states, deepest tick 12, 0 violation`.
* `make wcet` — rc 0: `WCET-PORTABLE-OK` (C11 `-pedantic`, POSIX 200809 only),
  159 C checks + 22 Python tests green, the campaign, the RTA, the target report.
* `make size` — one pre-existing violation, **not** mine and **not** fixed
  (rule 10): `core/src/arbiter.c` is 521 lines. Every file here is < 500 lines,
  every function < 50 lines.
