//! Rust-side unit tests of the mandate rules (mission M32, idea 74).
//!
//! These do NOT need the C bridge: they check the ported rules on their own, so
//! `cargo test` is a standalone verdict on the Rust arbiter. The confrontation
//! with the C reference lives in `ports/rust/difftest/run_difftest.py`.
//!
//! Requirements: MAN-02, MAN-03, MAN-04, MAN-05, MAN-06, MAN-10, MAN-11,
//! REL-05, REL-06, DET-01, DET-03, DET-09, REP-05, SEC-04, SEC-06, ACT-02,
//! ACT-03, ACT-05, I1, I6, DEG-05.

use relais_arbiter::arbiter::{Arb, ArbOut};
use relais_arbiter::params::Params;
use relais_arbiter::types::*;

fn order(sender: Node, tick: Tick, mandate: Mandate, value: f32) -> Msg {
    Msg { msg_type: MSG_ORDRE, sender, tick, mandate, value, ..Msg::default() }
}

fn shadow(sender: Node, tick: Tick, value: f32) -> Msg {
    Msg { msg_type: MSG_OMBRE, sender, tick, value, ..Msg::default() }
}

fn demande(sender: Node, tick: Tick, with_order: bool, value: f32) -> Msg {
    Msg {
        msg_type: MSG_DEMANDE,
        sender,
        tick,
        reason: 1,
        has_order: with_order,
        value,
        testament_len: if with_order { TESTAMENT_MAX as u8 } else { 0 },
        testament: [value; TESTAMENT_MAX],
        ..Msg::default()
    }
}

fn cession(sender: Node, mandate: Mandate) -> Msg {
    Msg { msg_type: MSG_CESSION, sender, mandate, reason: 1, ..Msg::default() }
}

fn step(a: &mut Arb, tick: Tick, msgs: &[Msg]) -> ArbOut {
    let mut out = ArbOut::default();
    a.step(tick, msgs, &Meas::default(), &mut out);
    out
}

fn has_event(out: &ArbOut, kind: EventKind) -> bool {
    out.events[..out.event_count].iter().any(|e| e.kind == kind)
}

#[test]
fn man06_boot_is_vacant() {
    let a = Arb::new(&Params::default());
    assert!(a.vacant());
    assert_eq!(a.mandate, 0);
    assert_eq!(a.holder, NODE_NONE);
    assert_eq!(a.lamp, LAMP_OFF);
}

#[test]
fn man05_grant_only_when_vacant_and_man03_one_step() {
    let mut a = Arb::new(&Params::default());
    let out = step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);
    assert_eq!(a.mandate, 1); /* MAN-03: exactly one step */
    assert_eq!(a.holder, NODE_A);
    assert_eq!(out.source, SRC_HOLDER); /* MAN-05: the demande carried the order */
    assert!(has_event(&out, EV_MANDATE_GRANTED));
    /* A demande while a holder is in place is refused (MAN-05). */
    let out = step(&mut a, 2, &[demande(NODE_B, 2, false, 0.0), order(NODE_A, 2, 1, 0.2)]);
    assert_eq!(a.holder, NODE_A);
    assert_eq!(a.mandate, 1);
    assert!(has_event(&out, EV_REQUEST_DENIED));
}

#[test]
fn man02_only_the_holder_current_mandate_this_tick() {
    /* patience 3 keeps A the holder while each refusal reason is exercised in
     * turn: with the default p = 1 the third miss would revoke the mandate and
     * every later frame would be refused as "not the holder" instead. */
    let p = Params { patience: 3, ..Params::default() };
    let mut a = Arb::new(&p);
    step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);

    let out = step(&mut a, 2, &[order(NODE_B, 2, 1, 0.3)]); /* not the holder */
    assert_eq!(a.rejected[REJ_NOT_HOLDER as usize], 1);
    assert_eq!(out.source, SRC_TESTAMENT); /* the will covers the miss (I6) */

    let out = step(&mut a, 3, &[order(NODE_A, 3, 99, 0.3)]); /* SEC-04 */
    assert_eq!(a.rejected[REJ_MANDATE_FUTURE as usize], 1);
    assert!(has_event(&out, EV_ORDER_REJECTED)); /* DET-03: the event carries it */
    assert_ne!(out.source, SRC_HOLDER);

    let out = step(&mut a, 4, &[order(NODE_A, 4, 0, 0.3)]); /* the returning machine */
    assert_eq!(a.rejected[REJ_MANDATE_STALE as usize], 1);
    assert_ne!(out.source, SRC_HOLDER);
    assert_eq!(a.holder, NODE_A); /* three misses, p = 3: still tolerated */

    let out = step(&mut a, 5, &[order(NODE_A, 4, 1, 0.3)]); /* I3: wrong tick */
    assert_eq!(a.rejected[REJ_TICK_MISSED as usize], 1);
    assert_ne!(out.source, SRC_HOLDER);
    /* MAN-04: the fourth miss is p + 1, the actuator withdraws on its own
     * evidence, and the event carries the count it measured (DET-03). */
    assert!(a.vacant());
    assert!(has_event(&out, EV_MANDATE_REVOKED));
    assert_eq!(out.misses, 4);
}

#[test]
fn man04_revoke_after_p_plus_one_misses_with_evidence() {
    let mut a = Arb::new(&Params::default()); /* p = 1 */
    step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);
    let out = step(&mut a, 2, &[]); /* miss 1: tolerated */
    assert_eq!(out.misses, 1);
    assert_eq!(a.holder, NODE_A);
    let out = step(&mut a, 3, &[]); /* miss 2 = p + 1: revoked (DET-01) */
    assert!(has_event(&out, EV_MANDATE_REVOKED));
    assert!(a.vacant());
    let evidence = out.events[..out.event_count]
        .iter()
        .find(|e| e.kind == EV_MANDATE_REVOKED)
        .map_or(0, |e| e.detail);
    assert_eq!(evidence, 2); /* DET-03: what the actuator counted itself */
}

#[test]
fn man10_auto_switch_to_the_certified_second() {
    let mut a = Arb::new(&Params::default());
    step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);
    for t in 2..5 {
        /* Three matching shadows certify B (REP-05, certify_ticks = 3). */
        step(&mut a, t, &[order(NODE_A, t, 1, 0.2), shadow(NODE_B, t, 0.2)]);
    }
    assert!(a.certified);
    assert_eq!(a.certified_node, NODE_B);
    step(&mut a, 5, &[shadow(NODE_B, 5, 0.2)]); /* A dies: miss 1 */
    let out = step(&mut a, 6, &[shadow(NODE_B, 6, 0.2)]); /* miss 2: switch */
    assert!(has_event(&out, EV_MANDATE_AUTO_SWITCH));
    assert_eq!(a.holder, NODE_B);
    assert_eq!(a.mandate, 2); /* MAN-03 */
    assert_eq!(a.handovers, 2);
    assert!(!a.certified); /* DEG-07: the new holder has no certified second */
}

#[test]
fn rel06_cession_takes_effect_next_tick() {
    let mut a = Arb::new(&Params::default());
    step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);
    let out = step(&mut a, 2, &[order(NODE_A, 2, 1, 0.2), cession(NODE_A, 1)]);
    assert!(has_event(&out, EV_CESSION));
    assert_eq!(a.holder, NODE_A); /* REL-06: still the holder for this tick */
    assert_eq!(out.source, SRC_HOLDER); /* its order was applied */
    let out = step(&mut a, 3, &[]);
    assert!(has_event(&out, EV_MANDATE_REVOKED));
    assert!(a.vacant());
}

#[test]
fn sec06_demande_flood_is_rate_limited() {
    let mut a = Arb::new(&Params::default()); /* 4 demandes per window of 4 */
    let mut denied = 0;
    for t in 1..=8 {
        let out = step(&mut a, t, &[demande(NODE_B, t, false, 0.0)]);
        if has_event(&out, EV_REQUEST_DENIED) {
            denied += 1;
        }
    }
    assert!(denied >= 4);
    assert_eq!(a.rejected_total(), 0); /* a denied demande is not a bad frame */
}

#[test]
fn det09_three_ticks_above_epsilon_light_the_lamp() {
    let mut a = Arb::new(&Params::default());
    step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);
    for t in 2..4 {
        let out = step(&mut a, t, &[order(NODE_A, t, 1, 0.2), shadow(NODE_B, t, 0.9)]);
        assert!(!a.disagreement);
        assert_ne!(out.lamp, LAMP_RED);
    }
    let out = step(&mut a, 4, &[order(NODE_A, 4, 1, 0.2), shadow(NODE_B, 4, 0.9)]);
    assert!(a.disagreement);
    assert!(has_event(&out, EV_DISAGREEMENT));
    assert_eq!(out.lamp, LAMP_RED); /* DEG-01, DEG-02 */
    assert_eq!(out.cause, CAUSE_DISAGREEMENT);
}

#[test]
fn i6_the_will_stops_at_its_horizon_then_act03_takes_over() {
    let mut a = Arb::new(&Params::default()); /* N = 8 */
    let out = step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);
    assert_eq!(out.source, SRC_HOLDER);
    for t in 2..=9 {
        let out = step(&mut a, t, &[]);
        assert_eq!(out.source, SRC_TESTAMENT, "tick {t} should be covered by the will");
    }
    let out = step(&mut a, 10, &[]);
    assert_eq!(out.source, SRC_FALLBACK); /* ACT-03 */
    assert!(has_event(&out, EV_FALLBACK_ENTER));
    assert!(out.applied); /* I1: one value per tick, always */
    let out = step(&mut a, 11, &[]);
    assert_eq!(out.source, SRC_FALLBACK);
    assert!(!has_event(&out, EV_FALLBACK_ENTER)); /* entered once */
}

#[test]
fn act05_values_stay_inside_the_bounds() {
    let mut a = Arb::new(&Params::default());
    let mut huge = demande(NODE_A, 1, true, 0.2);
    huge.value = 1.0e9;
    huge.testament = [1.0e9; TESTAMENT_MAX];
    let out = step(&mut a, 1, &[huge]);
    assert!(out.value <= a.params.max_amplitude);
    for t in 2..12 {
        let out = step(&mut a, t, &[]);
        assert!(out.value.abs() <= a.params.max_amplitude, "tick {t} out of bounds");
    }
}

#[test]
fn man11_snapshot_survives_a_restart() {
    let mut a = Arb::new(&Params::default());
    step(&mut a, 1, &[demande(NODE_A, 1, true, 0.2)]);
    step(&mut a, 2, &[order(NODE_A, 2, 1, 0.2)]);
    let snap = a.snapshot();
    let mandate = a.mandate;
    let mut restarted = Arb::new(&Params::default());
    assert!(restarted.restore(&snap));
    assert_eq!(restarted.mandate, mandate);
    assert_eq!(restarted.holder, NODE_A);
    /* A corrupted or truncated snapshot is refused, not guessed. */
    let mut bad = snap;
    bad[0] ^= 0xff;
    let mut other = Arb::new(&Params::default());
    assert!(!other.restore(&bad));
    assert_eq!(other.mandate, 0);
    assert!(!other.restore(&snap[..8]));
}

#[test]
fn deg05_degraded_mode_asks_to_stop_after_its_budget() {
    let p = Params { degraded_ticks: 3, ..Params::default() };
    let mut a = Arb::new(&p);
    for t in 1..=4 {
        step(&mut a, t, &[]);
    }
    assert!(a.stop_requested);
    let out = step(&mut a, 5, &[]);
    assert_eq!(out.lamp, LAMP_RED);
    assert_eq!(out.cause, CAUSE_STOP_REQUESTED);
    assert!(out.stop_requested);
}
