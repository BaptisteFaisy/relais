# The QNX 8.0 target layer

Four processes and one actuator, all thin. Everything that decides anything —
the mandate, the arbitration, the control law, the frames, the journal, the
invariants — lives in `core/`, is pure C11, and is the same code that passes the
unit tests, the exhaustive exploration and the fault campaign. **Nothing here
re-implements a line of protocol.**

```
        UDP frames                      QNX messages                 QNX messages
 links <--------->  relais-net  <------------------------>  relais-core  <-------->  relais-ctl
                    prio 62                                  prio 61                 prio 60
                    the only process                         the resource manager    the control law
                    that owns a socket                       /dev/relais, the        as a service
                                                             protocol state machine
                                                                    ^
                                                                    |  /dev/relais/{status,temoin,ctl}
                                                              relaisctl (prio 10)
```

| Process | Role | Priority | Requirement |
|---|---|---|---|
| `relais-net` | owns the sockets, validates the shape and the authenticator of every frame, hands the rest to the core | 62 | SEC-05, DET-04, CMD-04 |
| `relais-core` | the protocol state machine, the role, the replica; serves `/dev/relais` with `open`, `read` and `write` only | 61 | QNX-01, MAN-*, REP-*, OBS-02 |
| `relais-ctl` | the control law as a service: three functions, no socket, no device, no clock | 60 | CMD-09, CMD-03 |
| `relaisctl` | the service tool: status, lamp, handover, and the bench's test commands | 10 | REL-09, INJ-07, INJ-10 |
| `relais-actuator` | the actuator in a third guest (level C): tempo by pulse, arbitration, journal | 61 | ACT-07, ACT-08, MAN-11 |

Why three processes for one machine: a crash is *known*, not guessed. When
`relais-net` dies, `relais-core` sees its connection close — no polling, no
timeout — and cedes the mandate within the tick (DET-05, QNX-05). The message
passing carries priority inheritance, so the critical path from a received
`STATUT` to an emitted order never waits behind something less urgent, and a
kernel trace shows it (QNX-04, PERF-07).

## Building

```bash
source ~/qnx800/qnxsdp-env.sh     # QNX SDP 8.0, through QNX Everywhere
make -C target                    # qcc -V gcc_ntox86_64 -> target/build/*
mkqnximage ...                    # see image/README.md
```

## What a machine without the SDP can still prove

```bash
make -C target syntax
```

Every `.c` and `.h` here is compiled by `gcc -fsyntax-only` with `-Werror
-Wshadow -Wconversion` against `host_stub.h`, a header of QNX declarations that
exists for this one purpose and is never included by a QNX build (the real
headers come from `<sys/neutrino.h>`, `<sys/dispatch.h>`, `<sys/slog2.h>`,
`<sys/trace.h>` and `<hw/inout.h>`). CI runs it on every pull request.

That proves the code is well-formed C, typed against the QNX API surface it
uses, and free of the sloppiness `-Wconversion` catches. **It does not prove the
code runs.** What still needs a real SDP and a real boot, and is listed as a
hypothesis rather than a result:

- that `io-sock` and the chosen driver match the card QEMU emulates (probe **E1**);
- the guest-to-host round trip, and whether a 5 ms period holds under KVM (**E2**, **E5**);
- the guest's clock after `stop`/`cont` (**E3** — the protocol does not depend on it);
- I/O privileges and death by `isa-debug-exit` (**E4**; see `image/death_points.md`);
- the security policy actually refusing an intruding process (QNX-07);
- the delay from kernel start to certified second (QNX-08, **E6**);
- HAM restarting a process and the peers noticing through the closed connection.

Run the probes before believing any of it:
`python3 host/bench/relais_bench.py --probe E1`.

## Test commands are not in the production image

`relaisctl` can inject a value fault (`bias`) and arm a death point (`die`). Both
are behind `#ifndef RELAIS_PRODUCTION`, and the production build simply does not
embed `relaisctl` (see `image/fragments/buildfile`). The typed security policy
refuses an untrusted process any path to `relais-core`, `/dev/relais` or a socket
(SEC-05).
