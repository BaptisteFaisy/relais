/* Relais — see journal_writer.h. */
#include "journal_writer.h"

#include <string.h>

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

void relais_jw_write_header(FILE *f, const relais_params *params, const char *scenario,
                            uint32_t scenario_id) {
  if (f == NULL || params == NULL) return;
  relais_jr_header h;
  memset(&h, 0, sizeof h);
  h.params = *params;
  h.mutant = relais_mutant_id();
  h.scenario_id = scenario_id;
  snprintf(h.scenario, sizeof h.scenario, "%s", scenario != NULL ? scenario : "actuator");
  char line[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_header(&h, line, sizeof line) > 0) fprintf(f, "%s\n", line);
}

void relais_jw_write_tick(FILE *f, const relais_arb_out *out, relais_tick tick,
                          const relais_plant *plant, float setpoint) {
  if (f == NULL) return;
  relais_jr_tick r;
  relais_jw_fill_tick(&r, out, tick, plant, setpoint);
  char line[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_tick(&r, line, sizeof line) > 0) fprintf(f, "%s\n", line);
}

void relais_jw_write_events(FILE *f, const relais_arb_out *out, relais_tick tick) {
  if (f == NULL || out == NULL) return;
  for (uint8_t i = 0; i < out->event_count; i++) {
    relais_jr_event e;
    memset(&e, 0, sizeof e);
    e.tick = tick;
    e.kind = out->events[i].kind;
    e.node = out->events[i].node;
    e.mandate = out->events[i].mandate;
    e.detail = out->events[i].detail;
    snprintf(e.text, sizeof e.text, "%s", relais_event_name(e.kind));
    char line[RELAIS_JOURNAL_LINE_MAX];
    if (relais_journal_format_event(&e, line, sizeof line) > 0) fprintf(f, "%s\n", line);
  }
}
