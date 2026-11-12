# Reproduce the Relais proof bench (REP-01..REP-05)

This repository publishes *measurable claims*, not estimates. Every number in
`docs/RESULTS.md` comes from a command you can re-run on your own machine. This
document is the reproducibility promise: what runs where, what each step must
report, and the tolerance you may allow yourself when your machine is not ours.

One command covers the whole machine-independent part:

```bash
tools/bootstrap.sh
```

It checks the prerequisites (and *proposes* the install command for your
distribution — it never installs or downloads anything itself), builds, then
runs the unit tests, the exhaustive exploration, the fault campaign, the mutant
bench and the demonstration, and prints a summary in the same shape as
`docs/RESULTS.md`. Add `--dry-run` to list the steps without running them.

## 1. The three tiers

| Tier | What | Needs | Replayable by |
|---|---|---|---|
| Mach.-independent | build, 5 651 unit checks, exhaustive exploration, fault campaign, 8/8 mutant bench, deterministic demo | `gcc`, `make`, `python3` only | anyone, anywhere |
| VM demonstration | two guests frozen/thawed over QMP, real KVM timing | `qemu-system-x86_64` (+ a QNX image, or the bootable Linux guest) | anyone with QEMU |
| QNX target (level 3) | boot the real image, measure the 5 ms period, tracer, security policy | QNX SDP 8.0 (`qcc`, `mkqnximage`) | anyone with the SDP |

Tier 1 is the *truth* of the protocol (the same `core/` code, the same
invariants, the same journal). Tier 2 and 3 are about the *story* — timing,
real faults, a real kernel — and they degrade honestly: without a QNX image the
bench reports `BLOQUE` with the exact command to build one, never a silent or
obscure failure.

## 2. Tier 1 — machine-independent, exact verdicts

```bash
tools/bootstrap.sh                 # everything below, in one run
```

Or step by step, each with the exact verdict to expect:

```bash
make -j"$(nproc)" all              # zero warnings, -Werror
make test                          # 5 651 checks, all green (56 + 226 + 7 + 21
                                   # + 89 + 93 + 133 + 5026); 0 failure
make explore                       # 100 198 states, 0 violation (budget 100 000)
make campaign                      # CAMPAGNE-OK, 0 failure
python3 host/mutants/run_mutants.py   # 8/8 mutants flagged
python3 host/bench/relais_bench.py --demo --sim   # verdict OK, 0 violation
```

**Tolerances.** The only number you must never tolerate is a violation or a
failure: every invariant, every campaign run and every mutant verdict must be
**0 / 0 / 8-of-8**. The *state count* of the exploration and the *run time* are
not fixed points:

- `make explore` explores with a **budget of 100 000 states**; it must report
  `0 violation`. The exact "states" figure can move if you change the budget,
  the compiler, or the machine — that is fine and expected.
- the unit-check total (5 651) is the count for this tree; if a future change
  adds or splits a test, the total moves, but **0 failure** remains the rule.
- timings in `docs/RESULTS.md` (0.6 s, 51 s, 30 min) are *measurements on this
  PC*, not thresholds. A faster or slower machine reports the same verdicts.

## 3. Tier 2 — the VM demonstration (QEMU)

This tier exists to show a *real* freeze: a guest is stopped and resumed with
QMP, which no PC-process demo can pretend. It needs `qemu-system-x86_64`. The
full command line and its rationale are in `host/bench/QEMU.md`.

```bash
python3 host/bench/relais_bench.py --demo --mode qemu --out runs/bootstrap-vm
```

- With a QNX image (`image/qnx-ifs.img`, or `RELAIS_QNX_IMAGE=/path/to/ifs.img`)
  the two guests boot, exchange frames, and the injected death triggers the
  handover. Verdict: **OK**.
- Without a QNX image the bench reports **`BLOQUE`** and prints the exact
  `mkqnximage` command (see below); this is *not* a failure.
- KVM is used when `/dev/kvm` is present; on a host without it QEMU falls back
  to TCG and the bench says so — the timing probes (E2/E5) are only meaningful
  under KVM, and the verdict states that instead of pretending.

### Bootable Linux guest (no SDP required)

To show the QMP freeze/thaw *without* the QNX SDP, boot a tiny Linux guest and
freeze it with `stop`/`cont` (the same QMP commands the QNX bench uses). The
container in `.devcontainer/` already carries `cpio` and `busybox-static`; you
only need a kernel:

1. Obtain a kernel: reuse the host's `/boot/vmlinuz-$(uname -r)` (a Linux host
   already has one), or `apt-get install linux-image-virtual` / build one.
2. Assemble an initramfs whose `/init` just prints a marker and idles:

```sh
mkdir -p initfs/bin initfs/dev initfs/proc
cp /bin/busybox initfs/bin/busybox
ln -sf busybox initfs/bin/sh
printf '#!/bin/sh\nmount -t proc proc /proc\necho READY\nwhile true; do sleep 1; done\n' > initfs/init
chmod +x initfs/init
(cd initfs && find . -print0 | cpio --null -ov --format=newc | gzip -9) > initramfs.cpio.gz
```

3. Boot it, then freeze with QMP `stop`, thaw with `cont`:

```sh
qemu-system-x86_64 -kernel /boot/vmlinuz-$(uname -r) -initrd initramfs.cpio.gz \
  -nographic -append "console=ttyS0" -qmp unix:./qmp.sock,server=on,wait=off
```

The development container documents this recipe (see `.devcontainer/Dockerfile`
comments) because the repository's QNX image still requires the SDP.

## 4. Tier 3 — the QNX target (level 3 real)

Numbers that honestly need the QNX SDP 8.0 are marked in `docs/RESULTS.md` as
open: the measured 5 ms period under KVM, the process priorities in a kernel
trace (`tracer`), boot-to-certified-second, and the compiled security policy.

```bash
source ~/qnx800/qnxsdp-env.sh        # QNX Everywhere, free for non-commercial use
make -C target                        # qcc -V gcc_ntox86_64 -> target/build/*
mkqnximage -b -v \
  -B image/fragments/buildfile \
  -S image/fragments/startup.script \
  -f image/fragments/identity.sh \
  -f image/fragments/ham.script \
  -f image/fragments/security_policy.xml \
  -t qemu-x86_64 -o build/qnx-relais
python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/slalom-death-A.json
```

Details and the open hypotheses are in `image/README.md` and `target/README.md`.
The `.github/workflows/qnx-image.yml` workflow builds this image *only when* a
runner has the SDP; otherwise it ends in an explicit, commented `skipped`.

## 5. Prerequisites by distribution

`tools/bootstrap.sh` prints the exact command when a tool is missing. Required:
`gcc`, `make`, `python3`. Optional (only their step needs them): `cpio` and
`qemu-system-x86_64` (the VM demonstration). All are standard packages:

| Distro | Command |
|---|---|
| Debian / Ubuntu | `sudo apt-get install -y gcc make python3 cpio qemu-system-x86` |
| Fedora / RHEL | `sudo dnf install -y gcc make python3 cpio qemu-system-x86` |
| Arch | `sudo pacman -S --needed gcc make python cpio qemu-system-x86` |
| Alpine | `sudo apk add build-base make python3 cpio qemu-system-x86_64` |
| macOS (Homebrew) | `brew install gcc make python3 cpio qemu` |