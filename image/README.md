# Building the QNX 8.0 image

One image boots both computers. They tell themselves apart by the MAC address of
their network card, so the image never has to be rebuilt to swap the roles —
which is the point of a redundancy demonstrator: the two machines really run the
same bytes (CMD-03).

Everything here is a **fragment** for `mkqnximage`, plus the boot script, the
supervision script and the security policy. Nothing is generated: read them,
adapt the paths to your SDP, build.

```bash
# 1. the SDP, through QNX Everywhere (free for non-commercial use)
source ~/qnx800/qnxsdp-env.sh          # adjust to your installation

# 2. the binaries, for the emulated target
cd target && make                       # qcc -V gcc_ntox86_64, see target/Makefile

# 3. the image, for QEMU x86_64
mkqnximage -b -v \
  -B image/fragments/buildfile \
  -S image/fragments/startup.script \
  -f image/fragments/identity.sh \
  -f image/fragments/ham.script \
  -f image/fragments/security_policy.xml \
  -t qemu-x86_64 -o build/qnx-relais
```

Boot two guests with two different MAC addresses and the programmable cable
between them (see `host/bench/QEMU.md` for the exact command line):

```bash
python3 host/bench/relais_bench.py --mode qemu --scenario scenarios/slalom-death-A.json
```

## What goes in the image

| Fragment | Role |
|---|---|
| `buildfile` | the binaries to embed: the four target processes, `io-sock` with its network driver, the tools the bench needs (`pidin`, `slog2info`, `tracer`, `dumper`) |
| `startup.script` | what runs at boot: `io-sock` and the driver, the network up, then the supervision script |
| `identity.sh` | maps the MAC address to the role A or B, and exports it — the image is identical on both machines |
| `ham.script` | the High Availability Manager starts and restarts the three processes (QNX-06) |
| `security_policy.xml` | typed security policy (SEC-05): only `relais-net` may open a socket, only `relais-core` may serve `/dev/relais`, and the test commands are not in the production image |

## Two identities, one image

`identity.sh` reads the MAC address of the first interface and exports
`RELAIS_SELF=A` or `RELAIS_SELF=B`. Every process reads that variable; nothing
else differs between the two machines. Swap the cables, the roles swap.

```
52:54:00:12:35:0A  ->  RELAIS_SELF=A
52:54:00:12:35:0B  ->  RELAIS_SELF=B
```

Set the same addresses on the QEMU command line (`-device
virtio-net-pci,mac=52:54:00:12:35:0A`), or edit the table to yours.

## What has been verified here, and what has not

Verified on a PC without the SDP:

- every `.c` and `.h` of `target/` compiles clean under `-Werror -Wshadow
  -Wconversion` against `target/host_stub.h` — `make -C target syntax`;
- all the protocol logic those processes call is the same `core/` that passes
  5630 unit checks, 100 198 explored states and a 1053-run fault campaign.

**Not** verified without the SDP, and stated as hypotheses (spec §14):

- that `io-sock` with `devs-vtnet_pci.so` or `devs-em.so` matches the network
  card QEMU emulates for this image (feasibility probe **E1**);
- the round-trip latency between a guest and a PC process, and whether a 5 ms
  period holds under KVM (**E2**, **E5**);
- what the guest's clock reports after `stop` / `cont` (**E3** — the protocol
  does not depend on it, only the story does);
- that a process obtains I/O privileges and dies through `isa-debug-exit`
  (**E4**; `image/death_points.md` gives the fallback);
- the boot-to-certified-second delay (**QNX-08**, **E6**);
- the security policy as written: it must be validated against the SDP's own
  schema and compiler before it is claimed to enforce anything (QNX-07).

Run the probes before believing any of it:
`python3 host/bench/relais_bench.py --probe E1`.
