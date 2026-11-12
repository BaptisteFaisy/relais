# `transports/` — index of the transport layer (mission M32, idea 75)

This directory is an **index**, not a second home for the code. The mission that
created the transport layer names the files explicitly, and they live where it
says, next to the code they serve:

| what | where | why there |
|---|---|---|
| the interface | [`core/include/relais/transport.h`](../core/include/relais/transport.h) | the only place in the core that speaks of a medium, and it names none |
| UDP implementation | [`host/transport/udp.c`](../host/transport/udp.c) | the PC bench's wire, extracted unchanged behind the interface |
| CAN implementation | [`host/transport/can.c`](../host/transport/can.c) | classic CAN 2.0 with ISO 15765-2 segmentation |
| SocketCAN backend | [`host/transport/can_socketcan.c`](../host/transport/can_socketcan.c) | Linux-only, compiled when `<linux/can.h>` exists |
| memory transport | [`core/tests/transport_mem.h`](../core/tests/transport_mem.h) | test scaffolding, not part of the library |
| interface unit test | [`core/tests/test_transport.c`](../core/tests/test_transport.c) | built and run by `make test` |
| CAN test | [`host/transport/test_can.c`](../host/transport/test_can.c) | `make -j4 all` builds `build/test_transport_can` |
| UDP test | [`host/transport/udp_check.c`](../host/transport/udp_check.c) | `build/transport-udp-check`, drives the real `relais-node` |
| design and measurements | [`docs/PORT-TRANSPORTS.md`](../docs/PORT-TRANSPORTS.md) | the CAN vs SOME/IP choice, and every number |

```bash
make -j4 all && make transports
```

The property this layer exists to prove, measured rather than asserted: the same
200-tick scenario with a death and a handover gives **the same decision at every
tick** with no transport at all, over a memory transport, and over a fragmented
CAN bus — 0 violation of I1…I11 in all three runs, and the unmodified
`build/relais-node` binary driven over the new UDP transport accepts its mandate
and has 398 of 400 orders applied, 0 rejected, 0 forged.
