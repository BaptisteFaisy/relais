//! The actuator's arbiter, ported from `core/src/arbiter.c` (spec 4.3, 4.4, 4.5,
//! 5.2, 5.6, 5.7).
//!
//! Same order of decisions as the C original, because that order IS the
//! protocol: cession of the previous tick, scan, count the miss, revoke on p+1
//! misses, grant a demande, pick the one value, certify, liveness, lamp, then
//! publish. Changing the order changes the behaviour, so nothing here is
//! "improved": where the C code has a dead field (`have_order`, `have_shadow`,
//! never written by `arbiter.c`), this port keeps it and keeps it false, so the
//! two state dumps stay comparable field by field.

use crate::params::Params;
use crate::types::*;

/// The whole arbiter state, mirroring `relais_arb`.
#[derive(Clone, Copy, Debug)]
pub struct Arb {
    /// Protocol parameters (spec 6.3).
    pub params: Params,
    /// The mandate, the only copy that counts (MAN-06: vacant at boot).
    pub mandate: Mandate,
    /// Who holds it.
    pub holder: Node,
    /// Consecutive ticks without a valid holder order: the actuator's own
    /// evidence, never a claim (DET-01).
    pub misses: u8,
    /// A certified second became holder during this tick (REL-05).
    pub switch_pending: bool,
    /// The second is certified (REP-05).
    pub certified: bool,
    /// Which node that is.
    pub certified_node: Node,
    /// DEG-08: the certified second is a dissimilar backup.
    pub certified_dissimilar: bool,
    /// The pending certification streak used the widened band.
    pub widen_seen: bool,
    /// Matching ticks in a row.
    pub certify_streak: u8,
    /// Ticks without a shadow from the certified second.
    pub shadow_misses: u8,
    /// Last order value seen.
    pub last_order: f32,
    /// Last shadow value seen.
    pub last_shadow: f32,
    /// Dead in the C core too: never written, kept so both dumps match.
    pub have_order: bool,
    /// Dead in the C core too: never written, kept so both dumps match.
    pub have_shadow: bool,
    /// Ticks above epsilon in a row (DET-09).
    pub disagree_streak: u8,
    /// The two machines no longer agree.
    pub disagreement: bool,
    /// The will of the last order received (ACT-02).
    pub testament: [f32; TESTAMENT_MAX],
    /// How many entries are meaningful.
    pub testament_len: u8,
    /// First tick the will covers.
    pub testament_base: Tick,
    /// State of the local fallback loop (ACT-03).
    pub fb_state: f32,
    /// The fallback law is in charge.
    pub in_fallback: bool,
    /// Last tick each node was heard, indexed by [`Node`].
    pub last_heard: [Tick; NODE_SLOTS],
    /// Liveness as the actuator sees it (DEG-03).
    pub present: [bool; NODE_SLOTS],
    /// A first sighting is not a reintegration (I9).
    pub ever_seen: [bool; NODE_SLOTS],
    /// Warning lamp (DEG-01).
    pub lamp: Lamp,
    /// Why it is lit (DEG-02).
    pub cause: Cause,
    /// Ticks spent degraded (DEG-05).
    pub degraded_ticks: u32,
    /// Degraded mode ran out of time.
    pub stop_requested: bool,
    /// REL-06: a cession takes effect at the next tick.
    pub cession_pending: bool,
    /// SEC-06 rate limit window.
    pub request_window_tick: u16,
    /// SEC-06 demandes seen in that window.
    pub request_seen: u8,
    /// Rejection counters, indexed by [`Reject`].
    pub rejected: [u32; REJECT_SLOTS],
    /// Frames that failed the authenticator.
    pub forged: u32,
    /// Applied-value counters, indexed by [`Source`].
    pub applied_by_source: [u32; SRC_SLOTS],
    /// Ticks arbitrated.
    pub ticks: u32,
    /// Changes of holder.
    pub handovers: u32,
}

/// What one tick decided, mirroring `relais_arb_out`.
#[derive(Clone, Copy, Debug, Default)]
pub struct ArbOut {
    /// A value was applied at this tick: always true (I1).
    pub applied: bool,
    /// The bounded value applied.
    pub value: f32,
    /// Where it came from (spec 4.4).
    pub source: Source,
    /// Mandate in force.
    pub mandate: Mandate,
    /// Holder.
    pub holder: Node,
    /// Miss count.
    pub misses: u8,
    /// Certification that served the choice, not the one announced.
    pub certified: bool,
    /// Which node that was.
    pub certified_node: Node,
    /// Lamp.
    pub lamp: Lamp,
    /// Cause.
    pub cause: Cause,
    /// Stop requested.
    pub stop_requested: bool,
    /// Ready to send to both computers.
    pub statut: Statut,
    /// Journal events of this tick.
    pub events: [Event; EVENTS_MAX],
    /// How many of them are meaningful.
    pub event_count: usize,
}

/// What the scan of one tick's frames found.
#[derive(Default)]
pub(crate) struct Scan<'a> {
    pub(crate) order: Option<&'a Msg>,
    pub(crate) shadow: Option<&'a Msg>,
    pub(crate) demande: Option<&'a Msg>,
    pub(crate) order_valid: bool,
}

impl Default for Arb {
    fn default() -> Self {
        Self::new(&Params::default())
    }
}

impl Arb {
    /// `relais_arb_init`: vacant mandate, no will, lamp off.
    #[must_use]
    pub fn new(params: &Params) -> Self {
        Self {
            params: *params,
            mandate: 0,
            holder: NODE_NONE,
            misses: 0,
            switch_pending: false,
            certified: false,
            certified_node: NODE_NONE,
            certified_dissimilar: false,
            widen_seen: false,
            certify_streak: 0,
            shadow_misses: 0,
            last_order: 0.0,
            last_shadow: 0.0,
            have_order: false,
            have_shadow: false,
            disagree_streak: 0,
            disagreement: false,
            testament: [0.0; TESTAMENT_MAX],
            testament_len: 0,
            testament_base: TICK_NONE,
            fb_state: 0.0,
            in_fallback: false,
            last_heard: [0; NODE_SLOTS],
            present: [false; NODE_SLOTS],
            ever_seen: [false; NODE_SLOTS],
            lamp: LAMP_OFF,
            cause: CAUSE_NONE,
            degraded_ticks: 0,
            stop_requested: false,
            cession_pending: false,
            request_window_tick: 0,
            request_seen: 0,
            rejected: [0; REJECT_SLOTS],
            forged: 0,
            applied_by_source: [0; SRC_SLOTS],
            ticks: 0,
            handovers: 0,
        }
    }

    /// MAN-11: the mandate number survives a restart of the actuator.
    #[must_use]
    pub fn snapshot(&self) -> [u8; 16] {
        let mut buf = [0u8; 16];
        buf[0..4].copy_from_slice(&0x5241_5242u32.to_le_bytes()); /* "RARB" */
        buf[4] = 1;
        buf[5] = self.holder;
        buf[6] = u8::from(self.certified);
        buf[7] = self.certified_node;
        buf[8..12].copy_from_slice(&self.mandate.to_le_bytes());
        buf[12..16].copy_from_slice(&self.handovers.to_le_bytes());
        buf
    }

    /// Restore what a snapshot carries; a bad magic or version is refused.
    pub fn restore(&mut self, buf: &[u8]) -> bool {
        if buf.len() < 16 || u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]) != 0x5241_5242
            || buf[4] != 1
        {
            return false;
        }
        self.holder = buf[5];
        self.certified = buf[6] != 0;
        self.certified_node = if self.certified { buf[7] } else { NODE_NONE };
        self.mandate = u32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]);
        self.handovers = u32::from_le_bytes([buf[12], buf[13], buf[14], buf[15]]);
        true
    }

    /// Nobody holds the mandate.
    #[must_use]
    pub fn vacant(&self) -> bool {
        self.holder == NODE_NONE
    }

    /// Total frames rejected, every reason together.
    #[must_use]
    pub fn rejected_total(&self) -> u32 {
        self.rejected.iter().sum()
    }
}

/* ------------------------------------------------------------- one tick --- */

impl Arb {
    pub(crate) fn push_event(out: &mut ArbOut, kind: EventKind, node: Node, mandate: Mandate, tick: Tick, detail: u32) {
        if out.event_count >= EVENTS_MAX {
            return;
        }
        out.events[out.event_count] = Event { kind, node, mandate, tick, detail };
        out.event_count += 1;
    }

    fn reject(&mut self, out: &mut ArbOut, tick: Tick, why: Reject, m: &Msg) {
        if (why as usize) < REJECT_SLOTS {
            self.rejected[why as usize] += 1;
        }
        Self::push_event(out, EV_ORDER_REJECTED, m.sender, m.mandate, tick, u32::from(why));
    }

    /// MAN-02: an order counts only if it comes from the holder, carries the
    /// current mandate and targets the tick being applied.
    fn order_is_valid(&self, m: &Msg, tick: Tick) -> Reject {
        if m.sender != self.holder {
            return REJ_NOT_HOLDER;
        }
        if m.mandate > self.mandate {
            return REJ_MANDATE_FUTURE; /* SEC-04: nobody appoints itself */
        }
        if m.mandate != self.mandate {
            return REJ_MANDATE_STALE; /* the returning machine, spec 4.6 */
        }
        if m.tick != tick {
            return REJ_TICK_MISSED;
        }
        REJ_NONE
    }

    pub(crate) fn scan<'a>(&mut self, out: &mut ArbOut, tick: Tick, msgs: &'a [Msg]) -> Scan<'a> {
        let mut s = Scan::default();
        for m in msgs {
            if node_is_computer(m.sender) {
                self.last_heard[m.sender as usize] = tick;
            }
            match m.msg_type {
                MSG_ORDRE => {
                    let why = self.order_is_valid(m, tick);
                    if why == REJ_NONE {
                        s.order = Some(m); /* the last valid order wins */
                        s.order_valid = true;
                    } else {
                        self.reject(out, tick, why, m);
                    }
                }
                MSG_OMBRE => {
                    if m.tick == tick {
                        s.shadow = Some(m);
                    }
                }
                MSG_CESSION => {
                    /* DET-05: recorded now, the mandate is freed next tick (REL-06). */
                    if m.sender == self.holder && m.mandate == self.mandate {
                        self.cession_pending = true;
                        Self::push_event(out, EV_CESSION, m.sender, self.mandate, tick, u32::from(m.reason));
                    }
                }
                /* The first demande of the tick is the one processed (MAN-05). */
                MSG_DEMANDE if s.demande.is_none() => s.demande = Some(m),
                MSG_DEMANDE => {}
                _ => {} /* STATUT, ETAT and SUIVI never reach the actuator */
            }
        }
        s
    }

    /// The mandate goes vacant, or straight to the certified second (MAN-10).
    pub(crate) fn release_mandate(&mut self, out: &mut ArbOut, tick: Tick, kind: EventKind) {
        /* DET-03: the event carries its own evidence, the miss count this
         * actuator counted itself. */
        let evidence = u32::from(self.misses);
        Self::push_event(out, kind, self.holder, self.mandate, tick, evidence);
        if self.certified && self.certified_node != NODE_NONE && self.certified_node != self.holder {
            self.holder = self.certified_node;
            self.mandate += 1; /* MAN-03: exactly one step per change of holder */
            self.misses = 0;
            self.handovers += 1;
            self.switch_pending = true; /* REL-05: the shadow is still fresh */
            Self::push_event(out, EV_MANDATE_AUTO_SWITCH, self.holder, self.mandate, tick, evidence);
            return;
        }
        self.holder = NODE_NONE;
        self.certified = false;
        self.certified_node = NODE_NONE;
        self.certify_streak = 0;
    }

    /// SEC-06: a flood of demandes must not delay the orders.
    fn request_allowed(&mut self, tick: Tick) -> bool {
        let window = u16::try_from(tick / Tick::from(self.params.request_window.max(1)))
            .unwrap_or(u16::MAX);
        if window != self.request_window_tick {
            self.request_window_tick = window;
            self.request_seen = 0;
        }
        if self.request_seen >= self.params.request_burst {
            return false;
        }
        self.request_seen += 1;
        true
    }

    pub(crate) fn handle_demande(&mut self, out: &mut ArbOut, tick: Tick, s: &mut Scan<'_>) {
        let Some(m) = s.demande else { return };
        let vacant = self.holder == NODE_NONE;
        /* MAN-05: a grant needs the actuator's own evidence of silence. */
        let grant = vacant;
        if !grant || !self.request_allowed(tick) {
            Self::push_event(out, EV_REQUEST_DENIED, m.sender, self.mandate, tick, u32::from(grant));
            return;
        }
        self.holder = m.sender;
        self.mandate += 1;
        self.misses = 0;
        self.handovers += 1;
        self.certified = false;
        self.certified_node = NODE_NONE;
        self.certify_streak = 0;
        Self::push_event(out, EV_MANDATE_GRANTED, self.holder, self.mandate, tick, u32::from(m.reason));
        if m.has_order && m.tick == tick {
            s.order = Some(m);
            s.order_valid = true;
        }
    }
}
