# Relais long soak (mission M42)

The nightly probe (`host/nightly/`) replays the proofs once per night: it is a
snapshot. The soak is the curve. Same core, same campaigns, run continuously,
with a hunter that looks for the rare anomaly and **freezes the scene** the
moment one appears.

Requirement IDs: PERF-05 (long runs), ROB-06 (replayable campaigns),
INJ-08 (fault sweeps), OBS-07 (evidence published with its context),
ACT-01 (one value per tick), MAN-04 (revocation on the actuator's own
evidence), SEC-04 / ACT-09 (forged and stale orders), DET-03 / DET-09.

## The pieces

| File | Role |
| --- | --- |
| `host/soak/soak.py` | the pilot: launches runs, rotates the profiles, journals every run, freezes on an anomaly |
| `host/soak/anomaly.py` | the hunter and the versioned rulebook (the rules and their thresholds) |
| `host/soak/profiles.py` | the catalogue of fault profiles and the claim scope of each |
| `host/soak/machine.py` | what the pilot knows about the machine it runs on (load, memory, disk) |
| `host/soak/report.py` | the reliability curve, a standalone SVG written by hand |
| `host/soak/test_soak.py` | the unit tests of the hunter: a hand-made defect must trigger its rule |

The pilot reuses what already exists: the journal reader and the invariant
mirror of `host/report/` (`journal_io`, `analysis`), the statistics of
`host/report/stats.py`, the chart helpers of `host/report/charts.py`, the C
simulator `build/relais-simulate` and the C verifier `build/relais-verify`.
No logic is written twice.

## How to run it

```sh
make soak                              # three-minute smoke test (the CI budget)
make soak SOAK_MINUTES=30              # a real half-hour campaign
make soak SOAK_TICKS=250000            # bigger runs
make soak SOAK_JOBS=1                  # one bench process at a time
make soak-long                         # four hours in the background
```

The duration is a parameter: `SOAK_MINUTES` for `soak`, `SOAK_LONG_MINUTES`
for `soak-long`, both overridable on the command line. The campaign can run
for hours in the background and is stopped cleanly with

```sh
kill -TERM $(cat results/soak-long/soak.pid)      # or Ctrl-C in the foreground
```

The pilot then stops launching, stops the bench processes it started, writes
`summary.json` and `reliability.svg`, and exits 0. A `SIGKILL` would lose only
the summary, never the digest-chained log.

The output of one campaign:

```
results/soak/soak.jsonl      one JSON record per run, chained by sha256
results/soak/summary.json    the counters of the campaign
results/soak/reliability.svg the reliability curve (standalone SVG)
results/soak/soak.pid        the pid of the pilot, to stop it
results/soak/frozen/         the frozen scene of an anomaly (only if one fired)
```

## The rulebook

Every rule has an identifier, a threshold and the reason why that threshold is
the right one. The version of the rulebook is written in `summary.json` and in
the header of the SVG, so a published curve always names the rules that judged
it (`anomaly.RULES_VERSION`, currently `1.0.0`).

| Id | Rule | Threshold | Why |
| --- | --- | --- | --- |
| A01 | invariant violation reported by the core | the simulator exits 0 and reports `violations=0` | the core checks I1..I11 while it runs; any breach is a safety defect |
| A02 | invariant violation reported by the offline verifier | `relais-verify` exits 0 on the published journal | the journal is what a third party replays; it must tell the same story as the run |
| A03 | invariant violation reported by the python mirror | 0 violations after the profile preconditions are applied | a third reading of the same invariants, from the journal alone |
| A04 | two values applied on one tick | 0 duplicated tick numbers | the actuator applies exactly ONE value per tick (ACT-01) |
| A05 | stale order accepted | the mandate number never decreases | an order carrying an older mandate is stale and must be dropped (MAN-04, ACT-09) |
| A06 | tick without a fresh order outside a handover | `misses <= p+1` while a mandate is live | the actuator revokes on its own evidence after p+1 ticks without a valid order (MAN-04); more misses than that means it kept a mandate it had already proved empty |
| A07 | fresh order counted as missing | `misses == 0` on every tick whose source is the holder | a tick served by the holder's own fresh order cannot also be a missed tick |
| A08 | non-monotonic timestamp | tick numbers strictly increase, `ns` never decreases | the journal is an ordered trace of time; a stamp that goes backwards breaks every replay and every latency measurement |
| A09 | latency drift above the reference p99.9 | `cpu us per 1000 ticks <= p99.9(reference) * 4` | the calibration phase gives the reference; the observed spread of the metric is under 20 percent, so a factor of 4 leaves headroom for a busy machine while a real drift crosses any fixed factor |
| A10 | monotonic growth of the process memory | 0 runs in a row whose peak RSS grows by >= 1024 kB | `core/` never allocates; a sustained rise of one megabyte cannot be jitter |
| A11 | broken journal fingerprint | the digest chain of the log and the journal sha256 match | the soak log is chained by sha256; a broken chain means the evidence was altered, truncated or reordered |
| A12 | unreadable journal line | 0 malformed lines | a journal that cannot be parsed cannot be replayed, and the verifier would silently check less than the run did |

Three notes on the thresholds:

* **A09 uses the CPU time of the bench, not the wall clock.** On a shared
  machine the wall clock of a run varies by a factor of three (measured:
  0.86 s to 2.98 s for the same 250 000 ticks) while the CPU time of the same
  run varies by 19 percent. A drift rule built on the wall clock would fire on
  the neighbours, not on the protocol.
* **A09 calibrates on the first runs of the campaign** (12 by default,
  `--calib`). The reference is the p99.9 of that phase; the rule only applies
  after it.
* **A10 watches two series**: the peak RSS of the bench process, per profile
  (the same workload, again and again), and the RSS of the pilot itself. The
  pilot re-reads its log from the disk instead of keeping the campaign in
  memory, so its own memory stays flat: a rise there is a real leak.

## The fault profiles

One profile per family of the fault model (spec 3.3). The pilot rotates over
them, and over the seeds, and over the levels.

| Profile | What it injects | Levels |
| --- | --- | --- |
| `nominal` | nothing | socle, complet |
| `death` | `at:death:node` | socle, complet |
| `freeze` | `at:freeze:node:duration` | socle, complet |
| `cutact` | `at:cut-actuator:node` then `restore` | socle, complet |
| `cutpeer` | `at:cut-peer:node` then `restore` | socle, complet |
| `slow` | `at:slow:node:3` | socle, complet |
| `cession` | `at:handover:node` (REL-09) | socle, complet |
| `update` | `at:update:node:duration` (REL-10) | socle, complet |
| `crash` | `at:crash:node:20` (DET-05) | socle, complet |
| `restart` | `at:restart:node` (DEG-03) | socle only, see below |
| `forge` | `at:forge:node` (SEC-04, must be dropped) | socle, complet |
| `forgestale` | `at:forge-stale:node` (ACT-09, must be dropped) | socle, complet |
| `bias` | `at:bias:node:0.25` (a machine that computes wrong) | socle, complet |
| `lossy` | `--loss 0.01`, a channel that loses frames | socle, complet |
| `auto` | `--auto-faults 1`, one fault drawn at random | socle, complet |

Three profiles stay inside a narrower set of published claims, and the pilot
says so in the log:

* **`restart` is level S only.** A restart wipes the control state of the
  machine, so at level C the value it then computes legitimately differs from
  the twin, while I5 assumes a control state that persists. The combination is
  reproducible: `./build/relais-simulate --ticks 20000 --seed 21 --level
  complet --fault 8000:restart:0` reports `I5 violated at tick 8001`. The core
  is not modified here; the observation is published, not patched.
* **`lossy` is not verified offline.** The offline verifier cannot express a
  lossy channel, while the core relaxes I4 and I5 when frames are lost
  (spec 7.3, ROB-03). The in-process check of the core is the authority there,
  and the structural rules still read the journal.
* **`bias` is not verified offline at level C**, for the same reason: the
  verifier cannot express a wrong value, while the core exempts it from I5.

## The machine guard

The campaign must never overload a shared machine.

| Guard | Trigger | Effect |
| --- | --- | --- |
| load | load average above 20 | the campaign is shortened by half, never below 20 minutes |
| memory | less than 4 GiB available | same |
| disk | less than 2 GiB free | the campaign stops and writes its summary |
| jobs | always | at most two bench processes at a time |

The guard is applied at the start of the campaign and published in
`summary.json` under `machine_guard`.

## What the run really showed

The run published below was made on the machine described here, with the
command line given, and every number comes from `results/soak/summary.json`
and `results/soak/soak.jsonl`.

### The run

```
cd /tmp/rl-w42 && make soak SOAK_MINUTES=30 SOAK_TICKS=100000
```

12 November 2026, 15:42 to 16:02 CET, in the isolated copy of the tree
(`/tmp/rl-w42`, archive of `origin/main` 98af2ae). Artefacts:
`results/soak-real/soak.jsonl` (253 chained records), `results/soak-real/summary.json`,
`results/soak-real/reliability.svg`.

| | |
| --- | --- |
| requested / effective duration | 30 min requested, **20.0 min effective**: the machine guard shortened it (load average 92.1 above the limit of 20) |
| runs | 253, of 100 000 ticks each, never more than 2 bench processes at a time |
| ticks played | **25 300 000** |
| handovers (relèves) | **309**, on 282 mandates and 56 revocations |
| coverage | 29 of 29 profile x level combinations (15 profiles, socle and complet) |
| anomalies | **0** (rules A01 to A12, rulebook 1.0.0) |
| exit codes | 253 x 0 |
| malformed journal lines | 0 |
| latency, cpu us per 1000 ticks | min 1971, p50 3504, p99 3971, p99.9 4082, max 4086; reference 3684, A09 threshold 14736 |
| wall clock per run | 0.2 s to 19.1 s (a factor of 95): the machine was busy, and A09 measures the cpu time on purpose |
| peak rss of the bench | 20 156 kB to 102 216 kB, never monotone over 12 runs (A10) |
| digest chain of the log | intact, checked with `anomaly.verify_digest_chain` |

The hunt was real: the same pilot, with `--latency-factor 0.001`, fired A09 on
its second run, froze the scene in `frozen/run-0002/` (journal, output of the
bench, `anomaly.json` with the context) and exited 3. The unit tests of
`host/soak/test_soak.py` feed the hunter a hand-made defect for each rule and
check that the rule fires.

What this run does **not** prove: twenty minutes of a rotating campaign is a
curve, not a proof. The exhaustive proof of the protocol stays the explorer's
(10 000 330 states, two faults, 0 violation); the soak shows that the same
campaign, run again and again for twenty minutes on a machine shared with
fifty other agents, keeps telling the same story.


## What a 72 hour run would need

The pilot itself is ready: its memory stays flat (it re-reads the log from the
disk), the journals of the clean runs are deleted as soon as they are judged,
and the log of a 72 hour campaign stays small (about 700 bytes per run, a few
tens of megabytes). What is missing is the environment, not the code:

1. **A dedicated machine.** The latency rule A09 needs a quiet machine: with
   fifty agents sharing the box, the reference p99.9 is the noise of the
   neighbours. On the shared machine used for the run above, the campaign was
   automatically shortened because the load average was above 20.
2. **A supervisor.** The pilot stops itself when the disk fills up, but a crash
   (a kernel OOM, a reboot) must be caught: a `systemd` unit or a cron loop
   that restarts the campaign and keeps the previous `soak.jsonl` would do it.
3. **A rotating evidence folder.** The frozen scenes must be moved off the
   campaign folder as soon as they are published, otherwise a single anomaly
   with a 250 000 tick journal fills the disk.
4. **A daily digest.** The SVG of a 72 hour campaign is unreadable; the
   summary of each day (runs, ticks, handovers, latency p99.9 per slice) is
   what a human reads. `report.py` already reads any campaign folder, so the
   digest is one call per day.
5. **A longer horizon for the trend rules.** A09 and A10 are meaningful over
   hours, not over seconds: the calibration phase must be long enough for the
   reference to be stable, and the memory rule needs days to separate a leak
   from the allocator warming up.
