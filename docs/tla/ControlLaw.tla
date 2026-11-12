---- MODULE ControlLaw ----
(***************************************************************************)
(* ControlLaw.tla -- the CONTROL LAW and the rack plant of Relais.          *)
(*                                                                          *)
(* This is the physical half of the model. The protocol -- who may command,  *)
(* when the mandate moves, what the actuator applies at each tick -- is     *)
(* modelled in the protocol model; here we model what the command IS: the    *)
(* state of the controller, one period of the law, the integration of the   *)
(* rack, and the bounds that keep the actuator safe.                        *)
(*                                                                          *)
(* The C implementation is core/src/control.c (relais_ctl_step,             *)
(* relais_clamp_value, relais_clamp_slew) and core/src/plant.c              *)
(* (relais_plant_step). The constants below are theirs, in fixed point.     *)
(*                                                                          *)
(* TLC cannot check real numbers, so the law is transcribed in fixed point,  *)
(* SC = 1000: a real value r is the integer r*SC. All arithmetic is        *)
(* integer; the only rounding is the integer division of the filter and of  *)
(* the integral term, which is an abstraction, not a property of the system. *)
(*                                                                          *)
(* The two invariants of the mission are:                                  *)
(*   CmdBounded   the command never leaves the amplitude bound              *)
(*                (ACT-05, CMD-06), and                                     *)
(*   SlewBounded  the command never moves by more than max_slew in a tick   *)
(*                (ACT-05).                                                 *)
(* Both are TOTAL properties of the two clamps: they hold for every         *)
(* measurement, so the environment is modelled by a finite set of            *)
(* measurements and the abstraction is sound for these bounds. The closed    *)
(* loop (rack model, trajectory, disturbance) is verified by the C          *)
(* simulator and by the exhaustive explorer, not here.                      *)
(*                                                                          *)
(* SCOPE OF THE CHECKED STATE. The C controller also keeps prev_setpoint    *)
(* and setpoint_rate (the filtered slope of the steering request). They     *)
(* feed only the testament extrapolation (relais_testament_fill), never the  *)
(* command of the tick, so they are abstracted away from the checked state:  *)
(* keeping them would multiply the state space by a large factor without     *)
(* touching CmdBounded or SlewBounded. SetpointFilter below records that    *)
(* part of the law for the record.                                          *)
(*                                                                          *)
(* Check with TLC:  java -cp tla2tools.jar tlc2.TLC ControlLaw.tla         *)
(* (the model values live in ControlLaw.cfg).                               *)
(***************************************************************************)
EXTENDS Integers, TLC

CONSTANTS
    MaxAmp,        \* 1000  : max_amplitude * SC          (ACT-05)
    MaxSlew,       \* 250   : max_slew * SC               (ACT-05)
    IntegLimit,    \* 250   : integral term limit * SC
    RateLimit,     \* 20000 : setpoint slope limit * SC
    FilterNum,     \* 35    : measurement filter * 100
    DtNum,         \* 5     : period in milliseconds (T = 5 ms)
    DtDen,         \* 1000  : fixed point scale
    RateGain,      \* 200   : 1 / dt
    Kp,            \* 60    : position gain
    KdNum,         \* 5     : velocity gain numerator   (0.5)
    KdDen,         \* 10    : velocity gain denominator
    Ki             \* 4     : integral gain

(***************************************************************************)
(* The measurement grid TLC explores, in fixed point. It is finite on       *)
(* purpose: CmdBounded and SlewBounded are total properties of the clamps,  *)
(* so they hold for every measurement and a representative grid suffices.   *)
(*                                                                          *)
(* The rack follows the steering request, so the position is expressed as   *)
(* the setpoint plus a tracking error: ErrorSet is that error. The grid     *)
(* covers both the saturated regime (a large error, where the anti-windup  *)
(* cancels the integral increment) and the linear one (a small error, where *)
(* the command is proportional and the integral term accumulates).          *)
(***************************************************************************)
ErrorSet    == {-2000, -1000, -500, -100, -50, 0, 50, 100, 500, 1000, 2000}
VelSet      == {-2000, -1000, 0, 1000, 2000}
SetpointSet == {-1000, -500, 0, 500, 1000}

VARIABLES
    integ,         \* integral term of the controller
    cmd,           \* command applied at the last tick
    prevCmd        \* command applied at the tick before

vars == <<integ, cmd, prevCmd>>

(***************************************************************************)
(* The clamps, exact transcription of core/src/control.c.                   *)
(***************************************************************************)
ClampValue(v) ==
    IF v > MaxAmp THEN MaxAmp
    ELSE IF v < -MaxAmp THEN -MaxAmp
    ELSE v

ClampSlew(prev, wanted) ==
    IF wanted - prev > MaxSlew THEN prev + MaxSlew
    ELSE IF prev - wanted > MaxSlew THEN prev - MaxSlew
    ELSE wanted

ClampRate(r) ==
    IF r > RateLimit THEN RateLimit
    ELSE IF r < -RateLimit THEN -RateLimit
    ELSE r

ClampInteg(i) ==
    IF i > IntegLimit THEN IntegLimit
    ELSE IF i < -IntegLimit THEN -IntegLimit
    ELSE i

(***************************************************************************)
(* The environment: what the actuator publishes at each tick. The rack      *)
(* follows the steering request, so the position is the setpoint plus a     *)
(* tracking error drawn from ErrorSet. The finite set is the measurement    *)
(* grid TLC explores; the bounds below do not depend on which measurements  *)
(* occur, only on the clamps that follow them.                             *)
(***************************************************************************)
Measurements == { <<sp + e, v, sp>> : e \in ErrorSet, v \in VelSet, sp \in SetpointSet }

(***************************************************************************)
(* One period of the law: relais_ctl_step, line by line. It returns the      *)
(* command and the new integral term (anti-windup applied).                 *)
(***************************************************************************)
ControlStep(m) ==
    LET pos    == m[1]
        vel    == m[2]
        sp     == m[3]
        error  == sp - pos
        inc    == (error * DtNum) \div DtDen
        integ1 == ClampInteg(integ + inc)
        wanted == Kp * error - ((KdNum * vel) \div KdDen) + Ki * integ1
        bounded == ClampValue(wanted)
        integ2 == IF bounded # wanted THEN integ1 - inc ELSE integ1
        value  == ClampSlew(cmd, bounded)
    IN <<value, integ2>>

(***************************************************************************)
(* The part of the law that feeds the testament and not the command: the     *)
(* filtered slope of the steering request. Kept for the record; it is not   *)
(* part of the checked state (see the header).                              *)
(***************************************************************************)
SetpointFilter(prevSetpoint, spRate, sp) ==
    LET rate == ClampRate((sp - prevSetpoint) * RateGain)
    IN spRate + ((FilterNum * (rate - spRate)) \div 100)

(***************************************************************************)
(* The rack integration, core/src/plant.c: a second order model whose      *)
(* velocity is bounded by 10 * PlantGain. Kept here for the record: the    *)
(* command bounds above do not depend on it, and the closed loop with its  *)
(* continuous state is checked by the C simulator, not by TLC.             *)
(***************************************************************************)
PlantStep(posIn, velIn, cmdIn, disturbance, gain, damp, tau) ==
    LET acc  == (gain * cmdIn - damp * velIn) \div tau + disturbance
        vel2 == velIn + (acc * DtNum) \div DtDen
    IN <<posIn + (vel2 * DtNum) \div DtDen, vel2>>

(***************************************************************************)
(* The specification.                                                       *)
(***************************************************************************)
TypeOK ==
    /\ integ \in Int
    /\ cmd \in Int
    /\ prevCmd \in Int

Init ==
    /\ integ \in {-IntegLimit, 0, IntegLimit}
    /\ cmd = 0
    /\ prevCmd = 0

Next ==
    \E m \in Measurements :
        LET cs == ControlStep(m)
        IN  /\ cmd' = cs[1]
            /\ prevCmd' = cmd
            /\ integ' = cs[2]

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* The invariants.                                                          *)
(***************************************************************************)
\* ACT-05: the command never leaves the amplitude bound.
CmdBounded == -MaxAmp <= cmd /\ cmd <= MaxAmp

\* ACT-05: the command never moves by more than max_slew in one tick.
SlewBounded == -MaxSlew <= cmd - prevCmd /\ cmd - prevCmd <= MaxSlew

\* The integral term stays inside its own limit (anti-windup by
\* back-calculation, then the same increment subtracted).
IntegBounded == -IntegLimit <= integ /\ integ <= IntegLimit

====
