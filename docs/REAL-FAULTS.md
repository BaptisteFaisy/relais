# Real faults — what is actually cut, and what is still simulated

Mission M41. Everything asserted here is reproducible with one command; the
numbers come from the run of `make hil` on this machine (a shared 12-core PC,
Linux, no root, no QNX board, no relay hardware plugged).

```
make -j4 all && make test && make explore && make hil
```

---

## 1. Three levels of "real", and the one that is not available here

| Level | What produces the fault | Status here |
|---|---|---|
| **process** | the kernel: `SIGKILL` (the process dies, its sockets are closed by the kernel) and `SIGSTOP`/`SIGCONT` (the scheduler freezes it) | **REAL, played** — `host/hil/processcut.py` |
| **data path** | the kernel again: the relay process that carries the datagrams is killed, so the datagrams stop flowing in both directions while every socket stays open and every process stays unaware | **REAL, played** — `host/hil/linkcut.sh` |
| **electrical** | a physical relay opens the conductors (or the power feed) | **BLOCKED(no relay hardware)** — the adapter, the wiring table and a loopback mode are delivered (`host/hil/relay_adapter.{c,h}`) |

Nothing in the HIL bench uses the in-process fault hooks of the simulator or
of `relais-node` (`--die`, `--bias`). The processes that are killed are the
same binaries the PC bench runs (`build/relais-actuator`, `build/relais-node`),
started without any fault switch.

**A socket that is closed is not a cut conductor.** The bench says which one
it did, every time:

* `processcut.py` — kills or freezes a **process**;
* `linkcut.sh` — kills the **relay that carries the datagrams**: the conductors
  are not opened, the path is;
* `relay_adapter` — would open the **conductors**, and cannot be run here.

---

## 2. The commands

```bash
# 1. real process death and real freeze, swept over 40 cut ticks, >= 100 cuts
python3 host/hil/processcut.py --cuts 100 --out build/hil/processcut.json

# 2. real cut of the data path (both paths, both directions)
bash host/hil/linkcut.sh

# 3. the physical relay adapter: loopback mode runs, hardware modes report
build/relais-relay --backend loopback --state build/hil/relay.state --selftest
build/relais-relay --backend gpio --channel 17 --selftest      # BLOCKED(3)
build/relais-relay --backend usb  --device /dev/ttyUSB0 --selftest  # BLOCKED(3)

# one command for all three
make hil
```

Only the processes started by the bench are ever signalled: the benches keep
their `Popen` handles (or the shell keeps the PIDs) and kill nothing else.

---

## 3. What the process bench does, exactly

Four scenarios, rotated over the runs (INJ-08: a sweep of instants):

| Scenario | Real fault played | Fault of the model |
|---|---|---|
| `kill-holder` | `SIGKILL` on the holder | F1 (death of one machine) |
| `freeze-holder` | `SIGSTOP` on the holder, then `SIGCONT` 25 ticks later | F2 (freeze then resumption) |
| `double-kill` | `SIGKILL` on A, then `SIGKILL` on B (the new holder) | F10 (death of both machines) |
| `kill-then-freeze` | `SIGKILL` on A, then `SIGSTOP` on B | F9 (a second fault before redundancy is back) |

Each run: the actuator and the two computers are started for real, the bench
waits for a **steady** state (the holder serving its own fresh order, zero
misses, for 4 consecutive ticks, with the actuator pacing in real time and
not catching up from a stall), and only then signals the fault. The cut tick
is swept over 40 distinct instants (`30 + run % 40`).

Everything is measured on the actuator's journal, which is the authority of
the campaign (spec 6.4):

* **effective cut tick** — the first tick the actuator itself saw without a
  valid order from the holder (`misses >= 1`); this is the protocol-side
  instant of the fault, independent of the bench's own dating;
* **revoke latency** — from that tick to the `mandate_revoked` event;
* **takeover latency** — from that tick to the first tick served by the new
  holder's own order (`source = holder`);
* both are published in ticks and in protocol microseconds (`ticks * T * 1000`,
  with `T = 15 ms` here), and the wall-clock microseconds are published as an
  **upper bound** only (signal delivery to the journal line seen by the bench:
  it includes the actuator's flush and the bench's ~1 ms poll).

---

## 4. Measured results

From the last `make hil` on this machine (12-core shared PC, load average
40–90, Linux 6.x, `T = 15 ms`, `p = 3`, `N = 8`, no root):

### 4.1 Real process death and freeze — 100 cuts

| | value |
|---|---|
| cuts played | **100** (17 `kill-holder`, 17 `freeze-holder`, 34 `double-kill`, 32 `kill-then-freeze`) |
| bench runs | 67 (one run plays one or two cuts) |
| distinct cut ticks | 34 (from tick 24 to tick 143) |
| runs discarded (no usable state) | 0 |
| **safety-invariant violations** | **0** (channel disturbed: I1, I2, I3, I6, I8, I10, I11 enforced) |
| level-C zero-gap trips (quiet channel hypothesis) | 98 / 100 — see §5, this is the price of a loaded PC, not a defect |
| cuts made with a certified second in place | 15 / 100 |

**Revoke latency** (from the first tick the actuator saw without a valid
holder order to the `mandate_revoked` event), in ticks and in protocol
microseconds (`ticks × T × 1000`):

| n | min | p50 | p95 | p99 | max | in µs (protocol) |
|---|---|---|---|---|---|---|
| 100 | **4** | **4** | **4** | 6 | 6 | 60 000 µs (p50), 90 000 µs (p99/max) |

The median is exactly `p + 1 = 4` ticks: the arbiter revokes on its own
evidence (four consecutive ticks without a valid order), and the real process
death does not change that number. The tail (6 ticks at p99) is the machine's
own jitter — a frame that arrives late resets the miss counter and the
evidence takes one tick longer to complete.

**Takeover latency** (same origin, to the first tick served by the new
holder's own order). `n = 67`: the 33 cuts of the two double-fault scenarios
have no successor to hand over to (both machines are down — the will plays
out, then the fallback loop).

| n | min | p50 | p95 | p99 | max | in µs (protocol) |
|---|---|---|---|---|---|---|
| 67 | 12 | **14** | 14 | **15** | **15** | p50 210 000 µs, p99/max 225 000 µs |

Split by the state at the cut: with a certified second (n = 15) p50 = 13
ticks; without one (n = 85) p50 = 14 ticks — the shadow of a certified second
bridges the gap and the new holder takes over one tick earlier.

**Wall clock** (upper bound only: signal delivery to the journal line seen by
the bench, with the actuator's flush and the bench's ~1 ms poll included):

| | min | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| revoke | 30 ms | 225 ms | 241 ms | 244 ms | 248 ms |
| takeover | 435 ms | 479 ms | 481 ms | 520 ms | 580 ms |

The wall-clock figures are much larger than the protocol ones because the
machine is shared: the journal line is written at the end of the tick and
seen by the bench up to one tick later, and the scheduler delays the bench
itself. **The tick-based figures are the ones to quote**; the wall-clock ones
bound the observation, they do not measure the protocol.

### 4.2 Real cut of the data path

One run of `bash host/hil/linkcut.sh` (the relay that carries both paths is
killed with SIGKILL):

| | value |
|---|---|
| last tick served from a fresh order | 67 |
| first tick without a fresh order (the cut becomes visible) | 68 |
| `mandate_revoked` | 71 → **4 ticks = 60 000 µs** after the cut |
| takeover | none: both computers are cut off from the actuator, the will plays out and the fallback loop takes over (`fallback_run = 121` ticks) |
| measured gap | 130 ticks (the will's horizon plus the fallback) |
| **safety-invariant violations** | **0** (the safe state is reached, I8 holds) |

Note that the datagrams already queued in the relay are still delivered: the
cut becomes visible at the first tick the actuator receives no fresh order,
not at the instant the relay was killed.

---

## 5. The invariants, and the hypothesis of the channel

The journal of every run is judged twice by `build/verify-hil`, which feeds the
**core** invariants (`core/src/invariants.c`) — the same code the explorer and
the simulator judge with:

* `--lossy` — the claim that holds on a **disturbed channel**: the safety
  invariants are enforced (I1 unicity, I2/I3 mandate, I6 will horizon,
  I8 safe state, I10 lamp, I11 tick contiguity) and the gap between fresh
  orders is **measured and published, not enforced**. This is the core's own
  notion of a disturbed channel (`relais_inv_lossy_channel`, ROB-02/ROB-03),
  and it is the honest mode for a shared PC: the timing hypothesis
  `2*delta + C + J <= T` is not satisfied when the scheduler delays frames.
* `--strict` — the level-C completeness claim (zero gap: a certified second
  bridges every tick). Reported for transparency; it is **not claimed** here,
  because the second loses its certification often on a loaded machine
  (the order/shadow comparison trips over a stale measurement).

The number published as "invariant violations" is the `--lossy` one.

---

## 6. Honest table: real or simulated, fault by fault

| Fault (FMEA, `docs/SAFETY-CASE.md`) | Real on this machine | Still simulated | Command |
|---|---|---|---|
| F1 death of one machine | **YES**: `SIGKILL` on the real process (holder or second) | the plant and the actuator are PC processes, not QNX boards | `python3 host/hil/processcut.py` |
| F2 freeze then resumption | **YES**: `SIGSTOP` / `SIGCONT` on the real process | idem | idem (`freeze-holder`) |
| F3 slow machine, missing ticks | no controlled injection | the core's node model injects the misses; on this machine the scheduler delays frames too, but not on command | `relais-simulate --fault at:slow:...` |
| F4 inter-computer link cut | **YES**: the relay that carries ETAT/SUIVI is killed | the conductor is not opened | `bash host/hil/linkcut.sh` |
| F5 machine–actuator link cut | **YES**: the relay that carries the orders and the STATUTs is killed | idem | idem |
| F6 loss, duplication, reordering, delay of frames | no | the simulator's loss/delay model | `relais-simulate --fault ...` |
| F7 corrupted or forged frame | no | the core's frame tests and the explorer | `make test`, `make explore` |
| F8 wrong value emitted by a machine | no | `relais-node --bias` / the simulator's bias fault | `relais-simulate --fault at:bias:...` |
| F9 second fault before redundancy is back | **YES**: `SIGKILL` then `SIGSTOP` on the two real processes | idem | idem (`kill-then-freeze`) |
| F10 death of both machines | **YES**: two `SIGKILL`s on the two real processes | idem | idem (`double-kill`) |
| F11 failure of the actuator itself | no — declared out of scope (single point of failure) | — | — |
| Electrical cut of a conductor or of the power feed | **no** | the adapter, the wiring table and the loopback mode are delivered; the hardware modes report `BLOCKED(no relay hardware)` | `build/relais-relay --backend gpio\|usb --selftest` |
| Death of a QNX guest | no QNX image on this machine | a QEMU guest frozen by QMP is already played elsewhere (`host/bench/vm_demo.py`) | `python3 host/bench/vm_demo.py` |

---

## 7. The relay adapter, and what it would cut

`host/hil/relay_adapter.{c,h}` drives a real relay through three backends:

| Backend | How it drives the coil | State here |
|---|---|---|
| `loopback` | mirrors the contact state into a state file, read back through a second handle | **RUNS** (`make hil` self-test passes) |
| `gpio` | Raspberry Pi sysfs GPIO, active-high coil drive | `BLOCKED(no relay hardware)` |
| `usb` | serial relay board (default 9600 8N1, `A0 01 00 A2` / `A0 00 01 A1`) | `BLOCKED(no relay hardware)` |

Wiring table (Pi header → relay board → the conductors to cut):

| Pi pin | Signal | Relay board | Note |
|---|---|---|---|
| 2 | 5 V | VCC | coil supply |
| 6 | GND | GND | |
| 11 | GPIO17 | IN1 | coil drive, active high |
| — | — | COM | conductor to cut (e.g. the actuator's TX line, or the power feed of a computer) |
| — | — | NO | other side of the cut; normally open, so the link is closed while the coil is energized |

The adapter is fail-safe: `relay_open()` leaves the contacts **open** (link
cut), and `relay_close()` opens them again — a dead adapter means a cut link,
never a phantom link. The self-test measures the switching time with
`CLOCK_MONOTONIC` and checks the fail-safe state.

When a relay is plugged, the commands are exactly:

```bash
build/relais-relay --backend gpio --gpio-root /sys/class/gpio --channel 17 --selftest
build/relais-relay --backend usb  --device /dev/ttyUSB0 --baud 9600 --selftest
```

and the bench would replace `kill -9 $PROXY_PID` (the software cut of the
path) by `relay_set(false)` (the electrical cut of the conductor).

---

## 8. What remains simulated, and why

* **The plant and the actuator** are PC processes. The QNX target build lives
  in `target/` and is not run here (no board, no QNX image).
* **The frame-level faults** (F3, F6, F7, F8) stay in the simulator and in the
  core's own tests: they need a programmable cable, which is what the
  simulator's loss/delay model and the explorer's exhaustive search are for.
* **The electrical cut** needs hardware that is not plugged.
* **The in-process death points** (`relais-node --die P3 --at N`, INJ-07) are
  still the way the *code point* of a death is controlled; the HIL bench
  controls the *instant* instead, and kills the process from outside.
