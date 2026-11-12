/* Relais — relais-core, priority 61 (spec 4.9).
 *
 * The resource manager and the protocol state machine of one computer. It owns
 * /dev/relais (QNX-01: open, read and write, nothing else), it drives
 * relais_node_* from core/ on every frame relais-net hands it, and it asks
 * relais-ctl for the command through the three-function interface of CMD-09.
 *
 * It owns no socket (SEC-05) and no control law (that is relais-ctl). What it
 * owns is the role, the mandate it was given, and the replica.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "relais_qnx.h"

/* One process, one state: the file-static is the process, not the library. The
 * portable core stays free of global state; this is the glue around it. */
static relais_node_sm g_sm;
static int g_net = -1;
static int g_ctl = -1;

/* --- CMD-09: the law lives in relais-ctl, one message round trip per call --- */

static void law_init(void *ctx, const relais_params *p) {
  (void)ctx;
  (void)p; /* relais-ctl initialises its own state when it starts */
}

static float law_step(void *ctx, const relais_meas *m, const relais_params *p) {
  (void)ctx;
  (void)p;
  rl_compute_msg req;
  rl_value_msg rep;
  memset(&req, 0, sizeof req);
  memset(&rep, 0, sizeof rep);
  req.h.type = RL_MSG_COMPUTE;
  req.meas = *m;
  req.tick = (uint32_t)g_sm.now;
  req.role = (uint32_t)g_sm.role;
  if (MsgSend(g_ctl, &req, sizeof req, &rep, sizeof rep) == -1) {
    /* QNX-05 / DET-05: the law server is gone. Do not wait for a timeout and
     * do not invent a value: cede the mandate in this very tick. */
    rl_log(SLOG2_ERROR, "core: relais-ctl is gone, ceding");
    relais_node_out out;
    relais_node_on_local_fault(&g_sm, 1, &out);
    return 0.0f;
  }
  return rep.value;
}

static uint8_t law_export(void *ctx, uint8_t *buf, size_t cap) {
  (void)ctx;
  rl_compute_msg req;
  rl_value_msg rep;
  memset(&req, 0, sizeof req);
  memset(&rep, 0, sizeof rep);
  req.h.type = RL_MSG_STATUS; /* ask for the state, not for a value */
  if (MsgSend(g_ctl, &req, sizeof req, &rep, sizeof rep) == -1) return 0;
  if (rep.state_len == 0 || rep.state_len > cap) return 0;
  memcpy(buf, rep.state, rep.state_len);
  return rep.state_len;
}

static bool law_import(void *ctx, const uint8_t *buf, size_t len) {
  (void)ctx;
  rl_value_msg req;
  rl_status_msg rep;
  memset(&req, 0, sizeof req);
  memset(&rep, 0, sizeof rep);
  req.h.type = RL_MSG_VALUE;
  req.state_len = (uint8_t)(len > RELAIS_CTL_STATE_MAX ? RELAIS_CTL_STATE_MAX : len);
  memcpy(req.state, buf, req.state_len);
  return MsgSend(g_ctl, &req, sizeof req, &rep, sizeof rep) != -1;
}

/* --- what goes out: every frame the state machine produced, to relais-net --- */

static void emit(const relais_node_out *out) {
  for (uint8_t i = 0; i < out->count; i++) {
    rl_frame_msg body;
    rl_status_msg ack;
    memset(&body, 0, sizeof body);
    body.h.type = RL_MSG_FRAME;
    body.msg = out->out[i];
    rl_trace("core tx %s tick %llu", relais_msg_type_name(body.msg.type),
             (unsigned long long)body.msg.tick);
    if (MsgSend(g_net, &body, sizeof body, &ack, sizeof ack) == -1 && errno == ESRCH) {
      rl_log(SLOG2_ERROR, "core: relais-net is gone");
      relais_node_out cede;
      relais_node_on_local_fault(&g_sm, 2, &cede); /* DET-05 */
      (void)cede;                                  /* nowhere left to send it */
      exit(1);
    }
  }
  if (out->died) {
    /* INJ-07: the machine dies exactly here, at the armed point. */
    rl_log(SLOG2_INFO, "core: death point P%d at tick %llu", (int)out->at,
           (unsigned long long)g_sm.now);
    rl_qemu_exit((uint32_t)out->at);
    exit(3);
  }
}

/* --- /dev/relais: three names, one role each, no devctl (QNX-01) --- */

static void serve_read(rcvid_t rcvid, const char *text) {
  size_t len = strlen(text);
  MsgReply(rcvid, (long)len, text, len + 1);
}

static void lamp_text(char *buf, size_t cap) {
  snprintf(buf, cap, "%s %s\n", relais_lamp_name(RELAIS_LAMP_OFF),
           relais_cause_name(RELAIS_CAUSE_NONE));
}

static void handle_client(int channel, rl_command_msg *msg, char *text, size_t cap) {
  struct _msg_info info;
  /* MsgReceive() returns the receive id; the byte count is in the info. */
  rcvid_t rcvid = MsgReceive(channel, msg, sizeof *msg, &info);
  if (rcvid == -1) return;
  if (info.msglen <= 0) {
    MsgError(rcvid, EIO);
    return;
  }
  if (msg->h.type == RL_MSG_COMMAND) {
    relais_node_out out;
    switch (msg->command) {
      case RL_CMD_HANDOVER: relais_node_on_handover(&g_sm, &out); emit(&out); break;
      case RL_CMD_BIAS: relais_node_set_bias(&g_sm, msg->value); break;
      case RL_CMD_DIE:
        relais_node_arm_death(&g_sm, (relais_death_point)msg->arg, msg->tick);
        break;
      case RL_CMD_FAULT: relais_node_on_local_fault(&g_sm, 3, &out); emit(&out); break;
      default: break;
    }
    snprintf(text, cap, "ok\n");
    MsgReply(rcvid, 2, text, 3);
    return;
  }
  /* A read: the status text, or the lamp. */
  if (relais_node_status_text(&g_sm, text, cap) < 0) text[0] = '\0';
  serve_read(rcvid, text);
}

/* The three names of /dev/relais: the clients of the status text, and the
 * lamp. One poll wakes them all. */
static void serve_channels(int ch_status, int ch_lamp, int ch_ctl, rl_command_msg *msg,
                           char *text, size_t cap, const struct pollfd *fds) {
  if (fds[0].revents & POLLIN) handle_client(ch_status, msg, text, cap);
  if (fds[1].revents & POLLIN) {
    lamp_text(text, cap);
    struct _msg_info lamp_info;
    rcvid_t lamp_rcvid = MsgReceive(ch_lamp, msg, sizeof *msg, &lamp_info);
    if (lamp_rcvid != -1) serve_read(lamp_rcvid, text);
  }
  if (fds[2].revents & POLLIN) handle_client(ch_ctl, msg, text, cap);
}

/* One frame from relais-net: the state machine decides what goes out, and the
 * answer to the sender goes first, so the link never waits on the emit. */
static void handle_frame(const rl_frame_msg *in, rcvid_t rid) {
  relais_node_out out;
  rl_trace("core rx %s tick %llu", relais_msg_type_name(in->msg.type),
           (unsigned long long)in->msg.tick);
  switch (in->msg.type) {
    case RELAIS_MSG_STATUT: relais_node_on_status(&g_sm, &in->msg, &out); break;
    case RELAIS_MSG_ETAT: relais_node_on_etat(&g_sm, &in->msg, &out); break;
    case RELAIS_MSG_SUIVI: relais_node_on_suivi(&g_sm, &in->msg, &out); break;
    default: relais_node_clear_out(&out); break;
  }
  MsgReply(rid, 0, NULL, 0);
  emit(&out);
}

/* The wait loop: the three names of /dev/relais and the frames that relais-net
 * hands over. One poll covers everything, no busy waiting (QNX-02). */
static void core_loop(int ch_status, int ch_lamp, int ch_ctl, int timeout_ms) {
  struct pollfd fds[3];
  fds[0].fd = ch_status;
  fds[1].fd = ch_lamp;
  fds[2].fd = ch_ctl;
  for (int i = 0; i < 3; i++) fds[i].events = POLLIN;

  rl_command_msg msg;
  char text[RELAIS_STATUS_LEN];
  for (;;) {
    int ready = poll(fds, 3, timeout_ms);
    if (ready == 0) {
      /* No STATUT for a whole bail period: depose ourselves (MAN-09). Safety
       * does not depend on it, the actuator has already moved on; this is for
       * the diagnosis and for DET-07 ("without actuator"). */
      relais_node_out out;
      relais_node_on_silence(&g_sm, g_sm.now + 1, &out);
      emit(&out);
      continue;
    }
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[0].revents & POLLIN || fds[1].revents & POLLIN || fds[2].revents & POLLIN)
      serve_channels(ch_status, ch_lamp, ch_ctl, &msg, text, sizeof text, fds);

    /* Frames from relais-net arrive as messages on our own connection; on QNX
     * they wake us through the same poll, on the syntax host they do not. */
    rl_frame_msg in;
    struct _msg_info frame_info;
    rcvid_t rid = MsgReceive(ch_status, &in, sizeof in, &frame_info);
    if (rid == -1) continue;
    if (in.h.type != RL_MSG_FRAME) {
      MsgReply(rid, 0, NULL, 0);
      continue;
    }
    handle_frame(&in, rid);
  }
}

int main(int argc, char **argv) {
  relais_params params;
  relais_params_default(&params);
  relais_node self = RELAIS_NODE_A;
  char why[128] = "";

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--self") && i + 1 < argc) {
      self = strcmp(argv[++i], "B") == 0 ? RELAIS_NODE_B : RELAIS_NODE_A;
    } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
      i++; /* the priority is set by the supervisor, kept for symmetry */
    } else if (!strcmp(argv[i], "--params") && i + 1 < argc) {
      if (!relais_params_parse(&params, argv[++i], why, sizeof why)) {
        fprintf(stderr, "core: bad params: %s\n", why);
        return 2;
      }
    }
  }

  rl_realtime(RELAIS_PRIO_CORE);
  int ch_status = rl_channel(RELAIS_DEV_STATUS);
  int ch_lamp = rl_channel(RELAIS_DEV_TEMOIN);
  int ch_ctl = rl_channel(RELAIS_DEV_CTL);
#ifdef __QNX__
  if (ch_status < 0 || ch_lamp < 0 || ch_ctl < 0) {
    rl_log(SLOG2_ERROR, "core: cannot attach /dev/relais");
    return 2;
  }
#endif
  relais_node_init(&g_sm, &params, self);
  relais_law law;
  law.ctx = NULL;
  law.init = law_init;
  law.step = law_step;
  law.export_state = law_export;
  law.import_state = law_import;
  relais_node_set_law(&g_sm, &law);

  g_net = rl_connect_local(RELAIS_NET_CHANNEL, NULL);
  g_ctl = rl_connect_local(RELAIS_CTL_CHANNEL, NULL);
  rl_log(SLOG2_INFO, "core: up as %s, priority %d", relais_node_name(self),
         RELAIS_PRIO_CORE);

  core_loop(ch_status, ch_lamp, ch_ctl, (int)relais_params_period_ms(&params) * 3);
  return 0;
}
