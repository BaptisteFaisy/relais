/* Relais — see journal_writer.h. */
#include "journal_writer.h"

#include <string.h>

void relais_jw_open(relais_jw *w, FILE *f, const relais_key *chain_key) {
  if (w == NULL) return;
  memset(w, 0, sizeof *w);
  w->f = f;
  relais_chain_genesis(&w->chain);
  relais_key_clear(&w->key); /* not enabled: the core then uses its default key */
  if (chain_key != NULL && chain_key->enabled) w->key = *chain_key;
}

void relais_jw_fill_tick(relais_jr_tick *r, const relais_arb_out *out, relais_tick tick,
                         const relais_plant *plant, float setpoint) {
  if (r == NULL || out == NULL) return;
  memset(r, 0, sizeof *r);
  r->tick = tick;
  r->mandate = out->mandate;
  r->holder = out->holder;
  r->source = out->source;
  r->value = out->value;
  r->pos = plant != NULL ? plant->pos : 0.0f;
  r->vel = plant != NULL ? plant->vel : 0.0f;
  r->setpoint = setpoint;
  r->misses = out->misses;
  r->lamp = out->lamp;
  r->cause = out->cause;
  r->certified = out->certified;
  r->certified_node = out->certified_node;
  r->stop_requested = out->stop_requested;
}

/* One formatted, chained line out to the stream: the single place a journal
 * record reaches a file, so the chain can never skip a record. */
static void jw_emit(relais_jw *w, const char *line) {
  if (w == NULL || w->f == NULL || line == NULL) return;
  fprintf(w->f, "%s\n", line);
  w->records++;
}

int relais_jw_anchor(const relais_jw *w, char *out, size_t cap) {
  if (w == NULL) return -1;
  return relais_chain_to_hex(&w->chain, out, cap);
}

void relais_jw_write_header(relais_jw *w, const relais_params *params, const char *scenario,
                            uint32_t scenario_id) {
  if (w == NULL || params == NULL) return;
  relais_jr_header h;
  memset(&h, 0, sizeof h);
  h.params = *params;
  h.mutant = relais_mutant_id();
  h.scenario_id = scenario_id;
  snprintf(h.scenario, sizeof h.scenario, "%s", scenario != NULL ? scenario : "actuator");
  char line[RELAIS_JOURNAL_CHAIN_LINE_MAX];
  if (relais_journal_chain_header(&h, &w->chain, &w->key, line, sizeof line) > 0)
    jw_emit(w, line);
}

void relais_jw_write_tick(relais_jw *w, const relais_arb_out *out, relais_tick tick,
                          const relais_plant *plant, float setpoint) {
  if (w == NULL) return;
  relais_jr_tick r;
  relais_jw_fill_tick(&r, out, tick, plant, setpoint);
  char line[RELAIS_JOURNAL_CHAIN_LINE_MAX];
  if (relais_journal_chain_tick(&r, &w->chain, &w->key, line, sizeof line) > 0)
    jw_emit(w, line);
}

void relais_jw_write_events(relais_jw *w, const relais_arb_out *out, relais_tick tick) {
  if (w == NULL || out == NULL) return;
  for (uint8_t i = 0; i < out->event_count; i++) {
    relais_jr_event e;
    memset(&e, 0, sizeof e);
    e.tick = tick;
    e.kind = out->events[i].kind;
    e.node = out->events[i].node;
    e.mandate = out->events[i].mandate;
    e.detail = out->events[i].detail;
    snprintf(e.text, sizeof e.text, "%s", relais_event_name(e.kind));
    char line[RELAIS_JOURNAL_CHAIN_LINE_MAX];
    if (relais_journal_chain_event(&e, &w->chain, &w->key, line, sizeof line) > 0)
      jw_emit(w, line);
  }
}
