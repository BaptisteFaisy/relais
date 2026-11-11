/* Relais — protocol parameters (spec 6.3) and their consistency rule. */
#ifndef RELAIS_PARAMS_H
#define RELAIS_PARAMS_H

#include "relais/types.h"

typedef struct {
  uint32_t period_ms;      /* T, tick period, 1..50 ms */
  uint8_t patience;        /* p, consecutive misses tolerated */
  uint8_t testament_len;   /* N, horizon of the will, ticks */
  uint8_t certify_ticks;   /* matching ticks before a second is certified */
  uint8_t bail_ticks;      /* STATUT silence after which a node deposes itself */
  uint8_t request_burst;   /* SEC-06: demandes accepted per window */
  uint16_t request_window; /* ticks in that window */
  float epsilon;           /* order/shadow agreement tolerance, full-scale fraction */
  float delta;             /* continuity tolerance at handover, full-scale fraction */
  uint32_t degraded_ticks; /* DEG-05: ticks without redundancy before stop request */
  float max_amplitude;     /* ACT-05 command bound */
  float max_slew;          /* ACT-05 bound on the change per tick */
  bool slow_motion;        /* CMD-07: only T changes, the logic is tick based */
  uint32_t slow_factor;    /* multiplier applied to period_ms */
} relais_params;

void relais_params_default(relais_params *p);
/* N >= p + 2 + 3, plus every bound in range. why may be NULL. */
bool relais_params_valid(const relais_params *p, char *why, size_t why_len);
uint32_t relais_params_period_ms(const relais_params *p);
double relais_params_period_s(const relais_params *p);
/* Parse "T=5,p=1,N=8" style overrides; unknown keys are rejected. */
bool relais_params_parse(relais_params *p, const char *spec, char *why, size_t why_len);

#endif /* RELAIS_PARAMS_H */
