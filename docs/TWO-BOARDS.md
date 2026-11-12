# Two real boards over Ethernet (REP-05, INJ-05)

This page is a *procedure*, not a run: it describes how to move the Relais
demonstration from emulation onto two physical computers linked by a real
Ethernet cable, with a third board playing the actuator, and what you should
expect to differ from the QEMU emulation. Nothing here is executed on this
repository; you run it on your own hardware.

## 1. What you need

- **Two computers** (board A, board B). Any two machines that can run the QNX
  8.0 image — see `image/README.md` — and have one Ethernet port each.
- **One third board** for the actuator. Physically separate, so that cutting
  power or unplugging it is a real arbitration outage, not a process kill.
- **Two Ethernet cables** plus a switch, or a **crossover cable** for a direct
  pair; whatever medium you choose, the two computers only ever talk to the
  actuator, never straight to each other (the actuator is the hub).
- **One way to cut a link physically**: a relay in series on the actuator
  cable (controllable), or simply the discipline of unplugging the connector by
  hand at a marked instant. A relay is repeatable and is what the campaign
  assumes; a hand-unplug is the "real fault" sanity check.

## 2. Wiring

```
              ┌────────────┐
   computer A │  en0 .10   │──┐
              └────────────┘  │   (cable, can be cut by relay or unplug)
                              ▼
                      ┌────────────┐         ┌────────────┐
                      │ actuator   │─────────│ computer B │
                      │  board .1  │         │  en0 .20   │
                      └────────────┘         └────────────┘
```

The actuator is the only node every other node reaches directly; A and B never
have a link to each other. This matches the emulation, where the two guests
talk through the actuator (spec 1.4, the actuator is "arbitre ET horloge").

## 3. Addresses and identity

| Board | MAC (image/fragments/identity.sh) | IPv4 | Role |
|---|---|---|---|
| A | `52:54:00:12:35:0A` (or your own) | `10.0.0.10` | computer, `RELAIS_SELF=A` |
| B | `52:54:00:12:35:0B` (or your own) | `10.0.0.20` | computer, `RELAIS_SELF=B` |
| Actuator | — | `10.0.0.1` | tempo + arbitration + journal |

The same image boots both computers and tells them apart by MAC address
(`identity.sh`) — swap the cables and the roles swap, which is the point of a
redundancy demonstrator (CMD-03). Give the actuator its own image/process; at
level C it is a third guest, at level S it can be a PC process.

## 4. Cutting the link (the fault we actually inject)

| Fault | How to produce it on real hardware | What you measure |
|---|---|---|
| death of a computer | power it off | mandate gap ≤ p+1, fallback in time (I4, I8) |
| freeze | `SIGSTOP`-equivalent / halt the machine, keep power | handover after p+1 missed ticks (DET-03) |
| link cut | open the relay or unplug the cable | frames stop; the miss counter does its job (ACT-09) |
| actuator outage | power off the third board | both computers must reach the fallback law (I8) |

## 5. What to measure

For each fault, at a known instant and with a period `T` you set (the QNX
image claims `T = 5 ms`; on a slow or shared board raise it — `CMD-07`):

1. **Gap** between two fresh orders after the holder dies: ≤ p+1 ticks (I4).
2. **Deviation** from the shadow while the will serves the handover (I5).
3. **Handover evidence**: the mandate changes only on the actuator's own proof
   (p+1 silent ticks) — no handover on mere suspicion (DET-03).
4. The actuator journal (`/dev/relais/status`, JSON-lines) agrees with the
   verifier's verdict — record it, replay it with `relais-verify`.

Capture the traffic on both cables (a hub or a monitor port) so the frames can
be compared with the emulated pcap (`-object filter-dump` in `QEMU.md`).

## 6. Expected differences from the emulation

The emulation is honest about these, and so should your report be:

| | Emulation (QEMU) | Real boards |
|---|---|---|
| Latency | modeled as a discrete delay, configurable | physical: NIC + driver + wire, on the order of microseconds for a direct link |
| Jitter | zero unless you inject it | real: interrupt coalescing, scheduling, switch store-and-forward |
| The fault itself | a clean `stop`/`set_link` | a relay bounce or a torn connector: partial frames, carrier glitches, bursts |
| Clock | one host clock shared by both guests | two independent oscillators drift apart (the protocol does not depend on a shared clock, spec 4.2) |
| Common cause | a load spike hits both machines (documented limit) | a power glitch can still hit both if they share a PSU — isolate them |

Concretely, expect the measured gap/deviations to be *no better* than the
emulated numbers, and the jitter to be *wider*; the safety invariants (I1, I2,
I3, I6, I8, I10, DET-03) must still hold everywhere — that is the claim, and
the real bench is where it is finally earned.