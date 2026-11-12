/* Relais — corpus generator for the fuzz targets (host/fuzz/corpus).
 *
 * The seeds are the bytes the system really exchanges: frames produced by the
 * shared encoder relais_frame_encode (the same call the simulator, the PC
 * actuator and the QNX relais-net make), journal lines produced by the shared
 * formatters, and — with --journal — lines copied out of a journal a real
 * simulator run wrote. A fuzzing campaign that starts from those seeds reaches
 * the payload parsers instead of dying on the magic.
 *
 * Usage: gen_corpus OUT_DIR [--journal FILE]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "relais/frame.h"
#include "relais/journal.h"

#include "fuzz_script.h"

#define KEY_HEX "00112233445566778899aabbccddeeff"

typedef struct {
  uint8_t type, sender, flags, mode, tlen, age, reason, has_order;
  float value, t0, t1;
  uint32_t seen;
} rec_t;

static int write_file(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    fprintf(stderr, "gen_corpus: cannot write %s\n", path);
    return 1;
  }
  fwrite(data, 1, len, f);
  fclose(f);
  return 0;
}

static int write_frame(const char *dir, const char *name, const relais_msg *m,
                       const relais_key *k) {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t n = relais_frame_encode(m, k, buf, sizeof buf);
  if (n == 0) {
    fprintf(stderr, "gen_corpus: cannot encode %s\n", name);
    return 1;
  }
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  return write_file(path, buf, n);
}

static void fill_statut(relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_STATUT;
  m->sender = RELAIS_NODE_ACTUATOR;
  m->tick = 42;
  m->mandate = 3;
  m->holder = RELAIS_NODE_A;
  m->source = RELAIS_SRC_HOLDER;
  m->misses = 0;
  m->certified = true;
  m->certified_node = RELAIS_NODE_B;
  m->lamp = RELAIS_LAMP_OFF;
  m->cause = RELAIS_CAUSE_NONE;
  m->meas.pos = 0.25f;
  m->meas.vel = 0.10f;
  m->meas.applied = 0.20f;
  m->meas.setpoint = 0.30f;
}

static void fill_order(relais_msg *m, relais_node who, float value, uint8_t flags) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = who;
  m->flags = flags;
  m->tick = 42;
  m->mandate = 3;
  m->value = value;
  m->has_order = true;
  m->testament_len = 3;
  m->testament[0] = value * 0.9f;
  m->testament[1] = value * 0.8f;
  m->testament[2] = value * 0.7f;
}

/* The frames of a nominal tick: the actuator's STATUT and the holder's ORDRE, at
 * both authentication levels (CRC-32C and keyed MAC), plus the bench TEST flag. */
static int seed_frames_nominal(const char *dir, const relais_key *key) {
  relais_msg m;
  int rc = 0;
  fill_statut(&m);
  rc |= write_frame(dir, "frame_statut_crc.bin", &m, NULL);
  rc |= write_frame(dir, "frame_statut_mac.bin", &m, key);
  fill_order(&m, RELAIS_NODE_A, 0.35f, 0u);
  rc |= write_frame(dir, "frame_ordre_crc.bin", &m, NULL);
  rc |= write_frame(dir, "frame_ordre_mac.bin", &m, key);
  fill_order(&m, RELAIS_NODE_A, 0.35f, RELAIS_FLAG_TEST);
  rc |= write_frame(dir, "frame_ordre_test_flag.bin", &m, NULL);
  return rc;
}

/* The frames of a handover: the second's shadow, its demande carrying a first
 * order (MAN-05), and the holder's cession, which takes effect next tick (REL-06). */
static int seed_frames_handover(const char *dir) {
  relais_msg m;
  int rc = 0;
  relais_msg_clear(&m);
  m.type = RELAIS_MSG_OMBRE;
  m.sender = RELAIS_NODE_B;
  m.tick = 42;
  m.mandate = 3;
  m.value = 0.35f;
  m.replica_age = 1;
  rc |= write_frame(dir, "frame_ombre.bin", &m, NULL);

  relais_msg_clear(&m);
  m.type = RELAIS_MSG_DEMANDE;
  m.sender = RELAIS_NODE_B;
  m.flags = RELAIS_FLAG_HAS_ORDER;
  m.tick = 43;
  m.mandate = 3;
  m.seen_mandate = 3;
  m.reason = 1;
  m.has_order = true;
  m.value = 0.10f;
  m.testament_len = 2;
  m.testament[0] = 0.09f;
  m.testament[1] = 0.08f;
  rc |= write_frame(dir, "frame_demande_with_order.bin", &m, NULL);

  relais_msg_clear(&m);
  m.type = RELAIS_MSG_CESSION;
  m.sender = RELAIS_NODE_A;
  m.tick = 44;
  m.mandate = 3;
  m.reason = 2;
  rc |= write_frame(dir, "frame_cession.bin", &m, NULL);
  return rc;
}

/* The frames that travel between the two computers (spec 4.9): the control
 * state to replicate (REP-01) and the replica's readiness. */
static int seed_frames_peer(const char *dir) {
  relais_msg m;
  relais_msg_clear(&m);
  m.type = RELAIS_MSG_ETAT;
  m.sender = RELAIS_NODE_A;
  m.tick = 44;
  m.mandate = 3;
  m.last_value = 0.20f;
  m.ctl_state_len = 12;
  for (uint8_t i = 0; i < m.ctl_state_len; i++) m.ctl_state[i] = (uint8_t)(i * 7u + 1u);
  int rc = write_frame(dir, "frame_etat.bin", &m, NULL);

  relais_msg_clear(&m);
  m.type = RELAIS_MSG_SUIVI;
  m.sender = RELAIS_NODE_B;
  m.tick = 44;
  m.replica_age = 1;
  m.ready = true;
  rc |= write_frame(dir, "frame_suivi.bin", &m, NULL);
  return rc;
}

static int seed_frames(const char *dir) {
  relais_key key;
  relais_key_from_hex(&key, KEY_HEX);
  int rc = seed_frames_nominal(dir, &key);
  rc |= seed_frames_handover(dir);
  rc |= seed_frames_peer(dir);
  return rc;
}

static int seed_journal(const char *dir) {
  relais_jr_header h;
  memset(&h, 0, sizeof h);
  relais_params_default(&h.params);
  h.mutant = 0;
  h.scenario_id = 7u;
  snprintf(h.scenario, sizeof h.scenario, "slalom-nominal");
  char buf[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_header(&h, buf, sizeof buf) <= 0) return 1;
  char path[1024];
  snprintf(path, sizeof path, "%s/journal_header.txt", dir);
  if (write_file(path, buf, strlen(buf)) != 0) return 1;

  relais_jr_tick t;
  memset(&t, 0, sizeof t);
  t.tick = 42;
  t.ns = 1700000000000000000ULL;
  t.mandate = 3;
  t.holder = RELAIS_NODE_A;
  t.source = RELAIS_SRC_HOLDER;
  t.value = 0.35f;
  t.pos = 0.25f;
  t.vel = 0.10f;
  t.setpoint = 0.30f;
  t.misses = 0;
  t.lamp = RELAIS_LAMP_OFF;
  t.cause = RELAIS_CAUSE_NONE;
  t.certified = true;
  t.certified_node = RELAIS_NODE_B;
  if (relais_journal_format_tick(&t, buf, sizeof buf) <= 0) return 1;
  snprintf(path, sizeof path, "%s/journal_tick.txt", dir);
  if (write_file(path, buf, strlen(buf)) != 0) return 1;

  relais_jr_event e;
  memset(&e, 0, sizeof e);
  e.tick = 43;
  e.ns = 1700000000005000000ULL;
  e.kind = RELAIS_EV_MANDATE_AUTO_SWITCH;
  e.node = RELAIS_NODE_B;
  e.mandate = 4;
  e.detail = 2;
  snprintf(e.text, sizeof e.text, "%s", relais_event_name(e.kind));
  if (relais_journal_format_event(&e, buf, sizeof buf) <= 0) return 1;
  snprintf(path, sizeof path, "%s/journal_event.txt", dir);
  return write_file(path, buf, strlen(buf));
}

/* Real simulator output: the first header, the first tick and a few distinct
 * events of a journal the simulator wrote. */
static int seed_from_journal_file(const char *dir, const char *journal) {
  FILE *f = fopen(journal, "r");
  if (f == NULL) {
    fprintf(stderr, "gen_corpus: cannot read %s\n", journal);
    return 1;
  }
  char line[RELAIS_JOURNAL_LINE_MAX];
  char path[1024];
  int ticks = 0, events = 0, rc = 0;
  while (fgets(line, (int)sizeof line, f) != NULL) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n == 0) continue;
    if (relais_journal_is_header(line) && ticks == 0 && events == 0) {
      snprintf(path, sizeof path, "%s/sim_header.txt", dir);
      rc |= write_file(path, line, n);
    } else if (relais_journal_is_tick(line) && ticks < 2) {
      snprintf(path, sizeof path, "%s/sim_tick_%d.txt", dir, ticks);
      rc |= write_file(path, line, n);
      ticks++;
    } else if (relais_journal_is_event(line) && events < 4) {
      snprintf(path, sizeof path, "%s/sim_event_%d.txt", dir, events);
      rc |= write_file(path, line, n);
      events++;
    }
    if (ticks >= 2 && events >= 4) break;
  }
  fclose(f);
  return rc;
}

static void put_rec(uint8_t *p, const rec_t *r) {
  p[0] = r->type;
  p[1] = r->sender;
  p[2] = r->flags;
  p[3] = r->mode;
  fuzz_script_put_f32(p + 4, r->value);
  p[8] = r->tlen;
  p[9] = r->age;
  p[10] = r->reason;
  p[11] = r->has_order;
  fuzz_script_put_u32(p + 12, r->seen);
  fuzz_script_put_f32(p + 16, r->t0);
  fuzz_script_put_f32(p + 20, r->t1);
}

static void put_meas(uint8_t *p, const float *meas) {
  for (int i = 0; i < 4; i++) fuzz_script_put_f32(p + (size_t)i * 4u, meas[i]);
}

/* One nominal tick: the holder's order and the second's shadow, both aligned so
 * the arbiter accepts them and the certification streak grows (REP-05). */
static size_t put_order_shadow(uint8_t *buf, size_t at, int step) {
  rec_t r;
  memset(&r, 0, sizeof r);
  r.type = 2u; /* ORDRE */
  r.sender = 1u;
  r.mode = 1u;
  r.value = 0.30f + (float)step * 0.01f;
  r.tlen = 3u;
  r.t0 = 0.28f;
  r.t1 = 0.27f;
  put_rec(buf + at, &r);
  at += FUZZ_SCRIPT_REC;
  memset(&r, 0, sizeof r);
  r.type = 3u; /* OMBRE */
  r.sender = 2u;
  r.mode = 1u;
  r.value = 0.30f + (float)step * 0.01f;
  r.age = 1u;
  put_rec(buf + at, &r);
  return at + FUZZ_SCRIPT_REC;
}

/* The nominal life of a mandate: a demande granted on the actuator's own
 * evidence, eight ticks of order + shadow, then silence and revocation (MAN-04). */
static size_t script_nominal(uint8_t *buf) {
  static const float meas[4] = {0.25f, 0.10f, 0.20f, 0.30f};
  rec_t r;
  size_t at = 0;
  memset(&r, 0, sizeof r);
  r.type = 4u; /* DEMANDE */
  r.sender = 1u;
  r.mode = 1u; /* aligned: granted on the actuator's own evidence */
  r.has_order = 1u;
  r.value = 0.20f;
  r.tlen = 2u;
  r.t0 = 0.19f;
  r.t1 = 0.18f;
  buf[at++] = 1u; /* one message */
  put_rec(buf + at, &r);
  at += FUZZ_SCRIPT_REC;
  put_meas(buf + at, meas);
  at += FUZZ_SCRIPT_MEAS;
  for (int step = 0; step < 12; step++) {
    buf[at++] = step < 8 ? 2u : 0u;
    if (step < 8) at = put_order_shadow(buf, at, step);
    put_meas(buf + at, meas);
    at += FUZZ_SCRIPT_MEAS;
  }
  return at;
}

/* A hostile script: unaligned messages, stale and future mandates, demande
 * floods (SEC-06), cessions from a node that is not the holder. */
static size_t script_hostile(uint8_t *buf) {
  static const float meas[4] = {0.25f, 0.10f, 0.20f, 0.30f};
  size_t at = 0;
  for (int step = 0; step < 6; step++) {
    buf[at++] = 4u;
    for (int i = 0; i < 4; i++) {
      rec_t r;
      memset(&r, 0, sizeof r);
      r.type = (uint8_t)(1u + ((unsigned)step + (unsigned)i) % 7u);
      r.sender = (uint8_t)(1u + ((unsigned)i % 4u));
      r.flags = (uint8_t)(step * 3 + i);
      r.mode = 0u; /* not aligned: stale or future mandates */
      r.value = 5.0f * (float)(i - 2);
      r.tlen = 8u;
      r.age = (uint8_t)(step + i);
      r.reason = (uint8_t)i;
      r.has_order = 1u;
      r.seen = (uint32_t)(step * 1000 + i);
      put_rec(buf + at, &r);
      at += FUZZ_SCRIPT_REC;
    }
    put_meas(buf + at, meas);
    at += FUZZ_SCRIPT_MEAS;
  }
  return at;
}

static int seed_scripts(const char *dir) {
  static uint8_t buf[4096];
  char path[1024];
  snprintf(path, sizeof path, "%s/arbiter_nominal_life.bin", dir);
  if (write_file(path, buf, script_nominal(buf)) != 0) return 1;
  snprintf(path, sizeof path, "%s/arbiter_hostile_flood.bin", dir);
  return write_file(path, buf, script_hostile(buf));
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: gen_corpus OUT_DIR [--journal FILE]\n");
    return 2;
  }
  const char *out = argv[1];
  const char *journal = NULL;
  for (int i = 2; i < argc; i++)
    if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) journal = argv[++i];

  char dir[256];
  const char *subs[] = {"frame", "journal", "arbiter"};
  mkdir(out, 0755);
  for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
    snprintf(dir, sizeof dir, "%s/%s", out, subs[i]);
    mkdir(dir, 0755);
  }

  int rc = 0;
  snprintf(dir, sizeof dir, "%s/frame", out);
  rc |= seed_frames(dir);
  snprintf(dir, sizeof dir, "%s/journal", out);
  rc |= seed_journal(dir);
  if (journal != NULL) rc |= seed_from_journal_file(dir, journal);
  snprintf(dir, sizeof dir, "%s/arbiter", out);
  rc |= seed_scripts(dir);
  printf("gen_corpus: seeds written under %s (%s)\n", out, rc == 0 ? "ok" : "errors");
  return rc == 0 ? 0 : 1;
}
