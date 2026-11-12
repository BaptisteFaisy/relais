# QNX 8.0 bring-up — from zero to two guests and an actuator

Scope: the whole path that turns "the SDP is not here" into "the receipt runs on
a real QNX target". This document is the operator's companion to
`host/bench/qnx_env.py` (environment detection), `host/bench/qnx_build.sh`
(one-shot receipt) and `host/bench/QEMU.md` (the exact QEMU command line).

Requirement IDs exercised: QNX-01..QNX-08, SEC-05, PERF-01..PERF-08, OBS-04,
INJ-01..INJ-07.

Target of the whole exercise (spec §7.1):

| Level | Where | What is tested |
|---|---|---|
| 0 — unit | PC | Frames, arbiter, state machine, control law |
| 1 — exhaustive exploration | PC | All bounded interleavings, faults included |
| 2 — seeded simulation | PC | Long random sequences, timing model |
| **3 — real faults** | **QEMU** | **the full chain: machines killed, frozen, cables cut** |
| 4 — hardware | two boards | real latencies (extreme level) |

This document unblocks **level 3**. Nothing here requires root.

## 0. What you need before starting

| Item | Why | How to check |
|---|---|---|
| A Linux x86_64 host | the SDP host tools are x86_64 Linux; `qcc -V gcc_ntox86_64` targets that sysroot | `uname -m` → `x86_64` |
| A myQNX account | the official download needs it | https://qnx.com/everywhere |
| QEMU ≥ 8.2 | the bench drives guests over QMP; 8.2 is the tested line | `qemu-system-x86_64 --version` |
| KVM (recommended) | E2/E5 timings need hardware acceleration; TCG works but the timings are not usable | `ls -l /dev/kvm` |

`host/bench/qnx_env.py` checks all of these and returns `0` (ready), `1`
(partial) or `2` (absent).

## 1. Obtain the SDP (QNX Everywhere)

1. Go to https://qnx.com/everywhere and create a free myQNX account.
2. Download **QNX Software Development Platform 8.0** (non-commercial,
   x86_64 Linux host).
3. You receive an installer (`.run`) and a license key.

```bash
# install under ~/qnx800 (any directory works; this is the default searched)
chmod +x qnxsdp-8.0.*.run
./qnxsdp-8.0.*.run            # point it at ~/qnx800, follow the prompts
```

## 2. Verify the installation

```bash
source ~/qnx800/qnxsdp-env.sh
qcc -V gcc_ntox86_64 -dumpversion     # prints the GCC version when the toolchain is here
mkqnximage -v                          # prints the version when the image utilities are here
```

If `qcc` exists but `gcc_ntox86_64` is unknown, the x86_64 target sysroot is
not installed: re-run the installer and select the **x86_64le** target.

## 3. Environment check

```bash
python3 host/bench/qnx_env.py
echo $?        # 0 = ready, 1 = partial, 2 = absent
```

`2` on this machine: no SDP is installed. The report points at QNX Everywhere
and, until then, at the PC/sim paths that still work (`make -C target syntax`,
`relais_bench.py --mode pc|sim`).

## 4. Build the target binaries

```bash
source ~/qnx800/qnxsdp-env.sh
make -C target              # qcc -V gcc_ntox86_64 -> target/build/relais-{net,core,ctl}, relaisctl, relais-actuator
```

All protocol logic comes from `core/`; `target/` only adds the QNX plumbing
(resource manager, message passing, `io-sock`). Requirement IDs: QNX-01..QNX-03.

## 5. Build the bootable image

Either the one-shot receipt (preferred, it also verifies):

```bash
host/bench/qnx_build.sh image
```

or the raw command it wraps (`image/README.md`):

```bash
mkqnximage -b -v \
  -B image/fragments/buildfile \
  -S image/fragments/startup.script \
  -f image/fragments/identity.sh \
  -f image/fragments/ham.script \
  -f image/fragments/security_policy.xml \
  -t qemu-x86_64 -o build/qnx-image
```

`qnx_build.sh image` then copies the produced IFS to `image/qnx-ifs.img`, the
path the bench expects. The buildfile embeds the four target processes,
`io-sock` + the network driver (`devs-vtnet_pci.so` for virtio, `devs-em.so`
for e1000), `pidin`, `slog2info`, `tracer`, `dumper`, `if_up`, `ifconfig` and
the boot/supervision/security fragments.

## 6. Verify the image

```bash
host/bench/qnx_build.sh verify
```

Checks: the file exists, is at least 1 MiB (an incomplete IFS is smaller), and
prints its `sha256`. An image below 1 MiB almost always means `mkqnximage`
failed to pull a `$(QNX_TARGET)` path (a fragment references a target utility
that is not installed).

## 7. Two identities, one image

The two computers run the **same bytes** (CMD-03); `image/fragments/identity.sh`
reads the MAC address of the first interface and exports `RELAIS_SELF=A` or `B`.

| MAC | Role |
|---|---|
| `52:54:00:12:35:0a` | A |
| `52:54:00:12:35:0b` | B |

**Pitfall (bring-up risk #1).** `identity.sh` recognises
`52:54:00:12:35:0a/0b`, while the bench defaults in
`host/bench/qemu_config.py` are `52:54:00:00:0a:01` (A) and
`52:54:00:00:0b:02` (B). A guest booted with the bench defaults would fall into
`identity.sh`'s `*` case and come up as **B**. Before the first two-guest boot,
pick one convention and make both sides agree — either edit the MAC table in
`identity.sh`, or pass `52:54:00:12:35:0a/0b` on the QEMU command line. The
bench reads its MACs from `qemu_config.DEFAULT_MACS`, so changing the table is
the smallest fix.

## 8. The exact QEMU command line

Do not hand-craft it: the bench builds it from `host/bench/qemu_config.py`
(see `host/bench/QEMU.md` §1 for the option-by-option rationale). One guest:

```
qemu-system-x86_64 \
  -machine q35 -accel kvm -cpu host -smp 1 -m 512 -enable-kvm \
  -kernel image/qnx-ifs.img \
  -netdev dgram,id=ab,local.type=inet,local.host=127.0.0.1,local.port=<pa>,remote.type=inet,remote.host=127.0.0.1,remote.port=<pb> \
  -device e1000,netdev=ab,mac=52:54:00:00:0a:01 \
  -netdev user,id=act,net=10.0.2.0/24,host=10.0.2.2 \
  -device e1000,netdev=act,mac=52:54:00:00:0a:01 \
  -object filter-dump,id=dump-ab,netdev=ab,file=out/<run>/A-ab.pcap \
  -object filter-dump,id=dump-act,netdev=act,file=out/<run>/A-act.pcap \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -nographic -serial file:out/<run>/A-console.log -monitor none -no-reboot \
  -qmp unix:out/<run>/A-qmp.sock,server=on,wait=off
```

Without `/dev/kvm`, the bench substitutes `-accel tcg,thread=multi` and
`-cpu max` and says so in the verdict (QEMU.md §2).

## 9. Run the receipt

```bash
host/bench/qnx_build.sh run
# equivalent to:
python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/slalom-nominal.json
```

The bench starts the PC actuator (level S), the programmable cable, the two
QNX guests, injects the scenario's faults and writes the actuator journal.
Verify afterwards with `relais_bench.py --verify <run>/actuator.jsonl`.

## 10. The six feasibility trials E1..E6

Every probe lives in `host/bench/probes.py` and returns a verdict
(`OK`/`ECHEC`/`BLOQUE`) plus the measurement, never an exception. Without the
SDP **E1..E5 are `BLOQUE`** and the report says how to build an image. Run one
probe from the repository root:

```bash
python3 -c "import sys; sys.path.insert(0,'host/bench'); import probes, json; print(json.dumps(probes.run_probe('E1', probes.ProbeContext(root='.')), indent=2))"
```

> Note (`bring-up risk #2`): `host/bench/QEMU.md` §6 references
> `relais_bench.py --probe all`, but that flag is not wired to a CLI yet — the
> probes are only exposed through `probes.run_probe`/`probes.run_all`. Use the
> one-liner above until the flag lands.

| Trial | Question | What is measured | Fallback if the answer is no |
|---|---|---|---|
| E1 | Do two QNX guests built from the same image, linked by a `dgram` link through a relay on the PC, exchange UDP datagrams without root? | pcap bytes on both links, frames forwarded by the relay, round-trip | Multicast (`-netdev socket,mcast`); failing that, each guest goes through the user network and the relay runs on the PC |
| E2 | UDP round trip guest↔PC, median and worst case, with KVM | `measure_rtt` median / max / loss | Lengthen the period to 10–20 ms; put the actuator in a third machine |
| E3 | After `stop` then `cont`, what does the guest clock see? | guest status before/frozen/after, host frozen seconds, frames while frozen | None needed: the protocol is safe either way (I5); the answer only fixes the demo story |
| E4 | Does a QNX process obtain I/O-port access and die via `isa-debug-exit`? | QEMU exit code (1 → value 0x00, 3 → value 0x01) | Death triggered from the outside at the wanted instant, without an exact code point (`image/death_points.md`) |
| E5 | In-guest `STATUT`→order delay under load, and at which priority the stack handles frames | the in-guest kernel trace (OBS-03) | Adjust the network-stack priority; lengthen the period |
| E6 | How long from QEMU launch to a ready machine? | `qemu_to_qmp_ms`, `qemu_to_ready_ms` | Lighten the image; restore a snapshot instead of restarting |

## 11. What becomes demonstrable once the SDP is there

Everything below stays a **hypothesis** (spec §14) until the image boots.

| Requirement | What the SDP makes provable |
|---|---|
| QNX-01 | `relais-core` is a resource manager: `open`/`read`/`devctl` only |
| QNX-02 | timers and notifications are pulses — no active wait |
| QNX-03 | critical processes at fixed priority, memory locked at start |
| QNX-04 | a kernel trace shows the path `STATUT`→order with each step's priority |
| QNX-05 | a process death is known by connection close, not polling |
| QNX-06 | launch + restart by the QNX supervision service (HAM) |
| QNX-07 | the security policy is applied, and a refusal is shown |
| QNX-08 | the boot-to-certified-second delay is measured and published |
| SEC-05 (adversarial) | only `relais-net` may open sockets; only `relais-core` may serve `/dev/relais`; the test commands are absent from the production image — a refused attempt is the evidence |
| PERF-01..PERF-08 | the 1 ms / 2 ms `STATUT`→order bound, the p+1 mandate-change, the three-load robustness, the kernel-trace explanation, the breaking period — all measured on the real target under QEMU |
| Level 3 | machines killed, frozen, cables cut — the full fault chain, not just the core |

## 12. Requirement → command → expected result

| Requirement | Command | Expected result |
|---|---|---|
| environment | `python3 host/bench/qnx_env.py` | verdict `0` and a report with every check `OK` |
| QNX-01..03, SEC-05 | `make -C target` | `target/build/relais-{net,core,ctl,actuator}` + `relaisctl` built clean |
| QNX-07, QNX-08 | `host/bench/qnx_build.sh image` | `image/qnx-ifs.img` ≥ 1 MiB |
| image integrity | `host/bench/qnx_build.sh verify` | size printed, `sha256` printed, no `die` |
| E1..E6 | the probe one-liner, one per trial | `OK` on each, with the measurement attached |
| full receipt | `host/bench/qnx_build.sh run` | two guests + actuator, journal verified, `Verify: OK` |
| QNX-04, PERF-07 | in-guest `tracelogger` + `tracer` | the `STATUT`→order trace with priorities |
| QNX-07 adversarial | attempt a socket from a process that is not `relais-net` | the call is refused and logged |
| QNX-08 | `relais_bench.py --mode qemu` log | boot-to-ready delay published |

## 13. Observing without an SDP

`/home/baptiste/Bureau/qnx71-vm/` boots a **QNX 7.1** community guest (VGA
console via QMP, `qnx-vm.sh start/stop/console/ssh`). It is for **observing**
the QNX side only: it is not an SDP, nothing here can be cross-compiled with it.
It answers "what does QNX see of a device enumerate" — nothing more.