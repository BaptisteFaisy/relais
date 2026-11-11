# QEMU layer — what we launch, why, and what is still unmeasured

Scope: `host/bench/qemu_config.py` (command line), `qemu.py` (`QemuGuest`),
`qmp.py` (QMP client), `orchestrator.py` + `qemu_bench.py` + `faults.py` (the
bench), `probes.py` (E1..E6), `host_net.py` (the PC-side UDP helpers).
Requirement IDs: INJ-01..INJ-04, INJ-07, OBS-04.

## 1. The command line we keep

For guest A of a two-guest run (guest B is identical except `name`, `mac` and
the two dgram ports):

```
qemu-system-x86_64 \
  -machine q35 -cpu host -smp 1 -m 512 -enable-kvm \
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

| Option | Why |
|---|---|
| `qemu-system-x86_64` | Target of the QNX 8.0 image built by `mkqnximage` (spec 3.4). |
| `-machine q35` | Modern chipset: ECAM PCIe, no legacy ISA hole conflicts; the e1000 and isa-debug-exit both attach cleanly. |
| `-enable-kvm` | Measured timing (E2, E5) is only meaningful with hardware acceleration. Omitted on the TCG fallback, see §2. |
| `-cpu host` | Full CPU features under KVM; `-cpu max` under TCG. |
| `-smp 1`, `-m 512` | One vCPU per machine keeps the two guests from starving each other on a shared PC; 512 MB is enough for a QNX SDP 8.0 boot image plus the three processes. |
| `-kernel` + `-append` | QNX `mkqnximage --type=qemu` produces a bootable IFS; `-kernel` boots it directly (no bootloader, no CD, faster E6). `-cdrom`/`-drive` paths exist in `qemu_config._image_args` for ELF/disk images. |
| `-netdev dgram,…` | The link between computers. UDP unicast between two ports on `127.0.0.1`; each side is a real NIC to the guest, so QNX sees ordinary frames. No root (no tap, no bridge). |
| `-netdev user,…,hostfwd` | The link towards the actuator on the PC (mode `qemu` with `--actuator pc`, and probe E2). Slirp needs no root; `hostfwd` maps a high host port onto the guest's port. Ports below 1024 are rejected by `check_no_root_needed`. |
| `-device e1000` | QNX 8.0 ships an `e1000` (and virtio) driver; `e1000` is the dullest, most predictable choice, and `virtio-net-pci` stays available via `QemuConfig.net_model`. |
| `mac=…` | Identity by MAC, as spec 3.4 requires: same image, distinguished only by the NIC address (`52:54:00:00:0a:01` A, `…:0b:02` B, `…:0c:03` actuator guest). |
| `-object filter-dump` | OBS-04: every link captured to pcap, one file per netdev, inside the run directory. The dump is written by QEMU, so it also captures frames the guest never sees. |
| `-device isa-debug-exit` | INJ-07: an in-guest `outb` to `0xf4` exits QEMU with `(value << 1) | 1`. Death at a precise code point, without killing the process from outside. |
| `-nographic` + `-serial file:…` | The QNX console goes to a file: this is where `/dev/relais/status` lines and the readiness markers are read from. `-nographic` also keeps the bench usable over SSH. |
| `-monitor none` | HMP is not used; everything goes through QMP. Leaves the stdio free. |
| `-no-reboot` | A guest reset must be ordered by the bench (`system_reset`), never an automatic reboot that would silently hide a crash. |
| `-qmp unix:…,server=on,wait=off` | Control channel: `stop`, `cont`, `quit`, `system_reset`, `set_link`, `query-status`. `wait=off` lets QEMU start before the client connects. |

Nothing in this line requires root: no `tap`, no `bridge`, no `vhost-user`, no
raw socket. `check_no_root_needed()` fails loudly if a future change
reintroduces one, or any port below 1024.

## 2. KVM, and the documented TCG fallback

`detect_kvm()` looks at `/dev/kvm`: missing or not readable/writable by this
user means **no KVM** and a fallback to TCG, with the reason kept in the
verdict. The line then becomes:

```
-machine q35 -cpu max -smp 1 -m 512 -accel tcg,thread=multi …
```

`-accel tcg,thread=multi` is used instead of nothing so the two guests do not
serialise on one TCG thread. **`-accel` and `-enable-kvm` are never both
present**: QEMU 8.2.2 rejects it with `The -accel and "-machine accel=" options
are incompatible` (seen on this PC while building the layer). Requesting
`accel="kvm"` explicitly on a host without `/dev/kvm` raises instead of
silently falling back.

TCG is *not* usable for E2/E5 timings; the verdict says so.

## 3. QMP commands used

| Command | Use | Verified on this PC (QEMU 8.2.2) |
|---|---|---|
| greeting, `qmp_capabilities` | handshake, mandatory before any command | yes — real socket, real answers |
| `stop` | freeze a machine (INJ-03) | present, accepted |
| `cont` | thaw (INJ-03) | present, accepted |
| `quit` | orderly death (INJ-02) | present, accepted |
| `system_reset` | warm reboot (recovery, `restart` fault) | present, accepted |
| `set_link name=… up=false/true` | unplug/replug a netdev, with carrier loss (INJ-04) | executed successfully on a guest that had not started (`prelaunch`); **not yet exercised on a running QNX guest with traffic** |
| `query-status` | read `running`/`prelaunch`/`paused` | returns `{"status": "prelaunch", "running": false}` |

The client (`qmp.py`) reads JSON lines, ignores asynchronous `event` messages,
maps `error` to an exception (`QmpError`), times out on silence
(`QmpTimeout`), and reconnects after a stall or a broken pipe. `QemuGuest`
serialises every command through a queue served by one worker thread, so two
callers never interleave on the socket, and every command + answer is written
to the run log.

## 4. The link between the computers: three options, in order

1. **`-netdev dgram` (chosen default).** Two unicast ports on `127.0.0.1`
   paired `local`↔`remote`, with the **programmed cable** of `cable.py` between
   them: each guest's dgram peer port is a cable port, and the cable decides
   what actually crosses (loss, delay, duplication, reorder, by direction and
   by message type — INJ-05). Without the cable (`--no-cable`), A and B point
   straight at each other.
2. **`-netdev socket,mcast` (first fallback, spec E1).** One multicast group
   on loopback, no cable in the middle: simpler, but no per-frame control,
   and fault injection reverts to `set_link` only. Choose it if unicast dgram
   pairing turns out to be filtered or unreliable in the guest.
3. **`-netdev user` + a relay on the PC (second fallback).** Each guest gets a
   slirp NIC and talks to a UDP relay port on the host; `host_net.UdpRelay`
   forwards both ways (and is where the cable logic can live at level C). Costs
   one extra hop and some timings; still no root.

`host_net.py` provides `UdpRelay` (two pumps, per-direction counters),
`UdpEchoServer` (used by E2/E5 as the PC-side peer), `measure_rtt` and `stats`
(monotonic clock, median/p95/p99/max in milliseconds, loss count).

## 5. What was actually tried on this PC

QEMU 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18), user `baptiste`, no root:

- `qemu-system-x86_64 --version` answers; `/dev/kvm` exists and is
  readable/writable → **KVM available** on this host. `-accel help` lists
  `tcg` and `kvm`.
- The full command line of §1 is **accepted**: a real QEMU process started with
  the two dgram netdevs, the two e1000 devices, two `filter-dump` objects, the
  `isa-debug-exit` device, `-serial file:…`, `-no-reboot`, `-monitor none` and
  `-qmp unix:…`. The pcap file was created by QEMU; the QMP socket was created
  and removed on exit.
- **QMP over that socket works**: greeting read, `qmp_capabilities` → `{}`,
  `query-status` → `prelaunch`, `set_link` → `{}`, `quit` → `SHUTDOWN` event
  then `{}`. The socket file disappears when QEMU exits.
- `-accel` **and** `-enable-kvm` together are rejected by this QEMU — the
  builder therefore emits exactly one of them.
- `-object filter-dump,help` and `-netdev help` confirm `filter-dump` and the
  `socket`/`stream`/`dgram`/`user` backends are compiled in.
- **No frame has been exchanged between two guests yet**: no QNX image exists
  in this working copy (`image/` is empty), so E1 is unproven. This is the
  single biggest open risk of the layer.

Everything above was done with `-S` (or without any bootable image), so no
guest actually ran.

## 6. What remains to be measured (E1..E6)

| Probe | Question | State on this PC |
|---|---|---|
| E1 | Do two QNX guests exchange datagrams through a relay on the PC, without root? Latency? | **BLOQUE** — no QNX image. The line is accepted; frames are unproven. |
| E2 | UDP round trip guest↔PC, median and worst case with KVM | **BLOQUE** — no image. PC loopback reference is reported anyway (median ≈ 0.03 ms, max ≈ 0.1 ms). |
| E3 | What does the guest clock see after `stop`/`cont`? | **BLOQUE** — no image. `stop`/`cont` accepted by QMP on an idle guest. |
| E4 | Does a QNX process get I/O port access and die via `isa-debug-exit`? | **BLOQUE** — no image. Device accepted on the command line. |
| E5 | `STATUT`→order delay under load, network stack priority | **BLOQUE** — needs the in-guest trace (OBS-03). |
| E6 | QEMU launch to a ready machine | **BLOQUE (partial)** — measured: **QEMU reaches QMP in ≈ 220 ms** with `-S`, i.e. the emulator's own startup. Boot to "second certified" is unmeasured. |

To unblock all six: `mkqnximage --type=qemu --target=x86_64 --boot --image
image/qnx-ifs.img` (fragments in `image/`), or `RELAIS_QNX_IMAGE=/path/to/ifs.img`.
Then `python3 host/bench/relais_bench.py --probe all`.

## 7. Known limits

- **Both guests share the PC.** A load spike hits both machines at once, which
  biases every "one machine is slow" experiment. Reserve CPUs for the campaigns,
  publish timings separately, and never interpret a common spike as a fault.
- **Killing QEMU is not cutting power.** `quit`/`kill` stop the process; they do
  not corrupt state mid-write or model a brown-out. QMP `quit` is a clean exit.
- **`set_link` is a link-state change, not a physical cable.** It is the closest
  we get to unplugging, and it is done without root; the programmed cable is
  what provides loss/delay/reorder.
- **TCG (no `/dev/kvm`) invalidates the timing probes** (E2, E5) and slows the
  demo; the layer says so in the verdict rather than pretending.
- **Death points need in-guest cooperation.** The host can kill at the right
  instant, but a *code* point (P1..P9) requires the guest to perform the `outb`
  to `0xf4`. Without it, INJ-07 degrades to a timed kill.
- **No fine hardware faults.** Bit flips in memory are out of scope for the
  bench (spec 7.5, "limites connues").
- The actuator in a third guest requires a second MAC and a `user` netdev; the
  arbiter timings then carry one extra hop, which is exactly what E2 measures.
