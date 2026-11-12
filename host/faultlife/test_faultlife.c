/* Relais — lifecycle and time fault tests (mission M19).
 *
 * Five families, one property each, expressed and measured:
 *   13 clock skew  -> the actuator's decision never depends on a machine clock;
 *                     only the node's bail diagnostic (MAN-09) moves.
 *   14 power loss  -> a truncated frame is rejected (DET-04) and never applied;
 *                     a brutal loss mid-send still hands over inside p+2.
 *   15 boot loop   -> the mandate does not oscillate with the rebooting machine
 *                     (DEG-07); it converges on the stable peer.
 *   19 demand storm-> the holder's orders keep being served every tick (SEC-06),
 *                     no queue overflow, rate limit counts refused vs granted.
 *   20 OTA broken  -> interrupted at every step, the outcome is always a foreseen
 *                     state, never a double command, never a lost mandate with a
 *                     live machine.
 */
#include "faultlife.h"

#include <stdio.h>
#include <string.h>

#include "test_util.h"

#define TICKS 120
#define TRACE_MAX 240

typedef struct {
  relais_mandate mandate[TRACE_MAX];
  relais_node holder[TRACE_MAX];
  relais_source source[TRACE_MAX];
  uint32_t n;
} trace_t;

static void world_pair(lw_t *w, lw_t *twin) {
  relais_params p;
  relais_traj t;
  relais_params_default(&p);
  relais_traj_default(&t);
  lw_init(w, &p, &t);
  if (twin != NULL) lw_init(twin, &p, &t);
}

/* 13: run the clock-skew scenario and record the actuator's decision trace plus
 * the tick at which the deaf holder first reports "without actuator" (bail). */
static uint32_t run_clock(int wd_num, int wd_den, uint32_t jump_at, trace_t *tr,
                          bool *bailed) {
  lw_t w;
  world_pair(&w, NULL);
  lw_set_watchdog(&w, 0, wd_num, wd_den);
  if (jump_at != 0) lw_watchdog_jump(&w, 0, jump_at);
  if (tr != NULL) tr->n = 0;
  uint32_t bail_tick = 0;
  for (uint32_t tick = 1; tick <= 90; tick++) {
    if (tick == 41) w.deaf[0] = true; /* the holder stops hearing the actuator */
    if (tick == 71) w.deaf[0] = false;
    relais_arb_out out;
    lw_update(&w, tick, &out);
    if (tr != NULL && tr->n < TRACE_MAX) {
      tr->mandate[tr->n] = out.mandate;
      tr->holder[tr->n] = out.holder;
      tr->source[tr->n] = out.source;
      tr->n++;
    }
    if (bailed != NULL && w.node[0].without_actuator && bail_tick == 0) {
      bail_tick = tick;
      *bailed = true;
    }
  }
  return bail_tick;
}

static bool trace_equal(const trace_t *a, const trace_t *b) {
  if (a->n != b->n) return false;
  for (uint32_t i = 0; i < a->n; i++)
    if (a->mandate[i] != b->mandate[i] || a->holder[i] != b->holder[i] ||
        a->source[i] != b->source[i])
      return false;
  return true;
}

static void print_clock_table(uint32_t fast, uint32_t base, uint32_t jump, uint32_t slow,
                              uint32_t handover, uint32_t n) {
  printf("   clock   bail(diagnostic)  decision(handover)\n");
  printf("   2x      tick %-3u          tick %u\n", fast, handover);
  printf("   nominal tick %-3u          tick %u\n", base, handover);
  printf("   jump    tick %-3u          tick %u\n", jump, handover);
  printf("   0.5x    tick %-3u          tick %u\n", slow, handover);
  printf("   -> 4 scenarios, 0 diverging decision ticks over %u ticks\n", n);
}

static void test_clock_skew(void) {
  t_section("13 clock skew: the actuator never reads a machine clock");
  trace_t base;
  bool base_bailed = false;
  uint32_t base_bail = run_clock(1, 1, 0, &base, &base_bailed);

  /* A 2x clock, a 0.5x clock and a clock that jumps after a gel: the decision
   * trace must be identical, only the bail diagnostic moves. */
  trace_t fast;
  bool fast_bailed = false;
  uint32_t fast_bail = run_clock(2, 1, 0, &fast, &fast_bailed);
  trace_t slow;
  bool slow_bailed = false;
  uint32_t slow_bail = run_clock(1, 2, 0, &slow, &slow_bailed);
  trace_t jump;
  bool jump_bailed = false;
  uint32_t jump_bail = run_clock(1, 1, 43, &jump, &jump_bailed);

  T_CHECK(trace_equal(&base, &fast));
  T_CHECK(trace_equal(&base, &slow));
  T_CHECK(trace_equal(&base, &jump));

  /* The handover happens on the actuator's own evidence at the same tick in
   * every scenario, long before (or after) the node's bail fires. */
  uint32_t handover = 0;
  for (uint32_t i = 0; i < base.n; i++) {
    if (base.holder[i] == RELAIS_NODE_B) {
      handover = i + 1;
      break;
    }
  }
  T_MSG(handover == 43, "handover driven by the actuator, not by the clock");
  T_CHECK(base.holder[40] == RELAIS_NODE_A);
  T_CHECK(base.source[40] == RELAIS_SRC_HOLDER);

  /* The bail diagnostic genuinely differs: 2x early, 0.5x late, jump immediate. */
  T_CHECK(fast_bailed && slow_bailed && jump_bailed && base_bailed);
  T_CHECK(fast_bail < base_bail);
  T_CHECK(slow_bail > base_bail);
  T_CHECK(jump_bail < base_bail);
  print_clock_table(fast_bail, base_bail, jump_bail, slow_bail, handover, base.n);

  /* The arbiter is a pure function of (tick, frames): two identical runs agree. */
  trace_t again;
  run_clock(1, 1, 0, &again, NULL);
  T_CHECK(trace_equal(&base, &again));
}

static void test_frame_truncation(void) {
  t_section("14 power loss: a truncated frame is rejected, never applied");
  relais_params p;
  relais_params_default(&p);
  relais_key key;
  relais_key_clear(&key); /* level S: CRC-32C trailer */

  relais_msg order;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  t_order(&order, RELAIS_NODE_A, 1, 100, 0.25f, &p);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t full = relais_frame_encode(&order, &key, buf, sizeof buf);
  T_CHECK(full > RELAIS_HEADER_LEN + RELAIS_CHECK_LEN);

  /* Every prefix shorter than the encoded frame must fail to decode (DET-04). */
  uint32_t rejected = 0;
  for (size_t len = 0; len < full; len++) {
    relais_msg got;
    relais_reject why = RELAIS_REJ_NONE;
    if (!relais_frame_decode(buf, len, &key, &got, &why)) {
      rejected++;
      T_CHECK(why != RELAIS_REJ_NONE);
    } else {
      T_MSG(false, "a truncated frame decoded");
    }
  }
  T_EQ_U(rejected, (uint64_t)full);

  /* The whole frame still decodes and its value is intact. */
  relais_msg got;
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(relais_frame_decode(buf, full, &key, &got, &why));
  T_CHECK(got.type == RELAIS_MSG_ORDRE);
  T_NEAR(got.value, 0.25f, 1e-6);
  printf("   truncated prefixes rejected: %u/%zu (full frame %zu bytes decodes)\n",
         rejected, full, full);
}

/* Feed one tick of the faulted world and its twin value to the invariant
 * checker (the same one the explorer and the simulator use). */
static void feed_inv(relais_inv *inv, const relais_arb_out *out, uint32_t tick,
                     float twin_value) {
  relais_jr_tick r;
  memset(&r, 0, sizeof r);
  r.tick = tick;
  r.mandate = out->mandate;
  r.holder = out->holder;
  r.source = out->source;
  r.value = out->value;
  r.misses = out->misses;
  r.lamp = out->lamp;
  r.cause = out->cause;
  r.certified = out->certified;
  r.certified_node = out->certified_node;
  for (uint8_t e = 0; e < out->event_count; e++) {
    relais_jr_event ev;
    memset(&ev, 0, sizeof ev);
    ev.tick = tick;
    ev.kind = out->events[e].kind;
    ev.node = out->events[e].node;
    ev.mandate = out->events[e].mandate;
    ev.detail = out->events[e].detail;
    relais_inv_on_event(inv, &ev);
  }
  relais_inv_on_tick(inv, &r, &twin_value);
  relais_inv_end_tick(inv, tick);
}

/* 14: the holder dies mid-send; the in-flight frame is partial. Handover must
 * still land inside p+2 and never double-command. */
static void test_power_loss_mid_send(void) {
  t_section("14 power loss mid-send: handover inside p+2, no double command");
  lw_t w;
  lw_t twin;
  world_pair(&w, &twin);

  relais_inv inv;
  relais_params p;
  relais_params_default(&p);
  relais_inv_init(&inv, &p);
  relais_inv_expect_certified(&inv, true);
  relais_inv_fault_free(&inv, false);

  uint32_t loss_tick = 40;
  uint32_t handover = 0;
  for (uint32_t tick = 1; tick <= 80; tick++) {
    if (tick == loss_tick) lw_power_loss(&w, 0, true);
    relais_arb_out out;
    relais_arb_out tout;
    lw_update(&w, tick, &out);
    lw_update(&twin, tick, &tout);
    if (out.holder == RELAIS_NODE_B && handover == 0) handover = tick;
    feed_inv(&inv, &out, tick, tout.value);
  }
  T_CHECK(w.truncated_frames >= 1);
  T_MSG(handover > 0 && handover <= loss_tick + p.patience + 2,
        "handover within p+2 of the brutal loss");
  T_CHECK(relais_inv_ok(&inv));
  char rep[RELAIS_INV_MSG_MAX * 2];
  relais_inv_report(&inv, rep, sizeof rep);
  printf("   loss at %u, handover at %u (bound p+2=%u); %s\n", loss_tick, handover,
         p.patience + 2, rep);
}

/* 15: boot loop. Measure mandate changes over 1000 ticks for k = 2..50. */
static uint32_t boot_loop_changes(uint32_t k, bool holder_loops, relais_node *final_holder) {
  lw_t w;
  world_pair(&w, NULL);
  /* Let the system settle: A holds, B is the certified second. */
  for (uint32_t tick = 1; tick <= 30; tick++) {
    relais_arb_out out;
    lw_update(&w, tick, &out);
  }
  uint32_t changes_before = w.mandate_changes;
  lw_set_boot_period(&w, holder_loops ? 0 : 1, k);
  for (uint32_t tick = 31; tick <= 1030; tick++) {
    relais_arb_out out;
    lw_update(&w, tick, &out);
  }
  *final_holder = w.last_holder;
  return w.mandate_changes - changes_before;
}

static void test_boot_loop(void) {
  t_section("15 boot loop: the mandate does not oscillate with the machine");
  printf("   k   changes(second loops)  changes(holder loops)  final holder\n");
  for (uint32_t k = 2; k <= 50; k += (k < 10 ? 1 : 8)) {
    relais_node fh1;
    relais_node fh2;
    uint32_t c_second = boot_loop_changes(k, false, &fh1);
    uint32_t c_holder = boot_loop_changes(k, true, &fh2);
    /* DEG-07: the looping machine never re-certifies, so the mandate must stay
     * bounded (a handful of changes), never one per reboot. */
    T_CHECK(c_second <= 4);
    T_CHECK(c_holder <= 4);
    T_CHECK(fh1 == RELAIS_NODE_A); /* second loops -> the holder A keeps commanding */
    T_CHECK(fh2 == RELAIS_NODE_B); /* holder loops -> the peer B takes over, stays */
    printf("   %-3u %-22u %-20u second:%s holder:%s\n", k, c_second, c_holder,
           relais_node_name(fh1), relais_node_name(fh2));
  }
}

/* 19: demand storm. Both machines demand every tick; the holder must keep being
 * served, the queue must not overflow, and SEC-06 counts refused vs granted. */
static void test_demand_storm(void) {
  t_section("19 demand storm: orders still served, rate limit holds, no overflow");
  lw_t w;
  lw_t twin;
  world_pair(&w, &twin);
  /* Settle, then both machines storm demandes from tick 30 on. */
  for (uint32_t tick = 1; tick <= 30; tick++) {
    relais_arb_out out;
    relais_arb_out tout;
    lw_update(&w, tick, &out);
    lw_update(&twin, tick, &tout);
  }
  lw_set_storm(&w, 0, true);
  lw_set_storm(&w, 1, true);

  uint32_t served = 0;
  uint32_t not_served = 0;
  for (uint32_t tick = 31; tick <= 530; tick++) {
    relais_arb_out out;
    lw_update(&w, tick, &out);
    if (out.source == RELAIS_SRC_HOLDER) served++;
    else not_served++;
  }
  /* The storm must not delay or displace the holder's orders. */
  T_EQ_U(not_served, 0);
  T_CHECK(served == 500);
  T_EQ_U(w.queue_drops, 0);
  T_CHECK(w.denied > w.granted);
  printf("   orders served during storm: %u/500, not served: %u\n", served, not_served);
  printf("   demandes granted=%u refused=%u (rate-limited=%u) queue_drops=%u\n",
         w.granted, w.denied, w.rate_limited, w.queue_drops);
}

/* 20: OTA interrupted. Enumerate the steps, inject a power loss at each, and
 * require a foreseen outcome every time. */
typedef struct {
  const char *name;
  uint32_t at;    /* tick of the interruption */
  bool after_cede; /* the cession was already sent */
} ota_step_t;

static void check_ota_outcome(lw_t *w, const relais_arb_out *last, const char *step) {
  /* Never a double command: the holder field names at most one machine. */
  T_CHECK(last->holder == RELAIS_NODE_NONE || relais_node_is_computer(last->holder));
  /* Never a lost mandate with a live, certified machine that could command. */
  bool live = !w->dead[0] || !w->dead[1];
  if (last->holder == RELAIS_NODE_NONE) {
    /* vacant is a foreseen safe state only when nobody is certified to take it */
    T_MSG(!live || !w->arb.certified, step);
  }
}

static void run_ota(const ota_step_t *st) {
  lw_t w;
  world_pair(&w, NULL);
  for (uint32_t tick = 1; tick <= 30; tick++) {
    relais_arb_out out;
    lw_update(&w, tick, &out);
  }
  relais_arb_out last;
  relais_mandate prev_mandate = 0;
  for (uint32_t tick = 31; tick <= 120; tick++) {
    if (tick == st->at) {
      if (st->after_cede) lw_ota_cede(&w, 0);
      lw_power_loss(&w, 0, true); /* the update is interrupted mid-send */
    }
    lw_update(&w, tick, &last);
    /* I1/I2/I3 per tick: one holder at most, no order without a holder, and the
     * mandate number never goes backwards (no double command, ever). */
    T_CHECK(last.holder == RELAIS_NODE_NONE || relais_node_is_computer(last.holder));
    if (last.source == RELAIS_SRC_HOLDER) T_CHECK(last.holder != RELAIS_NODE_NONE);
    T_CHECK(last.mandate >= prev_mandate);
    prev_mandate = last.mandate;
  }
  check_ota_outcome(&w, &last, st->name);
  /* The system converges on a foreseen state: B commands, or the safe state. */
  T_MSG(last.holder == RELAIS_NODE_B || last.holder == RELAIS_NODE_NONE ||
            last.source == RELAIS_SRC_FALLBACK,
        st->name);
  printf("   OTA step '%-22s' -> holder=%s source=%s mandate=%u\n", st->name,
         relais_node_name(last.holder), relais_source_name(last.source),
         (unsigned)last.mandate);
}

static void test_ota_interrupted(void) {
  t_section("20 OTA interrupted at every step: always a foreseen state");
  static const ota_step_t steps[] = {
      {"1 before cession", 31, false},
      {"2 cession sent", 33, true},
      {"3 shutdown", 40, true},
      {"4 restart", 60, true},
      {"5 reintegration", 90, true},
  };
  for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) run_ota(&steps[i]);
}

int main(void) {
  test_clock_skew();
  test_frame_truncation();
  test_power_loss_mid_send();
  test_boot_loop();
  test_demand_storm();
  test_ota_interrupted();
  return t_summary();
}