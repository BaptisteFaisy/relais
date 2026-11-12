# QNX-RUN — booting a real QNX 8.0 image under QEMU/KVM and measuring in the guest

Mission M46. Everything on this page was **actually executed** on the host
`baptiste-MS-7D18` on 2026-11-12 with SDP 8.0.0 (`~/qnx800`), QEMU 8.2.2 and
a user-writable `/dev/kvm`. No number here is invented: each one is reproducible
with the commands given, and each run leaves its console transcript behind.

## What is proven (step 1 — minimal proof loop)

The whole path **compile → image → boot → measure** works on real QNX 8.0:

| Fact | Value | Evidence |
|---|---|---|
| Guest OS | `QNX 8.0.0 (2026/06/05-16:26:03EDT) x86pc`, node `relais-m46` | transcript, `rl-proof: uname` line |
| Accelerator | **KVM** (`-enable-kvm -cpu host`), not TCG | `runs/qnx-vm/qemu-command.txt` |
| Boot time (QEMU spawn → usable ksh) | **12.1–13.6 s** over 4 runs | `proof.json` / stderr of each run |
| `clock_gettime(CLOCK_MONOTONIC)` overhead in guest | min **0.008 µs**, mean **0.016–0.019 µs**, max 0.44–3.1 µs (10 000 calls) | `rl-proof: gettime_us` line |
| `nanosleep(1 ms)` overshoot in guest | min 308–647 µs, mean ~996–998 µs, max 1.07–1.66 ms (200 calls) | `rl-proof: sleep_overshoot_us` |
| Clean stop | QMP `quit`; `pgrep -a qemu` shows no orphan after every run | harness invariant |

**Honesty note (load):** the runs above were taken while the 12-core host
carried `loadavg` 8–18 (other agents compiling + two other QNX VMs). KVM does
not isolate vCPU scheduling from host load: the sleep-overshoot numbers are
therefore *upper bounds*, not real-time guarantees. `proof.json` records
`host_loadavg` next to every measurement for this reason. The
`clock_gettime` overhead (~16 ns mean) is a guest-kernel property and stays
meaningful. A quiet-host re-run is one command away (`make qnx-proof`).

## Exact commands

Image build (inside the VM directory `build/qnx-vm/`, driven by
`host/bench/qnx_image.py`):

```sh
. ~/qnx800/qnxsdp-env.sh
mkqnximage --type=qemu --noprompt --ssh-ident=none --hostname=relais-m46   # first time
# file injection: local/snippets/ifs_files.custom  (see pitfalls below)
mkqnximage --build --noprompt                                              # after changes
```

Effective QEMU line (written verbatim to `runs/qnx-vm/qemu-command.txt` by
every run; `mkqnximage --run` is NOT used — see "Why we drive QEMU ourselves"):

```
/usr/bin/qemu-system-x86_64 -smp 2 -m 1G -enable-kvm -cpu host \
  -kernel <vm>/output/ifs.bin -drive file=<vm>/output/disk-qemu.vmdk,if=ide,id=drv0 \
  -netdev user,id=net0,host=10.0.2.2 -device virtio-net-pci,netdev=net0,mac=52:54:00:46:4d:36 \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
  -chardev socket,id=ser0,path=<run>/console.sock,server=on,wait=off -serial chardev:ser0 \
  -qmp unix:<run>/qmp.sock,server=on,wait=off \
  -display none -monitor none -pidfile <run>/qemu.pid
```

Reproduce (from the repository root):

```sh
make qnx-vm-test         # 28+ unit tests, no guest needed
make qnx-proof           # step 1: compile rl-proof, image, boot KVM, measure, stop
make qnx-failover-stub   # step 2 (stub): kill-from-guest + takeover delay
make qnx-relais          # step 2 (real): BLOCKED until M45, says so explicitly
```

Console transcript of a real proof run: `docs/evidence/qnx-m46-proof-console.log`
(JSON summary: `qnx-m46-proof.json`); failover-stub run:
`qnx-m46-failover-console.log` / `qnx-m46-failover.json`.

## Why we drive QEMU ourselves

`mkqnximage --run` (its `runimage` script) hard-codes `-netdev bridge` plus a
setuid `qemu-bridge-helper` check — that needs root, which this project never
uses. We keep mkqnximage for what it does well (building `output/ifs.bin` +
`output/disk-qemu.vmdk`) and run QEMU with the same artifacts, KVM when
`/dev/kvm` is writable (`qemu_config.detect_kvm`), root-free `-netdev user`,
and a **bidirectional** serial (Unix-socket chardev) so commands can be sent to
the guest ksh (`on -d -t /dev/ser1 ksh -l` in the stock startup script).

## mkifs/mkqnximage pitfalls (learned the hard way, all verified with `mkifs -v`)

1. **`[+script]` is for TEXT scripts only.** It prepends a `#!/proc/boot/ksh`
   shebang: applied to an ELF binary it yields a ~32-byte stub — or the entry
   is silently dropped in a full image. Inject binaries with plain entries:
   `[perms=0755] name=/abs/host/path`.
2. **mkifs bracket attributes are sticky.** The custom snippet lands after
   `[type=link]` / `[uid=0 gid=0 perms=0444]` lines in the generated
   `output/build/ifs.build`; it must reset `[type=file]` and restore
   `[perms=0444]`, or later entries (QTD, …) change meaning.
3. **`[+optional]` hides everything**: a bad entry is skipped without a word.
   Always verify with `mkifs -v output/build/ifs.build /tmp/test.bin`.
4. Files named without `./` land in `/proc/boot/<name>` — which is on the
   stock guest `PATH` (`/proc/boot:/system/bin`), so `rl-proof` just runs.
5. **Console round-trips must survive ksh echo.** The harness sends
   quote-split sentinels (`echo "__RL""_DONE__$?"`): the typed echo can never
   match the clean output marker. Boot completion is gated on the guest's own
   `Startup complete` line, then a sentinel round-trip.
6. `qcc -std=c11` **requires** `-D_POSIX_C_SOURCE=200809L`, else every POSIX
   declaration (`clock_gettime`, `strtok_r`, …) is hidden. (This is also the
   root cause of the current `target/` build failure — M45's perimeter.)

## Step 2 — the banc in the guest (stub validated, real banc BLOCKED)

`host/bench/qnx_failover.py` drives the guest-side banc:

* **stub mode (validated for real):** `image/fragments/rl-failover.sh` runs
  inside the guest: a holder heartbeats every 10 ms over 127.0.0.1 UDP, the
  script `kill -9`s it **from inside the guest** (fault injection), and the
  second declares takeover after a 40 ms silence window (p+1 = 4 missed beats,
  p=3). Measured on 2026-11-12 under KVM:
  `rl-failover: takeover takeover_us=41981 heartbeats=92` — ~42 ms from the
  last heartbeat to takeover declaration (40 ms window + ~2 ms detection),
  guest journal recovered on the host as the console transcript.
  **This validates the harness mechanics only — it is NOT a protocol
  measurement** (the protocol lives in `core/`, reached through `target/`).
* **relais mode (real banc):** the mapping `image/fragments/vm_files.relais`
  lists the five `target/build/` binaries to inject. Today it exits with:
  `BLOCKED(target/ does not compile with qcc yet — see M45)` — verified output
  of `make qnx-relais`. When M45 lands, the same harness injects the real
  binaries; the in-guest scenario script for the real banc (actuator + two
  nodes, mandate handover) is the remaining piece to wire against the real
  binaries' CLI.

## Files of this mission

* `host/bench/qnx_image.py` — SDP/mkqnximage/qcc driving, snippet injection.
* `host/bench/qnx_vm.py` — QEMU command builder (KVM-honest), bidirectional
  serial console with transcript, VM lifecycle (start → wait shell → run →
  clean stop), proof CLI.
* `host/bench/qnx_failover.py` — step-2 harness (stub + real mapping).
* `host/bench/test_qnx_vm.py` — 15 unit tests without a guest (command line,
  snippets, sentinels, transcript parsing incl. a real captured transcript).
* `host/bench/guest/qnx_proof.c` — the in-guest measurement program.
* `host/bench/guest/qnx_failover_stub.c` — the substitute holder/second.
* `image/fragments/rl-failover.sh`, `vm_files.failover`, `vm_files.relais`.
* `host/bench/qnx_build.sh` — `image` step fixed to the real mkqnximage flow.
* `docs/evidence/qnx-m46-*` — transcripts and JSON of the real runs.

Safety invariants: one VM at a time; every path through the harness ends in
`QnxVm.stop()` (QMP `quit`, then TERM, then KILL); `pgrep -a qemu` is clean
after every run; no root, no sudo, host ports ≥ 1024 only.
