# Relais — comparison with neighbouring projects

*An honest, sourced comparison. Every claim below points to a file and section
of this repository. The neighbouring projects were read on 11 November 2026 and
evolve; the source of record for their description is
[`docs/SPEC.md` §2](SPEC.md), which is reproduced here and re-checked against the
actual repositories where noted. `qnx-hotswap` is known only through its
description, as the specification itself flags.*

*Measured counterpart: the same scenarios, the same faults and the same seeds, played on two implemented baselines (a heartbeat with a watchdog, a lease with a term) — see [`docs/COMPARISON-MEASURED.md`](COMPARISON-MEASURED.md).*

**Standing.** Relais is a demonstrator *aligned with* Annex 6 of UN Regulation
No. 79. It is **not** compliant, not approved and not certifiable, and every
latency is a **measured worst case under emulation**, never a proven worst case
([`docs/SPEC.md` §2](SPEC.md), [`docs/SAFETY.md`](SAFETY.md)).

---

## 1. The five neighbours, one row each

| Project (public GitHub) | What it does | What it does not do | Source |
|---|---|---|---|
| `TheUndefinedEngineer/Medical_Infusion_Pump` (Apr 2026) | Infusion pump on a Raspberry Pi 5: six processes, a main and a back-up sensor, a 200 ms beat, a **2 000 ms** silence timeout, promotion in < 1 ms, demonstrated by forcibly stopping processes | Process-level redundancy on **one** machine; no described protection against two active at once; no automated test bench | [`docs/SPEC.md` §2](SPEC.md) |
| `AhmedAdelWafdy7/autoware-qnx-guardian` (hackathon) | A QNX guest supervises a Linux guest running automated driving; **100 ms** silence timeout; on failure it **stops** with a minimal-risk manoeuvre | It stops, it does not continue (fail-safe); no delay measurement nor fault injection to date (per its README) | [`docs/SPEC.md` §2](SPEC.md) |
| `zuhairhalawa/faultline` (hackathon) | Fault-injection bench on a target: crash, freeze, processor steal, slow/stuck sensor; a supervisor restarts processes; verdict per step, report | A **single** machine; no handover, no second | [`docs/SPEC.md` §2](SPEC.md) |
| `Lainholic/qnx-safepulse-lab` (hackathon) | A watchdog restarts a crashed driver (**61–89 ms** measured over 50 crashes) and goes to emergency stop after **50 ms** of bus silence | A **single** machine | [`docs/SPEC.md` §2](SPEC.md) |
| `atss0/qnx-hotswap` (hackathon) | Updates a driver while it runs, without its clients noticing (description; not read in detail) | Continuity during a **deliberate update**, not during a failure | [`docs/SPEC.md` §2](SPEC.md) |

The two timeouts that frame the field are quoted in the specification itself
([`docs/SPEC.md` §1.4](SPEC.md)): **100 ms** (guardian) and **2 000 ms**
(infusion pump). Relais targets **two periods, i.e. 10 ms** at 200 Hz — a gap
only tenable if the worst wake-up latency is small and known, which is the
real-time argument for the microkernel (`docs/SPEC.md` §1.4).

## 2. What Relais does that they do not

From [`docs/SPEC.md` §2](SPEC.md), the five differentiators, stated without
ambiguity:

1. **Two machines, not two processes.** The whole machine may die, freeze or
   lose a cable. All five neighbours either work within one machine (infusion
   pump process pair, faultline, safepulse) or stop rather than continue
   (guardian).
2. **Continue, not stop.** The command is not interrupted: at full level **no
   tick is ever served without a fresh order** after the holder dies
   ([`docs/RESULTS.md`](RESULTS.md) §1, REL-05), and at base level the gap is ≤
   3 periods bridged by the will (REL-01).
3. **The double command made impossible by construction, then verified by
   exhaustion.** 100 198 states at one fault, 10 000 330 at two, **zero
   violation** ([`docs/RESULTS.md`](RESULTS.md) §1; verified by re-running
   `./build/relais-explore`).
4. **A bound measured at every instant of the cycle.** 3 972 deaths swept over
   all instants, 10 000 freezes, 5 040 link cuts — 19 659 campaign runs, 0
   failure ([`docs/RESULTS.md`](RESULTS.md) §2). Not a few trials.
5. **A bench that tests itself with mutants.** 8 out of 8 flagged
   ([`docs/RESULTS.md`](RESULTS.md) §5). A mutant the bench misses is a defect of
   the bench, not of the software.

## 3. What they do that Relais does not (yet) — the honest column

This is the column a comparison must not leave out. The neighbours do real
things Relais has not done:

| Neighbour does | Relais has not | Source (Relais side) |
|---|---|---|
| `Medical_Infusion_Pump` runs on a **physical Raspberry Pi 5** | Relais runs on a PC and under QEMU; the hardware level (two real boards, level 4) is a stretch target (PERF-09), not delivered | [`docs/SPEC.md` §2](SPEC.md); `docs/SPEC-2.md` PERF-09 |
| `autoware-qnx-guardian` runs a **real QNX guest** supervising Linux | The Relais QNX layer (`target/`, `image/`) is written and syntax-checked under `-Werror` against stubs, but has **never been compiled for or run on QNX** — it needs the SDP | [`README.md`](README.md) "Status of this tree" |
| `faultline` has a **working fault-injection bench on a target**, verdict per step | The Relais QEMU bench (`host/bench/`) is written and the QMP commands were validated, but level 3 (real kills under QEMU) has **not been run**: it needs a QNX image | [`README.md`](README.md) "Status of this tree"; [`docs/RESULTS.md`](RESULTS.md) §8 |
| `qnx-safepulse-lab` published a **real restart latency** (61–89 ms over 50 crashes) on target | Relais has **no on-target latency measurement**; every latency is emulated and published as such | [`docs/SAFETY.md`](SAFETY.md) §7 item 5 |
| `qnx-hotswap` ships a **running hot-update** of a driver | Relais "update without interruption" is a level-X requirement (REL-10), listed in section 12 of the spec, **not implemented** | [`docs/SPEC-2.md`](SPEC-2.md) REL-10; [`docs/SPEC-ANNEX.md`](SPEC-ANNEX.md) §12 item 6 |

The same picture, in one sentence: **the neighbours are earlier on the "runs on
real hardware/real QNX" axis, and Relais is earlier on the "double command made
impossible and proven" axis.** None of the five does a handover between two
machines at all ([`docs/SPEC.md` §2](SPEC.md) opening line), which is both
Relais's claim to novelty and the reason its most defensible results are the
*logical* ones (exploration, campaign, mutants) rather than the *physical* ones.

## 4. Where the comparison stands honestly

- **Against the guardian (100 ms, stops):** Relais continues instead of
  stopping and drops the failure-detection latency target by an order of
  magnitude (10 ms vs 100 ms, `docs/SPEC.md` §1.4) — but with emulated timings
  and no running QNX guest, something the guardian actually has.
- **Against the infusion pump (2 000 ms, process pair):** Relais crosses the
  machine boundary instead of the process boundary, and its handover bound is a
  *verified* property over all instants, not a timeout chosen by the author —
  but the pump's physical hardware is real and Relais's is not.
- **Against faultline and safepulse:** Relais is not a fault-injection bench or
  a watchdog; it is the thing being fault-injected and the protocol that
  survives it. The honest overlap is that Relais *rebuilt* a bench to prove
  itself — and that bench has, so far, run on the PC only.

The prudence rule from [`docs/SPEC.md` §2](SPEC.md) applies to this document as
well: Relais never claims regulatory conformity or approvability, and the delays
are worst cases measured under emulation, never proven worst cases.