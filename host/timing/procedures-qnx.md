# Relais — QNX measurement procedures (to execute on the target)

These are the exact procedures that turn the *assumptions* of `docs/TIMING.md`
into *measured* values. They require the QNX SDP 8.0 and two booted guests, as
described in `image/README.md`. Run them before believing any timing number.
Each section names the assumption it closes (see `docs/TIMING.md` §7) and the
trial it answers.

The image already runs `tracelogger -b 4m &` (see
`image/fragments/startup.script`) and the critical path already emits user
events through `rl_trace()` → `trace_logf(0, 0, ...)` (OBS-03): `net rx …`,
`core rx …`, `core tx …`, `net tx …`. Those four events stamp the chain.

Prerequisite on every guest:

```sh
source ~/qnx800/qnxsdp-env.sh
# trace dump after a run (the 4 MiB ring buffer flushed first):
pidin | grep tracelogger
```

---

## P1. Priority inheritance inside `io-sock` and on the MsgSend path

Closes: assumption 3 (`io-sock` priority ≥ critical path), assumption 2
(inheritance of `MsgSend`/`MsgReply`). Answers: trial **E5**, QNX-04.

What to see: the effective priority of every step from the received `STATUT`
(`net rx`) to the emitted order (`net tx`), network stack included, must never
drop below the client's priority.

```sh
# 1. Instrument: the four rl_trace events already exist; add the receive path
#    by enabling io-sock's protocol events at runtime:
traceprinter -e '_NTO_TRACE_*'      # list what classes exist on this image
pidin -p io-sock sched priority      # static priority of the stack process

# 2. Record a run under lower-priority load (PERF-06):
on -p 1 while :; do :; done &        # a busy low-priority load on CPU 1
python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/slalom-death-A.json

# 3. Flush and read the trace ring buffer:
slay tracelogger                     # or let the buffer wrap; then
traceprinter > trace.txt
grep -E 'net rx|core rx|core tx|net tx|TR_SEND|TR_REPLY|TR_RCV' trace.txt

# 4. For each step, read the effective priority column (the "pri" field) and
#    check the monotonic chain: io-sock(receive) >= 62, net >= 62,
#    core >= 61, ctl acquires 61 while serving core, net(tx) = 62.
```

Verdict criterion: no step on the `net rx → net tx` chain shows a priority
lower than its caller; if `io-sock` runs at 10 (default), raise it with
`on -p 62 io-sock` or the `priority` clause of the `io-sock` start script and
re-measure.

## P2. Effect of CPU partitioning

Closes: the "CPU partitioning" hypothesis (assumption in §7). Answers: PERF-06.

What to see: the chain stays on reserved vCPUs while a fully-loaded neighbour
vCPU is saturated by lower-priority work; the chain's response time is
statistically unchanged.

```sh
# 1. Discover and reserve CPUs for the critical path:
pidin syspage=cpuinfo                 # number and mapping of vCPUs
on -C 0 relais-net ; on -C 0 relais-core ; on -C 0 relais-ctl   # chain on CPU 0

# 2. Saturate a different CPU with lower priority:
on -C 1 -p 1 while :; do :; done &

# 3. Run the same scenario twice (chain pinned UNPINNED, then PINNED) and
#    capture the latency distribution with M02's --latency file:
python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/burst.json
python3 host/timing/wcet.py host/measure/latency-unpinned.jsonl
python3 host/timing/wcet.py host/measure/latency-pinned.jsonl
```

Verdict: p99.9 and the EVT bound must not grow when the load CPU is saturated;
if they do, the partition is not isolating (guest scheduling of `io-sock`
breaks it) and the period must be lengthened.

## P3. SCHED_SPORADIC behaviour under burst release

Closes: the sporadic-release assumption. Answers: PERF-08 (breaking-point
sweep).

What to see: with the three processes declared sporadic (budget = C_i, period
= T), a burst of `STATUT` frames does not push any stage past its budget, and
the chain never misses a deadline.

```sh
# 1. Declare budgets (values from docs/TIMING.md §3, refine with P1):
sched_ctl relais-net  SPORADIC 62  150 5000
sched_ctl relais-core SPORADIC 61  250 5000
sched_ctl relais-ctl  SPORADIC 60  100 5000

# 2. Burst the link (the programmable cable drops/reorders then floods):
python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/burst.json

# 3. Check for deadline misses and budget exhaustion:
pidin sched | grep -E 'relais-net|relais-core|relais-ctl'
traceprinter | grep -E 'SPORADIC|budget|miss'
```

Verdict: zero `_NTO_TRACE_...` budget-exhaustion or missed-deadline events over
the run; otherwise raise the budgets (re-run P1) or lengthen T.

## P4. Residual page faults after `mlockall`

Closes: assumption 1 (the critical path never faults). Answers: QNX-03.

What to see: after `rl_realtime()` has run `mlockall(MCL_NOWAIT)`, the number
of page faults on the critical path is zero across a full run.

```sh
# 1. Enable fault tracing on the three processes:
traceprinter -e '_NTO_TRACE_FAULT'   # or the fault class present on this image

# 2. Run and then count faults per process:
python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/slalom-death-A.json
traceprinter > trace.txt
grep -E 'relais-net|relais-core|relais-ctl' trace.txt | grep -ic fault
```

Verdict: zero faults; a non-zero count means the pulled-in working set is not
locked and `mlockall(MCL_CURRENT)` (or touching every page once at start-up)
must be added before the timing numbers are admissible.

---

Every procedure ends with a single restatable command and a pass/fail criterion,
so the whole set can be replayed by a third party (reproducibility) and its
output pasted into the safety case next to the hypothesis it verifies.