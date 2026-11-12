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
/* A chained line is the record plus ,"chain":"<16 hex>", so it needs a little
 * more room than the bare record. */
#define RELAIS_JOURNAL_CHAIN_LINE_MAX (RELAIS_JOURNAL_LINE_MAX + 32)

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

/* ------------------------------------------------------- chained journal (34) */
/* Every record carries the fingerprint of the record before it plus its own
 * content, so deleting, inserting or editing a record afterwards breaks the
 * chain and the break is located. The field is the last one of the object:
 *
 *   {"kind":"tick",...,"stop":false,"chain":"9a1f0c4d77b2e310"}
 *
 * The fingerprint is 8 bytes, written as 16 lower-case hexadecimal digits. A
 * journal whose records carry no "chain" stays readable: the field is optional,
 * and readers that do not know it ignore it.
 *
 * key is the chain key; NULL selects the built-in public one (see SECURITY.md
 * for what that does and does not protect against).
 */
#define RELAIS_CHAIN_LEN 8u
#define RELAIS_CHAIN_HEX 16u
#define RELAIS_CHAIN_FIELD "\"chain\":\""

typedef struct {
  uint8_t bytes[RELAIS_CHAIN_LEN];
} relais_chain;

typedef enum {
  RELAIS_CHAIN_OK = 0,     /* the record continues the chain */
  RELAIS_CHAIN_BREAK = 1,  /* wrong, malformed or missing fingerprint */
  RELAIS_CHAIN_ABSENT = 2  /* no fingerprint: a record of an older journal */
} relais_chain_status;

void relais_chain_genesis(relais_chain *c);
/* Fold one record body (the line without its chain field) into the chain. */
void relais_chain_advance(relais_chain *c, const char *body, size_t len,
                          const relais_key *key);
int relais_chain_to_hex(const relais_chain *c, char *out, size_t cap);
bool relais_chain_from_hex(relais_chain *c, const char *hex);
bool relais_chain_equal(const relais_chain *a, const relais_chain *b);
/* Read the fingerprint a line carries; false when it carries none. */
bool relais_chain_read(const char *line, relais_chain *out);
/* Copy the line without its chain field: the body the fingerprint covers. */
int relais_chain_body(const char *line, char *buf, size_t cap);

/* Format a record and chain it in one call. On success buf holds the line to
 * write and *chain holds the fingerprint of that line, ready for the next one. */
int relais_journal_chain_header(const relais_jr_header *h, relais_chain *chain,
                                const relais_key *key, char *buf, size_t cap);
int relais_journal_chain_tick(const relais_jr_tick *r, relais_chain *chain,
                              const relais_key *key, char *buf, size_t cap);
int relais_journal_chain_event(const relais_jr_event *r, relais_chain *chain,
                               const relais_key *key, char *buf, size_t cap);

/* Reading side: walk a journal in file order and locate the first break. */
typedef struct {
  relais_chain expected; /* fingerprint the next record must carry */
  size_t records;        /* records examined */
  size_t chained;        /* records that carried a fingerprint */
  size_t absent;         /* records that carried none */
  bool started;          /* a chained record has been seen */
} relais_chain_reader;

void relais_chain_reader_init(relais_chain_reader *r);
/* Examine one line. A trailing newline is ignored. why receives a short
 * English explanation of the first break and may be NULL. */
relais_chain_status relais_chain_reader_step(relais_chain_reader *r, const char *line,
                                             const relais_key *key, char *why,
                                             size_t why_cap);

#endif /* RELAIS_JOURNAL_H */
