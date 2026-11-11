# One machine in a real virtual machine, frozen by QMP

Produced by `python3 host/bench/vm_demo.py --ticks 110 --period-ms 50 --slow 10
--freeze-at 60 --freeze-ticks 8` on this repository. The journal of the run is
committed next to these images (`actuator.jsonl`), so the whole story can be
re-verified with `build/relais-verify actuator.jsonl --level socle`:
**98 ticks, 0 violation, worst gap 3 (bound p+1 = 4), deviation from the twin 0**.

What ran:

- the **actuator on the host**: it owns the tempo, the mandate and the rack;
- **machine B inside a QEMU guest** — a Linux kernel booted with a hand-made
  initramfs that contains nothing but a static `relais-node` and a 60-line init;
- **machine A on the host**, started after the guest so that the virtual machine
  holds the mandate;
- the fault injected through **QMP**: `stop` freezes the whole guest (CPU,
  timers, network), `cont` wakes it. That is spec 4.11's freeze, for real.

The period is 50 ms × slow-motion 10 (CMD-07): the guest runs under TCG, without
KVM on this host, and slow motion stretches the wall clock without touching the
physics — a slowed run is bit-identical to the nominal one.

| File | What it shows |
|---|---|
| `guest-frozen.png` | screendump of the guest **while frozen**: `node B role=holder mandate=1`, orders and shadows counted, the console stopped mid-line |
| `guest-after.png` | the same guest after `cont`: `role=second mandate=0 seen_holder=A`, importing the replica (`replica_age=0`) — the revenant stood down by itself (spec 4.6) |
| `proof.svg` | the command curve through the freeze, the source of every tick (holder → testament → holder), the mandates and the handover events |
| `actuator.jsonl` | the journal, the authority: re-verify it yourself |

The story in the journal:

```
tick 30  mandate_granted   B   mandate 1     the virtual machine takes the mandate
tick 60  (QMP stop)        misses 1, 2, 3    the testament covers every tick
tick 63  mandate_revoked   B   detail 4      revoked on the actuator's own evidence
tick 63  mandate_granted   A   mandate 2     the handover, zero tick without a value
tick 67  order_rejected    B   detail 1      the revenant's stale orders, four times
tick 76  certified         B   mandate 2     back as a certified second
```

Honesty: the guest is Linux, not QNX — there is no SDP on this machine to build
for QNX (see `target/README.md`). The protocol core is the same C11 either way;
what these images prove is the protocol across a real virtual-machine boundary,
with a real freeze of a real guest, and the bench mechanics (QMP stop/cont,
screendump) that level 3 will use on QNX.
