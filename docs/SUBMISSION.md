# Relais — submission notes against the hackathon rules

*How the project answers the QNX Global Hackathon 2026 criteria, one by one,
with the proof and its location in the repository. Automotive & Mobility
category. Submission deadline: Tuesday 15 December 2026, 20:00 Paris time
([`docs/SPEC.md`](SPEC.md) header).*

**Standing, stated first because the jury reads it first.** Relais is a
demonstrator *aligned with* Annex 6 of UN Regulation No. 79. It is **not**
compliant, not approved and not certifiable ([`docs/SAFETY.md`](SAFETY.md)
standing note). No timing in this submission is a proven worst case: every
latency is measured under emulation and published with its bounds.

---

## 1. Criterion — technical achievement

**Claim.** A verified handover between two computers, where the actuator is the
arbiter and the clock, and double command is impossible by construction.

**Proof and location.**

- The protocol core is a pure C11 library — no clock, no file, no socket, no
  allocation — compiled identically into the QNX processes, the PC simulator and
  the explorer (`core/`, ACT-06 in [`docs/SPEC-2.md` §5.6](SPEC-2.md)).
- Exhaustive exploration: **100 198 states at one fault** and **10 000 330
  states at two faults, zero violation** ([`docs/RESULTS.md`](RESULTS.md) §1;
  `make explore`, `./build/relais-explore`).
- Seeded campaign: **19 659 runs, 0 failure**, including 3 972 swept deaths,
  10 000 freezes, 5 040 link cuts, 12-hour runs under 1 % loss, and a
  one-billion-tick run ([`docs/RESULTS.md`](RESULTS.md) §1–§2).
- Unit and protocol layer: **5 651 checks green** (`make test`).
- The bench proves itself: **8/8 mutants flagged** (`python3
  host/mutants/run_mutants.py`, [`docs/RESULTS.md`](RESULTS.md) §5).

## 2. Criterion — complete and coherent product

**Claim.** From a one-command launch to a self-contained report, with a service
tool, a tutorial and a security/observability story.

**Proof and location.**

- One command replay without root: `make demo` (`README.md`; Makefile `demo`
  target).
- Offline invariant checker on the authoritative journal: `host/verify/`
  (`relais-verify`, OBS-07).
- Self-contained HTML campaign report, inline SVG, no external resource:
  `host/report/` (`README.md` "Status of this tree", OBS-05).
- Service tool and resource manager: `target/relaisctl.c` serves `/dev/relais`
  (QNX-01, OBS-02).
- Tutorial candidate for the "best tutorial" bonus: `codelab/`, expanded into
  `codelab/TP-3h.md`.
- Fault plans as replayable JSON: `scenarios/` (INJ-06).

## 3. Criterion — potential impact

**Claim.** A real problem in a QNX market, where real time is the point.

**Proof and location.**

- Steer-by-wire and automated driving both make silence a non-safe state, which
  is exactly what UN Regulation No. 79 Annex 6 §3.4.3.2 asks a manufacturer to
  explain ([`docs/SPEC.md` §1.1–1.2](SPEC.md), [`docs/SAFETY.md` §1](SAFETY.md)).
- **Why real time is the point:** detecting a fault by silence only works if
  delays are bounded. The neighbouring projects set their silence timeout at
  100 ms and 2 000 ms; Relais targets 10 ms, only tenable under a microkernel
  with bounded wake-up latency ([`docs/SPEC.md` §1.4](SPEC.md)).
- The QNX-specific technologies are mapped requirement by requirement: resource
  manager, message passing with priority inheritance, pulses, connection-death
  notification, security policy, HAM supervision, kernel trace
  ([`docs/SPEC.md` §4.9](SPEC.md), `README.md` "Use of QNX-specific technology").

## 4. Criterion — innovation and creativity

**Claim.** The actuator is the arbiter *and* the tempo; the will; the mutants
that prove the bench.

**Proof and location.**

- The two ideas that carry the design: the actuator hands out and revokes the
  mandate on its own evidence, and it gives the tempo so no clock synchronisation
  is needed ([`docs/SPEC.md` §0](SPEC.md), ideas 1–2).
- The **will (testament)**: every order carries the plan for the next N ticks, so
  the physical curve shows no hole during a handover ([`docs/SPEC.md` §0](SPEC.md)
  idea 3, §4.4).
- **Proof of the proof**: deliberately defective builds must be flagged, turning
  the test bench itself into a tested artefact ([`docs/SPEC.md` §0](SPEC.md)
  idea 4, §7.6; [`docs/RESULTS.md`](RESULTS.md) §5).

## 5. Criterion — use of AI

**Honest answer, in three tiers, from strongest to what actually exists today.**

1. **The automatic search for the worst fault scenario** — an optimiser that
   chooses fault instants and durations to maximise deviation from the fault-free
   twin. The specification calls this *"the most defensible use of AI in this
   project"* ([`docs/SPEC-ANNEX.md` §12 item 1](SPEC-ANNEX.md)). **Status: it is
   a level-X requirement (INJ-12), not implemented.** It must be labelled as a
   target, not a claim.
2. **The self-testing mutant bench** — automated adversarial testing: eight
   defective builds must each be flagged by the invariant they break
   ([`docs/SPEC.md` §7.6](SPEC.md), [`docs/RESULTS.md`](RESULTS.md) §5).
   Deterministic and reproducible; valuable, but automation, not learning.
   **Status: implemented, 8/8.**
3. **The repository's CI automation agents** — code that exists in the
   repository today: a conflict-resolution agent, a scope-check agent, a
   compilation gate and a red-test triage workflow
   (`.github/fireworks/` — `conflict_agent.py`, `scope_agent.py`,
   `compilation_gate.py`, `test_agent.py`; workflows `conflict-resolver.yml`,
   `triage-rouge.yml`, `pr-auto-merge.yml` in `.github/workflows/`).
   **Status: present in the repository.**

Read together: the *reproducible* AI item is the CI agent harness; the *best*
AI item (worst-case search) is honestly still a target, and saying so is part of
the submission's credibility.

---

## 6. Deliverables checklist, and their real status

| Required by the rules | Status | Location |
|---|---|---|
| Public repository | Licence and English README present; the repository is described as public in the spec and its CI is push-through-PR (`README.md` "Repository automation"). Public visibility to be confirmed at submission time | `LICENSE`, `README.md` |
| Free licence | **Apache-2.0**, `LICENSE` file present | `LICENSE`, `README.md` "Licence" |
| Description | **Present**: pitch, four ideas, architecture, measured guarantees | `README.md` |
| Build instructions with QNX version, target, toolchain | **Present**: QNX SDP 8.0 (via QNX Everywhere), target `QEMU x86_64`, toolchain `qcc -V gcc_ntox86_64`, image built with `mkqnximage`, QEMU ≥ 8.2 with KVM, no admin rights | `README.md` "Build requirements"; `codelab/README.md` Step 8 |
| Video < 3 minutes on YouTube | **Not yet recorded** — flagged in the acceptance checklist as such, with the storyboard written | [`docs/RESULTS.md`](RESULTS.md) §8 ("[ ] video under three minutes"); [`docs/SPEC-2.md` §8](SPEC-2.md) storyboard |
| All in English | **Present** for the repository and docs (the French original `docs/spec-fr.md` is kept as the reference of record for the jury) | rule 4, `docs/` |

## 7. Acceptance criteria, as claimed

The socle (base) and complet (full) acceptance checklists are in
[`docs/RESULTS.md` §8](RESULTS.md), reproduced with the "to be done" boxes left
open exactly as the document states them: the video (not recorded), the kernel
trace of the three worst cases (needs the target and `tracer`), and the refused
intrusive process (needs the security policy compiled under the SDP). Relais
submits what is measured (levels 0–2), and marks what is not (level 3 QEMU and
the QNX target) rather than implying it is done.