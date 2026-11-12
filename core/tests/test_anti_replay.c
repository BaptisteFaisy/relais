/* Relais — unit tests of the sliding-window anti-replay (SEC-03, idea 33).
 *
 * What is proved here:
 *   SEC-03  a frame already accepted is refused the second time, and a frame
 *           whose tick fell out of the window is refused even though nothing
 *           remembers it any more;
 *   ROB-02  the duplicated frame of a lossy link, which carries the very tick
 *           being applied, is told apart from the original;
 *   DET-04  the refusals are counted, so a replay attempt is observable;
 *   the verdict does not depend on the order frames arrive in, as long as they
 *   stay inside the window: a late frame is accepted, then remembered.
 */
#include "test_util.h"

#include <string.h>

#include "relais/anti_replay.h"

/* The window is 64 ticks wide by default, so 63 late ticks are still inside. */
void test_sec03_exact_replay_is_refused(void) {
  t_section("SEC-03 the same tick twice is a replay");
  relais_replay w;
  relais_replay_init(&w, RELAIS_REPLAY_WINDOW);
  T_CHECK(!w.started);
  T_EQ_U(w.bits, RELAIS_REPLAY_WINDOW);

  T_CHECK(relais_replay_accept(&w, 100));
  T_CHECK(w.started);
  T_EQ_U(w.highest, 100u);
  T_CHECK(!relais_replay_accept(&w, 100)); /* the exact replay */
  T_CHECK(!relais_replay_accept(&w, 100)); /* and again, and again */
  T_EQ_U(w.accepted, 1u);
  T_EQ_U(w.rejected_replay, 2u);
  T_EQ_U(w.rejected_old, 0u);
  T_CHECK(!relais_replay_accept(NULL, 100));
  T_CHECK(!relais_replay_would_accept(NULL, 100));
}

/* A frame that arrives late but inside the window is legitimate: the link may
 * reorder, and the protocol must not turn that into a lost tick. */
void test_sec03_disorder_inside_the_window_is_accepted(void) {
  t_section("SEC-03 out of order inside the window is accepted");
  relais_replay w;
  relais_replay_init(&w, RELAIS_REPLAY_WINDOW);
  T_CHECK(relais_replay_accept(&w, 500));
  T_CHECK(relais_replay_accept(&w, 502));
  T_CHECK(relais_replay_accept(&w, 501)); /* late by one tick: inside */
  T_CHECK(relais_replay_accept(&w, 499));
  T_EQ_U(w.accepted, 4u);
  T_EQ_U(w.highest, 502u);
  T_EQ_U(w.rejected_replay, 0u);
  /* The edge of the window: 63 ticks behind the highest is the last one in. */
  T_CHECK(relais_replay_would_accept(&w, 502u - (RELAIS_REPLAY_WINDOW - 1u)));
  T_CHECK(relais_replay_accept(&w, 502u - (RELAIS_REPLAY_WINDOW - 1u)));
  T_CHECK(!relais_replay_would_accept(&w, 502u - RELAIS_REPLAY_WINDOW));
  /* would_accept does not record: the tick it just accepted is still free. */
  T_CHECK(relais_replay_would_accept(&w, 503u));
  T_CHECK(relais_replay_would_accept(&w, 503u));
  T_EQ_U(w.accepted, 5u);
}

void test_sec03_too_old_is_refused_and_the_window_slides(void) {
  t_section("SEC-03 a tick out of the window is refused");
  relais_replay w;
  relais_replay_init(&w, RELAIS_REPLAY_WINDOW);
  T_CHECK(relais_replay_accept(&w, 1000));
  T_CHECK(relais_replay_accept(&w, 999));
  T_CHECK(!relais_replay_accept(&w, 1000u - RELAIS_REPLAY_WINDOW)); /* one too far */
  T_EQ_U(w.rejected_old, 1u);
  T_CHECK(!relais_replay_accept(&w, 0));    /* ancient history */
  T_CHECK(!relais_replay_accept(&w, 900));
  T_EQ_U(w.rejected_old, 3u);
  T_EQ_U(w.highest, 1000u); /* a refusal never moves the window */

  /* A jump ahead clears what fell behind: the window slides, it does not grow. */
  T_CHECK(relais_replay_accept(&w, 1000u + RELAIS_REPLAY_WINDOW));
  T_EQ_U(w.highest, 1000u + RELAIS_REPLAY_WINDOW);
  T_CHECK(!relais_replay_accept(&w, 1000)); /* it was inside, it is not any more */
  T_EQ_U(w.rejected_old, 4u);
  T_CHECK(relais_replay_accept(&w, 1000u + RELAIS_REPLAY_WINDOW - 1u));
  T_EQ_U(w.accepted, 4u);

  /* A window of one tick accepts nothing but the future. */
  relais_replay narrow;
  relais_replay_init(&narrow, 1u);
  T_EQ_U(narrow.bits, 1u);
  T_CHECK(relais_replay_accept(&narrow, 7));
  T_CHECK(!relais_replay_accept(&narrow, 6));
  T_CHECK(relais_replay_accept(&narrow, 8));
  T_CHECK(!relais_replay_accept(&narrow, 7));

  /* The width is clamped to what the bitmap can hold; a reset forgets all. */
  relais_replay clamped;
  relais_replay_init(&clamped, 0u);
  T_EQ_U(clamped.bits, RELAIS_REPLAY_WINDOW);
  relais_replay_init(&clamped, 4096u);
  T_EQ_U(clamped.bits, RELAIS_REPLAY_WINDOW);
  T_CHECK(relais_replay_accept(&clamped, 42));
  relais_replay_reset(&clamped);
  T_CHECK(!clamped.started);
  T_EQ_U(clamped.accepted, 0u);
  T_EQ_U(clamped.bits, RELAIS_REPLAY_WINDOW);
  T_CHECK(relais_replay_accept(&clamped, 42)); /* the same tick is new again */
}

/* One window per emitter: what A sends says nothing about what B may send. */
void test_sec03_one_window_per_emitter(void) {
  t_section("SEC-03 a window per emitter");
  relais_replay_set set;
  relais_replay_set_init(&set, RELAIS_REPLAY_WINDOW);
  T_EQ_U(set.bits, RELAIS_REPLAY_WINDOW);
  T_CHECK(relais_replay_set_slot(&set, RELAIS_NODE_A) != NULL);
  T_CHECK(relais_replay_set_slot(&set, RELAIS_NODE_ACTUATOR) != NULL);
  T_CHECK(relais_replay_set_slot(&set, (relais_node)RELAIS_REPLAY_SLOTS) == NULL);
  T_CHECK(relais_replay_set_slot(NULL, RELAIS_NODE_A) == NULL);

  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_A, 10));
  T_CHECK(!relais_replay_set_accept(&set, RELAIS_NODE_A, 10)); /* A replayed */
  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_B, 10));  /* B is untouched */
  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_ACTUATOR, 10));
  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_A, 11));
  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_B, 9));   /* B is late, inside */
  T_CHECK(!relais_replay_set_accept(&set, RELAIS_NODE_B, 9));  /* but only once */
  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_B, 400));
  T_CHECK(!relais_replay_set_accept(&set, RELAIS_NODE_B, 300)); /* out of B window */
  T_CHECK(!relais_replay_set_accept(&set, RELAIS_NODE_NONE, 12)); /* nobody emits */
  T_CHECK(relais_replay_set_slot(&set, RELAIS_NODE_NONE) == NULL);
  T_CHECK(!relais_replay_set_accept(&set, (relais_node)RELAIS_REPLAY_SLOTS, 12));
  T_EQ_U(relais_replay_set_slot(&set, RELAIS_NODE_A)->accepted, 2u);
  T_EQ_U(relais_replay_set_slot(&set, RELAIS_NODE_B)->rejected_replay, 1u);
  T_EQ_U(relais_replay_set_slot(&set, RELAIS_NODE_B)->rejected_old, 1u);
}

/* ROB-02: the link duplicates frames. Both copies carry the tick being applied,
 * so only the window can tell the second one from the first. */
void test_rob02_duplicated_frame_is_told_apart(void) {
  t_section("ROB-02 a duplicated frame is refused");
  relais_replay_set set;
  relais_replay_set_init(&set, RELAIS_REPLAY_WINDOW);
  relais_params p;
  relais_params_default(&p);
  relais_msg order;
  relais_key key;
  T_CHECK(relais_key_from_hex(&key, "000102030405060708090a0b0c0d0e0f"));
  uint8_t buf[RELAIS_FRAME_MAX];
  t_order(&order, RELAIS_NODE_A, 1u, 77u, 0.5f, &p);
  const size_t n = relais_frame_encode(&order, &key, buf, sizeof buf);
  T_CHECK(n > 0);
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(relais_frame_decode(buf, n, &key, &out, &why));
  T_EQ_U(out.tick, 77u);
  /* The same bytes arrive a second time in the same tick: the frame is valid,
   * its control is right, and it is still a replay. */
  T_CHECK(relais_replay_set_accept(&set, out.sender, out.tick));
  T_CHECK(!relais_replay_set_accept(&set, out.sender, out.tick));
  T_EQ_U(relais_replay_set_slot(&set, RELAIS_NODE_A)->rejected_replay, 1u);
  /* A captured frame replayed three periods later is out of the window. */
  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_A, 78u));
  T_CHECK(relais_replay_set_accept(&set, RELAIS_NODE_A, 200u));
  T_CHECK(!relais_replay_set_accept(&set, RELAIS_NODE_A, 77u));
  T_EQ_U(relais_replay_set_slot(&set, RELAIS_NODE_A)->rejected_old, 1u);
}

/* The verdict of the window against an independent model: a plain table of what
 * was accepted. Same question, two different answers paths, 20000 draws. */
void test_sec03_window_matches_a_reference_model(void) {
  t_section("SEC-03 20000 draws against a reference model");
  enum { MODEL_SPAN = 1 << 16 }; /* the walk must not saturate */
  static bool seen[MODEL_SPAN];
  relais_replay w;
  relais_replay_init(&w, RELAIS_REPLAY_WINDOW);
  memset(seen, 0, sizeof seen);
  t_rng rng;
  t_rng_seed(&rng, 0xABCDEF01u);
  relais_tick highest = 0;
  bool started = false;
  long mismatches = 0;
  for (long i = 0; i < 20000; i++) {
    /* A stream that goes forward, stalls, and repeats: what a real link does. */
    relais_tick tick = started ? highest : 1u;
    const uint32_t draw = t_rng_below(&rng, 10u);
    if (draw < 5u) tick = highest + 1u + t_rng_below(&rng, 3u);
    else if (draw < 8u) tick = (highest > t_rng_below(&rng, 70u))
                                   ? highest - t_rng_below(&rng, 70u)
                                   : 0u;
    else tick = highest;
    if (tick >= MODEL_SPAN) tick = MODEL_SPAN - 1u;
    bool expected = true;
    if (started) {
      const bool inside = (tick > highest) || (highest - tick < RELAIS_REPLAY_WINDOW);
      expected = inside && !seen[tick];
    }
    const bool got = relais_replay_accept(&w, tick);
    if (got != expected) mismatches++;
    if (got) {
      seen[tick] = true;
      if (!started || tick > highest) highest = tick;
      started = true;
    }
  }
  T_EQ_U((uint64_t)mismatches, 0u);
  T_EQ_U(w.highest, highest);
  T_EQ_U((uint64_t)(w.accepted + w.rejected_replay + w.rejected_old), 20000u);
}

int main(void) {
  test_sec03_exact_replay_is_refused();
  test_sec03_disorder_inside_the_window_is_accepted();
  test_sec03_too_old_is_refused_and_the_window_slides();
  test_sec03_one_window_per_emitter();
  test_rob02_duplicated_frame_is_told_apart();
  test_sec03_window_matches_a_reference_model();
  return t_summary();
}
