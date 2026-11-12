# Relais — what a real-time bus would change (idea 30)

**Standing of this document.** An architectural comparison, not a measurement.
No bus hardware is present on this machine; the numbers below are the published
characteristics of the standards, each cited. The Relais protocol itself would
**not change**: every claim here is about the substrate under `core/`, which stays
pure C11 and identical in the QNX processes, the simulator and the explorer.

## 1. The assumption the protocol carries today

The Relais protocol runs over UDP and uses the actuator as the clock: no machine
synchronises to anything, every frame names the tick it targets, and the machines
wake on the arrival of `STATUT`. The one timing assumption is the round-trip
constraint of spec §4.2 — `2Δ + C + J ≤ T` (two network delays, the compute time,
the jitter, all inside the period T). It is measured, not assumed (probe E2/E5 on
the target; the bound the safety argument relies on is `p+2` ticks between two
fresh orders, which holds even if that assumption fails). Sources: `docs/SPEC.md`
§4.2, `docs/SAFETY.md` §2.

A time-triggered bus would make that assumption unnecessary; a classic automotive
bus would make it harder to meet. What follows is what each choice buys and costs.

## 2. Time-triggered buses (TTP, FlexRay)

### What the protocol would gain

- **Guaranteed latency bounds.** Both TTP (SAE AS6003) and FlexRay (ISO
  17458-1..5) schedule every message into a reserved slot of a fixed cycle. A
  message's transmission time is known at configuration, so `2Δ + C + J` is a
  property of the schedule, not of the load. The patience `p` of Relais could be
  reduced, and the `p+2` fresh-order bound tightens toward the schedule's cycle.
- **Built-in redundancy.** FlexRay has two independent channels (A/B); TTP is
  defined for dual-channel. A single cable cut no longer costs a tick — which is
  exactly the fault Relais currently absorbs by patience, not by hardware.
- **Fault-tolerant clock synchronisation.** TTP/FlexRay maintain a common time
  base across nodes, so the "which tick is it" question is answered by the bus.

### What the protocol would lose

- **Cost and tooling.** A TTP/FlexRay controller, its message descriptor lists
  (MEDL for TTP, the FlexRay configuration database), and the scheduling tools.
  These are automotive-grade, priced for a production line, not for a
  demonstrator. (Sources: the SAE AS6003 and ISO 17458 standard families.)
- **Rigidity.** The schedule is computed offline; adding a message or changing a
  period means re-scheduling the whole network. Relais today needs no schedule —
  the period T is a parameter of `relais_params` and the frame names its tick.
- **The clock synchronisation Relais deliberately avoids.** This is the real
  cost. TTP/FlexRay require every node to agree on a global time (a gossip
  protocol in TTP, cold-start + integration in FlexRay). Relais does the
  opposite on purpose: the actuator distributes the *tick number*, and each
  machine wakes on `STATUT` arrival, so no node needs a clock that matches any
  other. A frozen guest (QEMU `stop`/`cont`) does not desynchronise anything,
  because nothing is synchronised. Trading that simplicity for a shared time base
  is a step backwards for this project's thesis.

## 3. Classic automotive buses (CAN, SOME/IP)

### CAN / CAN FD

- **What it gives:** ubiquity and cost. One physical layer, arbitration by
  priority, up to 1 Mbit/s (CAN FD raises the data phase). Source: ISO 11898-1.
- **What it takes:** arbitration is CSMA/CR, so a busy bus delays a low-priority
  message by an *unbounded* number of higher-priority ones — the opposite of a
  time-triggered schedule. The `2Δ + C + J ≤ T` assumption would hold only at a
  low bus load and with priority carefully assigned, and a burst of traffic can
  break it. CAN also carries no notion of a tick, so Relais would still need its
  own tick label — the bus would add cost without removing the assumption.

### SOME/IP (AUTOSAR)

- **What it gives:** service-oriented RPC over Ethernet, the current automotive
  standard for high-bandwidth in-vehicle networks. Source: AUTOSAR SOME/IP
  specification.
- **What it takes:** SOME/IP is a service layer, not a time-triggered transport;
  its determinism comes from the Ethernet layer underneath (TSN / switched, or
  FlexRay). SOME/IP over plain UDP is best-effort — the same assumption Relais
  already carries, plus a heavier stack. It would not tighten `2Δ + C + J ≤ T`
  on its own.

## 4. Why UDP + actuator-as-clock stays defensible

The demonstrator's thesis is the **protocol**, not the wire:

1. **The safety bound does not come from the bus.** The actuator is the clock
   *and* the arbiter; every order names its tick and its mandate, and a mandate
   is revoked only on the actuator's own evidence (MAN-04). A time-triggered bus
   would remove the *assumption* `2Δ + C + J ≤ T`, but the `p+2` fresh-order
   bound and the "never two mandates for the same tick" invariant hold without
   it — they are properties of `core/`, proven by the 10-million-state
   exploration, not of the transport.
2. **No clock synchronisation is a feature, not a gap.** A protocol that never
   needs nodes to agree on a time is simpler to reason about and survives a
   frozen guest, a lost frame or a reordered one without a protocol change.
   TTP/FlexRay buy determinism at the price of that synchronisation.
3. **The bus is swappable; the core is not.** `core/` has no socket, no clock, no
   file, no allocation (rule 2). Moving it onto FlexRay or TTP is a transport
   change in `target/net.c` and a config change in the schedule — the arbiter,
   the invariants and every existing proof stay byte-identical. The right answer
   for a production car is "measure the bus, then decide"; the right answer for
   the demonstrator is "UDP + actuator clock, and say so".

## Sources

- Relais: `docs/SPEC.md` §4.2 (`2Δ + C + J ≤ T`), §4.3 (mandate), `docs/SAFETY.md`
  §2 (switch-over principle), `docs/PROTOCOL.md` (bounds).
- CAN: ISO 11898-1 (data link layer); Bosch CAN specification.
- FlexRay: ISO 17458-1..5 (dual-channel, time-triggered, clock synchronisation).
- TTP: SAE AS6003 (time-triggered protocol, dual-channel, gossip synchronisation).
- SOME/IP: AUTOSAR specification, "SOME/IP Protocol Specification" (service layer
  over Ethernet; determinism from the underlying transport).
- QNX-08 / probe E2, E5 (`host/bench/QEMU.md`) for the measured latency on target.