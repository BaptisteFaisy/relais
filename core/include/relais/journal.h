/* Relais — the actuator's journal (spec 6.4). One record per tick and one per
 * event, JSON lines at level S. The journal is the authority the verifier
 * reads, so formatting and parsing live here, once, and are shared by the PC
 * actuator, the QNX actuator and relais-verify.
 *
 * Formatting is pure: it fills a caller buffer. Writing it out is the caller's
 * job (a file on PC, slog2 on QNX).
 */
#ifndef RELAIS_JOURNAL_H
#define RELAIS_JOURNAL_H

#include "relais/frame.h"
#include "relais/params.h"

#define RELAIS_JOURNAL_LINE_MAX 1024

typedef struct {
  relais_tick tick;
  uint64_t ns;            /* host timestamp, filled by the writer, 0 if none */
  relais_mandate mandate;
  relais_node holder;
  relais_source source;
  float value;
  float pos;
  float vel;
  float setpoint;
  uint8_t misses;
  relais_lamp lamp;
  relais_cause cause;
  bool certified;
  relais_node certified_node;
  bool stop_requested;
} relais_jr_tick;

typedef struct {
  relais_tick tick;
  uint64_t ns;
  relais_event_kind kind;
  relais_node node;
  relais_mandate mandate;
  uint32_t detail;
  char text[64];          /* optional detail, e.g. the reject reason */
} relais_jr_event;

typedef struct {
  relais_params params;
  relais_node holder_a;
  relais_node holder_b;
  int mutant;
  uint32_t scenario_id;
  char scenario[64];
} relais_jr_header;

int relais_journal_format_header(const relais_jr_header *h, char *buf, size_t cap);
int relais_journal_format_tick(const relais_jr_tick *r, char *buf, size_t cap);
int relais_journal_format_event(const relais_jr_event *r, char *buf, size_t cap);

/* Parsing, for relais-verify. Returns false on a malformed line. */
bool relais_journal_parse_header(const char *line, relais_jr_header *h);
bool relais_journal_parse_tick(const char *line, relais_jr_tick *r);
bool relais_journal_parse_event(const char *line, relais_jr_event *r);
/* True when the line is a record of that kind ("kind":"tick" / "event" / "header"). */
bool relais_journal_is_tick(const char *line);
bool relais_journal_is_event(const char *line);
bool relais_journal_is_header(const char *line);

#endif /* RELAIS_JOURNAL_H */
