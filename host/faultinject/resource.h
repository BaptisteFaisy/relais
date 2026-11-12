/* Relais — resource exhaustion faults (ROB-02, OBS-02, MAN-04): a full frame
 * queue, a saturated journal buffer, and a failing allocator on a path that is
 * not supposed to allocate. Each stays SAFE: the caller is told, nothing is
 * silently dropped, and the core keeps running without allocation.
 */
#ifndef RELAIS_FAULTINJECT_RESOURCE_H
#define RELAIS_FAULTINJECT_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relais/frame.h"
#include "relais/journal.h"

/* A bounded queue of decoded frames. When it is full the push is refused and
 * counted, so a saturated queue is a loud condition (ROB-02), not a silent
 * drop. */
typedef struct {
  relais_msg *buf;   /* caller storage, cap entries */
  uint32_t cap;
  uint32_t head;     /* next entry to pop */
  uint32_t count;    /* entries held */
  uint32_t dropped;  /* pushes refused because full */
} fi_queue;

void fi_queue_init(fi_queue *q, relais_msg *buf, uint32_t cap);
bool fi_queue_push(fi_queue *q, const relais_msg *m);
bool fi_queue_pop(fi_queue *q, relais_msg *m);
uint32_t fi_queue_count(const fi_queue *q);
uint32_t fi_queue_dropped(const fi_queue *q);

/* A fixed journal buffer. A line that does not fit returns false and is
 * counted, so a saturated journal is reported (OBS-02), never silent. */
typedef struct {
  char *buf;       /* caller storage */
  size_t cap;
  size_t used;
  uint32_t overflows;
} fi_journal;

void fi_journal_init(fi_journal *j, char *buf, size_t cap);
bool fi_journal_write_tick(fi_journal *j, const relais_jr_tick *r);
bool fi_journal_write_event(fi_journal *j, const relais_jr_event *e);

/* A counting allocator used by the host-side helpers, and only by them. The
 * core never calls it: a low-memory fault on a path that is not supposed to
 * allocate is a no-op for safety, which the test turns into a lemma (MAN-04). */
typedef struct {
  uint64_t alloc_calls;
  uint64_t live;
  int fail_after;   /* refuse while live >= fail_after; <= 0 never fails */
} fi_mem;

void fi_mem_init(fi_mem *m, int fail_after);
void *fi_mem_alloc(fi_mem *m, size_t sz);
void fi_mem_free(fi_mem *m, void *p, size_t sz);

#endif /* RELAIS_FAULTINJECT_RESOURCE_H */