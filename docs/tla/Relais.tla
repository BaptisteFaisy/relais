---- MODULE Relais ----
(***************************************************************************)
(* A formal model of the Relais actuator and its two computers.          *)
(*                                                                         *)
(* The model is an abstraction of core/src/arbiter.c: it keeps exactly     *)
(* what the safety invariants talk about (the mandate, the holder, the     *)
(* misses, the certification, the will, the source of the applied value,   *)
(* the lamps) and drops everything they do not (the numerical values, the  *)
(* plant, the CRC, the MAC).  Each action below carries the requirement   *)
(* ID it implements, so the table in README.md can be checked line by      *)
(* line.                                                                   *)
(*                                                                         *)
(* The model is NOT a conclusion: it is a hypothesis made explicit.  The  *)
(* three hypotheses are written as guards, not as conclusions:            *)
(*   H1 (single fault)  : at most one machine is out of service at a time;*)
(*   H2 (loss burst <= P): a burst of consecutive losses is at most P;     *)
(*   H3 (timing)        : a frame that is not delivered within its period  *)
(*                        is lost; a frame delivered in time reaches the    *)
(*                        actuator in the period it targets.                *)
(* A safety invariant holds even when H2 is violated: the actuator counts  *)
(* its own misses and revokes on the (P+1)-th one (MAN-04).  H2 only      *)
(* bounds what is CLAIMED (liveness), never what is PROVED (safety).      *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS MaxTick,     \* deepest tick the model explores
          MaxMandate,  \* highest mandate number the model may reach
          N,           \* horizon of the will (spec 4.4 item 3, ACT-02)
          P,           \* patience: misses tolerated before a revocation
          CertifyTicks \* agreeing ticks needed to certify the second

NODES == {"A", "B"}
NONE  == "none"

Src  == {"none", "holder", "shadow", "testament", "fallback"}
Role == {"none", "holder", "second"}
Lamp == {"off", "yellow", "red"}
Kinds == {"EMPTY", "ORDRE", "OMBRE", "DEMANDE", "CESSION"}

Frame == [kind : Kinds, from : NODES \cup {NONE}, mand : 0..MaxMandate,
          tick : 0..(MaxTick + 1)]
EmptyFrame == [kind |-> "EMPTY", from |-> NONE, mand |-> 0, tick |-> 0]

Other(m) == IF m = "A" THEN "B" ELSE "A"
Cap(x, m) == IF x > m THEN m ELSE x

VARIABLES
  \* the actuator (core/src/arbiter.c)
  tick, mandate, holder, misses, certified, certifiedNode, certifyStreak,
  testamentBase, testamentLen, testamentRun, source, lamp, cessionPending,
  changesThisTick, appliedFrom, appliedSet, appliedTick, appliedMandate,
  mandateHolders, lastMandate, lastHolder,
  \* the two computers and the channel
  alive, frozen, role, belief, synced, pending, uplink, downlink, inbox,
  channelLosses

vars == <<tick, mandate, holder, misses, certified, certifiedNode,
          certifyStreak, testamentBase, testamentLen, testamentRun, source,
          lamp, cessionPending, changesThisTick, appliedFrom, appliedSet,
          appliedTick, appliedMandate, mandateHolders, lastMandate,
          lastHolder, alive, frozen, role, belief, synced, pending, uplink,
          downlink, inbox, channelLosses>>

ActuatorVars == <<tick, mandate, holder, misses, certified, certifiedNode,
                  certifyStreak, testamentBase, testamentLen, testamentRun,
                  source, lamp, cessionPending, changesThisTick, appliedFrom,
                  appliedSet, appliedTick, appliedMandate, mandateHolders,
                  lastMandate, lastHolder>>

MachineVars == <<alive, frozen, role, belief, synced, pending, uplink,
                 downlink, inbox, channelLosses>>

\* The send actions assign `pending`, so it is left out of their UNCHANGED
\* list: listing it there would make the action self-contradictory and dead.
MachineVarsNoPending == <<alive, frozen, role, belief, synced, uplink,
                          downlink, inbox, channelLosses>>

(***************************************************************************)
(* Type discipline                                                          *)
(***************************************************************************)
TypeOK ==
  /\ tick \in 0..MaxTick
  /\ mandate \in 0..MaxMandate
  /\ holder \in NODES \cup {NONE}
  /\ misses \in 0..(P + 1)
  /\ certified \in BOOLEAN
  /\ certifiedNode \in NODES \cup {NONE}
  /\ certifyStreak \in 0..CertifyTicks
  /\ testamentBase \in 0..(MaxTick + 1)
  /\ testamentLen \in 0..N
  /\ testamentRun \in 0..N
  /\ source \in Src
  /\ lamp \in Lamp
  /\ cessionPending \in BOOLEAN
  /\ changesThisTick \in 0..1
  /\ appliedFrom \in NODES \cup {NONE}
  /\ appliedSet \subseteq NODES
  /\ appliedTick \in 0..(MaxTick + 1)
  /\ appliedMandate \in 0..MaxMandate
  /\ mandateHolders \in [0..MaxMandate -> SUBSET NODES]
  /\ lastMandate \in 0..MaxMandate
  /\ lastHolder \in NODES \cup {NONE}
  /\ alive \in [NODES -> BOOLEAN]
  /\ frozen \in [NODES -> BOOLEAN]
  /\ role \in [NODES -> Role]
  /\ belief \in [NODES -> 0..MaxMandate]
  /\ synced \in [NODES -> BOOLEAN]
  /\ pending \in [NODES -> Frame]
  /\ uplink \in [NODES -> BOOLEAN]
  /\ downlink \in [NODES -> BOOLEAN]
  /\ inbox \subseteq Frame
  /\ channelLosses \in 0..P

(***************************************************************************)
(* Helpers                                                                 *)
(***************************************************************************)

\* The mandate goes vacant, or straight to the certified second (MAN-10).
\* Returns <<holder, mandate, certified, certifiedNode>>.
Release(cert, cnode, h, m) ==
  IF cert /\ cnode # NONE /\ cnode # h
  THEN <<cnode, m + 1, FALSE, NONE>>
  ELSE <<NONE, m, FALSE, NONE>>

\* An order counts only if it comes from the holder, carries the current
\* mandate and targets the tick being applied (MAN-02).
OrderOK(f, h, m, t) == f.kind = "ORDRE" /\ f.from = h /\ f.mand = m /\ f.tick = t

(***************************************************************************)
(* The computers send frames                                                 *)
(***************************************************************************)

\* MAN-02: the holder orders for the tick to come.
SendOrder(m) ==
  /\ m \in NODES
  /\ alive[m] /\ ~frozen[m] /\ uplink[m]
  /\ pending[m].kind = "EMPTY"
  /\ role[m] = "holder" /\ holder = m
  /\ belief[m] = mandate
  /\ pending' = [pending EXCEPT ![m] =
        [kind |-> "ORDRE", from |-> m, mand |-> belief[m], tick |-> tick + 1]]
  /\ UNCHANGED <<ActuatorVars, MachineVarsNoPending>>

\* REL-05: the second sends its shadow for the tick to come.
SendShadow(m) ==
  /\ m \in NODES
  /\ alive[m] /\ ~frozen[m] /\ uplink[m]
  /\ pending[m].kind = "EMPTY"
  /\ role[m] = "second"
  /\ pending' = [pending EXCEPT ![m] =
        [kind |-> "OMBRE", from |-> m, mand |-> mandate, tick |-> tick + 1]]
  /\ UNCHANGED <<ActuatorVars, MachineVarsNoPending>>

\* MAN-05: a machine that knows the mandate is vacant asks for it.
SendDemande(m) ==
  /\ m \in NODES
  /\ alive[m] /\ ~frozen[m] /\ uplink[m]
  /\ pending[m].kind = "EMPTY"
  /\ role[m] = "none"
  /\ pending' = [pending EXCEPT ![m] =
        [kind |-> "DEMANDE", from |-> m, mand |-> mandate, tick |-> tick + 1]]
  /\ UNCHANGED <<ActuatorVars, MachineVarsNoPending>>

\* DET-05: the holder gives up; the mandate is freed at the next tick.
SendCession(m) ==
  /\ m \in NODES
  /\ alive[m] /\ ~frozen[m] /\ uplink[m]
  /\ pending[m].kind = "EMPTY"
  /\ role[m] = "holder" /\ holder = m
  /\ pending' = [pending EXCEPT ![m] =
        [kind |-> "CESSION", from |-> m, mand |-> mandate, tick |-> tick + 1]]
  /\ UNCHANGED <<ActuatorVars, MachineVarsNoPending>>

(***************************************************************************)
(* The channel                                                              *)
(***************************************************************************)

\* H3: a frame delivered in time reaches the actuator in its period.
DeliverFrame(m) ==
  /\ m \in NODES
  /\ pending[m].kind # "EMPTY"
  /\ uplink[m]
  /\ inbox' = inbox \cup {pending[m]}
  /\ pending' = [pending EXCEPT ![m] = EmptyFrame]
  /\ channelLosses' = 0
  /\ UNCHANGED <<ActuatorVars, alive, frozen, role, belief, synced, uplink,
                downlink>>

\* H2: a burst of consecutive losses is at most P frames.
LoseFrame(m) ==
  /\ m \in NODES
  /\ pending[m].kind # "EMPTY"
  /\ channelLosses < P
  /\ pending' = [pending EXCEPT ![m] = EmptyFrame]
  /\ channelLosses' = channelLosses + 1
  /\ UNCHANGED <<ActuatorVars, alive, frozen, role, belief, synced, uplink,
                downlink, inbox>>

\* The half-link of a machine is cut, then restored.
CutLink(m) ==
  /\ m \in NODES
  /\ uplink[m] \/ downlink[m]
  /\ uplink' = [uplink EXCEPT ![m] = FALSE]
  /\ downlink' = [downlink EXCEPT ![m] = FALSE]
  /\ UNCHANGED <<ActuatorVars, alive, frozen, role, belief, synced, pending,
                inbox, channelLosses>>

RestoreLink(m) ==
  /\ m \in NODES
  /\ ~uplink[m] \/ ~downlink[m]
  /\ uplink' = [uplink EXCEPT ![m] = TRUE]
  /\ downlink' = [downlink EXCEPT ![m] = TRUE]
  /\ UNCHANGED <<ActuatorVars, alive, frozen, role, belief, synced, pending,
                inbox, channelLosses>>

(***************************************************************************)
(* The faults (H1: a single fault at a time)                               *)
(***************************************************************************)

KillMachine(m) ==
  /\ m \in NODES
  /\ alive[m]
  /\ alive[Other(m)] /\ ~frozen[Other(m)]
  /\ alive' = [alive EXCEPT ![m] = FALSE]
  /\ UNCHANGED <<ActuatorVars, frozen, role, belief, synced, pending,
                uplink, downlink, inbox, channelLosses>>

FreezeMachine(m) ==
  /\ m \in NODES
  /\ ~frozen[m]
  /\ alive[Other(m)] /\ ~frozen[Other(m)]
  /\ frozen' = [frozen EXCEPT ![m] = TRUE]
  /\ UNCHANGED <<ActuatorVars, alive, role, belief, synced, pending,
                uplink, downlink, inbox, channelLosses>>

ThawMachine(m) ==
  /\ m \in NODES
  /\ frozen[m]
  /\ frozen' = [frozen EXCEPT ![m] = FALSE]
  /\ UNCHANGED <<ActuatorVars, alive, role, belief, synced, pending,
                uplink, downlink, inbox, channelLosses>>

(***************************************************************************)
(* The tick: the actuator beats, decides, publishes (spec 4.4, MAN-07)      *)
(***************************************************************************)
Tick ==
  /\ tick < MaxTick
  /\ LET
       \* The beat applies the tick to come: the C increments the
       \* tick before relais_arb_step, and a frame sent at t targets t+1.
       t == tick + 1
       \* 1. a cession recorded at the previous tick frees the mandate here
          r0 == IF cessionPending
                THEN Release(certified, certifiedNode, holder, mandate)
                ELSE <<holder, mandate, certified, certifiedNode>>
       h0 == r0[1]
       m0 == r0[2]
       c0 == r0[3]
       n0 == r0[4]
       \* 2. the scan: an order counts only if it comes from the holder and
       \* carries the current mandate for the tick being applied (MAN-02)
       ord == \E f \in inbox : OrderOK(f, h0, m0, t)
       shd == \E f \in inbox : f.kind = "OMBRE" /\ f.from = n0 /\ f.tick = t
       dem == \E f \in inbox : f.kind = "DEMANDE"
       ces == \E f \in inbox : f.kind = "CESSION" /\ f.from = h0 /\ f.mand = m0
       \* 3. the miss is counted first: the actuator's own evidence
       ms1 == IF ord THEN 0 ELSE Cap(misses + 1, P + 1)
       \* 4. revoke on p+1 consecutive misses (MAN-04)
       r1 == IF h0 # NONE /\ ms1 > P
             THEN Release(c0, n0, h0, m0)
             ELSE r0
       h1 == r1[1]
       m1 == r1[2]
       c1 == r1[3]
       n1 == r1[4]
       \* 5. a demande is granted when the mandate is vacant (MAN-05)
       demFrom == IF \E f \in inbox : f.kind = "DEMANDE" /\ f.from = "A"
                THEN "A" ELSE "B"
       r2 == IF h1 = NONE /\ dem /\ m1 < MaxMandate
             THEN <<demFrom, m1 + 1, FALSE, NONE>>
             ELSE <<h1, m1, c1, n1>>
       h2 == r2[1]
       m2 == r2[2]
       c2 == r2[3]
       n2 == r2[4]
       \* 6. the one value of this tick (spec 4.4)
       src == IF ord THEN "holder"
              ELSE IF c2 /\ shd THEN "shadow"
              ELSE IF testamentLen > 0 /\ t >= testamentBase
                      /\ t < testamentBase + testamentLen
                   THEN "testament"
                   ELSE "fallback"
       appFrom == IF src = "holder" THEN h2
                  ELSE IF src = "shadow" THEN n2
                  ELSE NONE
       \* 7. certification by agreement (REP-05)
       streak == IF h2 = NONE THEN 0
                 ELSE IF ord /\ (\E f \in inbox : f.kind = "OMBRE"
                                            /\ f.from = Other(h2)
                                            /\ f.tick = t)
                      THEN Cap(certifyStreak + 1, CertifyTicks)
                      ELSE 0
       c3 == IF streak >= CertifyTicks THEN TRUE
             ELSE IF h2 = NONE THEN FALSE ELSE c2
       n3 == IF c3 THEN (IF streak >= CertifyTicks THEN Other(h2) ELSE n2)
             ELSE NONE
       \* 8. the lamp follows the cause (DEG-01, DEG-02)
       peer == alive[Other(h2)] /\ ~frozen[Other(h2)]
       lmp == IF src = "fallback" THEN "red"
              ELSE IF src # "holder" THEN "yellow"
              ELSE IF h2 = NONE THEN "yellow"
              ELSE IF ~peer THEN "yellow"
              ELSE IF ~c3 THEN "yellow"
              ELSE "off"
       \* 9. the will is refreshed by the order that was applied (ACT-02)
       tl == IF src = "holder" THEN N ELSE testamentLen
       tb == IF src = "holder" THEN t + 1 ELSE testamentBase
       tr == IF src = "testament" THEN Cap(testamentRun + 1, N) ELSE 0
       \* 10. the STATUT is published (MAN-07)
       newRole == [m \in NODES |->
                     IF alive[m] /\ ~frozen[m] /\ downlink[m]
                     THEN (IF m = h2 THEN "holder"
                           ELSE IF h2 = NONE THEN "none" ELSE "second")
                     ELSE role[m]]
       newBelief == [m \in NODES |->
                       IF alive[m] /\ ~frozen[m] /\ downlink[m]
                       THEN m2 ELSE belief[m]]
       newSynced == [m \in NODES |->
                      alive[m] /\ ~frozen[m] /\ downlink[m]]
    IN
    /\ tick' = t
    /\ holder' = h2
    /\ mandate' = m2
    /\ misses' = ms1
    /\ certified' = c3
    /\ certifiedNode' = n3
    /\ certifyStreak' = streak
    /\ testamentBase' = tb
    /\ testamentLen' = tl
    /\ testamentRun' = tr
    /\ source' = src
    /\ lamp' = lmp
    /\ cessionPending' = ces
    /\ changesThisTick' = IF m2 > m1 THEN 1 ELSE 0
    /\ appliedFrom' = appFrom
    /\ appliedSet' = {appFrom} \ {NONE}
    /\ appliedTick' = t
    /\ appliedMandate' = m2
    /\ mandateHolders' =
         IF m2 > m1
         THEN [mandateHolders EXCEPT ![m2] = @ \cup {h2}]
         ELSE mandateHolders
    /\ lastMandate' = m2
    /\ lastHolder' = h2
    /\ role' = newRole
    /\ belief' = newBelief
    /\ synced' = newSynced
    \* H3: a frame that was not delivered in its period is lost
    /\ pending' = [m \in NODES |-> EmptyFrame]
    /\ inbox' = {}
    /\ channelLosses' = 0
    /\ UNCHANGED <<alive, frozen, uplink, downlink>>

(***************************************************************************)
(* The specification                                                        *)
(***************************************************************************)
Init ==
  /\ tick = 0
  /\ mandate = 0
  /\ holder = NONE
  /\ misses = 0
  /\ certified = FALSE
  /\ certifiedNode = NONE
  /\ certifyStreak = 0
  /\ testamentBase = 0
  /\ testamentLen = 0
  /\ testamentRun = 0
  /\ source = "none"
  /\ lamp = "off"
  /\ cessionPending = FALSE
  /\ changesThisTick = 0
  /\ appliedFrom = NONE
  /\ appliedSet = {}
  /\ appliedTick = 0
  /\ appliedMandate = 0
  /\ mandateHolders = [k \in 0..MaxMandate |-> {}]
  /\ lastMandate = 0
  /\ lastHolder = NONE
  /\ alive = [m \in NODES |-> TRUE]
  /\ frozen = [m \in NODES |-> FALSE]
  /\ role = [m \in NODES |-> "none"]
  /\ belief = [m \in NODES |-> 0]
  /\ synced = [m \in NODES |-> FALSE]
  /\ pending = [m \in NODES |-> EmptyFrame]
  /\ uplink = [m \in NODES |-> TRUE]
  /\ downlink = [m \in NODES |-> TRUE]
  /\ inbox = {}
  /\ channelLosses = 0

Next ==
  \/ Tick
  \/ \E m \in NODES :
       SendOrder(m) \/ SendShadow(m) \/ SendDemande(m) \/ SendCession(m)
       \/ DeliverFrame(m) \/ LoseFrame(m)
       \/ KillMachine(m) \/ FreezeMachine(m) \/ ThawMachine(m)
       \/ CutLink(m) \/ RestoreLink(m)

\* Weak fairness: the actuator beats, one tick after the other (spec 4.10).
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)

(***************************************************************************)
(* The invariants                                                           *)
(***************************************************************************)

\* I1: exactly one value per tick, from one of the four known sources.
Inv_I1 ==
  /\ (tick > 0) => (source \in {"holder", "shadow", "testament", "fallback"})
  /\ (source = "holder") => (holder # NONE)
  /\ (source = "shadow") => (certified /\ certifiedNode # NONE)

\* I2: the mandate never decreases, changes at most once per tick, and two
\* machines never hold the same mandate number.
Inv_I2 ==
  /\ (tick > 0) => (mandate >= lastMandate)
  /\ changesThisTick <= 1
  /\ \A k \in 0..MaxMandate : Cardinality(mandateHolders[k]) <= 1

\* I3: no order of a deposed machine is applied, and no order aimed at
\* another tick is applied.
Inv_I3 ==
  /\ (source = "holder") =>
       /\ appliedFrom = holder
       /\ appliedTick = tick
       /\ appliedMandate = mandate

\* I6: the will is never played past its horizon.
Inv_I6 ==
  /\ (source = "testament") =>
       /\ testamentLen <= N
       /\ testamentRun <= testamentLen
       /\ tick < testamentBase + testamentLen

\* Safety: never two machines applied at the same tick.
Inv_Safety == Cardinality(appliedSet) <= 1

\* DEG-01: a loss of redundancy shows on the lamp.
Inv_Lamp == (tick > 0 /\ source # "holder") => (lamp # "off")

=============================================================================
