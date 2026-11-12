/* Relais — protocol parameters (spec 6.3): defaults, the consistency rule
 * N >= p + 2 + 3, and a small parser used by the bench and the scenarios. */
#include "relais/params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPEC_BUF 256

void relais_params_default(relais_params *p) {
  if (p == NULL) return;
  memset(p, 0, sizeof *p);
  p->period_ms = 5;
  p->patience = 1;
  p->testament_len = RELAIS_TESTAMENT_MAX; /* N = 8 */
  p->certify_ticks = 3;
  p->bail_ticks = 4;
  p->request_burst = 4;
  p->request_window = 4;
  p->auth_strikes = 0; /* M20/DEG-09: off by default, behaviour unchanged */
  p->epsilon = 0.005f;  /* 0.5 % of full scale */
  p->delta = 0.02f;     /* 2 % of full scale */
  p->dissimilar_epsilon = 0.0f; /* DEG-08: off = identical backup, unchanged */
  p->dissimilar_delta = 0.02f;  /* = delta while off, so the rule always holds */
  p->degraded_ticks = 6000; /* 30 s at 5 ms */
  p->max_amplitude = RELAIS_FULL_SCALE;
  p->max_slew = 0.25f;
  p->slow_motion = false;
  p->slow_factor = 10;
}

static bool bad(const relais_params *p, const char *what, char *why, size_t n) {
  if (why != NULL && n > 0) snprintf(why, n, "%s", what);
  (void)p;
  return false;
}

bool relais_params_valid(const relais_params *p, char *why, size_t why_len) {
  if (p == NULL) return bad(p, "parametres absents", why, why_len);
  if (p->period_ms < 1 || p->period_ms > 50)
    return bad(p, "T hors de 1..50 ms", why, why_len);
  if (p->patience > 3) return bad(p, "p > 3 (N <= 8 ne couvrirait pas)", why, why_len);
  if (p->testament_len < 1 || p->testament_len > RELAIS_TESTAMENT_MAX)
    return bad(p, "N hors de 1..8", why, why_len);
  /* Consistency rule of spec 6.3: the will must cover the handover even when
   * three demandes are lost. */
  if (p->testament_len < (uint8_t)(p->patience + 2 + 3))
    return bad(p, "N < p + 2 + 3", why, why_len);
  if (p->certify_ticks < 1 || p->certify_ticks > RELAIS_TESTAMENT_MAX)
    return bad(p, "certification hors de 1..8", why, why_len);
  if (p->bail_ticks < 2) return bad(p, "bail < 2 ticks", why, why_len);
  if (!(p->epsilon > 0.0f) || !(p->delta > 0.0f))
    return bad(p, "epsilon ou delta nul", why, why_len);
  if (p->delta > 0.5f) return bad(p, "delta > 50 % de pleine echelle", why, why_len);
  /* DEG-08: a dissimilar backup widens the order/shadow band by
   * dissimilar_epsilon (disabled at 0, so the behaviour is unchanged) and, at
   * the handover, admits a continuity bump of dissimilar_delta. A bound is only
   * ever relaxed when the new number is published: the STATUT carries the new
   * flag, and dissimilar_delta is explicit in the parameter block. */
  if (!(p->dissimilar_epsilon >= 0.0f) || p->dissimilar_epsilon >= 1.0f)
    return bad(p, "dissimilar_epsilon hors de [0,1[", why, why_len);
  if (p->dissimilar_epsilon > 0.0f) {
    if (!(p->dissimilar_delta >= p->delta))
      return bad(p, "dissimilar_delta < delta", why, why_len);
    if (p->dissimilar_delta > 0.5f)
      return bad(p, "dissimilar_delta > 50 % de pleine echelle", why, why_len);
  }
  if (p->degraded_ticks < 1) return bad(p, "duree degradee nulle", why, why_len);
  if (!(p->max_amplitude > 0.0f) || p->max_amplitude > 10.0f)
    return bad(p, "amplitude hors borne", why, why_len);
  if (!(p->max_slew > 0.0f) || p->max_slew > 10.0f)
    return bad(p, "slew hors borne", why, why_len);
  if (p->request_burst < 1 || p->request_window < 1)
    return bad(p, "limitation de debit invalide", why, why_len);
  /* M20/DEG-09: the strike limit is a small number; 0 disables the mechanism. */
  if (p->auth_strikes > 32)
    return bad(p, "auth_strikes > 32", why, why_len);
  if (p->slow_factor < 1 || p->slow_factor > 1000)
    return bad(p, "facteur de ralenti hors de 1..1000", why, why_len);
  if (why != NULL && why_len > 0) why[0] = '\0';
  return true;
}

uint32_t relais_params_period_ms(const relais_params *p) {
  if (p == NULL) return 0;
  return p->slow_motion ? p->period_ms * p->slow_factor : p->period_ms;
}

double relais_params_period_s(const relais_params *p) {
  return (double)relais_params_period_ms(p) / 1000.0;
}

double relais_params_step_s(const relais_params *p) {
  if (p == NULL) return 0.0;
  return (double)p->period_ms / 1000.0;
}

static bool apply_kv(relais_params *p, const char *k, const char *v) {
  double d = strtod(v, NULL);
  unsigned long u = strtoul(v, NULL, 10);
  if (strcmp(k, "T") == 0) { p->period_ms = (uint32_t)u; return true; }
  if (strcmp(k, "p") == 0) { p->patience = (uint8_t)u; return true; }
  if (strcmp(k, "N") == 0) { p->testament_len = (uint8_t)u; return true; }
  if (strcmp(k, "certify") == 0) { p->certify_ticks = (uint8_t)u; return true; }
  if (strcmp(k, "bail") == 0) { p->bail_ticks = (uint8_t)u; return true; }
  if (strcmp(k, "strikes") == 0) { p->auth_strikes = (uint8_t)u; return true; }
  if (strcmp(k, "eps") == 0) { p->epsilon = (float)d; return true; }
  if (strcmp(k, "delta") == 0) { p->delta = (float)d; return true; }
  if (strcmp(k, "deps") == 0) { p->dissimilar_epsilon = (float)d; return true; }
  if (strcmp(k, "ddelta") == 0) { p->dissimilar_delta = (float)d; return true; }
  if (strcmp(k, "degraded") == 0) { p->degraded_ticks = (uint32_t)u; return true; }
  if (strcmp(k, "amp") == 0) { p->max_amplitude = (float)d; return true; }
  if (strcmp(k, "slew") == 0) { p->max_slew = (float)d; return true; }
  if (strcmp(k, "slow") == 0) {
    p->slow_factor = (uint32_t)u;
    p->slow_motion = u > 1;
    return true;
  }
  return false;
}

bool relais_params_parse(relais_params *p, const char *spec, char *why, size_t why_len) {
  char buf[SPEC_BUF];
  if (p == NULL) return false;
  if (spec == NULL || spec[0] == '\0') return relais_params_valid(p, why, why_len);
  if (strlen(spec) >= sizeof buf) return bad(p, "spec trop long", why, why_len);
  strcpy(buf, spec);
  for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok != NULL;
       tok = strtok_r(NULL, ",", &save)) {
    while (*tok == ' ') tok++;
    char *eq = strchr(tok, '=');
    if (eq == NULL) return bad(p, "cle sans valeur", why, why_len);
    *eq = '\0';
    if (!apply_kv(p, tok, eq + 1)) return bad(p, "cle inconnue", why, why_len);
  }
  return relais_params_valid(p, why, why_len);
}
