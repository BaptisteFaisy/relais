/* Relais — resource exhaustion faults: implementation.
 *
 * Each helper is a thin, verified shell around caller-provided storage. The one
 * rule repeated in all of them: a refusal is always counted and returned, so
 * there is never an unreported silence (ROB-02, OBS-02, MAN-04). The core
 * protocol itself stays allocation free, so the memory helper below exists only
 * for the host-side plumbing, not for anything the safety argument relies on.
 */
#include "resource.h"

#include <stdlib.h>
#include <string.h>

/* ---- frame queue -------------------------------------------------------- */

void fi_queue_init(fi_queue *q, relais_msg *buf, uint32_t cap) {
  if (q == NULL) return;
  memset(q, 0, sizeof *q);
  q->buf = buf;
  q->cap = cap;
}

bool fi_queue_push(fi_queue *q, const relais_msg *m) {
  if (q == NULL || m == NULL || q->buf == NULL || q->cap == 0) return false;
  if (q->count >= q->cap) {
    q->dropped++;
    return false;
  }
  uint32_t at = q->head + q->count;
  if (at >= q->cap) at -= q->cap;
  q->buf[at] = *m;
  q->count++;
  return true;
}

bool fi_queue_pop(fi_queue *q, relais_msg *m) {
  if (q == NULL || q->buf == NULL || q->count == 0) return false;
  if (m != NULL) *m = q->buf[q->head];
  q->head++;
  if (q->head >= q->cap) q->head = 0;
  q->count--;
  return true;
}

uint32_t fi_queue_count(const fi_queue *q) { return q != NULL ? q->count : 0; }
uint32_t fi_queue_dropped(const fi_queue *q) { return q != NULL ? q->dropped : 0; }

/* ---- journal buffer ----------------------------------------------------- */

void fi_journal_init(fi_journal *j, char *buf, size_t cap) {
  if (j == NULL) return;
  memset(j, 0, sizeof *j);
  j->buf = buf;
  j->cap = cap;
}

static bool fi_journal_line(fi_journal *j, const char *line) {
  if (j == NULL || line == NULL) return false;
  size_t need = strlen(line) + 1u;
  if (j->buf == NULL || j->used + need > j->cap) {
    j->overflows++;
    return false;
  }
  memcpy(j->buf + j->used, line, need);
  j->used += need - 1u;
  return true;
}

bool fi_journal_write_tick(fi_journal *j, const relais_jr_tick *r) {
  if (r == NULL) return false;
  char line[RELAIS_JOURNAL_LINE_MAX];
  int n = relais_journal_format_tick(r, line, sizeof line);
  if (n <= 0) return false;
  return fi_journal_line(j, line);
}

bool fi_journal_write_event(fi_journal *j, const relais_jr_event *e) {
  if (e == NULL) return false;
  char line[RELAIS_JOURNAL_LINE_MAX];
  int n = relais_journal_format_event(e, line, sizeof line);
  if (n <= 0) return false;
  return fi_journal_line(j, line);
}

/* ---- counting allocator ------------------------------------------------- */

void fi_mem_init(fi_mem *m, int fail_after) {
  if (m == NULL) return;
  memset(m, 0, sizeof *m);
  m->fail_after = fail_after;
}

void *fi_mem_alloc(fi_mem *m, size_t sz) {
  if (m == NULL || sz == 0) return NULL;
  m->alloc_calls++;
  if (m->fail_after > 0 && m->live >= (uint64_t)m->fail_after) return NULL;
  void *p = malloc(sz);
  if (p != NULL) m->live += sz;
  return p;
}

void fi_mem_free(fi_mem *m, void *p, size_t sz) {
  if (m == NULL || p == NULL) return;
  free(p);
  if (m->live >= sz) m->live -= sz;
  else m->live = 0;
}