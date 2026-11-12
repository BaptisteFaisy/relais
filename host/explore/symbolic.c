/* Relais — the projection of a world onto its equivalence class. See
 * symbolic.h for the full safety argument of what is kept and what is dropped. */
#include "symbolic.h"

#include <string.h>

/* FNV-1a over the class bytes. Same primitive the brute-force hash uses; kept
 * local so symbolic.c stays self-contained (world.c's is static). */
static uint64_t fnv(const void *data, size_t len) {
  const uint64_t prime = 0x100000001b3ull;
  uint64_t h = 0xcbf29ce484222325ull;
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= prime;
  }
  return h;
}

static sym_node project_node(const relais_node_sm *s) {
  sym_node n;
  memset(&n, 0, sizeof n);
  n.role = s->role;
  n.seen_holder = s->seen_holder;
  n.mandate = s->mandate;
  n.seen_mandate = s->seen_mandate;
  n.last_statut = s->last_statut;
  n.last_etat = s->last_etat;
  n.replica_age = s->replica_age;
  n.bail_ticks = s->bail_ticks;
  n.link_lost_ticks = s->link_lost_ticks;
  n.ready = s->ready;
  n.link_lost = s->link_lost;
  n.deposed = s->deposed;
  n.without_actuator = s->without_actuator;
  return n;
}

void sym_project(const explore_world *w, sym_key *k) {
  memset(k, 0, sizeof *k);
  k->tick = w->tick;
  k->source = w->source;
  k->arb = w->arb;   /* whole actuator protocol state: mandate, holder, misses,
                      * certification, testament horizon, lamp, liveness */
  k->inv = w->inv;   /* whole accumulator: the memory of every safety check */
  for (int i = 0; i < EXPLORE_NODES; i++) {
    k->node[i] = project_node(&w->node[i]);
    k->dead[i] = w->dead[i];
    k->frozen_until[i] = w->frozen_until[i];
  }
  for (int l = 0; l < EXPLORE_LINKS; l++) {
    k->cut[l] = w->cut[l];
    for (int s = 0; s < EXPLORE_SLOTS; s++) {
      k->slot[l][s].busy = w->slot[l][s].busy;
      k->slot[l][s].deliver_at = w->slot[l][s].deliver_at;
    }
  }
  k->channel_link = w->channel_link;
  k->burst_this_tick = w->burst_this_tick;
  k->pending_mutations = w->pending_mutations;
}

uint64_t sym_hash(const explore_world *w) {
  sym_key k;
  sym_project(w, &k);
  return fnv(&k, sizeof k);
}

size_t sym_key_bytes(void) { return sizeof(sym_key); }
