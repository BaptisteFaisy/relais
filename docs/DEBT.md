# Relais — assumed technical debt

Every entry states plainly: **what is shortcut**, **why it is acceptable now**,
**what would break if it were forgotten**, and **the effort to lift it**. This
is the honest complement to `docs/RESULTS.md` (what is measured) and
`docs/SAFETY.md` §7 (limits of effectiveness). Nothing here is hidden: the
README's "Status of this tree" table and `host/bench/QEMU.md` §6–7 carry the
same facts.

The debt is assumed because Relais is a **demonstrator**, not a product. The
safety argument is complete *for the emulated scope* and openly bounded; lifting
the debt below is what would turn it into something certifiable.

---

## DT1. The QNX target layer has never been compiled with the SDP

**Shortcut.** `target/` and `actuator/qnx/` are syntax-checked only
(`make -C target syntax`, `gcc -fsyntax-only` against `host_stub.h`). They have
never been built with `qcc` from the QNX SDP 8.0, never linked, never booted.
The five processes (`relais-net`, `relais-core`, `relais-ctl`, `relaisctl`,
`relais-actuator`) are **written, never run**.

**Why acceptable now.** The protocol core is pure C11 with no clock, no file, no
socket and no allocation (`core/include/relais/types.h`); the same `core/` is
what passes the unit tests, the exhaustive exploration and the fault campaign.
The QNX layer is thin glue that re-implements no protocol logic, so a
syntax-check under `-Werror -Wshadow -Wconversion` against typed QNX stubs is a
real (if partial) proof: it proves the code is well-formed C typed against the
QNX API surface it uses.

**What would break if forgotten.** A claim that "Relais runs on QNX 8.0" would
be the one claim in the repository that nothing backs. Seven things still need a
real SDP and a real boot (listed in `target/README.md`): `io-sock` and the
driver matching the emulated NIC (E1); the guest↔host round trip and whether a
5 ms period holds under KVM (E2, E5); the guest clock after `stop`/`cont` (E3);
I/O privileges and death by `isa-debug-exit` (E4); the security policy refusing
an intruder (QNX-07); boot-to-certified-second delay (E6); HAM restarting a
process and peers noticing.

**Effort to lift.** Obtain the QNX SDP 8.0 (free through QNX Everywhere),
`source qnxsdp-env.sh`, `make -C target`, build the image with `mkqnximage`,
boot two guests under QEMU/KVM and run the six probes E1–E6
(`python3 host/bench/relais_bench.py --probe all`). Days, gated on SDP access
and a working KVM host.

---

## DT2. The QEMU bench has never exchanged a frame between two guests

**Shortcut.** Level 3 (real faults under QEMU) is **written, not run**. No QNX
image exists in this working copy (`image/` is empty), so probes E1–E6 are all
`BLOQUE`. The only thing measured is QEMU's own startup (≈ 220 ms to QMP with
`-S`); no guest ever booted, no `ORDRE`/`STATUT` ever crossed a virtual link.
The QMP freeze demo (`stop`/`cont`) was accepted by QMP on an **idle** guest —
if a bootable guest is used at all for the demo, it is a **Linux** guest, not
QNX, because no QNX image has been built.

**Why acceptable now.** The bench code is exercised at the QMP/protocol level
(`set_link`, `stop`/`cont`, `system_reset`, `isa-debug-exit` are all accepted by
QEMU and driven correctly); what is unproven is the *guest* side, which is DT1.
The safety argument does not rest on level 3: levels 0–2 (unit, exploration,
simulation) already prove the protocol, and level 3 would only confirm it on
real hardware-emulated guests.

**What would break if forgotten.** Claiming "faults injected on real QNX guests"
or publishing a handover latency "measured on the target" would be false. The
INJ-02…INJ-10 requirements are *designed* but their guest-side execution is
unproven.

**Effort to lift.** Same as DT1 (build the QNX image), then run
`relais_bench.py --mode qemu` with two guests and the actuator, and record the
distributions. Days.

---

## DT3. The security policy has never been enforced

**Shortcut.** The typed QNX security policy (`image/`) is a **draft**: it states
the intent (one process owns the sockets, one owns `/dev/relais`, the law has
neither, an untrusted process has no path to any of them) but it has **never
been compiled, loaded, or opposed to a real intruding process**. The box "an
intruding process refused by the security policy" is deliberately left unticked
in `docs/RESULTS.md` rather than marked green.

**Why acceptable now.** The threat model of the demonstrator stops at frame
authentication (CRC-32C / SipHash MAC, SEC-01/02/03). The policy is defence in
depth for a production deployment, not part of the protocol's safety argument,
which holds regardless of who is allowed to talk to `/dev/relais`.

**What would break if forgotten.** Claiming SEC-05 / QNX-07 ("an untrusted
process is refused") as *verified* would be unsupported. The policy must be
validated against the real SDP schema before anything is claimed about it.

**Effort to lift.** Write the policy against the SDP schema, load it on a booted
guest, and run a probe that tries to open `/dev/relais` or a socket from an
untrusted process and confirms the refusal. Hours, gated on DT1.

---

## DT4. All timings are emulated, never measured on target

**Shortcut.** The 5 ms / 200 Hz period, the "3 periods at level S" and "0 tick
at level C" bounds, and every latency in `docs/RESULTS.md` are **measured worst
cases under emulation** on a shared PC, never proven worst cases on QNX under
KVM. Both guests share the PC's processor, so a load spike hits both machines at
once and biases every "one machine is slow" experiment.

**Why acceptable now.** The protocol's *safety* does not depend on timing — only
its *liveness* (how fast the handover is) does (`docs/SAFETY.md` §2, spec §4.3).
The bounds are claimed only under the timing hypothesis `2*delta + C + J <= T`
(D6/D9 in `docs/DECISIONS.md`); outside it the run is marked faulted and no
bound is claimed. So the emulation gives a real, reproducible worst case *for
the stated hypothesis*, which is exactly what the prudence rule allows.

**What would break if forgotten.** Publishing the 15 ms / 0 ms figures as
"proven" or "certified" latencies. They are targets to be re-measured on the
QNX target under KVM, with their full distribution, and marked "emulated" until
then.

**Effort to lift.** Run the campaign on real QNX guests under KVM with reserved
CPUs (DT1+DT2), publish the distributions, and replace every "emulated" tag with
a measured one. Days.

---

## DT5. Killing QEMU is not cutting power; `set_link` is not a cable

**Shortcut.** The bench's fault injection is process-level and link-state-level:
`quit`/`kill` stop a QEMU process (a clean exit, not a brown-out, not a
mid-write corruption), and `set_link up=false` is a netdev link-state change,
not a physically unplugged cable. No fine hardware fault is produced (a flipped
bit in memory, a stuck register, a partial write).

**Why acceptable now.** The fault *model* of spec 3.3 (F1–F11) is about
protocol-visible behaviour — a machine that stops answering, a link that drops
frames — and the bench reproduces exactly that. The protocol's safety argument
covers the model, not the physical cause. Fine hardware faults are M18's scope
(`host/faultinject/`), a separate mission.

**What would break if forgotten.** Claiming the bench tests "real hardware
faults" or "power loss". It tests the protocol's reaction to the *modelled*
faults; a brown-out that corrupts state mid-write is out of scope.

**Effort to lift.** Add a fault-injection layer that flips bits in the guest's
memory and models partial writes (M18), and a power-cut model that corrupts
state. Weeks.

---

## DT6. The actuator is a single point of failure (F11 unmitigated)

**Shortcut.** There is **one** actuator. Its arbitration core is deliberately
tiny (one file, `core/src/arbiter.c`, under 500 lines, no allocation, no clock,
no I/O) so it can be explored exhaustively, but a failure of the actuator itself
(F11) has **no mitigation measure and no covering test** — it is flagged by
`host/safety/fmea.py` as the one unmitigated fault of the model.

**Why acceptable now.** F11 is declared out of scope (`docs/SAFETY.md` §6–7);
the demonstrator's safety goal is about the *switch-over between two computers*,
which the actuator arbitrates. Making the actuator itself redundant is the
level-X extension (two actuator channels, ACT-10 / MAN-13), explicitly listed as
an extension, not a claim.

**What would break if forgotten.** Claiming the system tolerates an actuator
failure. It does not; it tolerates a *computer* failure, with the actuator as the
trusted arbiter.

**Effort to lift.** Build two actuator channels with a voter (M06/M20 scope),
re-explore the combined state space, and re-run the campaign. Weeks.

---

## DT7. No majority: a machine that computes wrong is detected, not outvoted

**Shortcut.** With two computers there is no majority. When the holder emits a
wrong value (F8), the actuator detects the disagreement (order/shadow comparison
over 3 ticks, DET-09), decertifies the second, lights the red lamp and **keeps
the holder** — it does not arbitrate or mask the fault. Masking needs a third
machine and a vote (level X, REP-08).

**Why acceptable now.** The demonstrator's claim is "double command is
impossible", not "a wrong value is masked". Detection + red lamp + the holder
kept is the honest behaviour for two machines; the disagreement is *reported*,
never silently absorbed.

**What would break if forgotten.** Claiming fault *masking* or "the system
corrects a wrong value". It detects and reports; it does not outvote.

**Effort to lift.** Add a third machine and a voter (M05 scope: "three machines
and vote"), re-prove the invariants with the vote, re-run exploration. Weeks.

---

## DT8. The dissimilar backup is never certified

**Shortcut.** DEG-08 (a dissimilar control law as backup) is implemented and
tested (`git 01f22aa`), but a dissimilar law is **never certified**, because its
shadow does not match the holder's order bit-for-bit. The actuator reports the
disagreement (DET-09) and lights the red lamp instead of pretending. The
zero-tick guarantee of REL-05 is asserted only on the built-in (similar) law.

**Why acceptable now.** This is the trade the spec states: a dissimilar backup
covers a defect common to both copies of the software, at the price of an
admitted bump at the handover. The demonstrator measures that limit honestly
rather than hiding it.

**What would break if forgotten.** Claiming the dissimilar backup gives bump-free
redundancy. It gives *defect-diverse* redundancy with an admitted bump.

**Effort to lift.** Define a certification path for a dissimilar law (e.g.
plausibility bounds instead of bit-equality), or accept the bump and document the
handover deviation for the dissimilar case. Days.

---

## DT9. The threat model stops at frame authentication

**Shortcut.** The security boundary is the frame check: CRC-32C at level S,
keyed SipHash-2-4 MAC at level C (SEC-01/02/03), plus the tick label as
anti-replay. An attacker able to **observe and inject on the actuator link in
real time, with the key**, is out of scope. Key rotation, anti-replay windows
beyond the tick label, and a chained journal are M21's scope, not built here.

**Why acceptable now.** The demonstrator's threat is accidental faults and
benign-frame forgery, not a keyed real-time adversary. The MAC stops forged
frames; the tick label stops captured-frame replay. That is the stated boundary.

**What would break if forgotten.** Claiming the system is secure against an
active, keyed, real-time adversary on the actuator link. It is not; that is
explicitly out of scope.

**Effort to lift.** M21: key management, rotation, anti-replay window, chained
journal. Weeks.

---

## DT10. The video is not recorded

**Shortcut.** The three-minute video deliverable is **to be recorded**
(`README.md` status table). Everything it would show — the demo, the campaign,
the report — exists and runs; only the recording is missing.

**Why acceptable now.** The proofs are reproducible by command (`docs/RESULTS.md`
gives the exact commands); the video is presentation, not evidence.

**What would break if forgotten.** The submission would be incomplete against
the deliverable list, though the technical content is all present.

**Effort to lift.** Record `make demo`, the campaign and the HTML report. Hours.

---

## How to verify this list

Each entry is cited against a file or a commit a reader can open:

```
make -C target syntax              # DT1: what is proven without the SDP
sed -n '125,160p' host/bench/QEMU.md   # DT2, DT4, DT5: BLOQUE probes, known limits
git show efbb369    # DT3: "the policy ... has never been compiled or enforced"
git show 01f22aa    # DT8: the dissimilar backup is never certified
grep -n 'Status of this tree' README.md # the same facts, in the README table
python3 host/safety/fmea.py --out /tmp/fmea.md   # DT6: F11 flagged as unmitigated
```