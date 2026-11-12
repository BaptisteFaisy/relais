# Relais — hypervisor scenario (spec 12, idea 8)

**Requirement it demonstrates:** QNX-09 — the two computers are two QNX guests
of the QNX hypervisor on the same board (or two real boards). **Standing of this
document:** a procedure and a configuration *template*, not a measured result.
The QNX SDP and the QNX Hypervisor (QHS) are **not installed on this machine**, so
**no part of this scenario can be executed or validated here**. Everything below
is to be run the day the target image exists; it is written so the trial is
reproducible and its verdict is checkable from the journal alone.

## 1. The scenario (spec 12 #8, SPEC-ANNEX.md)

Three guests on one board:

| Guest | Role | Processes | Priority tier |
|---|---|---|---|
| `calc-a` | computer A | `relais-net`, `relais-core`, `relais-ctl` | real-time (62/61/60) |
| `calc-b` | computer B | `relais-net`, `relais-core`, `relais-ctl` | real-time (62/61/60) |
| `infotain` | infotainment | a heavy unprivileged workload (video, browser) | low, non-critical |

The claim to prove: **crashing the `infotain` guest has zero effect on the
steering command.** A and B keep exchanging `STATUT`/`ORDRE`/`OMBRE`, the
actuator keeps applying exactly one value per tick, and the journal shows no
gap beyond the normal testament/fallback coverage. The two compute guests are
isolated from the infotainment guest by the hypervisor: no shared RAM, no shared
CPU budget, no shared I/O path.

## 2. Hardware and partitioning (to adapt to the actual board)

- One SoC with at least 3 cores, VT-x/VT-d (or the QNX equivalent) for device
  passthrough where used.
- **CPU pinning:** pin `calc-a` and `calc-b` to dedicated cores; give `infotain`
  the leftover core(s) — never the same core as a compute guest (QNX-03 needs a
  fixed priority that a co-resident load must not disturb).
- **Memory:** static regions, no overcommit, one region per guest; the actuator
  guest (level C, ACT-08) may be a fourth guest or a fourth core of the same
  board.
- **I/O:** the inter-computer link is a virtual network between the two guests;
  the infotainment guest has only its own display/input devices. Two real NICs
  (or two passthrough NICs) is the fallback if `vnet` timing is not bounded.

## 3. Virtual networks between guests

Three links, all captured to pcap on the host (OBS-04):

1. `A <-> actuator` (UDP `dgram`)
2. `B <-> actuator` (UDP `dgram`)
3. `A <-> B` (state replication: `ETAT`, `SUIVI`)

The infotainment guest is **not** attached to these; it has one `user` netdev
for its own traffic, so its crash cannot run the inter-computer links.

## 4. Hypervisor configuration (template — validate against the installed QHS)

Grammar differs between QHS 2.0 / 2.2 / 3.0; this is the shape, not a byte-exact
file. Replace device addresses and memory sizes with the board's.

```qvmconf
# relais.qvmconf — illustrative QNX Hypervisor template. NOT validated here.
system "relais-soak" {
    # one vCPU and one RAM region per guest; compute guests never share a core
    vm "calc-a" {
        vcpu 1
        mem  512M
        # pin to core 1
        # pass-through NIC for the inter-computer link (if vnet is not bounded)
        pass ...
        vdev-net "link-a-b" { ... }   # A <-> B state replication
        vdev-net "link-a-act" { ... } # A <-> actuator
    }
    vm "calc-b" {
        vcpu 1
        mem  512M
        vdev-net "link-a-b" { ... }
        vdev-net "link-b-act" { ... }
    }
    vm "infotain" {
        vcpu 1
        mem  256M
        # no access to link-a-b / link-a-act / link-b-act
        vdev-net "wan" { ... }        # its own slirp/user netdev
    }
}
```

The two properties that carry the safety claim are **pinning** (a compute guest
never waits behind the infotainment load) and **no shared device** (a crash in
`infotain` cannot drop a frame on the compute links). These two are what the
trial verifies.

## 5. Step-by-step trial

1. Boot the hypervisor with `relais.qvmconf`; the three guests come up, the
   actuator follows in its own guest (or on the PC, mode `--actuator pc`).
2. Wait for the two computers to certify a second (lamp off, `certified=true`
   in the journal header/tick records).
3. Turn the steering profile on and let the run settle for 30 s (a red lamp at
   this point is already a finding — no fault has been injected).
4. Crash the infotainment guest: `qvm kill infotain` (or SIGKILL the `qvm` VM
   process), the way the bench kills a guest in `faults.py`.
5. Keep the two compute guests and the actuator running for at least 60 s.
6. Stop, collect the journal, and run the verifier over it.

## 6. What to measure, and how the journal proves it

| Metric | Where it is read | Pass criterion |
|---|---|---|
| fresh order gap | journal `tick` records | no gap beyond the `p+2` bound while a certified second exists |
| value applied each tick | journal `tick` records (`source` != `none`) | every tick has exactly one applied value (ACT-05) |
| no handover | journal `event` records | no `MANDATE_REVOKED` / `MANDATE_GRANTED` around the crash |
| lamp stays off | journal `tick` records | `lamp == off` throughout step 5 (I10) |

The single authoritative line is the journal (ACT-07): if the `infotain` crash
had disturbed the compute guests, either a tick is missing, or a value is
missing, or a handover appears. **Zero of those three effects = the isolation
holds.** The host-side pcap (OBS-04) is the cross-check that no frame was even
lost on the compute links during the crash.

## 7. Known limits (say these, don't hide them)

- **The infotainment crash is a clean kill**, not a hostile device that drives a
  shared bus; it exercises isolation, not a malicious DMA attack.
- **One board means one clock and one power rail**: a power cut here is not the
  "one machine dies" fault of the two-board scenario (idea 7).
- **`vnet` timing is not yet bounded on this PC**; if a `vnet` link adds
  unbounded jitter, the fallback is two passthrough NICs (probe E1/E2).
- No code here is validated — the SDP and QHS are absent; this is a runbook.

## Sources

- Relais spec, section 12 idea 8 (`docs/spec-fr.md`, `docs/SPEC-ANNEX.md`).
- Requirement QNX-09 (`docs/spec-fr.md` §9), ACT-07/ACT-08, OBS-04, QNX-03.
- QNX Hypervisor User's Guide and `qvm`/`qvmconf` reference (to be checked
  against the installed QHS version — not consulted on this machine).