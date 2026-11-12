/* Relais — relais-ctl, priority 60 (spec 4.9).
 *
 * The control law as a service, and nothing else. It owns no socket, no device,
 * no clock and no notion of mandate or role: it answers "given these
 * measurements, what is the command?" and "give me / take back the state".
 *
 * That separation is what CMD-09 asks for: the law can be replaced — by another
 * module, by a dissimilar backup (DEG-08), or by a second profile such as the
 * braking one (CMD-08) — without touching a line of the protocol.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "relais_qnx.h"

static relais_ctl_state g_state;
static relais_params g_params;

static void answer_compute(rcvid_t rcvid, const rl_compute_msg *req) {
  rl_value_msg rep;
  memset(&rep, 0, sizeof rep);
  rep.h.type = RL_MSG_VALUE;
  if (req->h.type == RL_MSG_STATUS) {
    /* The core asks for the state to replicate (REP-01). */
    rep.state_len = relais_ctl_export(&g_state, rep.state, sizeof rep.state);
    rep.value = g_state.last_value;
  } else {
    /* CMD-06: the value is bounded here, before it ever reaches the wire. */
    rep.value = relais_clamp_value(
        relais_ctl_step(&g_state, &req->meas, &g_params), &g_params);
    rep.state_len = relais_ctl_export(&g_state, rep.state, sizeof rep.state);
  }
  MsgReply(rcvid, (long)sizeof rep, &rep, sizeof rep);
}

static void answer_import(rcvid_t rcvid, const rl_value_msg *req) {
  rl_status_msg rep;
  memset(&rep, 0, sizeof rep);
  rep.h.type = RL_MSG_ACK;
  /* REP-02: a state of an unknown version or length is refused; the replica
   * keeps what it had rather than jumping to something it cannot trust. */
  bool ok = relais_ctl_import(&g_state, req->state, req->state_len);
  snprintf(rep.text, sizeof rep.text, "%s\n", ok ? "ok" : "refused");
  MsgReply(rcvid, (long)sizeof rep, &rep, sizeof rep);
}

/* The wait loop: one poll on the channel, no log per tick, no allocation, no
 * file, so the critical path stays short and deterministic (CMD-04). */
static void ctl_loop(int channel) {
  union {
    rl_hdr h;
    rl_compute_msg compute;
    rl_value_msg value;
  } in;
  struct pollfd pfd;
  pfd.fd = channel;
  pfd.events = POLLIN;
  for (;;) {
    int ready = poll(&pfd, 1, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    struct _msg_info info;
    /* MsgReceive() returns the receive id; the byte count is in the info. */
    rcvid_t rcvid = MsgReceive(channel, &in, sizeof in, &info);
    if (rcvid == -1) continue;
    switch (in.h.type) {
      case RL_MSG_COMPUTE:
      case RL_MSG_STATUS:
        answer_compute(rcvid, &in.compute);
        break;
      case RL_MSG_VALUE:
        answer_import(rcvid, &in.value);
        break;
      default:
        MsgError(rcvid, ENOSYS);
        break;
    }
  }
}

int main(int argc, char **argv) {
  relais_params_default(&g_params);
  char why[128] = "";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--params") && i + 1 < argc) {
      if (!relais_params_parse(&g_params, argv[++i], why, sizeof why)) {
        fprintf(stderr, "ctl: bad params: %s\n", why);
        return 2;
      }
    }
  }

  rl_realtime(RELAIS_PRIO_CTL);
  relais_ctl_init(&g_state, &g_params);
  int channel = rl_channel(RELAIS_CTL_CHANNEL);
#ifdef __QNX__
  if (channel < 0) {
    rl_log(SLOG2_ERROR, "ctl: cannot attach %s", RELAIS_CTL_CHANNEL);
    return 2;
  }
#endif
  rl_log(SLOG2_INFO, "ctl: up, priority %d, T=%u ms", RELAIS_PRIO_CTL,
         (unsigned)relais_params_period_ms(&g_params));
  ctl_loop(channel);
  return 0;
}
