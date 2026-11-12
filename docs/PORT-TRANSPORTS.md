# Relais transports — the protocol does not know what carries it (mission M32, idea 75)

Until now the wire was UDP and that was an *implicit* fact: the socket calls
lived inside `host/node/relais-node.c`, `target/net.c` and
`actuator/pc/relais-actuator.c`. This mission makes the dependence explicit and
then removes it.

**One new file inside the core, and it is only an interface:**
`core/include/relais/transport.h` — 138 lines, no clock, no file, no socket, no
allocation, nothing that names a medium. No existing file of `core/` was touched:
not `arbiter.c`, not `plant.c`, not `control.c`, not `params.*`.

```c
typedef struct relais_transport {
  const char *name;                     /* "udp", "can", "memory" */
  void *ctx;                            /* owned by the caller and the impl */
  int (*init)(void *ctx, const relais_tr_cfg *cfg);
  int (*send)(void *ctx, relais_link link, const uint8_t *frame, size_t len);
  int (*recv)(void *ctx, relais_link *link, uint8_t *buf, size_t cap, size_t *len);
  void (*close)(void *ctx);
} relais_transport;
```

A transport moves **opaque, already-encoded frames**. Authentication, CRC and the
frame shape stay in `core/src/frame.c`, so a new transport cannot weaken them
(SEC-01, SEC-02, DET-04). Two links, because spec 4.9 has exactly two directions:
`relais_link_of(type)` is a one-liner over the core's own `relais_msg_to_peer()`.

## Three implementations

| file | medium | tested here |
|---|---|---|
| `core/tests/transport_mem.h` | static queues in memory | **yes**, 70 checks |
| `host/transport/udp.c` | POSIX UDP sockets | **yes**, loopback + interop with the real node |
| `host/transport/can.c` | classic CAN 2.0, segmented | **yes**, on a memory bus; SocketCAN backend compiled, not run |
| `host/transport/can_socketcan.c` | Linux SocketCAN | compiled with `<linux/can.h>`; running needs `CAP_NET_ADMIN` |

## What was measured, on this machine

### 1. The interface, and transport independence (`make test` → `build/test_transport`)

```
OK 70 checks
wired: 1044 frames over the memory transport, 0 rejected,
       sources holder=197 shadow=2 testament=0 fallback=1, violations=0
```

The decisive test runs the **same 200-tick scenario twice** — two computers, one
actuator, the rack model, a death at tick 120 and the handover that follows —
once with frames passed by direct call, once with every frame encoded, queued,
received, authenticated and decoded. Value, source, mandate, holder, misses,
lamp, cause and every journal event are compared tick by tick: **200/200
identical**, and the core's own invariant checker (I1…I11) reports **0 violation**
in both runs. Tampering is checked too: one flipped bit in the payload is refused
by the MAC (`REJ_CHECK`), one in the header by the shape check (`REJ_MALFORMED`),
and neither reaches the state machine.

### 2. The same scenario over fragmented CAN (`build/test_transport_can`)

```
OK 77 checks
can: 1238 Relais frames -> 10097 CAN frames on the bus (8.16 CAN frames per
     Relais frame), 0 lost flows, 8533 foreign, violations=0
can: sources holder=197 shadow=2 testament=0 fallback=1
```

Three runs of the scenario — no transport, memory transport, CAN — produce
**identical decisions on all 200 ticks**, with 0 invariant violation. Per message:

| Relais message | bytes | CAN frames |
|---|---|---|
| CESSION | 33 | 5 |
| OMBRE | 37 | 6 |
| STATUT | 56 | 9 |
| ORDRE | 69 | 10 |
| DEMANDE | 75 | 11 |
| ETAT | 75 | 11 |
| worst case (512 B frame) | 512 | 74 |

Two faults of the CAN layer were found by this test, not by reading: a single
reassembly buffer per endpoint (on a shared bus the flows of two senders
*interleave* — 88 of 200 ticks diverged before the fix, which became one channel
per identifier, as ISO-TP does), and a wrong receive filter table.

### 3. UDP for real (`build/transport-udp-check`)

```
== 1. two UDP transports on 127.0.0.1 ==
  3000/3000 frames made the round trip, byte for byte, in 18 ms
  round trip over 3000 frames of 69 bytes: min 0.005 ms, median 0.005 ms,
  p99 0.009 ms, max 0.046 ms, mean 0.006 ms
  throughput: 11.76 MB/s each way (payload only)
== 2. the unmodified build/relais-node, driven by the transport ==
  400 ticks in 551 ms (1.38 ms per tick, period 5 ms): 400 STATUT sent,
  397 ORDRE received, 397 ETAT and 398 SUIVI seen by the peer
  arbiter: 398/400 ticks applied the holder's order, mandate 1, holder A,
  0 rejected, 0 forged
UDP-TRANSPORT-OK
```

The second phase is the "the refactor changed nothing" proof: the harness binds
the actuator endpoint through the **new** interface and starts the **existing,
unmodified** `build/relais-node` binary as a child process. The node gets the
mandate, its orders are accepted on 398 of 400 ticks, no frame is rejected, no
frame fails the MAC, and the replicated state really reaches the peer link.

`host/node/relais-node.c` and `actuator/pc/relais-actuator.c` were deliberately
**not** rewritten to use the interface in this mission: they are owned by other
missions, and the equivalence above is measured against them as they are.
`host/transport/udp.c` reproduces their socket behaviour exactly (SOCK_DGRAM, one
socket for both links, `INADDR_ANY` bind for a computer and loopback for the
actuator, `SO_RCVTIMEO` when a timeout is configured and `MSG_DONTWAIT` when it
is 0, a send failure counted and never retried).

## CAN vs SOME/IP — the choice and what it costs

`host/transport/can.c` implements **classic CAN 2.0** (11-bit identifier, 8-byte
payload) with ISO 15765-2 style segmentation, not SOME/IP over automotive
Ethernet.

Gains: **bounded latency** — a CAN frame is at most ~130 bits and the identifier
fixes the arbitration order, so the actuator link always wins the bus, while
SOME/IP inherits switch and queueing delays that are harder to bound on a shared
trunk; **no IP stack**, no DHCP, no port — one identifier per direction is the
whole addressing, which is auditable (SEC-05).

Costs: **frame size** — a Relais frame is 33…512 bytes and a CAN payload is 8, so
everything is segmented (8.16 CAN frames per Relais frame measured above, i.e. the
bus load is roughly ×8; at 500 kbit/s and ~130 bits per CAN 2.0A frame, a 69-byte
ORDRE costs 10 frames ≈ 1300 bits ≈ 2.6 ms of bus time, which is **52 % of a 5 ms
tick** — the honest conclusion is that classic CAN forces a slower tick, a
shorter testament, or CAN FD); **one identifier per direction, not
per message** — the identifier is spent on routing and priority rather than on the
message type. Giving each of the seven types its own identifier would refine the
arbitration, at the cost of peeking into the frame header, which this layer
refuses to do: frames stay opaque, exactly as over UDP.

The identifier plan used by the tests gives the actuator the lowest identifiers
and, on every node, the actuator link a lower identifier than the peer link, so
ORDRE and STATUT beat ETAT and SUIVI in every arbitration.

## Reproduce

```bash
make -j4 all          # builds build/test_transport, build/test_transport_can,
                      #        build/transport-udp-check, build/transport/can_socketcan.o
make test             # includes build/test_transport (70 checks)
make transports       # the three proofs in one command
```

`make all` prints whether the SocketCAN backend was compiled against a real
`<linux/can.h>` on this host, or skipped.

## What is *not* claimed

No CAN controller and no vcan interface was available
(`ip link add dev vcan0 type vcan` answers *Operation not permitted* here), so
the CAN numbers come from a memory bus that models a shared medium — including
foreign traffic, interleaved flows and lost fragments — and not from a wire. The
UDP numbers are real, on loopback, with the real node binary.
