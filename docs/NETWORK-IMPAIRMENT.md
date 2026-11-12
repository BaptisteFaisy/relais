# Network impairment: the degraded channel, for real (M38, DEG/ROB-03/INJ)

Every other fault of this bench is injected *inside* the processes (the fault
model of `host/simulate`, the death points of `host/node`). The central
hypothesis of the protocol, however, is about the *channel*: there is no shared
clock and orders may arrive lost, duplicated, reordered, late, or asymmetrically
delayed (spec 1.2, ROB-03). This page describes how that hypothesis is played
on a real transport — three real OS processes, real UDP datagrams, a real
kernel loopback — and what it proved.

**Honest framing.** `tc`/netem exist on this machine but need root, which this
environment does not have. The degradation played here runs in **user space**:
a seeded UDP relay (`host/impair/impair_proxy.py`) sits between the bench
processes and decides, per datagram, what the far side actually receives.
This is **not** a kernel impairment: queuing disciplines, MTU/fragmentation,
driver and interrupt behaviour keep their normal loopback behaviour, and a
datagram the relay forwards still traverses the real kernel stack twice. The
kernel recipe for a two-board run is given in section 6 and is **NOT EXECUTED**
here. Nothing in this page claims kernel-level results.

## 1. Wiring

The real bench processes are used unmodified (`make all`):

```
                 alias ports (proxy listens)              real ports
  relais-node A ──A_to_act:7383──┐                 ┌── 7380: relais-actuator
  relais-node A ──A_to_B:7387────┤   impair_proxy  ├── 7382: relais-node B
  relais-node B ──B_to_act:7384──┤  (user space,   ├── 7381: relais-node A
  relais-node B ──B_to_A:7388────┤  seeded, one    │
  actuator ───────act_to_A:7385──┤  process)       │
  actuator ───────act_to_B:7386──┘                 └
```

Each endpoint is configured with the *alias* port of its correspondent, so
every datagram of the bench crosses the relay exactly once, in one of six named
directions: `A_to_act`, `B_to_act`, `act_to_A`, `act_to_B`, `A_to_B`, `B_to_A`.
The frames carry their own authenticity (CRC-32C + SipHash MAC, SEC-01), so the
relay is transparent to the protocol: it can only decide *whether, when, how
many times and in what state* a datagram arrives — exactly the power a real
cable has.

The runner (`host/impair/run_impaired.py`) picks a free block of nine UDP
ports, starts the relay, starts the three processes, waits for the actuator to
finish, then judges the actuator journal (the authority, spec 6.4).

## 2. Impairments and determinism

Per direction, in this fixed pipeline order:

1. **corrupt** — flip 1..`bytes_max` random bits (the MAC must catch it);
2. **truncate** — cut the datagram to a random length >= `keep_min`;
3. **loss** — `random` (rate) or `gilbert_elliott` (two-state Markov chain:
   `p_good_to_bad`, `p_bad_to_good`, `loss_in_good`, `loss_in_bad`);
4. **duplicate** — an extra copy, released `delay_ms` later;
5. **reorder** — a hold buffer of depth N: when full it releases a *random*
   buffered datagram; anything held longer than `hold_ms` is flushed;
6. **delay** — `fixed_ms` plus uniform `jitter_ms`.

**Determinism.** Every random decision is drawn from a per-direction generator
seeded by `sha256(seed | direction)`, in a fixed draw order. The decision for
the Nth datagram of a direction is therefore a pure function of
(seed, profile, direction, N) — verified by a built-in self-test that plays a
fixed 300-datagram sequence twice and compares the decision digests:

```
python3 host/impair/impair_proxy.py --profiles host/impair/profiles.json \
        --profile burst-gilbert --selftest        # prints SELFTEST-OK
```

What is *not* reproducible is the wall-clock arrival pattern of the real-time
bench itself (the same run replays with a few datagrams more or less): the
decisions are deterministic given the traffic, the traffic of three real-time
processes on a shared PC is not. This is the honest boundary between this page
and the fully deterministic seeded simulator (`host/simulate`).

## 3. Profiles (`host/impair/profiles.json`)

All profiles run with `T=25 ms, p=3, N=8` (the shared-PC parameters of the
demo, CMD-07: the protocol counts ticks, not milliseconds) over 300 ticks
(nominal: 400).

| Profile | Directions | Exact parameters |
|---|---|---|
| nominal | — | relay forwards everything (control run) |
| loss-1pct | all | random loss, rate 0.01 |
| loss-10pct | all | random loss, rate 0.10 |
| burst-gilbert | all | Gilbert-Elliott: p_good_to_bad 0.01, p_bad_to_good 0.25, loss_in_good 0.001, loss_in_bad 0.95 (mean burst ~4 datagrams > p) |
| reorder-deep | all | reorder buffer depth 8, hold 60 ms; no loss, no delay |
| delay-50ms-asym | A_to_act, B_to_act | delay fixed 50 ms, jitter 5 ms; the return way and the peer links are clean |
| duplicate-5pct | all | duplicate rate 0.05, copy 12 ms behind |
| corrupt-2pct | all | corrupt rate 0.02, 1-2 bits flipped |
| truncate-2pct | all | truncate rate 0.02, keep >= 8 bytes |
| blackout-orders | A_to_act, B_to_act | loss rate 1.0 (orders never arrive; STATUT and peer links clean) |

Seeds are fixed per profile (3801..3810) and published in the JSON file.

## 4. Verdicts: strict and lossy

`relais-verify` judges the journal with the invariants of spec 7.2. Mission
M38 added one additive flag, `--lossy-channel`, which exposes the *existing*
core knob `relais_inv_lossy_channel()` — the same one the simulator sets when
it injects loss or delay (`host/simulate/main.c`) and the explorer always sets
(`host/explore/world.c`). Its semantics are the core's own (ROB-03, spec 7.3
item 5): on a disturbed channel the continuity bounds of I4/I5/I9 are outside
the timing hypothesis; the gaps are still *measured and published*
(`max_gap`), and **every safety invariant stays enforced** — I1 (exactly one
value per tick), I2 (never two holders of one mandate, never two changes in a
tick), I3 (no order of a deposed machine), I6 (the will never plays past its
horizon), I8 (the fallback law takes over within N+1 ticks), I10 (the lamp
within two ticks), I11 (contiguous ticks).

The runner therefore publishes two verdicts per profile: `strict` (reference,
expected to flag I4 gaps on any lossy profile — even the nominal control can
show a scheduling gap on a shared PC) and `lossy` (**the safety verdict of this
campaign: it must be 0**). `make impair` fails if any profile has a non-zero
lossy verdict.

## 5. Results

Verification run of 2026-11-12 (snapshot of origin/main at `98af2ae`, this
PC, `make -j4 all && make test && make explore && make impair` all green).
Delays are `min/median/max` in ticks; microseconds = ticks x T x 1000
(T = 25 ms; the PC journal writer stamps `ns=0`, so microseconds are the
logical protocol time). "no-fresh" = ticks served by the will or the fallback
law. Full data: `build/impair/summary.json`.

| Profile | handovers | takeovers | takeover delay (ticks) | takeover delay (us) | no-fresh | max_gap | viol strict/lossy |
|---|---|---|---|---|---|---|---|
| nominal | 1 | 0 | — | — | 1 | 0 | 0/0 |
| loss-1pct | 1 | 0 | — | — | 15 | 1 | 14/0 |
| loss-10pct | 1 | 0 | — | — | 43 | 3 | 42/0 |
| burst-gilbert | 4 | 3 | 0/0/0 | 0/0/0 | 12 | 3 | 11/0 |
| reorder-deep | 45 | 58 | 1/31/139 | 25k/775k/3.475M | 289 | 142 | 278/0 |
| delay-50ms-asym | 71 | 70 | never re-held | never re-held | 300 | 0 | 0/0 |
| duplicate-5pct | 1 | 0 | — | — | 1 | 0 | 0/0 |
| corrupt-2pct | 1 | 0 | — | — | 6 | 1 | 5/0 |
| truncate-2pct | 1 | 0 | — | — | 10 | 1 | 9/0 |
| blackout-orders | 0 | 0 | — | — | 300 | 0 | 0/0 |

**Safety violations: 0 on every profile** (lossy verdict). What the run shows:

- **Bursts (Gilbert-Elliott)**: a 4-datagram burst exhausts the patience
  (p=3); the actuator revokes on its own evidence (MAN-04, DET-03 — the event
  carries the miss count) and, because a waiting DEMANDE is in the same tick's
  buffer, grants and applies the new holder's order **in the same tick**
  (REL-06 path): measured takeover delay 0 ticks. Journal of the run:
  `mandate_revoked node B detail 4` then `mandate_granted node A` at one tick.
- **10 % uniform loss**: no takeover at all — isolated losses cost a tick of
  freshness (43 will/fallback ticks) but never 4 consecutive misses; the
  mandate survives. This is the patience doing its job.
- **Deep reordering**: the harshest profile. Reordered orders and STATUTs
  carry stale tick labels, so 247 orders are rejected (MAN-02), the second is
  never certified (its ETAT replica arrives out of order), and the system
  churns through 58 takeovers with a median re-hold of 31 ticks (775 ms).
  Safety holds throughout: the will never plays past N=8 ticks (I6), the
  fallback law is always reached in time (I8), one value per tick (I1).
- **Asymmetric 50 ms delay (2 ticks)**: orders *always* arrive with a stale
  label — the network is definitively too bad to command. After the first
  revocation the actuator never re-holds: 70 revoke/grant cycles, 300/300
  ticks served by the fallback law, lamp lit, and **zero violations even in
  strict mode** (no fresh order ever armed the gap). This is the exact
  degraded-clean property the mission asks for: testament horizon respected,
  fallback law, safe state.
- **Duplication**: 130 copies delivered; the actuator's one-value-per-tick
  arbitration and the nodes' deduplication absorb every one — indistinguishable
  from the nominal run (1 startup handover, 0 rejections).
- **Corruption / truncation**: 34 corrupted and 52 truncated datagrams; every
  one is rejected by the CRC/MAC at decode (SEC-01, DET-04) — the cost is a
  handful of will ticks, never an applied forged value, never a takeover.
- **Blackout of the order way**: 1200/1200 node-to-actuator datagrams dropped;
  no mandate is ever granted, the actuator serves the fallback law for 300/300
  ticks with the lamp lit, 0 violations in both verdicts.

## 6. Kernel recipe for two real boards — NOT EXECUTED

**This section was NOT executed anywhere in this repository.** `tc`/netem need
root; this environment has none. It is the procedure for a real run, to be
executed by whoever has the hardware and the rights. Two variants:

### 6.1 QNX guests under QEMU (the existing level-C layer)

The two QNX computers of `docs/TWO-BOARDS.md` run as QEMU guests on a Linux
host; each guest NIC is a host `tap` interface. Impair the tap on the host
(root), per direction, with the same parameters as section 3:

```sh
# loss 10 % on computer A's cable, both ways:
tc qdisc add dev tapA root netem loss 10%
# Gilbert-Elliott burst (good->bad 1 %, bad->good 25 %, loss 0.1 % / 95 %):
tc qdisc add dev tapA root netem loss gem 1% 25% 95% 0.1%
# asymmetric delay: 50 ms +- 5 ms only A -> actuator (egress of A's tap is
# host->guest; use the ifb device or impair the guest-facing direction):
tc qdisc add dev tapA root netem delay 50ms 5ms
# duplication 5 %, corruption 2 %:
tc qdisc add dev tapA root netem duplicate 5% corrupt 2%
# deep reordering: netem has no depth-N buffer; the closest kernel behaviour
# is a jitter wider than the mean (crossing delays reorder by themselves):
tc qdisc add dev tapA root netem delay 30ms 30ms limit 16
tc qdisc del dev tapA root    # remove everything
```

Check the exact gem parameter order against `man tc-netem` on the executing
host before the run.

### 6.2 Physical boards through a Linux impairment bridge

QNX 8.0 has no `tc`/netem, so on bare hardware the degradation lives in a
small Linux box bridging each computer to the actuator board (two NICs,
`10.0.0.x` per `docs/TWO-BOARDS.md`):

```sh
ip link add name br0 type bridge
ip link set ethA master br0 && ip link set ethACT master br0 && ip link set br0 up
tc qdisc add dev ethA root netem loss gem 1% 25% 95% 0.1%   # per direction,
tc qdisc add dev ethACT root netem delay 50ms 5ms           # as in 6.1
```

Truncation has no netem primitive; the user-space relay of this mission is the
reference for that one (or an `nftables`/`iptables` length-based drop can
approximate it). Physical link cuts remain the relay/unplug procedure of
`docs/TWO-BOARDS.md` section 4.

## 7. Limits

- User space, not kernel: see the framing above. Loopback, no real NIC.
- The wall-clock traffic of the bench is not reproducible at the datagram
  level; only the relay's decisions are (section 2). Two runs of one profile
  can differ by a few datagrams and, on the churn profiles, by the exact
  takeover instants. The safety verdict did not move across the two full
  campaigns and the partial campaign run while writing this page.
- `max_gap` is published per profile but the I4 *bound* is not claimed on a
  disturbed channel — that is the core's own rule, not a relaxation invented
  here (`core/src/invariants.c`, check_gap).
- The relay sees whole datagrams; it does not simulate fragmentation, MTU
  discovery, checksum errors on the wire, or NIC-level drops.

## 8. Reproduce

```sh
make -j4 all            # build the three real bench binaries + the verifier
make impair             # the whole campaign (~2 min), verdict IMPAIR-OK
python3 host/impair/run_impaired.py --profile burst-gilbert   # one profile
python3 host/impair/impair_proxy.py --profiles host/impair/profiles.json \
        --profile reorder-deep --selftest                     # determinism
./build/relais-verify build/impair/burst-gilbert/actuator.jsonl \
        --level complet --lossy-channel                       # safety verdict
./build/relais-verify build/impair/burst-gilbert/actuator.jsonl \
        --level complet                                       # strict, reference
```
