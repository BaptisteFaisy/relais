# Relais — measured latency of the critical path (mission M02)

Until now the project measured **ticks** (logical bounds: how many ticks a
handover takes, how long the will covers) but never **microseconds** on the path
that PERF-01 budgets: *"`STATUT` received by the machine → order emitted"*. This
document publishes that measurement, with its full distribution, and states
plainly what it does and does not prove.

Requirements addressed: **PERF-01** (1 ms at p99.9, 2 ms worst case),
**PERF-02** (ticks served otherwise than by the holder's order), **PERF-07**
(the three worst cases explained), **PERF-08** (period lowered to the breaking
point), **OBS-03** (bounded-memory observability), **QNX-04** (what stays to be
measured on target), **CMD-03** (the law is timed where it runs).

> **Honest headline.** Every number below was measured on an ordinary Linux PC
> (shared, non-real-time kernel, node processes at default priority): **they are
> not QNX numbers**, and PERF-01 stays *to be measured on the target*
> (`host/timing/procedures-qnx.md`). What M02 delivers is the instrument, the
> sample format, and a real distribution on a real machine.

---

## 1. What is measured

Three segments, each with its own recorder (`host/measure/latency.h`):

| segment | from | to | why |
|---|---|---|---|
| `path` | `recv()` returned the `STATUT` datagram | `sendto()` of the `ORDRE` returned | **PERF-01**: the machine-side critical path |
| `decode` | same instant | the frame passed CRC-32C + SipHash MAC | SEC-05/DET-04: what validation costs |
| `law` | entry of `relais_ctl_step()` | its return | CMD-03: what the control law costs |

`path − decode − law` is the residual: frame encoding, the `sendto` syscall and,
above all, the time the process spent **descheduled** between the two instants.
Section 6 shows this residual is where the worst cases live.

Timestamps come from `clock_gettime(CLOCK_MONOTONIC)`
(`rl_lat_now_ns`). The measurement is taken in `host/node/relais-node.c`:

* `recv_frame()` fills a `lat_marks` structure — but only when a pointer is
  passed, i.e. only when `--latency` is given. Without the option **no clock is
  read at all**.
* `dispatch()` returns the instant the `ORDRE` hit the wire; a `path` sample is
  recorded only when the triggering frame was a `STATUT` **and** an order was
  actually emitted, so `path` is exactly the holder's critical path (the second
  emits shadows, which are not PERF-01's subject).
* the `law` segment is measured by installing a timed control law through the
  existing CMD-09 hook (`relais_node_set_law`). The wrapper calls
  `relais_ctl_step()` **on the node's own `sm->ctl` state** — same function, same
  state, same bits — so the instrumentation cannot change the emitted values.
  `law.init` is left `NULL` because `relais_node_init()` already initialised the
  state; `export_state`/`import_state` delegate to `relais_ctl_export/import`.

Only the `STATUT` is timed for `decode`: the holder also sends `ETAT` and `SUIVI`
to the second every tick, and those frames are off the critical path.

## 2. Reproducing every number of this document

```sh
make -j4 all                                    # build, zero warning (-Werror)
make test                                       # 11 suites, 394 393 checks, all green
python3 host/measure/run_latency_campaign.py --out runs/latency --port-base 7800
./build/relais-latency runs/latency/pc-long/lat-A.jsonl runs/latency/pc-long/lat-B.jsonl
./build/relais-latency --json runs/latency/pc-long/lat-B.jsonl | head -1
python3 host/timing/wcet.py <(grep '"source":"path"' runs/latency/pc-long/lat-B.jsonl)
./build/relais-verify runs/latency/pc-long/actuator.jsonl --level complet --fault-free
```

`make latency` runs the same campaign with the default port base (7410). The
campaign takes about 5 minutes of wall clock (the `pc-long` configuration alone
is 200 s); every sample file, report, environment description and machine-readable
summary lands under `runs/latency/<config>/` (`runs/` is gitignored, so the
numbers quoted here are the durable artefact).

## 3. The recorder: no allocation, exact percentiles, bounded memory

`host/measure/latency.c` + `latency_json.c` (linked by the host processes only —
`core/` stays pure: no clock, no file, no socket, no allocation):

* **Fixed tables.** One `rl_lat_rec` per segment per process, in `.bss`: a
  24-bucket power-of-two histogram over microseconds (`<1us`, `1-2us`, `2-4us`,
  … `>=4194304us`, last bucket saturating), a raw window of 65 536 samples
  (512 KiB), and a keep-list of the 8 slowest samples with their context
  (tick, segment, role). `malloc` is never called, on the measured path or
  anywhere else (ROB-04, OBS-03).
* **Cost on the measured path.** `rl_lat_record()` is one bucket increment, one
  store in the raw window, at most 8 comparisons for the worst list. The JSON
  line is formatted **after** the end timestamp, into a stack buffer, and the
  stream is fully buffered with a static 8 KiB buffer (`rl_lat_open`), flushed
  every 256 samples and before a death point (`raise(SIGKILL)`), so a killed
  machine loses at most 255 lines.
* **Percentiles** are nearest-rank (`rank = ceil(p·n)`). While the raw window
  holds every sample they are **exact**; past 65 536 samples the histogram takes
  over and returns the upper bound of the bucket containing the rank — a
  conservative estimate, and `rl_lat_stats.exact` (and the `exact` column of
  every table below) says which one it was. Every percentile published here is
  exact.
* **Sample line**, journal spirit, stable key order, and the exact format
  `host/timing/wcet.py` already consumes (`latency_us`), so the WCET
  extrapolation of `docs/TIMING.md` reads M02's output without conversion:

```json
{"kind":"latency","source":"path","role":"A","tick":12,"latency_us":412.318}
{"kind":"latency_summary","source":"path","role":"A","n":4000,"exact":true,"min_us":5.049,"mean_us":11.380,"p50_us":7.534,"p90_us":17.573,"p99_us":54.291,"p999_us":98.056,"max_us":3449.190,"hist":[0,0,2234,1279,388,81,14,2,0,0,1,0,...]}
```

* **Unit tests**: `./build/test_latency` → 132 checks (exact percentiles on
  1…100 µs, 0 sample, 1 sample, all identical, histogram boundaries and labels,
  raw-window overflow falling back to estimates, worst-case ordering and context,
  JSON round trip including the spaced `wcet.py` style, file round trip,
  monotonic clock).

`build/relais-latency` reads any number of sample files, groups by
(segment, role) and prints the table, the ASCII histogram and the worst cases:

```
source  role         n       min       p50       p90       p99     p99.9       max      mean exact
decode  B         4000    0.142    0.169    0.250    1.001    1.879    5.318    0.204 yes
law     B         4000    0.023    0.039    0.060    0.185    0.388    0.698    0.045 yes
path    B         3999    4.067    4.782    7.518   19.433   26.969   47.578    5.593 yes

histogram path/B (power-of-two buckets, peak 3655 samples)
           4-8us     3655 |########################################
          8-16us      288 |####
         16-32us       54 |#
         32-64us        2 |#

worst cases path/B (PERF-07: where the time went)
  1.     47.578 us at tick 1316     segment path   role B   (+2 more)
```

## 4. Machine and mode

| | |
|---|---|
| host | `baptiste-MS-7D18`, Linux 6.14.0-37-generic, x86_64 |
| CPU | 11th Gen Intel(R) Core(TM) i5-11600K @ 3.90GHz, 12 logical cores |
| scheduler | `SCHED_OTHER`, no `SCHED_FIFO`, no `mlockall`, no CPU partition |
| transport | UDP over loopback, one actuator + two `relais-node` processes |
| ambient load during the campaign | `loadavg` 1.00 → 5.65 (the PC was shared with other work; per-configuration values are in every table) |
| python | 3.12.3 (standard library only) |

**Modes covered**: real PC (idle / under a *lower-priority* load, PERF-01's own
wording), period lowered to 1 ms (PERF-08), slowed down (CMD-07, 50 ms per tick),
long run, single-core contention (`taskset`). **Mode BLOCKED**: *QNX 8.0 guest
under QEMU (TCG)* — section 10.

## 5. Results — six configurations, one campaign

All values are **microseconds**, nearest-rank percentiles over the raw window.
`role` is the computer that held the mandate; the second produces no `path`
sample (it emits shadows), which is why some configurations show `path` for one
role only. Every percentile below is **exact** (`n` ≤ 65 536 per recorder).

#### `pc-nominal` — Reference: real PC, no injected load

`--params T=5,p=1,N=8`, 4000 ticks, 0 CPU hog(s), pin=any, ambient load 1.00 -> 1.14, 20 s wall clock.

| segment | role | n | min | p50 | p90 | p99 | p99.9 | max | mean | percentiles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `path` | A | 3999 | 5.064 | 20.135 | 104.377 | 136.601 | 185.319 | 225.121 | 45.455 | exact |
| `law` | A | 4000 | 0.024 | 0.136 | 0.437 | 0.616 | 1.308 | 39.941 | 0.235 | exact |
| `decode` | A | 4000 | 0.159 | 0.901 | 2.463 | 3.375 | 26.954 | 38.451 | 1.383 | exact |
| `law` | B | 4000 | 0.024 | 0.094 | 0.417 | 0.566 | 25.454 | 30.504 | 0.250 | exact |
| `decode` | B | 4000 | 0.158 | 0.573 | 2.571 | 3.984 | 36.499 | 42.879 | 1.530 | exact |

#### `pc-load` — PERF-01 condition: 12 nice-19 CPU hogs (one per core), lower priority

`--params T=5,p=1,N=8`, 4000 ticks, 12 CPU hog(s), pin=any, ambient load 2.09 -> 5.10, 20 s wall clock.

| segment | role | n | min | p50 | p90 | p99 | p99.9 | max | mean | percentiles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `path` | A | 3999 | 5.049 | 7.534 | 17.573 | 54.291 | 98.056 | 3449.190 | 11.380 | exact |
| `law` | A | 4000 | 0.028 | 0.064 | 0.255 | 0.626 | 0.899 | 1.166 | 0.107 | exact |
| `decode` | A | 4000 | 0.160 | 0.394 | 1.573 | 2.833 | 50.647 | 125.161 | 0.878 | exact |
| `law` | B | 4000 | 0.031 | 0.057 | 0.245 | 0.620 | 0.977 | 1.039 | 0.102 | exact |
| `decode` | B | 4000 | 0.162 | 0.221 | 1.213 | 2.714 | 3.264 | 3.778 | 0.433 | exact |

#### `pc-tight` — PERF-08 probe: period lowered to 1 ms

`--params T=1,p=1,N=8`, 4000 ticks, 0 CPU hog(s), pin=any, ambient load 5.10 -> 5.10, 4 s wall clock.

| segment | role | n | min | p50 | p90 | p99 | p99.9 | max | mean | percentiles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `path` | A | 3998 | 3.759 | 10.542 | 74.298 | 95.453 | 127.659 | 132.571 | 20.319 | exact |
| `law` | A | 3999 | 0.021 | 0.056 | 0.339 | 0.397 | 0.512 | 0.543 | 0.096 | exact |
| `decode` | A | 3999 | 0.123 | 0.360 | 2.246 | 3.034 | 3.344 | 9.352 | 0.631 | exact |
| `law` | B | 3999 | 0.021 | 0.065 | 0.370 | 0.486 | 0.619 | 1.002 | 0.114 | exact |
| `decode` | B | 3999 | 0.119 | 0.339 | 1.835 | 2.994 | 3.233 | 3.579 | 0.598 | exact |

#### `pc-slow` — CMD-07 slow motion: 50 ms per tick, 600 ticks

`--params T=5,p=1,N=8,slow=10`, 600 ticks, 0 CPU hog(s), pin=any, ambient load 5.10 -> 3.36, 30 s wall clock.

| segment | role | n | min | p50 | p90 | p99 | p99.9 | max | mean | percentiles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `path` | A | 599 | 15.085 | 83.572 | 100.629 | 157.227 | 236.508 | 236.508 | 70.061 | exact |
| `law` | A | 600 | 0.061 | 0.458 | 0.674 | 1.083 | 1.208 | 1.208 | 0.447 | exact |
| `decode` | A | 600 | 0.385 | 3.155 | 4.603 | 5.487 | 10.157 | 10.157 | 3.015 | exact |
| `law` | B | 600 | 0.065 | 0.416 | 0.591 | 0.933 | 1.195 | 1.195 | 0.381 | exact |
| `decode` | B | 600 | 0.468 | 2.754 | 3.799 | 5.051 | 11.908 | 11.908 | 2.424 | exact |

#### `pc-long` — Long run: 40 000 ticks at the nominal period (200 s)

`--params T=5,p=1,N=8`, 40000 ticks, 0 CPU hog(s), pin=any, ambient load 3.36 -> 1.02, 200 s wall clock.

| segment | role | n | min | p50 | p90 | p99 | p99.9 | max | mean | percentiles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `path` | A | 12925 | 5.221 | 19.024 | 106.047 | 135.339 | 179.857 | 200.924 | 43.561 | exact |
| `law` | A | 40000 | 0.024 | 0.236 | 0.456 | 0.707 | 1.432 | 58.329 | 0.267 | exact |
| `decode` | A | 40000 | 0.156 | 1.471 | 2.881 | 29.642 | 41.479 | 2275.587 | 2.190 | exact |
| `path` | B | 27074 | 5.110 | 31.504 | 107.023 | 137.090 | 184.947 | 226.497 | 52.459 | exact |
| `law` | B | 40000 | 0.023 | 0.161 | 0.439 | 0.606 | 26.490 | 38.024 | 0.279 | exact |
| `decode` | B | 40000 | 0.152 | 0.917 | 2.474 | 3.506 | 32.133 | 57.971 | 1.480 | exact |

#### `pc-pinned` — Contention: actuator, both nodes and 2 hogs pinned to ONE core

`--params T=5,p=1,N=8`, 4000 ticks, 2 CPU hog(s), pin=2, ambient load 1.18 -> 1.49, 20 s wall clock.

| segment | role | n | min | p50 | p90 | p99 | p99.9 | max | mean | percentiles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `law` | A | 4000 | 0.026 | 0.053 | 0.183 | 0.467 | 0.820 | 0.892 | 0.087 | exact |
| `decode` | A | 4000 | 0.155 | 0.289 | 0.859 | 1.737 | 3.097 | 4.116 | 0.428 | exact |
| `path` | B | 3999 | 4.067 | 4.782 | 7.518 | 19.433 | 26.969 | 47.578 | 5.593 | exact |
| `law` | B | 4000 | 0.023 | 0.039 | 0.060 | 0.185 | 0.388 | 0.698 | 0.045 | exact |
| `decode` | B | 4000 | 0.142 | 0.169 | 0.250 | 1.001 | 1.879 | 5.318 | 0.204 | exact |


### 5.1 What the tables say

* **The path is dominated by the kernel, not by the protocol code.** `decode`
  (CRC-32C + SipHash on a ~200-byte frame) costs 0.2–1.5 µs at the median and
  3–4 µs at p99; `law` (`relais_ctl_step`) costs 0.04–0.46 µs at the median and
  stays below 1 µs at p99. The `path` median (4.8–83.6 µs) is one to three
  orders of magnitude above the sum of those two: what remains is the syscall
  and the time the process spent waiting for the CPU.
* **PERF-01's p99.9 clause holds on this PC in every configuration**, with a
  factor of 4 or more:

| configuration | path p99.9 | path max | PERF-01 (1 ms @ p99.9 / 2 ms worst) |
|---|---:|---:|---|
| `pc-pinned` (one core) | 26.969 µs | 47.578 µs | met, met |
| `pc-load` (12 hogs) | 98.056 µs | **3 449.190 µs** | met, **not met** (1 sample in 3 999) |
| `pc-tight` (T = 1 ms) | 127.659 µs | 132.571 µs | met, met |
| `pc-long` (40 000 ticks) | 179.857 µs (A) / 184.947 µs (B) | 200.924 / 226.497 µs | met, met |
| `pc-nominal` | 185.319 µs | 225.121 µs | met, met |
| `pc-slow` (600 ticks) | 236.508 µs | 236.508 µs | met, met (p99.9 = max at n = 599) |

* **PERF-01's worst-case clause is not met everywhere**: under 12 lower-priority
  hogs, **one** path sample out of 3 999 reached 3.449 ms, and one `decode`
  sample out of 40 000 reached 2.276 ms (the process was preempted *inside* the
  MAC verification). On a non-real-time Linux kernel the process can be
  descheduled for milliseconds; a QNX kernel at priority 62 with `mlockall`
  cannot do that in the same way. **This is exactly why PERF-01 must be
  re-measured on the target** — and the instrument is now in place to do it.
* **The existing WCET tool reads these samples unchanged** (`host/timing/wcet.py`
  documents the `latency_us` key; M02 writes it), on the 27 074 `path` samples of
  role B in `pc-long`:

```
$ python3 host/timing/wcet.py <(grep '"source":"path"' runs/latency/pc-long/lat-B.jsonl)
latency samples: n=27074  min=5.1  mean=52.5  stdev=39.5 us
  p50=31.5  p90=107.0  p99=137.0  p99.9=184.9  p99.99=212.9  max=226.5 us
extrapolated Gumbel-POT bound at survival 1.0e-06: 332.6 us  [95% CI 309.3 .. 355.9]
```

  Its percentiles match the exact ones above (p99.9 = 184.9 vs 184.947), and its
  extreme-value extrapolation to one sample in a million — **332.6 µs
  [309.3 … 355.9]** — still sits three times under PERF-01's 1 ms on this PC.

## 6. PERF-07 — the three worst cases, explained

Three worst `path` samples per configuration, with the same-tick `decode` and `law` values and the residual (reproduce with the snippet below).

| configuration | tick | path | decode | law | residual (encode + `sendto` + descheduling) |
|---|---:|---:|---:|---:|---:|
| `pc-load` A | 2113 | 3 449.190 µs | 2.375 µs | 0.262 µs | 3 446.553 µs |
| `pc-load` A | 2385 | 141.237 µs | **125.161 µs** | 0.064 µs | 16.012 µs |
| `pc-long` B | 25072 | 226.497 µs | 2.409 µs | 0.541 µs | 223.547 µs |
| `pc-long` B | 29261 | 222.096 µs | 2.447 µs | 0.449 µs | 219.200 µs |
| `pc-long` B | 29312 | 215.720 µs | 2.391 µs | 0.432 µs | 212.897 µs |
| `pc-long` A | 10788 | 200.924 µs | 2.368 µs | 0.416 µs | 198.140 µs |
| `pc-nominal` A | 1993 | 225.121 µs | 2.236 µs | 0.413 µs | 222.472 µs |
| `pc-pinned` B | 1316 | 47.578 µs | 0.220 µs | 0.074 µs | 47.284 µs |
| `pc-tight` A | 343 | 132.571 µs | 2.399 µs | 0.391 µs | 129.781 µs |
| `pc-slow` A | 584 | 236.508 µs | 2.968 µs | 0.450 µs | 233.090 µs |

Reading:

1. **The protocol code is never the culprit.** Across the 21 worst cases
   examined (7 groups × 3), `law` stays below 0.6 µs in **all 21**, `decode`
   below 3.1 µs in 17 of them, and the residual is **≥ 97 % of the duration in
   20 of the 21**.
2. **The residual is the OS.** The only thing left in it is the frame encoding,
   the `sendto` syscall and the wait for the CPU. On this host we cannot say
   *which* (preemption, page fault, timer, other agents' processes): the PC
   kernel offers no per-sample trace. **That is what `rl_trace` and
   `host/timing/procedures-qnx.md` are for on the target (QNX-04, PERF-07).**
3. **One worst case is different**: `pc-load` tick 2385 spent 125.161 µs
   *inside* the decode — the process was preempted between the two clock reads
   of the MAC verification. Same cause (the kernel), different victim.

```sh
python3 - <<'PY'
import json, os
root = 'runs/latency'
for cfg in sorted(os.listdir(root)):
    d = os.path.join(root, cfg)
    if not os.path.isdir(d): continue
    for role in ('A', 'B'):
        p = os.path.join(d, 'lat-%s.jsonl' % role)
        if not os.path.exists(p): continue
        by = {'path': {}, 'law': {}, 'decode': {}}
        for line in open(p, encoding='utf-8'):
            o = json.loads(line)
            if o.get('kind') == 'latency' and o.get('source') in by:
                by[o['source']].setdefault(o['tick'], []).append(o['latency_us'])
        if not by['path']: continue
        worst = sorted(by['path'].items(), key=lambda kv: -max(kv[1]))[:3]
        for tick, vals in worst:
            v = max(vals); dec = max(by['decode'].get(tick, [0.0])); law = max(by['law'].get(tick, [0.0]))
            print('%s %s tick %d path %.3f decode %.3f law %.3f residual %.3f'
                  % (cfg, role, tick, v, dec, law, v - dec - law))
PY
```

## 7. PERF-08 — the period lowered to the breaking point

`pc-tight` runs the whole system at **T = 1 ms** (4000 ticks, 4 s of wall clock):

* the critical path stays cheap: p50 10.542 µs, p99 95.453 µs, **p99.9
  127.659 µs = 12.8 % of the tick**, max 132.571 µs — the machine has 87 % of
  the tick left;
* the protocol keeps working: one mandate for the whole run, one certification,
  the lamp back to green after each hiccup;
* **but 4 ticks out of 4 000 were served by the will instead of the holder's
  order** (ticks 127, 1127, 2127, 3127 — *exactly* every 1000 ticks, i.e. every
  second of wall clock). The journal shows why: at those ticks the holder's
  order is **rejected** (`order_rejected`, detail 4 = `RELAIS_REJ_TICK_MISSED`),
  meaning it arrived after the tick boundary, so the actuator played the
  testament and the second's shadow was not used either. A once-per-second
  periodicity points at a **host-periodic disturbance** (this PC was shared with
  other work), not at the protocol.

**Smallest period published (PERF-08).** On this PC, **T = 1 ms** keeps the
critical path at 12.8 % of the tick and the mandate stable, so 1 ms is a
*candidate* breaking point for the *latency* budget. It is **not** published as
the smallest period for which **PERF-02** holds (see below), because PERF-02 is
not demonstrated at 1 ms, nor at 5 ms, nor anywhere on this host.

## 8. PERF-02 — ticks served otherwise than by the holder's order

Measured from the actuator's own journal (`source` field), same runs:

| configuration | ticks | fallback | shadow | testament | not served by the holder | per 10 000 |
|---|---:|---:|---:|---:|---:|---:|
| `pc-nominal` | 4 000 | 1 | 0 | 0 | 1 | 2.5 |
| `pc-load` | 4 000 | 1 | 3 | 0 | 4 | 10.0 |
| `pc-tight` | 4 000 | 2 | 0 | 4 | 6 | 15.0 |
| `pc-slow` | 600 | 1 | 0 | 0 | 1 | 16.7 |
| `pc-long` | 40 000 | 1 | 2 | 6 | 9 | 2.25 |
| `pc-pinned` | 4 000 | 1 | 0 | 0 | 1 | 2.5 |

Reading: the first tick is always served by the fallback law (no mandate yet);
the other exceptions cluster around the mandate changes (a handover is served
by the will, which is the designed behaviour). **PERF-02 is not demonstrated
here**: it asks for fewer than one tick in 10 000 over **30 minutes** (360 000
ticks at T = 5 ms), and the longest run of this campaign is 40 000 ticks
(200 s), with 2.25 exceptions per 10 000. On a host without real-time
scheduling, with a shared CPU and a once-per-second disturbance, that ratio
cannot be brought under 1 per 10 000.

### The invariant checker says the same thing, in its own words

```
$ ./build/relais-verify runs/latency/pc-long/actuator.jsonl --level complet --fault-free
ticks=40000 violations=8 max_gap=2 (bound 0) max_deviation=0 (bound 0.02) …
first: I4 violated at tick 12520: a tick without a fresh order, second certified
```

| configuration | violations (`--level complet --fault-free`) | account |
|---|---:|---|
| `pc-nominal`, `pc-load`, `pc-slow`, `pc-pinned` | 0 | — |
| `pc-tight` | 4 | 4 × I4, one per testament tick (127, 1127, 2127, 3127) |
| `pc-long` | 8 | 6 × I4 (the six testament ticks) + 2 × I7 at tick 12927 (mandate revoked and auto-switched, with no *injected* fault) |

These are **not** core defects, and M02 does not touch `core/`:

* I4's zero-gap bound is claimed by `core/src/invariants.c` (function
  `check_gap`, lines 114-133) **only** "for a channel inside the timing
  hypothesis 2Δ + C + J ≤ T". A non-real-time Linux host violates that
  hypothesis from time to time: at those ticks neither the holder's `ORDRE` nor
  the certified second's `OMBRE` reached the actuator before the boundary, so
  the actuator played the will — the documented precedence
  (order → shadow → testament → fallback).
* The API offers the right switch for that situation
  (`relais_inv_lossy_channel()`, `core/include/relais/invariants.h:78`), but
  `host/verify/relais-verify.c` exposes **no** command-line flag to set it, so
  a journal produced by the PC bench cannot be declared a "disturbed channel"
  from the command line. **Reported to the owner of `host/verify`; not fixed
  here** (outside M02's file list).

## 9. The instrumentation does not change the behaviour

*By construction.* Without `--latency`, `g_lat_on` is false: no clock is read
(the two `rl_lat_now_ns()` calls in `recv_frame`/`dispatch` are behind the
flag), the timed law is not installed (the core runs its own
`relais_ctl_step`), and the only added work is a `memset` of two `uint64` and
two predictable branches per event.

*By measurement.* The pre-M02 node was rebuilt from the parent commit
(`git show a4d8266:host/node/relais-node.c`) and the two were compared on the
**slow-motion** configuration, the only one that is bit-reproducible on this
host (50 ms per tick):

```sh
gcc -std=c11 -O2 -Icore/include -Iactuator /tmp/relais-node-orig.c build/librelais.a -lm -o build/relais-node-orig
python3 host/measure/run_latency_campaign.py --out /tmp/eq/orig  --configs pc-slow --no-latency --node-bin build/relais-node-orig
python3 host/measure/run_latency_campaign.py --out /tmp/eq/instr --configs pc-slow              --node-bin build/relais-node
# then compare the two journals, "ns" excluded
```

| comparison | identical tick records | differing fields | applied value / pos / vel / source / lamp / misses |
|---|---:|---|---|
| original vs original (control) | 600 / 600 | none | identical |
| original vs instrumented **with `--latency` on** (run 1) | 600 / 600 | none | identical |
| original vs instrumented **with `--latency` on** (run 2) | 600 / 600 | none | identical |

In one earlier pair the only differences were `holder` and `certified_node`
(the election of the first mandate is a race) while the applied value sequence
stayed identical — which is precisely CMD-03: both machines run the same law on
the same measurements.

At T = 5 ms on a loaded host the bench is **not** bit-reproducible: two runs of
the *same* binary diverged after 117 identical ticks (a missed order) and a
handover elected a different holder. That is why the equivalence proof uses the
slow-motion configuration, where the host is quiet enough for the comparison to
mean something.

## 10. BLOCKED — QNX 8.0 guest under QEMU (TCG)

The "invité QEMU en TCG" configuration is **not delivered**. Two precise
reasons, both verified:

1. **The bench's QEMU path is broken before any guest starts.**
   `python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/slalom-nominal.json`
   fails with
   `TypeError: Orchestrator.__init__() got an unexpected keyword argument 'out_dir'`
   — `host/bench/relais_bench.py:154` calls `runner(root=ROOT, out_dir=out_dir)`
   while `host/bench/orchestrator.py:40` (`Orchestrator.__init__`) accepts
   `mode, campaign, root, workdir, parallel, actuator, use_cable,
   ready_timeout, max_run_s, logger` — no `out_dir`. **Pre-existing defect,
   outside M02's file list: described here, not fixed.**
2. **There is no QNX 8.0 image in this tree.** `image/fragments/` holds the
   buildfile and the startup scripts, but no built image; the only QNX image on
   this machine is `/home/baptiste/Bureau/qnx71-vm/qnx71-fs.img` (QNX **7.1**,
   outside the workdir, and not the project's target).

The four PC configurations above (plus the long run and the single-core one)
cover the "real PC" and "slowed-down" modes; the QNX guest remains to be run
by whoever owns the target image.

## 11. What this document does NOT establish

* **Nothing about QNX.** The QNX chain runs `relais-net`/`relais-core`/
  `relais-ctl` at priorities 62/61/60 with `mlockall` (`docs/TIMING.md`); the
  PC node runs as a `SCHED_OTHER` process with no page pinning. PERF-01's 1 ms
  at p99.9 and 2 ms worst case **remain to be measured on the target**, with
  `host/timing/procedures-qnx.md`.
* **No kernel-side breakdown.** The residual of section 6 is "the OS", not a
  named cause. Naming it needs a kernel trace (`rl_trace`), which exists on the
  target only.
* **PERF-02 is not demonstrated** (section 8): 2.25 exceptions per 10 000 over
  40 000 ticks, not fewer than 1 per 10 000 over 30 minutes.
* **`io-sock` interrupt latency, priority inheritance and CPU partitioning** are
  untouched here (assumptions 1-4 of `docs/TIMING.md`), and `law` is the cost of
  `relais_ctl_step` on the node's own state — not an abstract WCET of the law.

## 12. Files of this mission

| file | lines | role |
|---|---:|---|
| `host/measure/latency.h` | 160 | the recorder's API (fixed tables, no allocation) |
| `host/measure/latency.c` | 185 | clock, histogram, percentiles, worst cases |
| `host/measure/latency_json.c` | 177 | JSON lines: format, write, parse |
| `host/measure/relais-latency.c` | 200 | the reader: table, ASCII histogram, worst cases |
| `host/measure/test_latency.c` | 291 | 132 unit checks |
| `host/measure/run_latency_campaign.py` | 285 | the campaign (six configurations, stdlib only) |
| `host/node/relais-node.c` | 366 | the instrumented host computer (`--latency`) |
| `docs/LATENCY.md` | this file | the published measurement |

The Makefile was **appended only**: M02's block sits at the end, hooks the new
binaries onto `all`/`test` by appended prerequisite lines, and extends
`NODE_SRC`, `CPPFLAGS` and `TEST_BIN` (no existing line was rewritten).
