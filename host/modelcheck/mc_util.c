/* Relais — shared helpers of the bounded exhaustive harness (mission M36).
 * One implementation, used by the three spaces and by the CBMC/WP harnesses. */
#include "mc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "relais/control.h"

mc_space mc_sp_a = {"A-wire", 0, 0, 0, 0.0};
mc_space mc_sp_b = {"B-arbiter", 0, 0, 0, 0.0};
mc_space mc_sp_c = {"C-control", 0, 0, 0, 0.0};

double mc_now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void mc_key(relais_key *k, bool level_c) {
  relais_key_clear(k);
  if (!level_c) return; /* level S: relais_check_compute falls back to CRC-32C */
  for (size_t i = 0; i < sizeof k->bytes; i++)
    k->bytes[i] = (uint8_t)(0x11u + (uint32_t)i * 7u); /* fixed, reproducible */
  k->enabled = true;
}

/* A fixed measurement, so every encoded sample frame carries the same bytes. */
static void sample_meas(relais_meas *m) {
  m->pos = 0.25f;
  m->vel = -0.125f;
  m->applied = 0.5f;
  m->setpoint = -0.375f;
}

static void sample_statut(relais_msg *m) {
  relais_meas meas;
  sample_meas(&meas);
  t_statut(m, 12, 3, RELAIS_NODE_A, RELAIS_SRC_HOLDER, 0, &meas);
  m->certified = true;
  m->certified_node = RELAIS_NODE_B;
  m->lamp = RELAIS_LAMP_YELLOW;
  m->cause = RELAIS_CAUSE_SECOND_UNCERTIFIED;
  m->misses = 1;
}

static void sample_ordre(relais_msg *m) {
  relais_params p;
  relais_params_default(&p);
  t_order(m, RELAIS_NODE_A, 3, 13, 0.25f, &p);
}

static void sample_ombre(relais_msg *m) { t_shadow(m, RELAIS_NODE_B, 13, 0.25f, 1); }

static void sample_demande(relais_msg *m) {
  relais_params p;
  relais_params_default(&p);
  t_demande(m, RELAIS_NODE_B, 3, 13, true, 0.125f, &p);
}

static void sample_etat(relais_msg *m) {
  relais_params p;
  relais_ctl_state st;
  relais_params_default(&p);
  relais_ctl_init(&st, &p);
  t_etat(m, RELAIS_NODE_A, 13, &st, 0.25f);
}

void mc_fill_msg(relais_msg *m, relais_msg_type t) {
  relais_msg_clear(m);
  switch (t) {
    case RELAIS_MSG_STATUT: sample_statut(m); break;
    case RELAIS_MSG_ORDRE: sample_ordre(m); break;
    case RELAIS_MSG_OMBRE: sample_ombre(m); break;
    case RELAIS_MSG_DEMANDE: sample_demande(m); break;
    case RELAIS_MSG_CESSION: t_cession(m, RELAIS_NODE_A, 3, 2); break;
    case RELAIS_MSG_ETAT: sample_etat(m); break;
    case RELAIS_MSG_SUIVI:
      t_shadow(m, RELAIS_NODE_B, 13, 0.0f, 2); /* reuse the fixtures */
      m->type = RELAIS_MSG_SUIVI;
      m->ready = true;
      break;
    default: break;
  }
}

size_t mc_encode(relais_msg_type t, const relais_key *k, uint8_t *buf, size_t cap) {
  relais_msg m;
  mc_fill_msg(&m, t);
  return relais_frame_encode(&m, k, buf, cap);
}

void mc_params_set(relais_params *p, unsigned which) {
  if (p == NULL) return;
  relais_params_default(p);
  switch (which) {
    case 0: break; /* the shipped default: T=5 ms, p=1, N=8 */
    case 1:
      p->period_ms = 1;   /* fastest legal period */
      p->patience = 3;    /* longest legal patience */
      p->max_slew = 0.05f;
      break;
    case 2:
      p->period_ms = 50;  /* slowest legal period */
      p->max_amplitude = 0.5f;
      p->epsilon = 0.02f;
      p->delta = 0.1f;
      p->dissimilar_epsilon = 0.05f; /* DEG-08 branch enabled */
      p->dissimilar_delta = 0.1f;
      break;
    default: break;
  }
}

void mc_desc(char *buf, size_t cap, const char *what, uint64_t a, uint64_t b) {
  if (buf == NULL || cap == 0) return;
  snprintf(buf, cap, "%s (a=%llu b=%llu)", what, (unsigned long long)a,
           (unsigned long long)b);
}

char mc_case[224];

void mc_case_set(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(mc_case, sizeof mc_case, fmt, ap);
  va_end(ap);
}

void mc_fail(char *buf, size_t cap, const char *prop) {
  if (buf == NULL || cap == 0) return;
  snprintf(buf, cap, "%s | %s", prop, mc_case);
}

void mc_report(const mc_space *sp) {
  printf("MC-SPACE %-10s cases=%llu rejected=%llu accepted=%llu time=%.3fs\n",
         sp->name, (unsigned long long)sp->cases, (unsigned long long)sp->rejected,
         (unsigned long long)sp->accepted, sp->seconds);
}

uint64_t mc_total_cases(void) { return mc_sp_a.cases + mc_sp_b.cases + mc_sp_c.cases; }

double mc_total_seconds(void) {
  return mc_sp_a.seconds + mc_sp_b.seconds + mc_sp_c.seconds;
}
