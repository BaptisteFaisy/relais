/* Relais — differential-test bridge (M13, idea 1 & 84).
 *
 * A tiny JSON-lines host that exposes the C arbiter to a Python driver. The
 * Python reimplementation of the arbiter (arbiter_py.py) is compared against
 * this reference after every tick. One command per line on stdin, one JSON
 * object per line on stdout. It exists only to serve the confrontation; the
 * protocol itself stays in core/.
 *
 * Commands:
 *   {"cmd":"init","params":{...}}            -> {"ok":true}
 *   {"cmd":"step","tick":N,"meas":{...},"msgs":[...]} -> {"tick":N,"state":{...},"out":{...}}
 *
 * CMD-02, MAN-02, MAN-04, REL-05, DET-01, DET-09 are exercised by the Python
 * mirror and compared field by field.
 */
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relais/arbiter.h"
#include "relais/params.h"

/* ---- minimal JSON DOM, only what the driver emits ---- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_OBJ, J_ARR } jtype;

typedef struct jval {
  jtype t;
  double num;
  bool b;
  char *s;
  struct jval **kid;
  char **key;
  size_t n, cap;
} jval;

static char *xstr(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1);
  if (p == NULL) return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

static void grow(jval *v, size_t need) {
  if (v->n + need <= v->cap) return;
  size_t nc = v->cap ? v->cap : 4;
  while (nc < v->n + need) nc *= 2;
  struct jval **nk = (struct jval **)realloc(v->kid, nc * sizeof *nk);
  char **nkey = (char **)realloc(v->key, nc * sizeof *nkey);
  if (nk != NULL) v->kid = nk;
  if (nkey != NULL) v->key = nkey;
  v->cap = nc;
}

static void skip_ws(const char **p) {
  while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static jval *parse(const char **p);

static jval *parse_str(const char **p) {
  (*p)++; /* opening quote */
  const char *start = *p;
  char buf[256];
  size_t m = 0;
  jval *v = (jval *)calloc(1, sizeof *v);
  v->t = J_STR;
  while (**p && **p != '"') {
    if (**p == '\\') {
      (*p)++;
      char c = **p;
      if (c == 'n') buf[m++] = '\n';
      else buf[m++] = c;
      (*p)++;
      continue;
    }
    buf[m++] = **p;
    (*p)++;
    if (m >= sizeof buf - 1) break;
  }
  buf[m] = '\0';
  (void)start;
  if (**p == '"') (*p)++;
  v->s = xstr(buf, m);
  return v;
}

static jval *parse_num(const char **p) {
  char *end = NULL;
  double d = strtod(*p, &end);
  jval *v = (jval *)calloc(1, sizeof *v);
  v->t = J_NUM;
  v->num = d;
  *p = end;
  return v;
}

static void eat(const char **p, char c) { if (**p == c) (*p)++; }

static jval *parse_obj(const char **p) {
  jval *v = (jval *)calloc(1, sizeof *v);
  v->t = J_OBJ;
  (*p)++; /* consume '{' */
  skip_ws(p);
  if (**p == '}') { (*p)++; return v; }
  for (;;) {
    skip_ws(p);
    char *k = parse_str(p)->s;
    skip_ws(p);
    eat(p, ':');
    jval *c = parse(p);
    grow(v, 1);
    v->key[v->n] = k;
    v->kid[v->n] = c;
    v->n++;
    skip_ws(p);
    if (**p == ',') { (*p)++; continue; }
    eat(p, '}');
    return v;
  }
}

static jval *parse_arr(const char **p) {
  jval *v = (jval *)calloc(1, sizeof *v);
  v->t = J_ARR;
  (*p)++; /* consume '[' */
  skip_ws(p);
  if (**p == ']') { (*p)++; return v; }
  for (;;) {
    jval *c = parse(p);
    grow(v, 1);
    v->kid[v->n++] = c;
    skip_ws(p);
    if (**p == ',') { (*p)++; continue; }
    eat(p, ']');
    return v;
  }
}

static jval *parse_lit(const char **p, const char *word, jtype t, bool b) {
  jval *v = (jval *)calloc(1, sizeof *v);
  size_t n = strlen(word);
  if (strncmp(*p, word, n) == 0) { v->t = t; v->b = b; *p += n; }
  return v;
}

static jval *parse(const char **p) {
  skip_ws(p);
  switch (**p) {
    case '{': return parse_obj(p);
    case '[': return parse_arr(p);
    case '"': return parse_str(p);
    case 't': return parse_lit(p, "true", J_BOOL, true);
    case 'f': return parse_lit(p, "false", J_BOOL, false);
    case 'n': return parse_lit(p, "null", J_NULL, false);
    default:  return parse_num(p);
  }
}

static jval *jget(const jval *o, const char *k) {
  if (o == NULL || o->t != J_OBJ) return NULL;
  for (size_t i = 0; i < o->n; i++)
    if (strcmp(o->key[i], k) == 0) return o->kid[i];
  return NULL;
}

static double jnum(const jval *v, double dflt) {
  return (v != NULL && v->t == J_NUM) ? v->num : dflt;
}

static int jint(const jval *v, int dflt) {
  return (v != NULL && v->t == J_NUM) ? (int)v->num : dflt;
}

static bool jbool(const jval *v, bool dflt) {
  return (v != NULL && v->t == J_BOOL) ? v->b : dflt;
}

/* ---- float output that round-trips float32 exactly ---- */

static void print_f32(double d) {
  printf("%.9g", d);
}

static void print_f32_arr(const float *a, size_t n) {
  printf("[");
  for (size_t i = 0; i < n; i++) {
    if (i) printf(",");
    print_f32((double)a[i]);
  }
  printf("]");
}

/* ---- build a decoded message from JSON ---- */

static void fill_msg(relais_msg *m, const jval *o, const relais_params *p) {
  relais_msg_clear(m);
  m->type = (relais_msg_type)jint(jget(o, "type"), 0);
  m->sender = (relais_node)jint(jget(o, "sender"), 0);
  m->flags = (uint8_t)jint(jget(o, "flags"), 0);
  m->tick = (relais_tick)(uint64_t)jnum(jget(o, "tick"), 0.0);
  m->mandate = (relais_mandate)(uint32_t)jnum(jget(o, "mandate"), 0.0);
  m->value = (float)jnum(jget(o, "value"), 0.0);
  m->testament_len = (uint8_t)jint(jget(o, "testament_len"), 0);
  const jval *ta = jget(o, "testament");
  if (ta != NULL && ta->t == J_ARR) {
    size_t k = ta->n < RELAIS_TESTAMENT_MAX ? ta->n : RELAIS_TESTAMENT_MAX;
    for (size_t i = 0; i < k; i++) m->testament[i] = (float)jnum(ta->kid[i], 0.0);
    if ((uint8_t)k < m->testament_len) m->testament_len = (uint8_t)k;
  }
  m->has_order = jbool(jget(o, "has_order"), false);
  m->seen_mandate = (relais_mandate)(uint32_t)jnum(jget(o, "seen_mandate"), 0.0);
  m->reason = (uint8_t)jint(jget(o, "reason"), 0);
  m->replica_age = (uint8_t)jint(jget(o, "replica_age"), 0);
  const jval *meas = jget(o, "meas");
  if (meas != NULL) {
    m->meas.pos = (float)jnum(jget(meas, "pos"), 0.0);
    m->meas.vel = (float)jnum(jget(meas, "vel"), 0.0);
    m->meas.applied = (float)jnum(jget(meas, "applied"), 0.0);
    m->meas.setpoint = (float)jnum(jget(meas, "setpoint"), 0.0);
  }
  (void)p;
}

/* ---- state dump (the whole arbiter) ---- */

static void dump_state(const relais_arb *a) {
  printf("\"mandate\":%" PRIu32 ",\"holder\":%d,\"misses\":%u", a->mandate,
         (int)a->holder, (unsigned)a->misses);
  printf(",\"switch_pending\":%s,\"certified\":%s,\"certified_node\":%d",
         a->switch_pending ? "true" : "false", a->certified ? "true" : "false",
         (int)a->certified_node);
  printf(",\"certify_streak\":%u,\"shadow_misses\":%u", (unsigned)a->certify_streak,
         (unsigned)a->shadow_misses);
  printf(",\"last_order\":"); print_f32((double)a->last_order);
  printf(",\"last_shadow\":"); print_f32((double)a->last_shadow);
  printf(",\"have_order\":%s,\"have_shadow\":%s",
         a->have_order ? "true" : "false", a->have_shadow ? "true" : "false");
  printf(",\"disagree_streak\":%u,\"disagreement\":%s", (unsigned)a->disagree_streak,
         a->disagreement ? "true" : "false");
  printf(",\"testament\":");
  print_f32_arr(a->testament, RELAIS_TESTAMENT_MAX);
  printf(",\"testament_len\":%u,\"testament_base\":%" PRIu64, (unsigned)a->testament_len,
         (uint64_t)a->testament_base);
  printf(",\"fb_state\":"); print_f32((double)a->fb_state);
  printf(",\"in_fallback\":%s", a->in_fallback ? "true" : "false");
  printf(",\"last_heard\":[");
  for (int i = 0; i < 5; i++) { if (i) printf(","); printf("%" PRIu64, (uint64_t)a->last_heard[i]); }
  printf("],\"present\":[");
  for (int i = 0; i < 5; i++) { if (i) printf(","); printf("%s", a->present[i] ? "true" : "false"); }
  printf("],\"ever_seen\":[");
  for (int i = 0; i < 5; i++) { if (i) printf(","); printf("%s", a->ever_seen[i] ? "true" : "false"); }
  printf("],\"lamp\":%d,\"cause\":%d", (int)a->lamp, (int)a->cause);
  printf(",\"degraded_ticks\":%" PRIu32 ",\"stop_requested\":%s", a->degraded_ticks,
         a->stop_requested ? "true" : "false");
  printf(",\"cession_pending\":%s,\"request_window_tick\":%u,\"request_seen\":%u",
         a->cession_pending ? "true" : "false", (unsigned)a->request_window_tick,
         (unsigned)a->request_seen);
  printf(",\"rejected\":[");
  for (int i = 0; i < (int)RELAIS_REJECT_SLOTS; i++) { if (i) printf(","); printf("%" PRIu32, a->rejected[i]); }
  printf("],\"forged\":%" PRIu32 ",\"applied_by_source\":[", a->forged);
  for (int i = 0; i < 5; i++) { if (i) printf(","); printf("%" PRIu32, a->applied_by_source[i]); }
  printf("],\"ticks\":%" PRIu32 ",\"handovers\":%" PRIu32, a->ticks, a->handovers);
}

/* ---- out dump ---- */

static void dump_out(const relais_arb_out *out) {
  printf("\"applied\":true,\"value\":");
  print_f32((double)out->value);
  printf(",\"source\":%d,\"mandate\":%" PRIu32 ",\"holder\":%d,\"misses\":%u",
         (int)out->source, out->mandate, (int)out->holder, (unsigned)out->misses);
  printf(",\"certified\":%s,\"certified_node\":%d", out->certified ? "true" : "false",
         (int)out->certified_node);
  printf(",\"lamp\":%d,\"cause\":%d,\"stop_requested\":%s", (int)out->lamp, (int)out->cause,
         out->stop_requested ? "true" : "false");
  printf(",\"statut\":{\"type\":%d,\"sender\":%d,\"tick\":%" PRIu64 ",\"mandate\":%" PRIu32
         ",\"holder\":%d,\"source\":%d,\"misses\":%u,\"certified\":%s,\"certified_node\":%d,\"lamp\":%d,\"cause\":%d,\"stop_requested\":%s,\"meas\":{\"pos\":",
         (int)out->statut.type, (int)out->statut.sender, (uint64_t)out->statut.tick,
         out->statut.mandate, (int)out->statut.holder, (int)out->statut.source,
         (unsigned)out->statut.misses, out->statut.certified ? "true" : "false",
         (int)out->statut.certified_node, (int)out->statut.lamp, (int)out->statut.cause,
         out->statut.stop_requested ? "true" : "false");
  print_f32((double)out->statut.meas.pos);
  printf(",\"vel\":"); print_f32((double)out->statut.meas.vel);
  printf(",\"applied\":"); print_f32((double)out->statut.meas.applied);
  printf(",\"setpoint\":"); print_f32((double)out->statut.meas.setpoint);
  printf("}}");
  printf(",\"events\":[");
  for (int i = 0; i < out->event_count; i++) {
    if (i) printf(",");
    printf("{\"kind\":%d,\"node\":%d,\"mandate\":%" PRIu32 ",\"tick\":%" PRIu64
           ",\"detail\":%" PRIu32 "}",
           (int)out->events[i].kind, (int)out->events[i].node, out->events[i].mandate,
           (uint64_t)out->events[i].tick, out->events[i].detail);
  }
  printf("]");
}

static void handle_init(const jval *root, relais_params *params, relais_arb *arb) {
  const jval *pp = jget(root, "params");
  relais_params_default(params);
  if (pp != NULL) {
    params->period_ms = (uint32_t)jint(jget(pp, "period_ms"), (int)params->period_ms);
    params->patience = (uint8_t)jint(jget(pp, "patience"), (int)params->patience);
    params->testament_len = (uint8_t)jint(jget(pp, "testament_len"), (int)params->testament_len);
    params->certify_ticks = (uint8_t)jint(jget(pp, "certify_ticks"), (int)params->certify_ticks);
    params->bail_ticks = (uint8_t)jint(jget(pp, "bail_ticks"), (int)params->bail_ticks);
    params->request_burst = (uint8_t)jint(jget(pp, "request_burst"), (int)params->request_burst);
    params->request_window = (uint16_t)jint(jget(pp, "request_window"), (int)params->request_window);
    params->epsilon = (float)jnum(jget(pp, "epsilon"), (double)params->epsilon);
    params->delta = (float)jnum(jget(pp, "delta"), (double)params->delta);
    params->degraded_ticks = (uint32_t)jnum(jget(pp, "degraded_ticks"), (double)params->degraded_ticks);
    params->max_amplitude = (float)jnum(jget(pp, "max_amplitude"), (double)params->max_amplitude);
    params->max_slew = (float)jnum(jget(pp, "max_slew"), (double)params->max_slew);
  }
  relais_arb_init(arb, params);
  printf("{\"ok\":true}\n");
}

static void handle_step(const jval *root, relais_arb *arb, const relais_params *params) {
  relais_tick tick = (relais_tick)(uint64_t)jnum(jget(root, "tick"), 0.0);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  const jval *mj = jget(root, "meas");
  if (mj != NULL) {
    meas.pos = (float)jnum(jget(mj, "pos"), 0.0);
    meas.vel = (float)jnum(jget(mj, "vel"), 0.0);
    meas.applied = (float)jnum(jget(mj, "applied"), 0.0);
    meas.setpoint = (float)jnum(jget(mj, "setpoint"), 0.0);
  }
  relais_msg msgs[RELAIS_IN_MSG_MAX];
  size_t n = 0;
  const jval *arr = jget(root, "msgs");
  if (arr != NULL && arr->t == J_ARR) {
    for (size_t i = 0; i < arr->n && n < RELAIS_IN_MSG_MAX; i++)
      fill_msg(&msgs[n++], arr->kid[i], params);
  }
  relais_arb_out out;
  relais_arb_step(arb, tick, msgs, n, &meas, &out);
  printf("{\"tick\":%" PRIu64 ",\"state\":{", (uint64_t)tick);
  dump_state(arb);
  printf("},\"out\":{");
  dump_out(&out);
  printf("}}\n");
}

int main(void) {
  relais_arb arb;
  relais_params params;
  relais_params_default(&params);
  relais_arb_init(&arb, &params);

  char *line = NULL;
  size_t cap = 0;
  while (getline(&line, &cap, stdin) != -1) {
    const char *p = line;
    jval *root = parse(&p);
    const jval *cmd = (root != NULL && root->t == J_OBJ) ? jget(root, "cmd") : NULL;
    if (cmd != NULL && cmd->t == J_STR && strcmp(cmd->s, "init") == 0) {
      handle_init(root, &params, &arb);
    } else if (cmd != NULL && cmd->t == J_STR && strcmp(cmd->s, "step") == 0) {
      handle_step(root, &arb, &params);
    } else {
      printf("{\"error\":\"bad json or unknown cmd\"}\n");
    }
    fflush(stdout);
  }
  free(line);
  return 0;
}