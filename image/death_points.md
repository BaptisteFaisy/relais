# Death points — killing a machine at an exact line of code

INJ-07, spec §7.5. A fault that lands "somewhere in the middle" proves little:
the interesting cases are the ones that land between two statements — after the
order is out but before the replicated state is, after the cession reached the
actuator but not the peer. Nine such points are compiled into the protocol core,
armed from the bench, and fired by the machine itself.

## How a machine dies

The core never calls `exit`. It reports, and the process layer kills:

```c
/* core/src/node.c — die_here() sets the flag, the caller acts on it */
if (s->death_point != dp) return false;
if (s->death_at != 0 && s->now < s->death_at) return false;
o->died = true;
```

| Layer | What happens on `out.died` |
|---|---|
| `host/node/relais-node.c` (PC) | prints the point, then `raise(SIGKILL)` |
| `target/core.c` (QNX) | `rl_qemu_exit(code)` — writes the `isa-debug-exit` port, QEMU quits |
| `host/simulate/` (level 2) | the node simply stops being run |

Arming, from the bench or from `relaisctl die P3 500`:

```c
relais_node_arm_death(&node, RELAIS_DP_P3, 500);
```

## The nine points

All nine live in `core/src/node.c`, in the two role branches and in the local
fault handler. Line numbers move; the function names do not.

| Point | Where | Function | What has already happened | What has not |
|---|---|---|---|---|
| **P1** | `node.c:132` | `as_holder` | the `STATUT` was received | nothing was computed, nothing sent |
| **P2** | `node.c:139` | `as_holder` | the command is computed and bounded | no frame left the machine |
| **P3** | `node.c:148` | `as_holder` | `ORDRE` is on the wire | `ETAT` is not: the second keeps an old replica |
| **P4** | `node.c:152` | `as_holder` | `ETAT` went out on one link | the second link never got it (REP-07) |
| **P5** | `node.c:317,325` | `relais_node_on_local_fault` | `CESSION` reached the actuator | the peer was not told — it keeps trusting a dead holder |
| **P6** | `node.c:197` | `as_second` | the suspicion is raised | the `DEMANDE` is not sent: nobody asks for the mandate |
| **P7** | `node.c:142` | `as_holder` | the mandate was just granted | the first `ORDRE` of the new holder is not out |
| **P8** | `node.c:252` | `relais_node_on_status` | the machine learned it was deposed | it has not finished becoming a second |
| **P9** | `node.c:180` | `as_second` | the machine is rejoining | it is not certified yet |

Every point is covered by `core/tests/test_protocol.c` (which arms each one and
checks that the frames expected *before* it went out, and none of the ones after)
and by the swept campaign of `host/bench/sweep.py`.

## The `isa-debug-exit` mechanism, and its fallback

QEMU's `isa-debug-exit` device turns one write to an I/O port into a guest exit,
with the written value as the exit code. That is what lets the *guest* decide the
exact instant of its own death, with no help from the host:

```c
/* target/qnx_util.c */
if (ThreadCtl(_NTO_TCTL_IO, NULL) == 0) out32(0x501u, code | 0x10u);
```

`ThreadCtl(_NTO_TCTL_IO, 0)` grants I/O privileges to this thread; it needs the
capability, and a typed security policy may refuse it — which is exactly what the
policy of `image/fragments/security_policy.xml` should do in a production image.
The device must be on the QEMU command line:

```
-device isa-debug-exit,iobase=0x501,iosize=0x04
```

**Fallback, decided in advance (spec §11, E4).** If the SDP or the policy refuses
I/O privileges, the death points are not lost: the bench kills the guest from
outside at the tick it wants (`quit` over QMP, or `SIGKILL` on the QEMU process).
What is lost is the precision *inside* the tick — an external kill cannot land
between "order sent" and "state sent". In that case P3, P4 and P5 are covered by
the level-2 simulator, which drives the same core and can stop it between any two
statements, and the report says which of the two means produced the result.
