//! Shared vocabulary: the same constants, the same field names and the same
//! integer values as `core/include/relais/types.h`. Every enum of the C core is
//! a `u8`/`u32` here, because the differential test compares the numbers that
//! go on the wire and in the journal, not Rust type names.

/// Tick number, given by the actuator (spec 4.2).
pub type Tick = u64;
/// Mandate number, handed out by the actuator and by nobody else (MAN-03).
pub type Mandate = u32;
/// Who is speaking.
pub type Node = u8;
/// Where the value applied at a tick came from (spec 4.4).
pub type Source = u8;
/// Warning lamp (DEG-01).
pub type Lamp = u8;
/// Why the lamp is lit (DEG-02).
pub type Cause = u8;
/// Why a frame was dropped (MAN-02, DET-04, SEC-01).
pub type Reject = u8;
/// Journal event kind (spec 6.4).
pub type EventKind = u8;

/// No tick: the sentinel of `testament_base` before the first order.
pub const TICK_NONE: Tick = Tick::MAX;

/// Nobody: the mandate is vacant.
pub const NODE_NONE: Node = 0;
/// The first computer.
pub const NODE_A: Node = 1;
/// The second computer.
pub const NODE_B: Node = 2;
/// A third computer, present in the vocabulary, unused by the spec.
pub const NODE_C: Node = 3;
/// The actuator, which is a node too: it sends STATUT.
pub const NODE_ACTUATOR: Node = 4;
/// Slots of the liveness arrays, indexed by [`Node`].
pub const NODE_SLOTS: usize = 5;

/// Actuator to both computers, every tick: the tempo (CMD-02).
pub const MSG_STATUT: u8 = 1;
/// Holder to actuator, every tick: the command (MAN-02).
pub const MSG_ORDRE: u8 = 2;
/// Second to actuator, every tick at level C: the shadow (REL-05).
pub const MSG_OMBRE: u8 = 3;
/// Non-holder to actuator, on suspicion (MAN-05).
pub const MSG_DEMANDE: u8 = 4;
/// Holder to actuator and second: I give up (DET-05, REL-06).
pub const MSG_CESSION: u8 = 5;

/// No value applied: never happens, one value per tick (I1).
pub const SRC_NONE: Source = 0;
/// A fresh order from the mandate holder.
pub const SRC_HOLDER: Source = 1;
/// Substitution by the shadow of the certified second.
pub const SRC_SHADOW: Source = 2;
/// The will of the last order received (ACT-02).
pub const SRC_TESTAMENT: Source = 3;
/// The local damped loop inside the actuator (ACT-03).
pub const SRC_FALLBACK: Source = 4;
/// Slots of `applied_by_source`, indexed by [`Source`].
pub const SRC_SLOTS: usize = 5;

/// Full redundancy (DEG-01).
pub const LAMP_OFF: Lamp = 0;
/// Redundancy lost, command still fresh.
pub const LAMP_YELLOW: Lamp = 1;
/// Fallback law, disagreement, or a stop request.
pub const LAMP_RED: Lamp = 2;

/// Nothing to report.
pub const CAUSE_NONE: Cause = 0;
/// A computer is missing.
pub const CAUSE_NODE_MISSING: Cause = 1;
/// The state link between the two computers is lost.
pub const CAUSE_LINK_LOST: Cause = 2;
/// The second is not certified (REP-05).
pub const CAUSE_SECOND_UNCERTIFIED: Cause = 3;
/// The two machines no longer agree (DET-09).
pub const CAUSE_DISAGREEMENT: Cause = 4;
/// The fallback law is in charge (ACT-03).
pub const CAUSE_FALLBACK: Cause = 5;
/// Degraded mode ran out of time (DEG-05).
pub const CAUSE_STOP_REQUESTED: Cause = 6;

/// Not rejected.
pub const REJ_NONE: Reject = 0;
/// The sender does not hold the mandate (MAN-02).
pub const REJ_NOT_HOLDER: Reject = 1;
/// The frame carries an older mandate: the returning machine.
pub const REJ_MANDATE_STALE: Reject = 2;
/// The frame claims a mandate nobody handed out (SEC-04).
pub const REJ_MANDATE_FUTURE: Reject = 3;
/// The order does not target the tick being applied (I3).
pub const REJ_TICK_MISSED: Reject = 4;
/// Slots of the rejection counters.
pub const REJECT_SLOTS: usize = 10;

/// A demande was granted (MAN-05).
pub const EV_MANDATE_GRANTED: EventKind = 1;
/// The actuator withdrew the mandate on its own evidence (MAN-04).
pub const EV_MANDATE_REVOKED: EventKind = 2;
/// The mandate went straight to the certified second (MAN-10).
pub const EV_MANDATE_AUTO_SWITCH: EventKind = 3;
/// A demande was refused (SEC-06).
pub const EV_REQUEST_DENIED: EventKind = 4;
/// The second became certified (REP-05).
pub const EV_CERTIFIED: EventKind = 5;
/// The second lost its certification.
pub const EV_DECERTIFIED: EventKind = 6;
/// A frame was dropped, with its reason (DET-04).
pub const EV_ORDER_REJECTED: EventKind = 7;
/// Three ticks above epsilon (DET-09).
pub const EV_DISAGREEMENT: EventKind = 8;
/// The will is exhausted, the local loop takes over.
pub const EV_FALLBACK_ENTER: EventKind = 9;
/// A valid order came back.
pub const EV_FALLBACK_EXIT: EventKind = 10;
/// The holder gave up the mandate (DET-05).
pub const EV_CESSION: EventKind = 11;
/// The warning lamp changed (DEG-02).
pub const EV_LAMP_CHANGE: EventKind = 12;
/// Degraded mode ran out of time (DEG-05).
pub const EV_STOP_REQUESTED: EventKind = 13;
/// A machine came back, which is not a first sighting (I9).
pub const EV_NODE_BACK: EventKind = 15;

/// Horizon of the will embedded in an order (ACT-02).
pub const TESTAMENT_MAX: usize = 8;
/// Journal events produced by one tick.
pub const EVENTS_MAX: usize = 8;
/// Frames handed to one arbiter step.
pub const IN_MSG_MAX: usize = 8;
/// STATUT carries the dissimilar-second flag of DEG-08.
pub const FLAG_DISSIMILAR: u8 = 0x10;
/// Liveness window of the arbiter, in ticks.
pub const LIVENESS_TICKS: Tick = 8;
/// Damping of the local fallback loop (ACT-03).
pub const FALLBACK_DAMP: f32 = 0.5;

/// A computer or the actuator is a node; only computers hold a mandate.
#[must_use]
pub const fn node_is_computer(n: Node) -> bool {
    (n as usize) >= NODE_A as usize && (n as usize) <= NODE_C as usize
}

/// Measurements carried by STATUT and used by the control law.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Meas {
    /// Rack position, normalised.
    pub pos: f32,
    /// Rack velocity.
    pub vel: f32,
    /// Value applied by the actuator at the previous tick.
    pub applied: f32,
    /// Steering wheel request for the next tick.
    pub setpoint: f32,
}

/// One decoded frame, restricted to the fields the arbiter reads. The C
/// `relais_msg` is larger (ETAT carries a control state); the arbiter never
/// looks at those, so this port does not carry them either.
#[derive(Clone, Copy, Debug, Default)]
pub struct Msg {
    /// Frame type, one of the `MSG_*` constants.
    pub msg_type: u8,
    /// Who sent it.
    pub sender: Node,
    /// Frame flags of spec 6.2.
    pub flags: u8,
    /// Tick emitted (STATUT) or targeted (orders).
    pub tick: Tick,
    /// Mandate the sender believes it holds.
    pub mandate: Mandate,
    /// Command value of an order, a shadow or a demande carrying one.
    pub value: f32,
    /// The will of the holder for the next ticks (ACT-02).
    pub testament: [f32; TESTAMENT_MAX],
    /// How many entries of `testament` are meaningful.
    pub testament_len: u8,
    /// A DEMANDE may carry its first order (MAN-05).
    pub has_order: bool,
    /// Reason code of a DEMANDE or a CESSION.
    pub reason: u8,
    /// Measurements of a STATUT.
    pub meas: Meas,
}

/// One journal event, shaped exactly like `relais_event` (DET-03: the event
/// carries its own evidence).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Event {
    /// What happened.
    pub kind: EventKind,
    /// Who it happened to.
    pub node: Node,
    /// Mandate at that moment.
    pub mandate: Mandate,
    /// Tick of the decision.
    pub tick: Tick,
    /// Evidence: reject reason, miss count, lamp value, streak length.
    pub detail: u32,
}

/// The STATUT published to both computers at the end of a tick (MAN-07).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Statut {
    /// Always [`MSG_STATUT`].
    pub msg_type: u8,
    /// Always [`NODE_ACTUATOR`].
    pub sender: Node,
    /// Tick this STATUT tempo-es.
    pub tick: Tick,
    /// Mandate in force.
    pub mandate: Mandate,
    /// Who holds it.
    pub holder: Node,
    /// Where the applied value came from.
    pub source: Source,
    /// Consecutive ticks without a valid holder order.
    pub misses: u8,
    /// The second is certified (REP-05).
    pub certified: bool,
    /// Which node that is.
    pub certified_node: Node,
    /// Warning lamp (DEG-01).
    pub lamp: Lamp,
    /// Why it is lit (DEG-02).
    pub cause: Cause,
    /// Degraded mode ran out of time (DEG-05).
    pub stop_requested: bool,
    /// Wire flags, including [`FLAG_DISSIMILAR`] (DEG-08).
    pub flags: u8,
    /// Measurements, with `applied` set to the value of this tick.
    pub meas: Meas,
}

/// ACT-05: amplitude bound. NaN collapses to `-max_amplitude`, exactly like the
/// C `relais_clamp_value` does with its `!(v > -max)` test.
#[must_use]
#[allow(clippy::neg_cmp_op_on_partial_ord)] /* deliberate: see the comment below */
pub fn clamp_value(v: f32, max_amplitude: f32) -> f32 {
    /* The negated comparison is the point, not an oversight: it is the only form
     * that sends NaN to -max_amplitude, which is what the C `relais_clamp_value`
     * does. Rewriting it with `partial_cmp` would silently change NaN handling
     * and break the differential test against the C arbiter. */
    if !(v > -max_amplitude) {
        return -max_amplitude;
    }
    if v > max_amplitude {
        return max_amplitude;
    }
    v
}

/// ACT-05: bound on the change per tick.
#[must_use]
pub fn clamp_slew(previous: f32, wanted: f32, max_slew: f32) -> f32 {
    let delta = wanted - previous;
    if delta > max_slew {
        return previous + max_slew;
    }
    if delta < -max_slew {
        return previous - max_slew;
    }
    wanted
}

/// I6: the value of the will for a tick, or `None` when the horizon is over.
#[must_use]
pub fn testament_at(testament: &[f32; TESTAMENT_MAX], len: u8, base: Tick, tick: Tick) -> Option<f32> {
    if len == 0 || base == TICK_NONE || tick < base {
        return None;
    }
    let offset = tick - base;
    if offset >= len as Tick {
        return None;
    }
    Some(testament[offset as usize])
}
