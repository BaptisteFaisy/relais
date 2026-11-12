/* Relais — space C: bounded exhaustion of the control law (mission M36).
 *
 * Every case calls the REAL control functions of core/src/control.c:
 * relais_clamp_value, relais_clamp_slew, relais_ctl_step, relais_ctl_export,
 * relais_ctl_import and relais_testament_at.
 *
 * Properties checked on every case:
 *   CC-1 (ACT-05, CMD-06) the command is a finite number inside max_amplitude,
 *         and the change per tick stays inside max_slew.
 *   CC-2 (CMD-03) the law is deterministic: the same state and the same
 *         measurement give the same bits, twice in a row.
 *   CC-3 (REP-01, REP-02) the state exported can be imported back and gives
 *         the same next command: a second can take over without a bump.
 *   CC-4 (ACT-02, I6) the will is played only inside its horizon.
 *   CC-5 (ROB-09) no measurement, however corrupted, can make the law emit a
 *         non-numeric or unbounded command.
 */
#include "mc.h"

#include <math.h>
#include <string.h>

#include "relais/control.h"

uint64_t mc_obs_slew_nonfinite = 0;

static const float EXTREMES[MC_C_CLAMP_VALUES] = {-INFINITY, -1e30f, -2.0f,  -1.0f,
                                                   -0.5f,    -1e-9f,  0.0f,  1e-9f,
                                                   0.5f,     1.0f,    2.0f,  1e30f,
                                                   INFINITY, NAN};
static const float GRID[MC_C_MEAS_GRID] = {-2.0f, -1.0f, -0.5f, -0.1f, 0.0f,
                                           0.1f,  0.5f,  1.0f,  2.0f};
static const float APPLIED[2] = {0.0f, 0.5f};

static float amplitude_of(unsigned a) { return a == 0 ? 1.0f : (a == 1 ? 0.5f : 2.0f); }

/* C1: the two clamps of the actuator (ACT-05 amplitude, CMD-06 rate). */
static void c1_clamp(mc_space *sp) {
  for (unsigned a = 0; a < MC_C_AMPLITUDES; a++) {
    relais_params p;
    mc_params_set(&p, 0);
    p.max_amplitude = amplitude_of(a);
    for (unsigned i = 0; i < MC_C_CLAMP_VALUES; i++) {
      float out = relais_clamp_value(EXTREMES[i], &p);
      char d[112];
      mc_desc(d, sizeof d, "CC-1 clamp amplitude", i, a);
      T_MSG(isfinite(out), d);
      T_MSG(out <= p.max_amplitude && out >= -p.max_amplitude, d);
      sp->cases++;
    }
    for (unsigned i = 0; i < MC_C_CLAMP_VALUES; i++) {
      for (unsigned j = 0; j < MC_C_CLAMP_VALUES; j++) {
        float prev = EXTREMES[i];
        float want = EXTREMES[j];
        float out = relais_clamp_slew(prev, want, &p);
        char d[112];
        mc_desc(d, sizeof d, "CC-1 clamp rate", i, j);
        if (isfinite(prev) && isfinite(want)) {
          T_MSG(isfinite(out), d);
          T_MSG(fabsf(out - prev) <= p.max_slew + 1e-6f, d);
        } else {
          mc_obs_slew_nonfinite++;
        }
        sp->cases++;
      }
    }
  }
}

/* CC-2 and CC-3: determinism and the export/import round trip of the state. */
static void c2_step(mc_space *sp, size_t n) {
  for (unsigned v = 0; v < MC_C_PARAMS; v++) {
    relais_params p;
    mc_params_set(&p, v);
    for (size_t i = 0; i < n; i++) {
      for (size_t j = 0; j < n; j++) {
        for (size_t k = 0; k < n; k++) {
          for (size_t l = 0; l < 2; l++) {
            relais_meas meas;
            meas.pos = GRID[i];
            meas.vel = GRID[j];
            meas.setpoint = GRID[k];
            meas.applied = APPLIED[l];
            relais_ctl_state s1, s2, s3;
            uint8_t buf[64];
            float o1, o2, o3;
            uint8_t len;
            char d[112];
            relais_ctl_init(&s1, &p);
            o1 = relais_ctl_step(&s1, &meas, &p);
            relais_ctl_init(&s2, &p);
            o2 = relais_ctl_step(&s2, &meas, &p);
            mc_desc(d, sizeof d, "CC-2 law not deterministic", (uint64_t)v, (uint64_t)i);
            T_MSG(memcmp(&s1, &s2, sizeof s1) == 0 && o1 == o2, d);
            mc_desc(d, sizeof d, "CC-1 command out of bounds", (uint64_t)v, (uint64_t)j);
            T_MSG(isfinite(o1), d);
            T_MSG(o1 <= p.max_amplitude && o1 >= -p.max_amplitude, d);
            T_MSG(fabsf(o1 - 0.0f) <= p.max_slew + 1e-6f, d);
            len = relais_ctl_export(&s1, buf, sizeof buf);
            memset(&s3, 0, sizeof s3);
            mc_desc(d, sizeof d, "CC-3 state export refused", (uint64_t)v, len);
            T_MSG(len > 0, d);
            T_MSG(relais_ctl_import(&s3, buf, len), d);
            /* REP-01: the second continues the holder's trajectory: the imported
             * state must give the same next command as the exported one. */
            o3 = relais_ctl_step(&s3, &meas, &p);
            o1 = relais_ctl_step(&s1, &meas, &p);
            mc_desc(d, sizeof d, "CC-3 imported state gives another command", (uint64_t)v,
                    (uint64_t)k);
            T_MSG(o3 == o1, d);
            sp->cases++;
          }
        }
      }
    }
  }
}

/* CC-5: no corrupted measurement can make the law emit a bad command. */
static void c5_extremes(mc_space *sp) {
  for (unsigned v = 0; v < MC_C_PARAMS; v++) {
    relais_params p;
    mc_params_set(&p, v);
    for (unsigned i = 0; i < MC_C_CLAMP_VALUES; i++) {
      for (unsigned j = 0; j < MC_C_CLAMP_VALUES; j++) {
        for (unsigned k = 0; k < MC_C_CLAMP_VALUES; k++) {
          relais_meas meas;
          meas.pos = EXTREMES[i];
          meas.vel = EXTREMES[j];
          meas.setpoint = EXTREMES[k];
          meas.applied = 0.0f;
          relais_ctl_state s;
          float out;
          char d[112];
          relais_ctl_init(&s, &p);
          out = relais_ctl_step(&s, &meas, &p);
          mc_desc(d, sizeof d, "CC-5 corrupted measurement broke the command",
                  (uint64_t)v * 1000000u + i * 1000u + j, k);
          T_MSG(isfinite(out), d);
          T_MSG(out <= p.max_amplitude && out >= -p.max_amplitude, d);
          sp->cases++;
        }
      }
    }
  }
}

/* C4: the will is played only inside its horizon (I6). */
static void c4_testament(mc_space *sp) {
  static const relais_tick bases[MC_C_TST_BASES] = {RELAIS_TICK_NONE, 0u, 1u, 5u,
                                                    (relais_tick)UINT64_MAX - 2u};
  static const relais_tick ticks[MC_C_TST_TICKS] = {0u,  1u,  2u,  3u,  4u,  5u,  6u,
                                                    7u,  8u,  9u,  10u, 11u, 12u,
                                                    (relais_tick)1 << 63, (relais_tick)UINT64_MAX};
  float will[RELAIS_TESTAMENT_MAX];
  for (uint8_t i = 0; i < RELAIS_TESTAMENT_MAX; i++) will[i] = 0.1f * (float)i;
  for (unsigned len = 0; len < MC_C_TST_LEN; len++) {
    for (unsigned b = 0; b < MC_C_TST_BASES; b++) {
      for (unsigned t = 0; t < MC_C_TST_TICKS; t++) {
        float out = 0.0f;
        bool got = relais_testament_at(will, (uint8_t)len, bases[b], ticks[t], &out);
        bool want = len > 0 && bases[b] != RELAIS_TICK_NONE && ticks[t] >= bases[b] &&
                    (ticks[t] - bases[b]) < (relais_tick)len;
        char d[112];
        mc_desc(d, sizeof d, "CC-4 will horizon", (uint64_t)len * 1000u + b, t);
        T_MSG(got == want, d);
        if (want) T_MSG(out == will[ticks[t] - bases[b]], d);
        sp->cases++;
      }
    }
  }
}

/* The same law, twice, over a long run: same bits at every tick (CMD-03, the
 * twin without fault is the reference of I5). */
static void c5_determinism(mc_space *sp) {
  for (unsigned v = 0; v < MC_C_PARAMS; v++) {
    relais_params p;
    mc_params_set(&p, v);
    relais_ctl_state a, b;
    relais_ctl_init(&a, &p);
    relais_ctl_init(&b, &p);
    for (uint32_t i = 0; i < MC_C_RUN_TICKS; i++) {
      relais_meas meas;
      meas.pos = 0.5f * sinf((float)i * 0.01f);
      meas.vel = 0.25f * cosf((float)i * 0.02f);
      meas.setpoint = 0.75f * sinf((float)i * 0.005f);
      meas.applied = 0.0f;
      float oa = relais_ctl_step(&a, &meas, &p);
      float ob = relais_ctl_step(&b, &meas, &p);
      char d[112];
      mc_desc(d, sizeof d, "CC-2 long run not reproducible", (uint64_t)v, i);
      T_MSG(oa == ob && memcmp(&a, &b, sizeof a) == 0, d);
      T_MSG(isfinite(oa) && oa <= p.max_amplitude && oa >= -p.max_amplitude, d);
      sp->cases++;
    }
  }
}

void mc_space_control(mc_space *sp) {
  double t0 = mc_now();
  sp->cases = 0;
  t_section("M36 space C: bounded exhaustion of the control law");
  c1_clamp(sp);
  c2_step(sp, MC_C_MEAS_GRID);
  c5_extremes(sp);
  c4_testament(sp);
  c5_determinism(sp);
  sp->seconds = mc_now() - t0;
}
