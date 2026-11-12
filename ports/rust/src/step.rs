//! The tick pipeline of the arbiter: pick the one value, certify the second,
//! watch liveness, drive the lamp, publish the STATUT. Split from `arbiter.rs`
//! only for size; these are inherent methods of the same [`Arb`].

use crate::arbiter::{Arb, ArbOut, Scan};
use crate::types::*;

impl Arb {
    /// Spec 4.4: exactly one value per tick, in this order — holder, certified
    /// shadow, will, local fallback law.
    fn pick_value(&mut self, out: &mut ArbOut, tick: Tick, s: &Scan<'_>, meas: &Meas) {
        let amp = self.params.max_amplitude;
        if let Some(order) = s.order.filter(|_| s.order_valid) {
            out.value = clamp_value(order.value, amp);
            out.source = SRC_HOLDER;
            self.applied_by_source[SRC_HOLDER as usize] += 1;
            /* ACT-02: the will of the last order, covering the next N ticks. */
            self.testament_len = order.testament_len.min(TESTAMENT_MAX as u8);
            let n = usize::from(self.testament_len);
            self.testament[..n].copy_from_slice(&order.testament[..n]);
            /* The order covers `tick`, so the will starts at tick + 1. */
            self.testament_base = tick.wrapping_add(1);
            if self.in_fallback {
                self.in_fallback = false;
                Arb::push_event(out, EV_FALLBACK_EXIT, self.holder, self.mandate, tick, 0);
            }
            return;
        }
        if let Some(shadow) = s.shadow {
            if self.certified && shadow.sender == self.certified_node {
                out.value = clamp_value(shadow.value, amp);
                out.source = SRC_SHADOW; /* REL-05: substitution, no stale tick */
                self.applied_by_source[SRC_SHADOW as usize] += 1;
                return;
            }
        }
        if let Some(planned) = testament_at(&self.testament, self.testament_len, self.testament_base, tick) {
            out.value = clamp_value(planned, amp);
            out.source = SRC_TESTAMENT;
            self.applied_by_source[SRC_TESTAMENT as usize] += 1;
            return;
        }
        /* ACT-03: the will is exhausted, the local damped loop takes over. */
        let wanted = clamp_value(-FALLBACK_DAMP * meas.vel, amp);
        self.fb_state = clamp_slew(self.fb_state, wanted, self.params.max_slew);
        out.value = clamp_value(self.fb_state, amp);
        out.source = SRC_FALLBACK;
        self.applied_by_source[SRC_FALLBACK as usize] += 1;
        if !self.in_fallback {
            self.in_fallback = true;
            Arb::push_event(out, EV_FALLBACK_ENTER, NODE_NONE, self.mandate, tick, 0);
        }
    }

    /// REP-05 and DET-09: certification by agreement, disagreement alert.
    fn certify(&mut self, out: &mut ArbOut, tick: Tick, s: &Scan<'_>) {
        if self.holder == NODE_NONE {
            self.certify_streak = 0;
            return;
        }
        let second = if self.holder == NODE_A { NODE_B } else { NODE_A };
        let shadow = match s.shadow {
            Some(m) if m.sender == second => m,
            _ => {
                self.certify_streak = 0;
                self.widen_seen = false;
                /* The C original writes `if (certified && ++shadow_misses > p)`:
                 * the counter only moves while there is a certification to lose,
                 * so an uncertified second does not accumulate misses. */
                if self.certified {
                    self.shadow_misses += 1;
                    if self.shadow_misses > self.params.patience {
                        self.certified = false;
                        self.certified_node = NODE_NONE;
                        self.certified_dissimilar = false;
                        self.shadow_misses = 0;
                        Arb::push_event(out, EV_DECERTIFIED, second, self.mandate, tick, 0);
                    }
                }
                return;
            }
        };
        self.shadow_misses = 0;
        if let Some(order) = s.order.filter(|_| s.order_valid) {
            self.compare_order_shadow(out, tick, second, order.value, shadow.value);
        }
        if !self.certified && self.certify_streak >= self.params.certify_ticks {
            self.certified = true;
            self.certified_node = second;
            self.certified_dissimilar = self.widen_seen;
            Arb::push_event(out, EV_CERTIFIED, second, self.mandate, tick, u32::from(self.certify_streak));
        }
    }

    /// DET-09 / DEG-08: one comparison, three outcomes — agreement, agreement
    /// inside the widened band of a dissimilar backup, disagreement.
    fn compare_order_shadow(&mut self, out: &mut ArbOut, tick: Tick, second: Node, order: f32, shadow: f32) {
        let gap = (order - shadow).abs();
        self.last_order = order;
        self.last_shadow = shadow;
        let identical = gap <= self.params.epsilon;
        let dissimilar = !identical
            && self.params.dissimilar_epsilon > 0.0
            && gap <= self.params.dissimilar_epsilon;
        if identical || dissimilar {
            self.disagree_streak = 0;
            self.disagreement = false; /* DEG-02: a stopped disagreement stops lighting */
            if dissimilar {
                self.widen_seen = true;
                if self.certified {
                    self.certified_dissimilar = true;
                }
            }
            self.certify_streak = self.certify_streak.saturating_add(1);
            return;
        }
        self.certify_streak = 0;
        self.widen_seen = false;
        if self.certified {
            self.certified = false;
            self.certified_node = NODE_NONE;
            self.certified_dissimilar = false;
            Arb::push_event(out, EV_DECERTIFIED, second, self.mandate, tick, 0);
        }
        self.disagree_streak = self.disagree_streak.saturating_add(1);
        if self.disagree_streak >= 3 && !self.disagreement {
            self.disagreement = true; /* three ticks above epsilon */
            Arb::push_event(out, EV_DISAGREEMENT, second, self.mandate, tick, (gap * 1000.0) as u32);
        }
    }

    /// DEG-03 and I9: a machine coming BACK is an event; a first sighting, while
    /// the system arms itself, is not.
    fn liveness(&mut self, out: &mut ArbOut, tick: Tick) {
        for i in usize::from(NODE_A)..=usize::from(NODE_C) {
            let seen = self.last_heard[i] != 0 && tick.saturating_sub(self.last_heard[i]) <= LIVENESS_TICKS;
            if seen && !self.present[i] {
                self.present[i] = true;
                if self.ever_seen[i] {
                    Arb::push_event(out, EV_NODE_BACK, i as Node, self.mandate, tick, 0);
                }
            } else if !seen && self.present[i] {
                self.present[i] = false;
            }
            if seen {
                self.ever_seen[i] = true;
            }
        }
    }
}

/// `pick_value` and friends need to append events; the helper is private to
/// `arbiter.rs`, so this crate-private alias keeps one implementation.
impl Arb {
    /// DEG-01, DEG-02 and I10: the lamp follows the cause and stays lit while
    /// the cause lasts.
    fn update_lamp(&mut self, out: &mut ArbOut, tick: Tick) {
        let second = if self.holder == NODE_A { NODE_B } else { NODE_A };
        let peer_present = self.present[usize::from(second)];
        let mut lamp = LAMP_OFF;
        let mut cause = CAUSE_NONE;
        if out.source == SRC_FALLBACK {
            lamp = LAMP_RED;
            cause = CAUSE_FALLBACK;
        } else if self.disagreement {
            lamp = LAMP_RED;
            cause = CAUSE_DISAGREEMENT;
        } else if out.source != SRC_HOLDER {
            /* DEG-01: the holder is not delivering, redundancy is lost even
             * while the substitution keeps the command fresh. */
            lamp = LAMP_YELLOW;
            cause = CAUSE_NODE_MISSING;
        } else if self.holder == NODE_NONE || !peer_present {
            lamp = LAMP_YELLOW;
            cause = if self.holder == NODE_NONE { CAUSE_NODE_MISSING } else { CAUSE_LINK_LOST };
        } else if !self.certified || self.certified_dissimilar {
            lamp = LAMP_YELLOW; /* DEG-08: a dissimilar second is not an equivalent one */
            cause = CAUSE_SECOND_UNCERTIFIED;
        }
        if self.stop_requested {
            lamp = LAMP_RED;
            cause = CAUSE_STOP_REQUESTED;
        }
        if lamp != self.lamp {
            Arb::push_event(out, EV_LAMP_CHANGE, self.holder, self.mandate, tick, u32::from(lamp));
            self.lamp = lamp;
            self.cause = cause;
        } else {
            self.cause = cause;
        }
        self.degraded_timer(out, lamp, tick);
    }

    /// DEG-05: degraded mode is limited in time.
    fn degraded_timer(&mut self, out: &mut ArbOut, lamp: Lamp, tick: Tick) {
        if lamp == LAMP_OFF {
            self.degraded_ticks = 0;
            return;
        }
        if self.degraded_ticks >= self.params.degraded_ticks {
            return;
        }
        self.degraded_ticks += 1;
        if self.degraded_ticks >= self.params.degraded_ticks && !self.stop_requested {
            self.stop_requested = true;
            Arb::push_event(out, EV_STOP_REQUESTED, self.holder, self.mandate, tick, self.degraded_ticks);
        }
    }

    /// One tick. `msgs` are the frames received during the previous period,
    /// already decoded and authenticated by the transport layer.
    pub fn step(&mut self, tick: Tick, msgs: &[Msg], meas: &Meas, out: &mut ArbOut) {
        *out = ArbOut::default();
        self.ticks += 1;
        /* REL-06: a cession recorded at the previous tick frees the mandate
         * here, so the order already received for that tick was applied under a
         * known holder and a new one can be granted in the same tick. */
        if self.cession_pending {
            self.cession_pending = false;
            self.release_mandate(out, tick, EV_MANDATE_REVOKED);
        }
        let mut s = self.scan(out, tick, msgs);
        /* 2. the miss is counted first: the actuator's own evidence (DET-01). */
        self.misses = if s.order_valid { 0 } else { self.misses.saturating_add(1) };
        /* 3. revoke on p + 1 consecutive misses (MAN-04). */
        if self.holder != NODE_NONE && self.misses > self.params.patience {
            self.release_mandate(out, tick, EV_MANDATE_REVOKED);
        }
        /* 4. a DEMANDE can now be granted. */
        self.handle_demande(out, tick, &mut s);
        /* 5. the one value of this tick. */
        self.pick_value(out, tick, &s, meas);
        /* What the journal reports is the certification that served the choice;
         * what the STATUT announces is what the machines must act on now. */
        let certified_when_picked = self.certified;
        let certified_node_when_picked = self.certified_node;
        if self.switch_pending {
            /* DEG-07: the new holder has no certified second any more. */
            self.switch_pending = false;
            self.certified = false;
            self.certified_node = NODE_NONE;
            self.certify_streak = 0;
        }
        self.certify(out, tick, &s);
        self.liveness(out, tick);
        self.update_lamp(out, tick);
        self.publish(out, tick, meas, certified_when_picked, certified_node_when_picked);
    }

    /// 7. What both computers are told (MAN-07).
    #[allow(clippy::too_many_arguments)]
    fn publish(
        &self,
        out: &mut ArbOut,
        tick: Tick,
        meas: &Meas,
        certified_when_picked: bool,
        certified_node_when_picked: Node,
    ) {
        out.applied = true;
        out.mandate = self.mandate;
        out.holder = self.holder;
        out.misses = self.misses;
        out.certified = certified_when_picked;
        out.certified_node = certified_node_when_picked;
        out.lamp = self.lamp;
        out.cause = self.cause;
        out.stop_requested = self.stop_requested;
        let mut meas_out = *meas;
        meas_out.applied = out.value;
        out.statut = Statut {
            msg_type: MSG_STATUT,
            sender: NODE_ACTUATOR,
            tick,
            mandate: self.mandate,
            holder: self.holder,
            source: out.source,
            misses: self.misses,
            certified: self.certified,
            certified_node: self.certified_node,
            lamp: self.lamp,
            cause: self.cause,
            stop_requested: self.stop_requested,
            flags: if self.certified && self.certified_dissimilar { FLAG_DISSIMILAR } else { 0 },
            meas: meas_out,
        };
    }
}
