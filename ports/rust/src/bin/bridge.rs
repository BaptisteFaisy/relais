//! Relais — the Rust arbiter behind the SAME JSON-lines protocol as
//! `host/difftest/bridge.c` (mission M32, idea 74).
//!
//! One command per line on stdin, one JSON object per line on stdout:
//!   {"cmd":"init","params":{...}}                     -> {"ok":true}
//!   {"cmd":"step","tick":N,"meas":{...},"msgs":[...]} -> {"tick":N,"state":{...},"out":{...}}
//!
//! Because the protocol of the bridge is identical, the driver of the C-vs-Rust
//! confrontation (`ports/rust/difftest/run_difftest.py`) can reuse the sequence
//! generators and the comparator of `host/difftest/difftest.py` unchanged: the
//! only thing swapped is the child process. The C binary stays the reference.
//!
//! This binary is a test harness, so it uses `std`. The library it drives does
//! not: `cargo build --no-default-features` builds the arbiter alone, `no_std`.

use std::io::{self, BufRead, Write};

use relais_arbiter::arbiter::{Arb, ArbOut};
use relais_arbiter::json::{self, JVal};
use relais_arbiter::params::Params;
use relais_arbiter::types::*;

fn f32_to_json(out: &mut String, v: f32) {
    /* Rust prints the shortest representation that round-trips, so the driver's
     * JSON reader gets exactly the f32 the C bridge would have printed. */
    out.push_str(&format!("{v}"));
}

fn f32_array(out: &mut String, a: &[f32]) {
    out.push('[');
    for (i, v) in a.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        f32_to_json(out, *v);
    }
    out.push(']');
}

fn u32_array(out: &mut String, a: &[u32]) {
    out.push('[');
    for (i, v) in a.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&v.to_string());
    }
    out.push(']');
}

fn u64_array(out: &mut String, a: &[u64]) {
    out.push('[');
    for (i, v) in a.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&v.to_string());
    }
    out.push(']');
}

fn bool_array(out: &mut String, a: &[bool]) {
    out.push('[');
    for (i, v) in a.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(if *v { "true" } else { "false" });
    }
    out.push(']');
}

/// One decoded frame, built exactly like `fill_msg` in the C bridge: a missing
/// field takes the value the C side would have given it, so the two children
/// are fed the same thing.
fn fill_msg(v: &JVal) -> Msg {
    let mut m = Msg {
        msg_type: v.num_or("type", 0.0) as u8,
        sender: v.num_or("sender", 0.0) as Node,
        flags: v.num_or("flags", 0.0) as u8,
        tick: v.num_or("tick", 0.0) as Tick,
        mandate: v.num_or("mandate", 0.0) as Mandate,
        value: v.num_or("value", 0.0) as f32,
        testament_len: v.num_or("testament_len", 0.0) as u8,
        reason: v.num_or("reason", 0.0) as u8,
        has_order: v.bool_or("has_order", false),
        ..Msg::default()
    };
    let arr = v.arr_or("testament");
    let n = arr.len().min(TESTAMENT_MAX);
    for (i, item) in arr.iter().take(n).enumerate() {
        if let JVal::Num(x) = item {
            m.testament[i] = *x as f32;
        }
    }
    if (n as u8) < m.testament_len {
        m.testament_len = n as u8;
    }
    if let Some(meas) = v.get("meas") {
        m.meas = Meas {
            pos: meas.num_or("pos", 0.0) as f32,
            vel: meas.num_or("vel", 0.0) as f32,
            applied: meas.num_or("applied", 0.0) as f32,
            setpoint: meas.num_or("setpoint", 0.0) as f32,
        };
    }
    m
}

fn params_from(root: &JVal) -> Params {
    let mut p = Params::default();
    let Some(pp) = root.get("params") else { return p };
    p.period_ms = pp.num_or("period_ms", f64::from(p.period_ms)) as u32;
    p.patience = pp.num_or("patience", f64::from(p.patience)) as u8;
    p.testament_len = pp.num_or("testament_len", f64::from(p.testament_len)) as u8;
    p.certify_ticks = pp.num_or("certify_ticks", f64::from(p.certify_ticks)) as u8;
    p.bail_ticks = pp.num_or("bail_ticks", f64::from(p.bail_ticks)) as u8;
    p.request_burst = pp.num_or("request_burst", f64::from(p.request_burst)) as u8;
    p.request_window = pp.num_or("request_window", f64::from(p.request_window)) as u16;
    p.epsilon = pp.num_or("epsilon", f64::from(p.epsilon)) as f32;
    p.delta = pp.num_or("delta", f64::from(p.delta)) as f32;
    p.degraded_ticks = pp.num_or("degraded_ticks", f64::from(p.degraded_ticks)) as u32;
    p.max_amplitude = pp.num_or("max_amplitude", f64::from(p.max_amplitude)) as f32;
    p.max_slew = pp.num_or("max_slew", f64::from(p.max_slew)) as f32;
    p
}

/// The whole arbiter state, with the same keys as `dump_state` in the C bridge:
/// the comparator walks both objects and reports any key present on one side
/// only, so the key set has to match, not just the values.
fn dump_state(out: &mut String, a: &Arb) {
    out.push_str(&format!(
        "\"mandate\":{},\"holder\":{},\"misses\":{},\"switch_pending\":{},\"certified\":{},\
         \"certified_node\":{},\"certify_streak\":{},\"shadow_misses\":{},\"last_order\":",
        a.mandate, a.holder, a.misses, a.switch_pending, a.certified, a.certified_node,
        a.certify_streak, a.shadow_misses
    ));
    f32_to_json(out, a.last_order);
    out.push_str(",\"last_shadow\":");
    f32_to_json(out, a.last_shadow);
    out.push_str(&format!(
        ",\"have_order\":{},\"have_shadow\":{},\"disagree_streak\":{},\"disagreement\":{},\"testament\":",
        a.have_order, a.have_shadow, a.disagree_streak, a.disagreement
    ));
    f32_array(out, &a.testament);
    out.push_str(&format!(
        ",\"testament_len\":{},\"testament_base\":{},\"fb_state\":",
        a.testament_len, a.testament_base
    ));
    f32_to_json(out, a.fb_state);
    out.push_str(&format!(",\"in_fallback\":{},\"last_heard\":", a.in_fallback));
    u64_array(out, &a.last_heard);
    out.push_str(",\"present\":");
    bool_array(out, &a.present);
    out.push_str(",\"ever_seen\":");
    bool_array(out, &a.ever_seen);
    out.push_str(&format!(
        ",\"lamp\":{},\"cause\":{},\"degraded_ticks\":{},\"stop_requested\":{},\
         \"cession_pending\":{},\"request_window_tick\":{},\"request_seen\":{},\"rejected\":",
        a.lamp, a.cause, a.degraded_ticks, a.stop_requested, a.cession_pending,
        a.request_window_tick, a.request_seen
    ));
    u32_array(out, &a.rejected);
    out.push_str(&format!(",\"forged\":{},\"applied_by_source\":", a.forged));
    u32_array(out, &a.applied_by_source);
    out.push_str(&format!(",\"ticks\":{},\"handovers\":{}", a.ticks, a.handovers));
}

fn dump_meas(out: &mut String, m: &Meas) {
    out.push_str("{\"pos\":");
    f32_to_json(out, m.pos);
    out.push_str(",\"vel\":");
    f32_to_json(out, m.vel);
    out.push_str(",\"applied\":");
    f32_to_json(out, m.applied);
    out.push_str(",\"setpoint\":");
    f32_to_json(out, m.setpoint);
    out.push('}');
}

/// One tick's decision, with the same keys as `dump_out` in the C bridge.
fn dump_out(out: &mut String, o: &ArbOut) {
    out.push_str("\"applied\":true,\"value\":");
    /* The shortest representation that round-trips the f32: closer to the exact
     * value the C bridge prints with %.9g than a fixed number of decimals. */
    f32_to_json(out, o.value);
    out.push_str(&format!(
        ",\"source\":{},\"mandate\":{},\"holder\":{},\"misses\":{},\
         \"certified\":{},\"certified_node\":{},\"lamp\":{},\"cause\":{},\"stop_requested\":{},\
         \"statut\":{{\"type\":{},\"sender\":{},\"tick\":{},\"mandate\":{},\"holder\":{},\
         \"source\":{},\"misses\":{},\"certified\":{},\"certified_node\":{},\"lamp\":{},\
         \"cause\":{},\"stop_requested\":{},\"meas\":",
        o.source, o.mandate, o.holder, o.misses, o.certified, o.certified_node,
        o.lamp, o.cause, o.stop_requested, o.statut.msg_type, o.statut.sender, o.statut.tick,
        o.statut.mandate, o.statut.holder, o.statut.source, o.statut.misses, o.statut.certified,
        o.statut.certified_node, o.statut.lamp, o.statut.cause, o.statut.stop_requested
    ));
    dump_meas(out, &o.statut.meas);
    out.push_str("},\"events\":[");
    for (i, e) in o.events.iter().take(o.event_count).enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"kind\":{},\"node\":{},\"mandate\":{},\"tick\":{},\"detail\":{}}}",
            e.kind, e.node, e.mandate, e.tick, e.detail
        ));
    }
    out.push(']'); /* handle_step closes the "out" object and the root */
}

fn handle_step(out: &mut String, root: &JVal, arb: &mut Arb) {
    let tick = root.num_or("tick", 0.0) as Tick;
    let mut meas = Meas::default();
    if let Some(mj) = root.get("meas") {
        meas = Meas {
            pos: mj.num_or("pos", 0.0) as f32,
            vel: mj.num_or("vel", 0.0) as f32,
            applied: mj.num_or("applied", 0.0) as f32,
            setpoint: mj.num_or("setpoint", 0.0) as f32,
        };
    }
    let msgs: [Msg; IN_MSG_MAX] = {
        let mut buf = [Msg::default(); IN_MSG_MAX];
        for (slot, item) in buf.iter_mut().zip(root.arr_or("msgs")) {
            *slot = fill_msg(item);
        }
        buf
    };
    let n = root.arr_or("msgs").len().min(IN_MSG_MAX);
    let mut result = ArbOut::default();
    arb.step(tick, &msgs[..n], &meas, &mut result);
    out.push_str(&format!("{{\"tick\":{tick},\"state\":{{"));
    dump_state(out, arb);
    out.push_str("},\"out\":{");
    dump_out(out, &result);
    out.push_str("}}");
}

fn main() {
    let mut params = Params::default();
    let mut arb = Arb::new(&params);
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut sink = stdout.lock();
    let mut line = String::new();
    let mut out = String::with_capacity(4096);
    loop {
        line.clear();
        let Ok(got) = stdin.lock().read_line(&mut line) else { break };
        if got == 0 {
            break; /* end of the driver's stream */
        }
        out.clear();
        match json::parse(&line) {
            Some(root) if root.cmd() == Some("init") => {
                params = params_from(&root);
                arb = Arb::new(&params);
                out.push_str("{\"ok\":true}");
            }
            Some(root) if root.cmd() == Some("step") => handle_step(&mut out, &root, &mut arb),
            _ => out.push_str("{\"error\":\"bad json or unknown cmd\"}"),
        }
        out.push('\n');
        if sink.write_all(out.as_bytes()).is_err() || sink.flush().is_err() {
            break;
        }
    }
}
