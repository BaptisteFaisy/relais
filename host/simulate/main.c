/* Relais — the seeded simulator, level 2 of the validation plan (spec 7.4).
 *
 * The same core drives the two computers and the actuator. A seed fixes every
 * draw, so a run is reproducible; the twin without fault runs in parallel and
 * gives the reference the continuity invariant is measured on.
 *
 * Usage: relais-simulate [--ticks N] [--seed S] [--params "T=5,p=1,N=8"]
 *                        [--trajectory "kind=slalom amp=0.6 freq=0.5"]
 *                        [--fault at:kind:node[:arg[:value]]]...
 *                        [--level socle|complet] [--journal f] [--twin-journal f]
 *                        [--loss 0.0] [--delay 0] [--auto-faults N]
 * Exit code: 0 when no invariant is violated, 1 when one is.
 */
#include "sim.h"

#include <stdlib.h>
#include <string.h>

static void usage(void) {
  fprintf(stderr,
          "usage: relais-simulate [--ticks N] [--seed S] [--params SPEC]\n"
          "       [--trajectory SPEC] [--fault at:kind:node[:arg[:value]]]\n"
          "       [--level socle|complet] [--journal FILE] [--twin-journal FILE]\n"
          "       [--loss 0.0] [--delay N] [--auto-faults N] [--quiet]\n");
}

int main(int argc, char **argv) {
  uint32_t ticks = 100000;
  uint64_t seed = 1;
  uint32_t auto_faults = 0;
  uint32_t restart_actuator_at = 0;
  bool complete = false;
  bool quiet = false;
  double loss = 0.0;
  uint32_t delay = 0;
  const char *journal = NULL;
  const char *twin_journal = NULL;
  const char *scenario = "simulate";
  uint32_t scenario_id = 0;
  char param_spec[128] = "";
  char traj_spec[192] = "";
  char why[128] = "";

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--ticks") == 0 && i + 1 < argc) ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--params") == 0 && i + 1 < argc)
      snprintf(param_spec, sizeof param_spec, "%s", argv[++i]);
    else if (strcmp(a, "--trajectory") == 0 && i + 1 < argc)
      snprintf(traj_spec, sizeof traj_spec, "%s", argv[++i]);
    else if (strcmp(a, "--fault") == 0 && i + 1 < argc) {
      /* kept for sim_add_fault below, after the sim exists */
      static char faults[SIM_MAX_FAULTS][96];
      static int fault_count = 0;
      if (fault_count < SIM_MAX_FAULTS)
        snprintf(faults[fault_count++], sizeof faults[0], "%s", argv[++i]);
      else i++;
    } else if (strcmp(a, "--level") == 0 && i + 1 < argc) complete = strcmp(argv[++i], "complet") == 0;
    else if (strcmp(a, "--journal") == 0 && i + 1 < argc) journal = argv[++i];
    else if (strcmp(a, "--twin-journal") == 0 && i + 1 < argc) twin_journal = argv[++i];
    else if (strcmp(a, "--loss") == 0 && i + 1 < argc) loss = strtod(argv[++i], NULL);
    else if (strcmp(a, "--delay") == 0 && i + 1 < argc) delay = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--restart-actuator") == 0 && i + 1 < argc)
      restart_actuator_at = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--auto-faults") == 0 && i + 1 < argc)
      auto_faults = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--scenario") == 0 && i + 1 < argc) scenario = argv[++i];
    else if (strcmp(a, "--id") == 0 && i + 1 < argc) scenario_id = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--quiet") == 0) quiet = true;
    else { usage(); return 2; }
  }

  /* The faults are collected in a second pass, the sim does not exist yet. */
  char fault_specs[SIM_MAX_FAULTS][96];
  size_t fault_spec_count = 0;
  for (int i = 1; i < argc && fault_spec_count < SIM_MAX_FAULTS; i++)
    if (strcmp(argv[i], "--fault") == 0 && i + 1 < argc)
      snprintf(fault_specs[fault_spec_count++], sizeof fault_specs[0], "%s", argv[i + 1]);

  relais_params params;
  relais_params_default(&params);
  if (param_spec[0] != '\0' && !relais_params_parse(&params, param_spec, why, sizeof why)) {
    fprintf(stderr, "parametres invalides: %s\n", why);
    return 2;
  }
  relais_traj traj;
  relais_traj_default(&traj);
  if (traj_spec[0] != '\0' && !relais_traj_parse(&traj, traj_spec, why, sizeof why)) {
    fprintf(stderr, "trajectoire invalide: %s\n", why);
    return 2;
  }

  sim_t world;
  sim_t twin;
  sim_init(&world, &params, &traj, seed, complete);
  sim_init(&twin, &params, &traj, seed, complete);
  world.loss = loss;
  world.max_delay = delay;
  twin.loss = 0.0;
  twin.max_delay = 0;

  for (size_t i = 0; i < fault_spec_count; i++) {
    if (!sim_parse_fault(&world, fault_specs[i]))
      fprintf(stderr, "faute ignoree (syntaxe) : %s\n", fault_specs[i]);
  }
  if (auto_faults > 0)
    sim_seed_random_faults(&world, auto_faults, ticks / 4, ticks / 2);

  FILE *fj = journal != NULL ? fopen(journal, "w") : NULL;
  FILE *ft = twin_journal != NULL ? fopen(twin_journal, "w") : NULL;
  if (fj != NULL) sim_write_header(fj, &world, scenario, scenario_id);
  if (ft != NULL) sim_write_header(ft, &twin, scenario, scenario_id);

  relais_inv inv;
  relais_inv_init(&inv, &params);
  relais_inv_expect_certified(&inv, complete);
  relais_inv_fault_free(&inv, world.fault_count == 0);
  relais_inv_lossy_channel(&inv, loss > 0.0 || delay > 0);
  for (size_t i = 0; i < fault_spec_count; i++) {
    if (strstr(fault_specs[i], "bias") == NULL) continue;
    uint32_t at = (uint32_t)strtoul(fault_specs[i], NULL, 10);
    relais_inv_value_fault(&inv, true, (relais_tick)at);
  }

  for (uint32_t tick = 1; tick <= ticks; tick++) {
    if (sim_apply_faults(&world, tick) > 1) relais_inv_double_fault(&inv, tick);
    if (restart_actuator_at != 0 && tick == restart_actuator_at) {
      /* MAN-11: the actuator restarts, the mandate number must survive. */
      uint8_t snap[64];
      size_t len = relais_arb_snapshot(&world.arb, snap, sizeof snap);
      relais_arb_init(&world.arb, &params);
      if (len > 0) relais_arb_restore(&world.arb, snap, len);
    }
    relais_arb_out out;
    relais_arb_out twin_out;
    sim_run_tick(&world, tick, &out);
    sim_run_tick(&twin, tick, &twin_out);

    relais_jr_tick record;
    memset(&record, 0, sizeof record);
    record.tick = tick;
    record.mandate = out.mandate;
    record.holder = out.holder;
    record.source = out.source;
    record.value = out.value;
    record.pos = world.plant.pos;
    record.vel = world.plant.vel;
    record.setpoint = out.statut.meas.setpoint;
    record.misses = out.misses;
    record.lamp = out.lamp;
    record.cause = out.cause;
    record.certified = out.certified;
    record.certified_node = out.certified_node;
    record.stop_requested = out.stop_requested;
    float twin_value = twin_out.value;
    for (uint8_t e = 0; e < out.event_count; e++) {
      relais_jr_event ev;
      memset(&ev, 0, sizeof ev);
      ev.tick = tick;
      ev.kind = out.events[e].kind;
      ev.node = out.events[e].node;
      ev.mandate = out.events[e].mandate;
      ev.detail = out.events[e].detail;
      snprintf(ev.text, sizeof ev.text, "%s", relais_event_name(ev.kind));
      relais_inv_on_event(&inv, &ev);
    }
    relais_inv_on_tick(&inv, &record, &twin_value);
    relais_inv_end_tick(&inv, tick);

    if (fj != NULL) {
      sim_write_events(fj, &out, tick);
      sim_write_tick(fj, &out, tick, &world.plant, out.statut.meas.setpoint);
    }
    if (ft != NULL) {
      sim_write_events(ft, &twin_out, tick);
      sim_write_tick(ft, &twin_out, tick, &twin.plant, twin_out.statut.meas.setpoint);
    }
  }
  if (fj != NULL) fclose(fj);
  if (ft != NULL) fclose(ft);

  char report[RELAIS_INV_MSG_MAX * 2];
  relais_inv_report(&inv, report, sizeof report);
  if (!quiet) {
    printf("%s\n", report);
    printf("simulate: %u ticks, seed %llu, faults %u, mandats %u, rejeux %u, "
           "ordres_rejetes %u (dont non-titulaire %u)\n",
           (unsigned)ticks, (unsigned long long)seed, (unsigned)world.faults_applied,
           (unsigned)world.arb.mandate, (unsigned)world.arb.handovers,
           (unsigned)relais_arb_rejected_total(&world.arb),
           (unsigned)world.arb.rejected[RELAIS_REJ_NOT_HOLDER]);
  }
  return relais_inv_ok(&inv) ? 0 : 1;
}
