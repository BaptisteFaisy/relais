#!/usr/bin/env python3
"""Relais -- the failure modes of the FMEDA (mission M39).

The data of the model, kept apart from the arithmetic (``model.py``). Every
entry declares:

* the element it belongs to and the F-code of the safety model it comes from
  (read from ``host/safety/fmea.py``, never typed twice),
* the *share* of the element's rate spent on that mode -- a HYPOTHESIS, with
  its rationale,
* the effect on the safety goal (safe or dangerous),
* the diagnostic mechanism that really exists in the code, with the file that
  implements it, and the requirement it answers,
* the diagnostic-coverage class: the class values are cited (ISO 26262-5,
  Annex D, Table D.2), the assignment is engineering judgement.

Standard library only.
"""

from __future__ import annotations

# ---------------------------------------------------------------------------
# Failure modes of the elements. Every share is a HYPOTHESIS; every diagnostic
# mechanism must exist in the code (the evidence column points at it).
# ---------------------------------------------------------------------------
MODES = [
    # --- ECU: the QNX computer, identical for the two machines --------------
    {"element": "ECU", "code": "F1", "mode": "machine stops responding (death)",
     "effect": "safe", "share": 0.20, "dc": "high",
     "mechanism": ("actuator counts p+1 ticks without a valid order and "
                   "revokes the mandate (MAN-04), then auto-switches to the "
                   "certified second (REL-01)"),
     "requirements": "MAN-04, REL-01, DET-03",
     "evidence": "core/src/arbiter.c:416 (miss counter), core/src/arbiter.c:109 (cession event)",
     "share_source": "HYPOTHESIS -- share of the ECU rate spent on a hard stop",
     "note": "Safe at system level: the second machine keeps the service."},
    {"element": "ECU", "code": "F2", "mode": "machine freezes then resumes",
     "effect": "safe", "share": 0.15, "dc": "high",
     "mechanism": ("same silence counter, and on thaw the stale order is "
                   "dropped twice over: mandate number and tick label "
                   "(MAN-08, REL-07)"),
     "requirements": "MAN-04, MAN-08, REL-07",
     "evidence": "core/src/arbiter.c:54 (holder + mandate + tick accepted), core/src/arbiter.c:70 (mandate check)",
     "share_source": "HYPOTHESIS -- share of the ECU rate spent on a freeze",
     "note": "Safe: the returning machine is disarmed by construction."},
    {"element": "ECU", "code": "F3", "mode": "machine runs slow, misses ticks",
     "effect": "safe", "share": 0.20, "dc": "medium",
     "mechanism": ("miss counter published in every STATUT; the machine is "
                   "deposed only when the misses are consecutive (DET-03)"),
     "requirements": "DET-03, ROB-03",
     "evidence": "core/src/arbiter.c:126 (miss count in the event)",
     "share_source": "HYPOTHESIS -- share of the ECU rate spent on slowness",
     "note": "Safe: isolated misses are absorbed within the parameter bounds."},
    {"element": "ECU", "code": "F8", "mode": "machine computes a wrong value, a certified shadow exists",
     "effect": "dangerous", "share": 0.09, "dc": "high",
     "mechanism": ("order/shadow comparison over three ticks raises the "
                   "disagreement alert (DET-09, REP-05)"),
     "requirements": "DET-09, REP-05, DEG-01",
     "evidence": "core/src/arbiter.c:241 (REP-05/DET-09), core/src/arbiter.c:293 (three-tick streak)",
     "share_source": "HYPOTHESIS -- 90 % of the wrong-value cases happen while a shadow exists",
     "note": "Dangerous for the value applied, but detected and alarmed."},
    {"element": "ECU", "code": "F8", "mode": "machine computes a wrong value, no certified shadow",
     "effect": "dangerous", "share": 0.01, "dc": "none",
     "mechanism": ("none: the comparison needs a certified second; without it "
                   "the wrong value is applied silently"),
     "requirements": "DET-09, REP-08",
     "evidence": "core/src/arbiter.c:265 (comparison needs the shadow)",
     "share_source": "HYPOTHESIS -- 10 % of the wrong-value cases happen with no shadow",
     "note": ("The one dangerous-undetected share of the software: a third "
              "machine (vote) is the level-X answer, REP-08.")},
    {"element": "ECU", "code": "F6", "mode": "emitting side loses, duplicates, reorders or delays frames",
     "effect": "safe", "share": 0.20, "dc": "medium",
     "mechanism": ("tick label, mandate number and miss counter reject a "
                   "reordered or late frame (MAN-02, DET-03, ROB-02)"),
     "requirements": "MAN-02, DET-03, ROB-02",
     "evidence": "core/src/arbiter.c:54, core/src/arbiter.c:416",
     "share_source": "HYPOTHESIS -- share of the ECU rate spent on frame handling",
     "note": "Safe: dropped and counted, absorbed within the bounds."},
    {"element": "ECU", "code": "F7", "mode": "corrupted or forged frame emitted",
     "effect": "safe", "share": 0.05, "dc": "high",
     "mechanism": ("CRC-32C at level S, keyed SipHash-2-4 MAC at level C "
                   "(DET-04, SEC-02)"),
     "requirements": "DET-04, SEC-02, SEC-03",
     "evidence": "core/src/check.c:1 (frame integrity), core/src/check.c:78 (crc32c), core/src/check.c:117 (mac)",
     "share_source": "HYPOTHESIS -- share of the ECU rate spent on corrupted frames",
     "note": "Safe: dropped, counted, journaled, no effect on the service."},
    {"element": "ECU", "code": "F9", "mode": "latent (dormant) fault, no effect until a second fault",
     "effect": "safe", "share": 0.10, "dc": "none",
     "mechanism": ("none at runtime: the invariant exploration and the "
                   "mutant bench are development evidence, not a runtime "
                   "diagnostic"),
     "requirements": "ROB-04, DEG-08",
     "evidence": "core/src/invariants.c (I1..I11), host/explore/ (explorer), host/mutants/ (M1..M8)",
     "share_source": "HYPOTHESIS -- share of the ECU rate spent on dormant defects",
     "note": ("Dormant: safe today, but it is what makes a second fault "
              "dangerous. It drives the latent-fault metric, not the PMHF.")},

    # --- ECU: the double faults, whose rate is computed, not shared ---------
    {"element": "ECU", "code": "F9", "mode": "second fault strikes before the redundancy is restored",
     "effect": "dangerous", "share": 0.0, "dc": "high",
     "rate_kind": "combination", "combo_window": "repair", "residual": True,
     "mechanism": ("the actuator counts p+1 ticks without a valid order, "
                   "plays the will up to its horizon and then falls back to "
                   "its own damped loop: red lamp and stop requested "
                   "(ACT-02, ACT-03, DEG-05)"),
     "requirements": "ACT-02, ACT-03, DEG-05, DEG-06",
     "evidence": "core/src/control.c:165 (I6, the horizon of the will), core/src/arbiter.c:231 (the fallback loop)",
     "share_source": ("COMPUTED -- lambda(ECU)^2 x the repair time; the repair "
                      "time is a HYPOTHESIS (1 h)"),
     "note": ("Dangerous for the goal (no fresh command) but detected: the "
              "safe state is reached and announced.")},
    {"element": "ECU", "code": "F10", "mode": "both machines dead",
     "effect": "dangerous", "share": 0.0, "dc": "high",
     "rate_kind": "combination", "combo_window": "ftti", "residual": True,
     "mechanism": ("same path: the will covers the horizon, then the local "
                   "damped loop holds the rack (ACT-02, DEG-05)"),
     "requirements": "ACT-02, ACT-03, DEG-05, DEG-06",
     "evidence": "core/src/control.c:165 (I6, the horizon of the will), core/src/arbiter.c:231 (the fallback loop)",
     "share_source": ("COMPUTED -- lambda(ECU)^2 x the FTTI (lambda is a "
                      "HYPOTHESIS): the two machines must die inside the "
                      "horizon of the will"),
     "note": "Dangerous for the goal, detected: the safe state is reached."},

    # --- ACT-CTRL: the arbitration controller of the actuator ---------------
    {"element": "ACT-CTRL", "code": "F11", "mode": "arbitration core stops (crash, deadlock)",
     "effect": "dangerous", "share": 0.30, "dc": "none",
     "mechanism": ("none: the actuator is a single channel, no second "
                   "arbitration path at the base level (ACT-10 is the level-X "
                   "extension)"),
     "requirements": "ACT-10, MAN-13",
     "evidence": "core/src/arbiter.c (one arbiter), host/safety/fta.py (F11 out of scope)",
     "share_source": "HYPOTHESIS -- share of the controller rate spent on a hard stop",
     "note": "Single point of failure: nothing observes the arbiter but itself."},
    {"element": "ACT-CTRL", "code": "F11", "mode": "arbitration core computes a wrong mandate or value",
     "effect": "dangerous", "share": 0.20, "dc": "none",
     "mechanism": ("none at runtime: the invariants are checked offline "
                   "(explorer, verifier), not on the target"),
     "requirements": "DEG-08, ROB-04",
     "evidence": "core/src/invariants.c (offline checks), host/verify/relais-verify.c",
     "share_source": "HYPOTHESIS -- share of the controller rate spent on a wrong computation",
     "note": ("Systematic/software defect: excluded from the random-failure "
              "budget by ISO 26262, but counted here because the honest "
              "answer is that nothing detects it at runtime.")},
    {"element": "ACT-CTRL", "code": "F11", "mode": "position feedback path faulty (sensor and wiring on the target; simulated plant in the demonstrator)",
     "effect": "dangerous", "share": 0.20, "dc": "none",
     "mechanism": ("none: the law trusts the measurement, no plausibility "
                   "check compares the rack with the command (CMD-06 bounds "
                   "the command, not the response)"),
     "requirements": "CMD-06, ACT-05",
     "evidence": "core/src/control.c:65 (error = setpoint - pos), core/src/control.c:72 (clamp of the command)",
     "share_source": "HYPOTHESIS -- share of the controller rate spent on the feedback path",
     "note": ("In the demonstrator the rack is the software plant model "
              "(core/include/relais/control.h:85); on a real target this is a "
              "sensor.")},
    {"element": "ACT-CTRL", "code": "F11", "mode": "signalling (lamp) faulty: the degradation is not shown",
     "effect": "safe", "share": 0.10, "dc": "none",
     "mechanism": "none: no channel observes the lamp of the actuator",
     "requirements": "DEG-01, DEG-02",
     "evidence": "core/src/arbiter.c:326 (DEG-01, DEG-02, I10)",
     "share_source": "HYPOTHESIS -- share of the controller rate spent on signalling",
     "note": "Safe for the actuation, but it hides the degradation from the driver."},
    {"element": "ACT-CTRL", "code": "F11", "mode": "journal or observer faulty: the record of the ticks is lost",
     "effect": "safe", "share": 0.10, "dc": "none",
     "mechanism": "none: the journal is an observation channel, not a barrier",
     "requirements": "OBS-07, OBS-01",
     "evidence": "core/src/journal.c",
     "share_source": "HYPOTHESIS -- share of the controller rate spent on observation",
     "note": "Safe for the actuation; it destroys the evidence of the run."},
    {"element": "ACT-CTRL", "code": "F11", "mode": "power or clock transient leaves the arbitration running with a corrupted state",
     "effect": "dangerous", "share": 0.10, "dc": "none",
     "mechanism": ("none: no watchdog, no self-test of the arbitration state "
                   "at the base level"),
     "requirements": "ACT-10",
     "evidence": "core/src/arbiter.c (no supervisory loop around the arbiter)",
     "share_source": "HYPOTHESIS -- share of the controller rate spent on transients",
     "note": "Dangerous and silent: a watchdog is the cheapest lift."},

    # --- ACT-PWR: the power path and the mechanics --------------------------
    {"element": "ACT-PWR", "code": "F11", "mode": "power stage or motor failure: no torque is delivered",
     "effect": "dangerous", "share": 0.50, "dc": "none",
     "mechanism": ("none: the arbiter sees a command, never the reaction of "
                   "the rack"),
     "requirements": "ACT-05, ACT-10",
     "evidence": "core/src/arbiter.c:192 (the applied value), core/include/relais/control.h:85 (the plant is simulated)",
     "share_source": "HYPOTHESIS -- share of the power path rate spent on the drive",
     "note": "Dominant term of the whole FMEDA: see the sensitivity analysis."},
    {"element": "ACT-PWR", "code": "F11", "mode": "mechanical jam or excessive friction",
     "effect": "dangerous", "share": 0.30, "dc": "none",
     "mechanism": "none: no current, speed or position plausibility check",
     "requirements": "ACT-05",
     "evidence": "core/src/control.c (no deviation check between command and rack)",
     "share_source": "HYPOTHESIS -- share of the power path rate spent on the mechanics",
     "note": "Same blind spot as the drive failure."},
    {"element": "ACT-PWR", "code": "F11", "mode": "short or insulation fault in the power path",
     "effect": "dangerous", "share": 0.20, "dc": "none",
     "mechanism": "none: the fault is invisible to the arbitration",
     "requirements": "ACT-10",
     "evidence": "core/src/arbiter.c (no supervision of the power path)",
     "share_source": "HYPOTHESIS -- share of the power path rate spent on cabling",
     "note": "Would also break the actuation channel itself."},

    # --- LINK-MA: the link between one machine and the actuator -------------
    {"element": "LINK-MA", "code": "F5", "mode": "link cut (open circuit, connector)",
     "effect": "safe", "share": 0.30, "dc": "high",
     "mechanism": ("the actuator sees no order (p+1 silence) and the machine "
                   "sees no STATUT after bail_ticks, so it deposes itself "
                   "(DET-07, DET-08, MAN-09)"),
     "requirements": "DET-07, DET-08, MAN-09, REL-01",
     "evidence": "core/src/node.c:294 (bail_ticks), core/src/node.c:297 (MAN-09/DET-07)",
     "share_source": "HYPOTHESIS -- share of the link rate spent on a cut",
     "note": "Safe: the other machine keeps the service, the handover is automatic."},
    {"element": "LINK-MA", "code": "F6", "mode": "frames lost, duplicated, reordered or delayed",
     "effect": "safe", "share": 0.40, "dc": "medium",
     "mechanism": ("tick label, mandate number and miss counter reject a "
                   "reordered or late frame (MAN-02, DET-03, ROB-02)"),
     "requirements": "MAN-02, DET-03, ROB-02",
     "evidence": "core/src/arbiter.c:54, core/src/arbiter.c:416",
     "share_source": "HYPOTHESIS -- share of the link rate spent on congestion",
     "note": "Safe: dropped and counted, absorbed within the bounds."},
    {"element": "LINK-MA", "code": "F7", "mode": "corrupted or forged frame (EMC, ground loop)",
     "effect": "safe", "share": 0.20, "dc": "high",
     "mechanism": "CRC-32C at level S, keyed SipHash-2-4 MAC at level C (DET-04)",
     "requirements": "DET-04, SEC-02",
     "evidence": "core/src/check.c:1, core/src/check.c:78, core/src/check.c:117",
     "share_source": "HYPOTHESIS -- share of the link rate spent on corrupted frames",
     "note": "Safe: dropped, counted, journaled."},
    {"element": "LINK-MA", "code": "F5", "mode": "transceiver degraded: the frames no longer arrive but the link stays up",
     "effect": "safe", "share": 0.10, "dc": "high",
     "mechanism": ("same silence path: the machine deposes itself after "
                   "bail_ticks, the actuator revokes after p+1 (DET-07, MAN-04)"),
     "requirements": "DET-07, MAN-04, MAN-09",
     "evidence": "core/src/node.c:294, core/src/arbiter.c:416",
     "share_source": "HYPOTHESIS -- share of the link rate spent on a silent transceiver",
     "note": "Safe: behaves like a cut, and is detected like a cut."},

    # --- LINK-AB: the link between the two machines -------------------------
    {"element": "LINK-AB", "code": "F4", "mode": "inter-computer link cut",
     "effect": "safe", "share": 0.50, "dc": "high",
     "mechanism": ("loss of ETAT declared after two ticks (DET-06); the "
                   "holder keeps commanding, the redundancy is reported "
                   "degraded"),
     "requirements": "DET-06, REP-04, REP-07",
     "evidence": "core/src/node.c:164 (DET-06)",
     "share_source": "HYPOTHESIS -- share of the link rate spent on a cut",
     "note": "Safe: no handover, the holder keeps the service."},
    {"element": "LINK-AB", "code": "F6", "mode": "ETAT frames lost or delayed",
     "effect": "safe", "share": 0.30, "dc": "medium",
     "mechanism": "the same two-tick ETAT watchdog (DET-06)",
     "requirements": "DET-06, ROB-02",
     "evidence": "core/src/node.c:164",
     "share_source": "HYPOTHESIS -- share of the link rate spent on congestion",
     "note": "Safe: the replica ages and the redundancy is reported degraded."},
    {"element": "LINK-AB", "code": "F7", "mode": "ETAT frame corrupted",
     "effect": "safe", "share": 0.20, "dc": "high",
     "mechanism": "CRC-32C at level S, keyed MAC at level C (DET-04)",
     "requirements": "DET-04, SEC-02",
     "evidence": "core/src/check.c:1, core/src/check.c:117",
     "share_source": "HYPOTHESIS -- share of the link rate spent on corrupted frames",
     "note": "Safe: dropped, counted."},
]
