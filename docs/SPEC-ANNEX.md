# Relais — specification, sections 9 to 16 (annex)

Last part of the specification. See [SPEC.md](SPEC.md) (sections 0–4) and
[SPEC-2.md](SPEC-2.md) (sections 5–8).

---

## 9. Deliverables and repository

```
relais/
├── README.md              English: pitch, demo in one command, proof of QNX usage
├── LICENSE                Apache-2.0
├── core/                  portable C library: arbiter, state machine, frames
├── target/                QNX processes: relais-net, relais-core, relais-ctl, relaisctl
├── actuator/              arbiter + rack model, for PC and for QNX
├── image/                 fragments for mkqnximage, security policy, identity by MAC address
├── host/
│   ├── bench/             machine launcher, orchestrator, programmed cable
│   ├── explore/           exhaustive exploration of the core
│   ├── simulate/          seeded simulation, fault-free twin
│   ├── verify/            relais-verify: invariants on a journal
│   ├── mutants/           variants with a deliberate defect
│   └── report/            HTML report, curves
├── scenarios/             reference trajectories, fault plans
├── docs/                  this specification, protocol, truth table, campaign results
└── codelab/               step-by-step tutorial
```

- **Languages**: C11 for the core, the QNX processes and the actuator;
  Python 3 without external dependency for the bench and the report.
- **Build instructions**: QNX version, target (QEMU x86_64), toolchain, as the
  hackathon rules require.
- **Proofs of QNX usage** to point at in the description: resource manager,
  message passing and priority inheritance, pulses, security policy,
  supervision, kernel trace.
- **Short safety document**, modelled on Annex 6 §3.4.3.2: principle of the
  transfer, logic, level of redundancy, built-in controls, limits of
  effectiveness.
- **Tutorial**, candidate for the bonus: "Two QNX machines, one master: detect
  a failure in two periods without mistaking the chief".
- **Everything in English** in the repository and the video.

---

## 10. Schedule

Thirty-four days separate Wednesday 11 November from Tuesday 15 December.

| Week | Dates | Content | Milestone |
|---|---|---|---|
| 1 | 11 – 17 Nov. | Feasibility trials E1 to E6. Pure core and unit tests. Explorer, first version. Simulated actuator and programmed cable on the PC. | The exploration passes on the PC |
| 2 | 18 – 24 Nov. | QNX processes. First end-to-end handover under QEMU: A is killed, B takes over. | One handover succeeded and measured |
| 3 | 25 Nov. – 1 Dec. | Freeze and thaw, cable cuts, reintegration, will. Orchestrator and first campaign. | **Base level reached**: 200 deaths, 2 000 freezes, zero violation |
| 4 | 2 – 8 Dec. | Shadows and substitution, certification, order-vs-shadow comparison. Death points, mutants, seeded simulation. Security policy, traces. | **Full level reached** |
| 5 | 9 – 14 Dec. | README, tutorial, video, campaign report. One extreme item of choice. Submission on the evening of the 14th. | Submitted, one day of margin |

**Feasibility trials (first three days).**

| Trial | Question | If the answer is no |
|---|---|---|
| E1 | Do two QNX machines built from the same image, linked by a `-netdev dgram` link through a relay on the PC, exchange UDP datagrams without administrator rights? | Multicast link (`-netdev socket,mcast`); failing that, each machine goes through the QEMU user network and the relay runs on the PC |
| E2 | What is the UDP round trip between a machine and a PC program, in median and in worst case, with KVM? | Lengthen the period to 10 or 20 ms; put the actuator in a third machine |
| E3 | After `stop` then `cont`, what does the guest's clock see? Does the machine know it slept? | No fallback necessary: the protocol is safe in both cases. The answer only fixes the narrative of the demonstration |
| E4 | Does a QNX process obtain access to the I/O ports and power the machine off through `isa-debug-exit`? | Death triggered from the outside at the wanted instant, without an exact code point |
| E5 | Inside the guest, what is the delay from the `STATUT` received to the order emitted under load, and at which priority does the network stack handle the frames? | Adjust the priorities of the network stack; lengthen the period |
| E6 | How long from the QEMU launch to a ready machine? | Lighten the image; restore a snapshot instead of restarting |

**What is dropped first in case of delay**: the actuator in a third machine,
the frame authenticator, the live dashboard, the braking profile. The base
level, the exhaustive exploration and the mutants M1 to M4 are not dropped.

---

## 11. Risks

| Risk | Severity | Countermeasure |
|---|---|---|
| The PC introduces delays that make ticks missed without any fault | High | Trials E2 and E5 from day one; processors reserved for each machine; slow-motion mode for the logic campaigns; timing measurements published separately, with their distribution |
| The direct link between two QEMU does not work as expected with QNX | High | Trial E1; two fallback solutions described |
| "The actuator is a single point of failure" | Medium | Say it in the scope; tiny arbitration logic explored in its entirety; two channels at extreme level |
| The project is perceived as a simple heartbeat with a switchover | Medium | Put forward the arbiter, the returning machine, the exploration and the mutants; the video shows the freeze and the cable cut, not only the death |
| The QEMU durations are challenged | Medium | "Emulated" mention; safety does not depend on time; real boards if available |
| The scope overflows | High | Levels S, C, X; base milestone at the end of week 3 |
| A defect common to both machines kills them together | Medium | Out of scope, announced; fallback law tested; dissimilar back-up at extreme level |
| A regulatory claim is judged abusive | Medium | "Aligned with" wording, never "compliant" |
| A neighbouring project appears before the deadline | Low | Check the gallery before submitting; differentiation of section 2 |
| This project is added to other submissions led in parallel | High | Reuse the tooling already written (QEMU launch and stop in a loop, state exploration); check in the rules that submissions sharing tooling remain distinct |
| Access to the I/O ports is refused to an ordinary process | Low | Death points reserved to a test image; fallback by external stop |

---

## 12. Pushing the idea to its limit

Beyond the full level, in order of interest for a jury:

1. **Automatic search for the worst scenario.** An optimiser drives the bench:
   it chooses fault instants, durations and combinations to maximise the
   deviation from the fault-free twin. The report publishes the worst case
   found and the search time. This is the most defensible use of AI in this
   project.
2. **Three machines and voting.** With a third computer, the actuator applies
   the value on which two machines agree: a machine that computes wrongly is
   unmasked, not merely detected.
3. **Independent formal specification.** The protocol is described in a
   specification language (TLA+ for example) and checked by a model explorer.
   The two explorations must agree on the invariants.
4. **Dissimilar back-up.** The second runs a simpler control law, written
   separately. It covers the common software defect, at the price of an
   admitted deviation at handover.
5. **Two actuator channels.** Each channel has its own arbiter, and the loss of
   one channel is tolerated. The actuator stops being a single point of
   failure.
6. **Update without interruption.** Update the second, hand it the mandate,
   update the other one. No tick without a fresh order.
7. **Real hardware.** Two Raspberry Pi boards linked by Ethernet, a third one
   for the actuator. Faults become power cuts through a relay.
8. **Hypervisor.** The two computers are two QNX guests on the same board,
   next to a guest playing the infotainment, which is made to crash.
9. **Handover log for post-accident analysis.** Every mandate change is handed
   to an event data recorder, with its cause and its delay.
10. **Reduced safety file.** Failure mode analysis generated from the truth
    table, and traceability matrix from requirements to tests.
11. **Replay of a fault, identical.** QEMU record and replay on one machine, to
    review the worst case found instruction by instruction.
12. **Keys renewed at every start-up** and rotated while running, without a
    lost tick.

---

## 13. Acceptance criteria

**Base — submission possible.**

- [ ] All level-S requirements are verified.
- [ ] A single command replays the demonstration under QEMU, without
      administrator rights.
- [ ] 100 000 core states explored, zero violation.
- [ ] 200 deaths, 2 000 freezes and 500 link cuts for real, zero violation.
- [ ] On a handover after death, never more than p+2 ticks between two fresh
      orders.
- [ ] The mutants M1 to M4 are flagged.
- [ ] Thirty minutes under load without a wrongful handover.
- [ ] Public repository, licence, README in English, video of less than three
      minutes.

**Full — targeted version.**

- [ ] All level-C requirements are verified.
- [ ] Ten million states explored with two faults; one billion ticks simulated.
- [ ] 2 000 deaths swept, all death points, 10 000 freezes, 5 000 cuts: zero
      tick without a fresh order with a certified second.
- [ ] The eight mutants are flagged.
- [ ] Twelve hours under load without a wrongful handover; the three worst
      cases explained by a trace.
- [ ] An intrusive process is refused by the security policy.
- [ ] Safety document and tutorial published.

**Extreme.**

- [ ] At least two items of section 12 realised and measured.

---

## 14. What is verified and what remains to be validated

| Claim | Status |
|---|---|
| Paragraphs of Regulation No. 79 cited in 1.2 | Read on 11 November 2026 on a PDF copy of revision 4 (7 November 2018, series 03 of amendments) hosted by a third-party site. Later amendments exist, and a UN working group discusses in 2025–2026 precisions on steer-by-wire. **To be re-read against the text in force.** |
| Steer-by-wire is approvable under the name *full-power steering* since 2003, and falls under Annex 6 | According to the British approval agency, via a research summary; page not read in full |
| ISO 26262 definitions (FTTI, FDTI, FRTI, EOTTI) | Concordant secondary sources; the standard itself was not consulted |
| Infiniti Q50 and Tesla Cybertruck | Specialised press and Wikipedia; no manufacturer document |
| QEMU options of section 4.11 | Tried on this PC with QEMU 8.2.2: the full command line is accepted, the `stop`, `cont`, `quit`, `system_reset` and `set_link` commands exist, `set_link` succeeded on a stopped machine. **No frame exchange between two guests has been tried.** |
| KVM available | The device is present on this PC; not tried with QNX |
| Content of the neighbouring repositories | Read on 11 November 2026; they evolve. `qnx-hotswap` is known only through its description |
| No public project of handover between two QNX machines | Search on the names and descriptions of GitHub repositories only, not on their content nor on private repositories |
| Access to the I/O ports under QNX 8.0 (`ThreadCtl`, I/O level, required capability) | Verified in the QNX 8.0 documentation; not tried (E4) |
| User events in the kernel trace (`trace_logf`) | Verified in the QNX 8.0 documentation |
| Network drivers `devs-vtnet_pci.so` and `devs-em.so` for `io-sock` | Cited in the QNX 8.0 documentation; correspondence with the cards emulated by QEMU not confirmed (E1) |
| Two QNX machines exchange frames through a `dgram` link | **Hypothesis**, trial E1 |
| Delay Δ of at most 1 ms, computation in 0.5 ms, jitter of at most 1 ms | **Hypotheses**, trials E2 and E5 |
| 5 ms period tenable under QEMU | **Hypothesis**, trials E2 and E5 |
| Behaviour of the guest clock after a freeze | Unknown, trial E3; the protocol does not depend on it |
| Priority inheritance down to the network stack | **Hypothesis** founded on the general operation of QNX, to be shown by trace (QNX-04) |
| Handover bounds (3 periods at base level, zero tick at full level) | Reasoning on the protocol, section 4.5; **targets**, to be measured |
| Exploration sizes (100 000 and ten million states) | Orders of magnitude estimated, not measured |
| Bit-identical computation on the two machines | **Hypothesis** (same binary, same virtual processor), checked by DET-09 |
| Supervision service and security policy under QNX 8.0 | Seen in competing repositories, to be confirmed in the documentation |
| Cluster managers, floating addresses, COLO, time-triggered buses, flight computers | Cited from memory as landmarks, not re-verified |
| Increasing token refused by the resource when it is stale | Principle described by M. Kleppmann in 2016; article located, read by summary |

---

## 15. Glossary

| Term | Meaning |
|---|---|
| Fail-safe, fail-operational | A system that becomes safe by falling silent; a system that must keep operating to stay safe |
| Tick | Periodic beat given by the actuator; time unit of the protocol |
| Mandate | Right to command, numbered by the actuator |
| Holder | Machine that holds the current mandate |
| Second | Machine that follows the holder, ready to replace it |
| Certified ready | Second whose shadows agree with the orders, noted by the actuator |
| Fresh order | Order computed from the `STATUT` of the previous tick |
| Shadow | Order computed by the second, sent to the actuator without a mandate |
| Substitution | Tick served by the second's shadow because the holder's order is missing |
| Will (testament) | Values planned for the following ticks, embedded in every order |
| Fallback law | Local command of the actuator when it has neither order nor will left |
| Handover | Transfer of the mandate from one machine to the other |
| Cession | Voluntary abandonment of the mandate by its holder |
| Deposed | Machine from which the actuator has revoked the mandate |
| Returning machine | Machine that resumes after a freeze believing itself still holder |
| Double command | Two machines whose orders would be applied at the same time; what the project makes impossible |
| Fault-free twin | The same scenario replayed without fault, used as reference |
| Programmed cable | Software relay that drops, delays or reorders frames on demand |
| Death point | Place in the code where the machine powers off on command, for the tests |
| Mutant | Variant of the software with a deliberate defect, used to test the bench |
| Lamp | Warning signal: off, yellow or red |
| Resource manager | QNX process that appears as a file or a directory, for example under `/dev` |
| Pulse | Small non-blocking QNX message, used for notifications |
| Security policy | QNX rules that fix which types of processes may talk to each other |

---

## 16. Sources

**Regulation and standards**

- UN Regulation No. 79, revision 4 (document E/ECE/324/Rev.1/Add.78/Rev.4),
  copy consulted: https://villanyautosok.hu/wp-content/uploads/2019/05/r079r4e.pdf
- Ongoing work on Regulation No. 79:
  https://globalautoregs.com/rules/90/documents?year=2025
- British approval agency, steer-by-wire:
  https://www.vehicle-certification-agency.gov.uk/blog/assessing-the-compliance-of-steer-by-wire-systems-for-individual-vehicle-approval
- ISO 26262, definitions (secondary source):
  https://www.arteris.com/blog/critical-safety-overview-and-definitions/
- ISO 26262, fault tolerant time interval (SAE paper 2024-01-2791):
  https://saemobilus.sae.org/downloads/papers/2024-01-2791/Full%20Text%20PDF

**Production examples**

- Steer-by-wire: https://www.wikipedia.org/wiki/Steer-by-wire
- Cybertruck redundancy, teardown:
  https://theautopian.com/heres-the-fascinating-redundancy-manufacturing-experts-found-when-they-tore-into-the-tesla-cybertrucks-steer-by-wire-system

**QNX**

- Thread I/O privileges:
  https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_thread_IO_privileges.html
- `ThreadCtl()`:
  https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/threadctl.html
- `trace_logf()`:
  https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/trace_logf.html
- Clocks and timers:
  https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/kernel_CLOCKANDTIMER.html
- `io-sock` start-up and drivers:
  https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/start_io-sock.html
- Network drivers for `io-sock`:
  https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxsdp.migration/topic/bsp_support.html
- Client-side resumption library (QNX 7.1):
  https://www.qnx.com/developers/docs/7.1/com.qnx.doc.ham/topic/client_funcs.html

**QEMU**

- Launch options: https://www.qemu.org/docs/master/system/invocation.html
- Control commands (QMP):
  https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html
- Record and replay: https://www.qemu.org/docs/master/system/replay.html

**Principles**

- M. Kleppmann, "How to do distributed locking" (2016):
  https://martin.kleppmann.com/2016-02-08/how-to-do-distributed-locking.html

**Hackathon and neighbouring projects**

- Page and rules: https://qnx.devpost.com/ and https://qnx.devpost.com/rules
- https://github.com/TheUndefinedEngineer/Medical_Infusion_Pump
- https://github.com/AhmedAdelWafdy7/autoware-qnx-guardian
- https://github.com/zuhairhalawa/faultline
- https://github.com/Lainholic/qnx-safepulse-lab
- https://github.com/atss0/qnx-hotswap
