/* Relais — the actuator in a third QNX machine (ACT-08).
 *
 * Same arbitration, same rack model, same journal as the PC actuator: the logic
 * is core/, and the journal writer is actuator/journal_writer.c, shared with it.
 * What changes is the tempo, which here comes from a QNX timer and a pulse
 * instead of clock_nanosleep, so the beat does not drift with the host's load
 * (spec 1.4: the tempo must be given by a real-time system).
 *
 * MAN-11: the mandate number is snapshotted to --state and restored at start,
 * so restarting the actuator cannot hand the same mandate to two machines.
 */
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "journal_writer.h"
#include "relais_qnx.h"

#define MAX_PEERS 2
#define PULSE_TICK 1

typedef struct {
  relais_params params;
  relais_traj traj;
  relais_key key;
  int sock;
  struct {
    struct sockaddr_in addr;
    bool set;
  } peer[MAX_PEERS];
  FILE *journal;
  relais_jw jw;         /* chained journal writer, shared with the PC actuator */
  relais_key chain_key; /* journal chain key, distinct from the frame MAC key */
  const char *state_path;
  uint32_t ticks;
  bool realtime;
} act_t;

static void usage(void) {
  fprintf(stderr,
          "usage: relais-actuator --port 7100 --peer A=HOST:PORT --peer B=HOST:PORT\n"
          "       [--ticks N] [--params T=5,p=1,N=8] [--trajectory \"kind=slalom\"]\n"
          "       [--journal FILE] [--state FILE] [--key HEX32]\n"
          "       [--chain-key HEX32] [--no-realtime]\n");
}

static bool parse_peer(const char *spec, act_t *cfg) {
  char name[8];
  char hostport[80];
  if (sscanf(spec, "%7[^=]=%79s", name, hostport) != 2) return false;
  int index = strcmp(name, "A") == 0 ? 0 : (strcmp(name, "B") == 0 ? 1 : -1);
  if (index < 0) return false;
  if (rl_udp_peer(hostport, &cfg->peer[index].addr, sizeof cfg->peer[index].addr) != 0)
    return false;
  cfg->peer[index].set = true;
  return true;
}

static size_t drain(act_t *cfg, relais_msg *msgs, size_t cap) {
  size_t n = 0;
  uint8_t buf[RELAIS_FRAME_MAX];
  while (n < cap) {
    ssize_t got = recv(cfg->sock, buf, sizeof buf, MSG_DONTWAIT);
    if (got <= 0) break;
    relais_reject why = RELAIS_REJ_NONE;
    if (relais_frame_decode(buf, (size_t)got, &cfg->key, &msgs[n], &why)) n++;
    else rl_log(SLOG2_WARNING, "actuator: frame dropped (%s)", relais_reject_name(why));
  }
  return n;
}

static void publish(act_t *cfg, const relais_msg *statut) {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(statut, &cfg->key, buf, sizeof buf);
  if (len == 0) return;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!cfg->peer[i].set) continue;
    ssize_t sent = sendto(cfg->sock, buf, len, 0,
                          (const struct sockaddr *)&cfg->peer[i].addr,
                          sizeof cfg->peer[i].addr);
    (void)sent; /* UDP: a lost STATUT is a fault the protocol absorbs */
  }
}

static void restore_state(act_t *cfg, relais_arb *arb) {
  if (cfg->state_path == NULL) return;
  FILE *f = fopen(cfg->state_path, "rb");
  if (f == NULL) return; /* first start: nothing to restore */
  uint8_t buf[512];
  size_t got = fread(buf, 1, sizeof buf, f);
  fclose(f);
  if (got > 0 && relais_arb_restore(arb, buf, got))
    rl_log(SLOG2_INFO, "actuator: mandate %u restored", (unsigned)arb->mandate);
}

static void save_state(const act_t *cfg, const relais_arb *arb) {
  if (cfg->state_path == NULL) return;
  uint8_t buf[512];
  size_t len = relais_arb_snapshot(arb, buf, sizeof buf);
  if (len == 0) return;
  FILE *f = fopen(cfg->state_path, "wb");
  if (f == NULL) return;
  if (fwrite(buf, 1, len, f) != len) rl_log(SLOG2_WARNING, "actuator: short state write");
  fclose(f);
}

/* One period: collect, arbitrate, move the rack, journal, publish. */
static void beat(act_t *cfg, relais_arb *arb, relais_plant *plant, relais_tick tick) {
  relais_msg msgs[RELAIS_IN_MSG_MAX];
  size_t n = drain(cfg, msgs, RELAIS_IN_MSG_MAX);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  double seconds = (double)(tick > 0 ? tick - 1 : 0) * relais_params_step_s(&cfg->params);
  meas.pos = plant->pos;
  meas.vel = plant->vel;
  meas.setpoint = relais_traj_setpoint(&cfg->traj, seconds);
  relais_arb_out out;
  relais_arb_step(arb, tick, msgs, n, &meas, &out);
  relais_plant_step(plant, out.value, relais_traj_disturbance(&cfg->traj, seconds),
                    (float)relais_params_step_s(&cfg->params));
  meas.pos = plant->pos;
  meas.vel = plant->vel;
  meas.applied = out.value;
  out.statut.meas = meas;

  /* Events before the tick record: the order the verifier reads them in. */
  relais_jw_write_events(&cfg->jw, &out, tick);
  relais_jw_write_tick(&cfg->jw, &out, tick, plant, meas.setpoint);
  publish(cfg, &out.statut);
  rl_trace("act tick %llu src %s man %u", (unsigned long long)tick,
           relais_source_name(out.source), (unsigned)out.mandate);
}

/* Arm the periodic pulse that gives the tempo. On a QNX build this is a kernel
 * timer; the syntax-check host has no such timer and simply returns. */
static void arm_tempo(bool realtime, uint32_t period_ms, int channel) {
#ifdef __QNX__
  if (!realtime) return;
  /* A pulse is delivered on a CONNECTION, not on a channel: attach to our own
   * channel so that the kernel timer has somewhere to send the beat. */
  int coid = ConnectAttach(_NTO_SIDE_CHANNEL, getpid(), channel, 0, 0);
  if (coid == -1) {
    rl_log(SLOG2_ERROR, "actuator: no connection for the tempo pulse");
    return;
  }
  struct sigevent ev;
  memset(&ev, 0, sizeof ev);
  /* SIGEV_PULSE_INIT() lives behind __EXT_QNX, which -D_POSIX_C_SOURCE turns
   * off, so the fields are filled one by one, with the same shape. */
  ev.sigev_notify = SIGEV_PULSE;
  ev.sigev_coid = coid;
  ev.sigev_priority = RELAIS_PRIO_CORE;
  ev.sigev_code = PULSE_TICK;
  ev.sigev_value.sival_ptr = NULL;
  /* QNX 8.0: TimerCreate() takes the clock and the event, and RETURNS the
   * timer id; there is no third argument. */
  int timer_id = TimerCreate(CLOCK_MONOTONIC, &ev);
  if (timer_id == -1) {
    rl_log(SLOG2_ERROR, "actuator: no kernel timer, the tempo will drift");
    return;
  }
  struct _itimer it;
  memset(&it, 0, sizeof it);
  /* On QNX 8.0 the two fields are nsec and interval_nsec. */
  uint64_t period_ns = (uint64_t)period_ms * 1000000ull;
  it.nsec = period_ns;
  it.interval_nsec = period_ns;
  if (TimerSettime((timer_t)timer_id, 0, &it, NULL) == -1)
    rl_log(SLOG2_ERROR, "actuator: timer not armed");
#else
  (void)realtime;
  (void)period_ms;
  (void)channel;
#endif
}

/* The command line of the actuator: one helper, so that main() stays short.
 * Returns 0, or 2 when the line is refused (the usage has been printed). */
static int parse_args(act_t *cfg, int argc, char **argv, int *port) {
  char why[128] = "";
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--port") && i + 1 < argc) *port = atoi(argv[++i]);
    else if (!strcmp(a, "--peer") && i + 1 < argc) {
      if (!parse_peer(argv[++i], cfg)) { usage(); return 2; }
    } else if (!strcmp(a, "--ticks") && i + 1 < argc) cfg->ticks = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(a, "--params") && i + 1 < argc) {
      if (!relais_params_parse(&cfg->params, argv[++i], why, sizeof why)) {
        fprintf(stderr, "actuator: bad params: %s\n", why);
        return 2;
      }
    } else if (!strcmp(a, "--trajectory") && i + 1 < argc) {
      if (!relais_traj_parse(&cfg->traj, argv[++i], why, sizeof why)) {
        fprintf(stderr, "actuator: bad trajectory: %s\n", why);
        return 2;
      }
    } else if (!strcmp(a, "--journal") && i + 1 < argc) {
      cfg->journal = fopen(argv[++i], "w");
    } else if (!strcmp(a, "--state") && i + 1 < argc) cfg->state_path = argv[++i];
    else if (!strcmp(a, "--key") && i + 1 < argc) {
      if (!relais_key_from_hex(&cfg->key, argv[++i])) { usage(); return 2; }
    } else if (!strcmp(a, "--chain-key") && i + 1 < argc) {
      if (!relais_key_from_hex(&cfg->chain_key, argv[++i])) { usage(); return 2; }
    } else if (!strcmp(a, "--no-realtime")) cfg->realtime = false;
    else { usage(); return 2; }
  }
  if (*port <= 0 || cfg->journal == NULL) { usage(); return 2; }
  return 0;
}

/* The beat: one tick per period, the socket and the timer waited on together
 * (QNX-02). */
static void actuator_loop(act_t *cfg, relais_arb *arb, relais_plant *plant, int channel) {
  struct pollfd fds[2];
  fds[0].fd = cfg->sock;
  fds[1].fd = channel;
  fds[0].events = fds[1].events = POLLIN;

  for (relais_tick tick = 1; tick <= (relais_tick)cfg->ticks; tick++) {
    if (cfg->realtime) {
      int ready = poll(fds, channel >= 0 ? 2 : 1,
                       (int)relais_params_period_ms(&cfg->params) * 2);
      if (ready < 0 && errno != EINTR) break;
      if (channel >= 0 && (fds[1].revents & POLLIN)) {
        /* Drain the beat: a pulse left on the channel would keep poll()
         * readable for ever and turn the tempo into a busy loop. */
        struct _pulse pulse;
        (void)MsgReceivePulse(channel, &pulse, sizeof pulse, NULL);
      }
    }
    beat(cfg, arb, plant, tick);
  }
}

int main(int argc, char **argv) {
  act_t cfg;
  memset(&cfg, 0, sizeof cfg);
  relais_params_default(&cfg.params);
  relais_traj_default(&cfg.traj);
  relais_key_clear(&cfg.key);
  relais_key_clear(&cfg.chain_key);
  cfg.ticks = 4000;
  cfg.realtime = true;
  int port = 0;
  if (parse_args(&cfg, argc, argv, &port) != 0) return 2;

  rl_realtime(RELAIS_PRIO_CORE);
  cfg.sock = rl_udp_bind(port);
  if (cfg.sock < 0) {
    rl_log(SLOG2_ERROR, "actuator: port %d unavailable", port);
    return 2;
  }
  relais_arb arb;
  relais_arb_init(&arb, &cfg.params);
  restore_state(&cfg, &arb);
  relais_plant plant;
  relais_plant_init(&plant, 0.0f, cfg.traj.seed);
  relais_jw_open(&cfg.jw, cfg.journal, &cfg.chain_key);
  relais_jw_write_header(&cfg.jw, &cfg.params, relais_mutant_name(), cfg.traj.seed);

  /* QNX-02: the tempo is a pulse from a kernel timer, not a sleep loop. The
   * socket and the timer are waited on in the same poll, so a frame that
   * arrives early is never held behind the beat. The channel comes first: the
   * pulse needs a connection to it. */
  int channel = rl_channel("relais-actuator");
  arm_tempo(cfg.realtime, relais_params_period_ms(&cfg.params), channel);
  actuator_loop(&cfg, &arb, &plant, channel);

  save_state(&cfg, &arb);
  char anchor[RELAIS_CHAIN_HEX + 1];
  if (relais_jw_anchor(&cfg.jw, anchor, sizeof anchor) > 0)
    rl_log(SLOG2_INFO, "actuator: journal chain anchor %s (%zu records)", anchor,
           cfg.jw.records);
  if (fflush(cfg.journal) != 0) rl_log(SLOG2_ERROR, "actuator: journal flush failed");
  fclose(cfg.journal);
  close(cfg.sock);
  rl_log(SLOG2_INFO, "actuator: %u ticks, mandate %u, holder %s, %u handovers",
         (unsigned)cfg.ticks, (unsigned)arb.mandate, relais_node_name(arb.holder),
         (unsigned)arb.handovers);
  return 0;
}
