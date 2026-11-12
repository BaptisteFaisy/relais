# The two-machine demo, with a REAL QNX 8.0 as machine B

Mission **M49**. This document is the procedure, the measured numbers and the
honesty statement of `host/bench/qnx_demo.py`.

The previous version of this demo (`host/bench/vm_demo.py`) froze a **Linux**
guest and said so in its docstring: *"Honesty: the guest is Linux, not QNX"*.
This one runs a **real QNX 8.0** guest, built with the SDP 8.0 `mkqnximage`,
booted under **KVM**, and the machine inside it is the **same `core/` state
machine** as the host machine, cross-compiled with the SDP `qcc`.

## 1. What runs, and where

| Process | Where | What it is |
|---|---|---|
| `relais-actuator` | **host** | the actuator: tempo, mandate, rack, journal (the authority) |
| `relais-node --self A` | **host** | machine A: the `core/` state machine |
| `rl-node --self B` | **QNX 8.0 guest** | machine B: the *same* `core/` state machine, built for QNX |

The guest is a QEMU 8.2.2 virtual machine with **KVM** acceleration
(`-enable-kvm -cpu host`), its image built by the SDP 8.0 `mkqnximage`
(`host/bench/qnx_image.py`, reused as is).

## 2. The boot line: machine B starts BY ITSELF, at boot time

`image/fragments/rl-node-boot.sh` is injected in the image and launched by
`ifs_start.custom`, that is **by the guest's own start-up script**:

```
/proc/boot/ksh /proc/boot/rl-node-boot.sh T=10,p=3,N=8 40000 7100 7101 &
```

The script waits for the system partition (`/system/bin/ifconfig`), then for
the DHCP lease of the QEMU user-net (`vtnet0` gets `10.0.2.15`), and finally
execs the node:

```
exec /proc/boot/rl-node --self B --port 7101 \
    --actuator 10.0.2.2:7100 --peer 10.0.2.2:7101 \
    --params T=10,p=3,N=8 --ticks 40000
```

`10.0.2.2` is the **QEMU user-net gateway**: it IS the host. So machine B
sends its ORDRE to the host actuator at `10.0.2.2:7100`, and machine A (on
the host, `0.0.0.0:7101`) is reached at `10.0.2.2:7101`. In the other
direction the actuator publishes the STATUT to `127.0.0.1:7302`, which QEMU
forwards into the guest (`hostfwd=udp:127.0.0.1:7302-:7101`).

**No `--verbose` on the guest node**: the console of the guest is a serial
line, and one line per tick was enough to slow the node below the tick
period (measured on a loaded host: its replies then arrive late and the
actuator counts misses). The journal of the actuator is the authority.

## 3. The fault: a real freeze of the whole guest

The guest is stopped through **QMP**, not simulated:

```
> {"execute":"stop"}          -> << {"return": {}}   (query-status: "paused")
> {"execute":"cont"}          -> << {"return": {}}   (query-status: "running")
```

`stop` halts the guest CPUs, therefore its timers and its network: machine B
is silent exactly like a hung computer (spec 4.11). The QMP history is kept
in `docs/evidence/qnx-demo-qmp.json`, with the three screendumps taken
before, during and after the freeze.

## 4. The measurement

The takeover is timed on the **tap's** clock (the host monotonic clock), never
on the QMP round-trip: `pre_freeze_t_us` is the instant of the last STATUT
before the freeze, and the handover STATUT's `t_us` the instant the actuator
announced the new holder. The tap (`StatutTap` in `host/bench/qnx_demo.py`)
sits between the actuator and machine A, forwards every datagram verbatim and
decodes the documented header (spec 6.1); it implements no protocol.

## 5. Measured run (this host, 2026-11-12)

```
guest shell up after 14.7 s (accel=kvm)
guest_uname = QNX relais-m49 8.0.0 2026/06/05-16:26:03EDT x86pc x86_64
machine B (QNX guest) holds the mandate at tick 1469
machine A certified as the second at tick 1474
== QMP stop: the whole guest freezes ==
takeover at tick 1494, 49994 us after the freeze
```

What the journal (the authority) says about the freeze:

```
tick 1490  misses 0  holder B  source holder     <- the last fresh order of B
tick 1491  misses 1  holder B  source shadow     <- the guest is frozen
tick 1492  misses 2  holder B  source shadow
tick 1493  misses 3  holder B  source shadow
tick 1494  misses 0  holder A  source shadow     <- MANDATE_REVOKED(B) +
                                                    MANDATE_AUTO_SWITCH(A),
                                                    evidence = 4 misses
tick 1495  misses 0  holder A  source holder     <- A commands
```

* the actuator revoked the mandate of the frozen machine on **its own
  evidence**: 4 consecutive ticks without a valid order (`p+1 = 4`, MAN-04,
  DET-03 — the evidence travels with the event);
* it gave the mandate to the **certified second**, machine A (MAN-10,
  `mandate_auto_switch`, mandate 1 -> 2);
* the takeover is **4 ticks after the last fresh order of B**, that is
  **50.0 ms** of wall clock at T = 10 ms, measured on the host clock.

Verdict of the C verifier on the journal of that run:

```
ticks=2486 violations=0 max_gap=0 (bound 4) max_deviation=0 (bound 0.02)
fallback_run=964 lamp_delay=0 level=socle
```

`violations=0`: no safety invariant is broken, and the same verdict is
obtained with `--lossy-channel` (the mode for a channel that loses bursts).

## 6. Honesty

* **Machine B is a real QNX 8.0**, not a Linux guest and not a simulation:
  the image is built by the SDP 8.0 `mkqnximage`, the guest reports
  `QNX relais-m49 8.0.0 ... x86pc x86_64`, and the node inside it is
  `host/node/relais-node.c` cross-compiled with the SDP `qcc`. Nothing of the
  protocol is duplicated for the demo: the frames, the arbitration, the
  journal and the invariants are the `core/` ones.
* **The freeze is real**: the QMP `stop` pauses the guest CPUs (the
  `query-status` answers are in the evidence). The screendumps of the guest
  are the three frames of its VGA; this QNX image has **no graphics driver**,
  so they are blank and they do NOT prove anything by themselves — the proof
  of the freeze is the QMP status, the silence of the guest and the misses
  the actuator counted.
* **The times are the times of the emulator**, not hard real time. The
  actuator's tempo (T = 10 ms) is the clock of the protocol: the safety is
  expressed in **ticks** (4 ticks = p+1), and the microseconds are the host
  monotonic clock of the bench, which is a shared and loaded machine
  (`loadavg` is recorded in the summary). The takeover latency of a real
  system would be read on the target's clock, not on the host's.
* **The journal is not sealed** when the actuator is stopped before its last
  tick: the last buffered record is truncated (the reader counts it as
  malformed, `malformed_lines: 1` in the summary). The records of the
  takeover are far from the tail and are complete.
* **The bench is shared**: at the time of the run the host carried a load
  average of about 20 on 12 cores, and the guest's orders were occasionally
  one tick late. The actuator absorbs that (that is what `p` is for), but it
  shows in the journal: a few `order_rejected` events with the reason
  `TICK_MISSED` before the freeze, and a certification that is gained and
  lost a few times. The takeover itself is exactly `p+1` ticks.
* **The certification flickers at T = 10 ms**: the law is tuned for T = 5 ms
  (the agreement band `eps` is crossed when the slew limit saturates), so at
  T = 10 ms the second is certified, then decertified, then certified again
  a few times. The demo waits for the pair to be stable (holder B, certified
  second A) for 15 ticks before freezing. At T = 5 ms the certification is
  rock solid, but the guest then cannot answer inside the tick on a loaded
  host and the invariant I4 (the gap between two fresh orders) is broken —
  the timing hypothesis of the specification (spec 7.3 item 5) is not met by
  a shared, loaded machine.

## 7. How to run it

```sh
. ~/qnx800/qnxsdp-env.sh          # the SDP 8.0 (qcc, mkqnximage)
cd /tmp/rl-w49
make qnx-demo                     # builds the image, boots the guest, freezes it
```

It needs `/dev/kvm` (otherwise the demo falls back to TCG and says so in
`accel`), the SDP 8.0, and it boots **one** VM, which is stopped (QMP
`quit`) before the target returns: no orphan is left behind.

Useful options: `--params`, `--ticks`, `--stable-ticks`, `--out`,
`--evidence`, `--figures`.

## 8. The files

| File | What it is |
|---|---|
| `host/bench/qnx_demo.py` | the orchestrator (image, boot, arm, freeze, takeover, verdict) |
| `image/fragments/rl-node-boot.sh` | the boot line of machine B, inside the guest |
| `host/bench/test_qnx_demo.py` | 12 tests that need no QEMU (boot line, QEMU command, STATUT decoding, console and journal fixtures) |
| `docs/evidence/qnx-demo-actuator.jsonl` | the journal of the actuator (the authority) |
| `docs/evidence/qnx-demo-nnx-demo.json` | the numbers of the run |
| `docs/evidence/qnx-demo-qmp.json` | every QMP command and answer |
| `docs/figures/qnx-demo-proof.svg` | the command curve, the source of every tick, the mandates, the verdict |
| `docs/figures/qnx-demo-guest-{before,frozen,after}.png` | the screendumps of the guest |

Requirement IDs: QNX-09 (a real QNX node on the wire), INJ-07 (the fault is
injected through QMP), PERF-02 (the takeover is measured, never invented),
OBS-04 (the console transcript and the screendumps are the evidence),
MAN-04/MAN-10/DET-03 (the revoke happens on the actuator's own proof).
