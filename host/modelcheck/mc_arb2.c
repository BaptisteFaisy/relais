/* Relais — space B, the enumerations (mission M36). See mc_arb.c for the
 * states, the oracle and the property checks. Every case runs the real
 * relais_arb_step() of core/src/arbiter.c. */
#include "mc.h"

#include <math.h>
#include <string.h>

#include "relais/arbiter.h"
#include "relais/check.h"
#include "relais/frame.h"

static const int64_t TICK_OFFSET[MC_B_TICK_OFFSETS] = {-1, 0, 1, 2, 3, 8, 64};
static const int64_t PAIR_OFFSET[MC_B_PAIR_TICKS] = {-1, 0, 1, 2, 8};
static const relais_node SENDERS[MC_B_SENDERS] = {RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_NODE_C,
                                                  RELAIS_NODE_ACTUATOR};
static const relais_msg_type TYPES[MC_B_TYPES] = {RELAIS_MSG_STATUT, RELAIS_MSG_ORDRE,
                                                  RELAIS_MSG_OMBRE, RELAIS_MSG_DEMANDE,
                                                  RELAIS_MSG_CESSION, RELAIS_MSG_ETAT,
                                                  RELAIS_MSG_SUIVI};
static const float VALUES[MC_B_VALUES] = {-2.0f, -1.0f, -0.004f, 0.0f, 0.004f,
                                          1.0f,  2.0f,  NAN,     INFINITY};

static relais_tick offset_tick(relais_tick base, int64_t off) {
  return off < 0 ? base - (relais_tick)(-off) : base + (relais_tick)off;
}

static relais_mandate mandate_axis(const mc_state *st, unsigned mi) {
  switch (mi) {
    case 0: return st->arb.mandate - 1u;
    case 1: return st->arb.mandate;
    case 2: return st->arb.mandate + 1u;
    case 3: return 0;
    default: return (relais_mandate)UINT32_MAX;
  }
}

/* One frame of the enumeration: every field comes from an axis. */
static void build_frame(relais_msg *m, relais_msg_type type, relais_node sender, relais_tick tick,
                        relais_mandate mandate, float value) {
  relais_msg_clear(m);
  m->type = type;
  m->sender = sender;
  m->tick = tick;
  m->mandate = mandate;
  m->value = value;
  for (uint8_t i = 0; i < RELAIS_TESTAMENT_MAX; i++)
    m->testament[i] = value + (float)i * 0.01f;
  switch (type) {
    case RELAIS_MSG_ORDRE:
      m->has_order = true;
      m->testament_len = RELAIS_TESTAMENT_MAX;
      break;
    case RELAIS_MSG_OMBRE: m->replica_age = 1; break;
    case RELAIS_MSG_DEMANDE:
      m->seen_mandate = mandate;
      m->reason = 1;
      m->has_order = true;
      m->flags |= RELAIS_FLAG_HAS_ORDER;
      m->testament_len = RELAIS_TESTAMENT_MAX;
      break;
    case RELAIS_MSG_CESSION: m->reason = 2; m->flags |= RELAIS_FLAG_CESSION; break;
    case RELAIS_MSG_ETAT: m->ctl_state_len = 0; m->last_value = value; break;
    case RELAIS_MSG_SUIVI: m->ready = true; m->replica_age = 2; break;
    default: break;
  }
}

static relais_node other_computer(relais_node holder) {
  return holder == RELAIS_NODE_A ? RELAIS_NODE_B : RELAIS_NODE_A;
}

/* The frame vocabulary, built for one state and one tick. The first entries
 * are the ones that change the arbiter's decision. */
static size_t build_vocab(relais_msg *vocab, const mc_state *st, relais_tick tick, size_t cap) {
  relais_mandate m = st->arb.mandate;
  relais_node holder = st->arb.holder;
  /* The frames the arbiter sees are decoded frames: their sender is always a
   * computer or the actuator (space A proves the decoder rejects any other).
   * When the mandate is vacant there is no holder to speak, so the vocabulary
   * uses a computer that is not the holder. */
  if (holder == RELAIS_NODE_NONE) holder = RELAIS_NODE_A;
  relais_node second = other_computer(holder);
  size_t n = 0;
  build_frame(&vocab[n++], RELAIS_MSG_ORDRE, holder, tick, m, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_ORDRE, holder, offset_tick(tick, -1), m, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_ORDRE, holder, tick, m - 1u, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_ORDRE, holder, tick, m + 1u, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_ORDRE, other_computer(holder), tick, m, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_OMBRE, second, tick, m, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_OMBRE, second, tick, m, 0.75f);
  build_frame(&vocab[n++], RELAIS_MSG_OMBRE, second, offset_tick(tick, -1), m, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_CESSION, holder, tick, m, 0.0f);
  build_frame(&vocab[n++], RELAIS_MSG_DEMANDE, other_computer(holder), tick, m, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_DEMANDE, holder, tick, m, 0.25f);
  build_frame(&vocab[n++], RELAIS_MSG_STATUT, RELAIS_NODE_ACTUATOR, tick, m, 0.0f);
  if (n < cap) build_frame(&vocab[n++], RELAIS_MSG_OMBRE, RELAIS_NODE_C, tick, m, 0.25f);
  if (n < cap) build_frame(&vocab[n++], RELAIS_MSG_CESSION, other_computer(holder), tick, m, 0.0f);
  if (n < cap) build_frame(&vocab[n++], RELAIS_MSG_DEMANDE, other_computer(holder), tick, m, 0.0f);
  if (n < cap) build_frame(&vocab[n++], RELAIS_MSG_SUIVI, second, tick, m, 0.0f);
  return n;
}

/* B1: the complete cross product state x tick x type x sender x tick label x
 * mandate x value, one frame per tick. */
static void b1_single(mc_space *sp, const mc_state *states) {
  for (unsigned s = 0; s < MC_B_STATES; s++) {
    for (unsigned d = 0; d < MC_B_TICK_OFFSETS; d++) {
      relais_tick tick = offset_tick(states[s].next, TICK_OFFSET[d]);
      for (unsigned ty = 0; ty < MC_B_TYPES; ty++) {
        for (unsigned se = 0; se < MC_B_SENDERS; se++) {
          for (unsigned ft = 0; ft < MC_B_FRAME_TICKS; ft++) {
            for (unsigned mi = 0; mi < MC_B_MANDATES; mi++) {
              for (unsigned vi = 0; vi < MC_B_VALUES; vi++) {
                relais_msg m;
                build_frame(&m, TYPES[ty], SENDERS[se], offset_tick(tick, (int64_t)ft - 1),
                            mandate_axis(&states[s], mi), VALUES[vi]);
                mc_case_set("B1 state=%u off=%d type=%u sender=%u ftick=%u mandate=%u val=%u "
                            "tick=%llu holder=%d M=%u",
                            s, (int)TICK_OFFSET[d], ty, se, ft, mi, vi,
                            (unsigned long long)tick, (int)states[s].arb.holder,
                            states[s].arb.mandate);
                mc_run_case(sp, &states[s].arb, &m, 1, tick, true, NULL, NULL);
              }
            }
          }
        }
      }
    }
  }
}

/* B2: every ordered pair of the vocabulary, plus every single frame and the
 * empty set, for every state and every published tick offset. */
void mc_arb_pairs(mc_space *sp, const mc_state *states) {
  for (unsigned s = 0; s < MC_B_STATES; s++) {
    for (unsigned d = 0; d < MC_B_PAIR_TICKS; d++) {
      relais_tick tick = offset_tick(states[s].next, PAIR_OFFSET[d]);
      mc_state cur;
      cur.arb = states[s].arb;
      cur.next = tick;
      relais_msg vocab[MC_B_PAIR_VOCAB];
      relais_msg pair[2];
      size_t vn = build_vocab(vocab, &cur, tick, MC_B_PAIR_VOCAB);
      mc_case_set("B2 state=%u off=%d frames=none tick=%llu holder=%d M=%u", s,
                  (int)PAIR_OFFSET[d], (unsigned long long)tick, (int)states[s].arb.holder,
                  states[s].arb.mandate);
      mc_run_case(sp, &states[s].arb, NULL, 0, tick, true, NULL, NULL);
      for (size_t i = 0; i < vn; i++) {
        mc_case_set("B2 state=%u off=%d frames=1 tick=%llu", s, (int)PAIR_OFFSET[d],
                    (unsigned long long)tick);
        mc_run_case(sp, &states[s].arb, &vocab[i], 1, tick, true, NULL, NULL);
        for (size_t j = 0; j < vn; j++) {
          mc_case_set("B2 state=%u off=%d frames=2 i=%zu j=%zu tick=%llu", s,
                      (int)PAIR_OFFSET[d], i, j, (unsigned long long)tick);
          /* the two frames of the period, in the order the channel delivered
           * them: the first one is scanned first. */
          pair[0] = vocab[i];
          pair[1] = vocab[j];
          mc_run_case(sp, &states[s].arb, pair, 2, tick, true, NULL, NULL);
        }
      }
    }
  }
}

/* B3: every sequence of up to MC_B_SEQ_DEPTH frames, one tick each. The
 * vocabulary is rebuilt at every level from the state the sequence produced. */
static void b3_walk(mc_space *sp, const mc_state *state, unsigned depth, relais_tick tick) {
  mc_state cur;
  cur.arb = state->arb;
  cur.next = tick;
  relais_msg vocab[MC_B_SEQ_VOCAB];
  size_t vn = build_vocab(vocab, &cur, tick, MC_B_SEQ_VOCAB);
  for (size_t i = 0; i < vn; i++) {
    mc_state next;
    next.arb = state->arb;
    next.next = tick + 1u;
    mc_case_set("B3 depth=%u i=%zu tick=%llu holder=%d M=%u", depth, i,
                (unsigned long long)tick, (int)state->arb.holder, state->arb.mandate);
    mc_run_case(sp, &state->arb, &vocab[i], 1, tick, true, &next.arb, NULL);
    if (depth + 1u < MC_B_SEQ_DEPTH) b3_walk(sp, &next, depth + 1u, tick + 1u);
  }
}

void mc_arb_sequences(mc_space *sp, const mc_state *states) {
  for (unsigned s = 0; s < MC_B_STATES; s++) b3_walk(sp, &states[s], 0, states[s].next);
}

/* B4: a corrupted frame is equivalent to a frame that was never sent. The
 * decoder rejects it (SEC-01, ROB-09), so the arbiter never sees it and its
 * decision is the one it would have taken with an empty input. */
static bool same_outcome(const relais_arb_out *x, const relais_arb_out *y) {
  return x->value == y->value && x->source == y->source && x->holder == y->holder &&
         x->mandate == y->mandate && x->misses == y->misses && x->certified == y->certified &&
         x->certified_node == y->certified_node && x->lamp == y->lamp && x->cause == y->cause &&
         x->stop_requested == y->stop_requested && x->applied == y->applied;
}

void mc_arb_corrupted(mc_space *sp, const mc_state *states) {
  for (unsigned s = 0; s < MC_B_STATES; s++) {
    for (unsigned ty = 0; ty < MC_B_TYPES; ty++) {
      for (unsigned mode = 0; mode < MC_A_KEY_MODES; mode++) {
        relais_key k;
        mc_key(&k, mode == 1);
        uint8_t buf[RELAIS_FRAME_MAX];
        size_t len = mc_encode(TYPES[ty], &k, buf, sizeof buf);
        for (unsigned pos = 0; pos < 3; pos++) {
          size_t at = pos == 0 ? 0 : (pos == 1 ? len / 2 : len - 1);
          buf[at] = (uint8_t)(buf[at] ^ 0x5au);
          relais_msg decoded;
          relais_reject why = RELAIS_REJ_NONE;
          bool ok = relais_frame_decode(buf, len, &k, &decoded, &why);
          char d[288];
          mc_case_set("B4 state=%u type=%u mode=%u byte=%zu tick=%llu", s, ty, mode, at,
                      (unsigned long long)states[s].next);
          mc_fail(d, sizeof d, "B4 corrupted frame accepted");
          T_MSG(!ok, d);
          size_t ndec = ok ? 1u : 0u;
          relais_arb_out drop, empty;
          mc_run_case(sp, &states[s].arb, &decoded, ndec, states[s].next, true, NULL, &drop);
          mc_run_case(sp, &states[s].arb, NULL, 0, states[s].next, true, NULL, &empty);
          mc_fail(d, sizeof d, "B4 corrupted frame changed the decision");
          T_MSG(same_outcome(&drop, &empty), d);
        }
      }
    }
  }
}

/* B5: the boundary of the arbiter's documented input contract. arbiter.h says
 * the frames are already decoded and authenticated, and space A proves the
 * decoder never produces a sender outside {A,B,C,ACTUATOR}. The cases below are
 * therefore unreachable on a real path: they are PUBLISHED as observations, not
 * asserted, because the arbiter trusts its caller (ROB-09 applies to the
 * parser, not to this precondition). */
static void b5_contract(mc_space *sp, const mc_state *states) {
  for (unsigned s = 0; s < MC_B_STATES; s++) {
    for (unsigned kind = 0; kind < 4; kind++) {
      relais_msg m;
      relais_arb out_state;
      build_frame(&m, RELAIS_MSG_ORDRE, RELAIS_NODE_NONE, states[s].next,
                  states[s].arb.mandate, 0.25f);
      if (kind == 1u || kind == 3u) {
        m.type = RELAIS_MSG_DEMANDE;
        m.has_order = true;
        m.flags |= RELAIS_FLAG_HAS_ORDER;
      }
      if (kind == 2u) m.sender = RELAIS_NODE_ACTUATOR;
      if (kind == 3u) m.sender = RELAIS_NODE_ACTUATOR;
      mc_run_case(sp, &states[s].arb, &m, 1, states[s].next, false, &out_state, NULL);
      if (out_state.holder == RELAIS_NODE_NONE && out_state.applied_by_source[RELAIS_SRC_HOLDER] > 0)
        mc_obs_contract_violation++;
    }
  }
}

void mc_space_arbiter(mc_space *sp) {
  double t0 = mc_now();
  mc_state states[MC_B_STATES];
  sp->cases = 0;
  t_section("M36 space B: bounded exhaustion of relais_arb_step");
  for (unsigned s = 0; s < MC_B_STATES; s++) mc_arb_build_state(s, &states[s]);
  b1_single(sp, states);
  mc_arb_pairs(sp, states);
  mc_arb_sequences(sp, states);
  mc_arb_corrupted(sp, states);
  b5_contract(sp, states);
  sp->seconds = mc_now() - t0;
}
