/* Relais — the runner of the measured comparison (mission M43).
 *
 * One process plays one scenario, one implementation and N seeds, and prints
 * one JSON object per seed. The Python driver (host/baseline/compare.py)
 * aggregates them into the tables of docs/COMPARISON-MEASURED.md.
 *
 * Usage: relais-baseline --impl relais|heartbeat|lease [--ticks N] [--seed S]
 *                         [--reps N] [--timeout N] [--window N] [--loss P]
 *                         [--delay N] [--fault at:kind:node[:arg]]...
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const bl_impl *bl_impl_by_name(const char *name) {
  if (name == NULL) return NULL;
  if (strcmp(name, "relais") == 0) return bl_impl_relais();
  if (strcmp(name, "heartbeat") == 0) return bl_impl_heartbeat();
  if (strcmp(name, "lease") == 0) return bl_impl_lease();
  return NULL;
}

static void usage(void) {
  fprintf(stderr,
          "usage: relais-baseline --impl relais|heartbeat|lease [--ticks N] [--seed S]\n"
          "       [--reps N] [--timeout N] [--window N] [--loss P] [--delay N]\n"
          "       [--fault at:kind:node[:arg]]...\n");
}

static void print_metrics(const bl_config *cfg, uint64_t seed, const bl_metrics *m) {
  long handover = m->handover_done ? (long)m->handover_ticks : -1L;
  long fresh = m->fresh_done ? (long)m->fresh_ticks : -1L;
  printf("{\"impl\":\"%s\",\"seed\":%llu,\"timeout\":%u,"
         "\"handover\":%ld,\"fresh\":%ld,\"stale\":%u,\"double\":%u,\"split\":%u,"
         "\"gap\":%.6f,\"track\":%.6f,\"jump\":%.6f,\"hjump\":%.6f,\"rejected\":%u,\"rejected_total\":%u}\n",
         cfg->impl->name, (unsigned long long)seed, cfg->timeout, handover, fresh,
         m->stale_ticks, m->double_cmds, m->split_cmds, (double)m->twin_gap_max,
         (double)m->track_err_max, (double)m->cmd_jump_max, (double)m->handover_jump,
         m->counters[0], m->counters[1]);
}

int main(int argc, char **argv) {
  bl_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.impl = bl_impl_relais();
  cfg.ticks = 4000u;
  cfg.seed = 1u;
  cfg.timeout = 20u;
  cfg.window = 200u;
  cfg.loss = 0.0;
  cfg.burst_guard = true;
  cfg.delay = 0u;
  const char *impl_name = "relais";
  uint32_t reps = 1u;
  uint32_t jitter = 0u;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--impl") == 0 && i + 1 < argc) impl_name = argv[++i];
    else if (strcmp(a, "--ticks") == 0 && i + 1 < argc) cfg.ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--seed") == 0 && i + 1 < argc) cfg.seed = strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--reps") == 0 && i + 1 < argc) reps = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--timeout") == 0 && i + 1 < argc) cfg.timeout = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--at") == 0 && i + 1 < argc) cfg.measure_at = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--window") == 0 && i + 1 < argc) cfg.window = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--loss") == 0 && i + 1 < argc) cfg.loss = strtod(argv[++i], NULL);
    else if (strcmp(a, "--jitter") == 0 && i + 1 < argc) jitter = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--burst") == 0 && i + 1 < argc) cfg.burst_guard = strtoul(argv[++i], NULL, 10) != 0;
    else if (strcmp(a, "--delay") == 0 && i + 1 < argc) cfg.delay = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--trace") == 0) bl_trace_enable(true);
    else if (strcmp(a, "--fault") == 0 && i + 1 < argc) {
      if (!bl_fault_parse(&cfg, argv[++i])) {
        fprintf(stderr, "faute ignoree : %s\n", argv[i]);
        return 2;
      }
    } else {
      usage();
      return 2;
    }
  }
  cfg.impl = bl_impl_by_name(impl_name);
  if (cfg.impl == NULL) {
    fprintf(stderr, "implementation inconnue : %s\n", impl_name);
    return 2;
  }
  if (reps == 0) reps = 1u;
  for (uint32_t r = 0; r < reps; r++) {
    /* The jitter sweeps the instant of the fault over the cycle, the way the
     * campaign of docs/RESULTS.md sweeps it. */
    bl_config run_cfg = cfg;
    run_cfg.seed = cfg.seed + r;
    if (jitter > 0) {
      uint32_t shift = (uint32_t)((run_cfg.seed * 37u) % jitter);
      for (size_t i = 0; i < run_cfg.fault_count; i++) run_cfg.faults[i].at += shift;
    }
    bl_metrics m;
    bl_run(&run_cfg, &m);
    print_metrics(&run_cfg, run_cfg.seed, &m);
  }
  return 0;
}
