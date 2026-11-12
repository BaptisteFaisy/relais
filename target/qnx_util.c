/* Relais — helpers shared by every QNX process. One implementation, no
 * copy-paste between relais-net, relais-core, relais-ctl and relaisctl.
 *
 * Requirements covered: QNX-03 (fixed priority, memory locked at start),
 * OBS-01 (slog2), OBS-03 (user events in the kernel trace), INJ-07 (the
 * isa-debug-exit death point).
 *
 * Every QNX call below is written against the SDP 8.0 headers, and
 * host_stub.h transcribes the same prototypes: `make -C target syntax` then
 * checks the very shapes the real qcc build uses, instead of a private set of
 * made-up ones.
 */
#include "relais_qnx.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/* A QNX name always starts with '/': the local namespace is rooted there.
 * One helper, so that "relais-net" and "/relais-net" are the same channel
 * for every process. */
static int rl_abs_name(char *buf, size_t cap, const char *name) {
  if (name == NULL || cap < 2) return -1;
  int n = snprintf(buf, cap, "%s%s", name[0] == '/' ? "" : "/", name);
  if (n < 0 || (size_t)n >= cap) return -1;
  return 0;
}

/* QNX-03: the critical path must never wait on a page fault, and its priority
 * must not drift. Called once at start-up by each process. */
void rl_realtime(int priority) {
  /* QNX 8.0 has no setprio(): the name is absent from the whole SDP, and so
   * is the setprio(PRIO_PROCESS, 0, prio) shape it was written with. The real
   * call is the kernel's SchedSet() with SCHED_FIFO, which pins the calling
   * thread to a fixed priority. */
  struct sched_param param;
  memset(&param, 0, sizeof param);
  param.sched_priority = priority;
  (void)SchedSet(0, 0, SCHED_FIFO, &param);
  /* The pages of the critical path are locked, the ones already mapped and the
   * ones to come. MCL_NOWAIT is a Linux flag with no QNX equivalent: the real
   * flags are MCL_CURRENT and MCL_FUTURE, and their cost is paid here, at
   * start-up, on purpose. */
  (void)mlockall(MCL_CURRENT | MCL_FUTURE);
}

void rl_log(int severity, const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  /* OBS-01: slog2c takes the finished line as DATA, not as a format: the
   * message is already formatted above, so no format string is read twice. */
  (void)slog2c(NULL, 0, (uint8_t)severity, line);
}

/* OBS-03 / QNX-04: the trace shows the whole path from the received STATUT to
 * the emitted order, with the effective priority of every step, network stack
 * included. */
void rl_trace(const char *fmt, ...) {
  char line[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  /* trace_logf takes the code and the format: nothing else. */
  (void)trace_logf(0, "%s", line);
}

int rl_udp_bind(int port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in local;
  memset(&local, 0, sizeof local);
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&local, sizeof local) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int rl_udp_peer(const char *spec, void *addr_out, size_t cap) {
  char host[64];
  int port = 0;
  if (spec == NULL || addr_out == NULL || cap < sizeof(struct sockaddr_in)) return -1;
  if (sscanf(spec, "%63[^:]:%d", host, &port) != 2 || port <= 0) return -1;
  struct sockaddr_in *out = (struct sockaddr_in *)addr_out;
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET;
  out->sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &out->sin_addr) != 1) return -1;
  return 0;
}

/* The attachments must outlive the channels they name: one bounded table, one
 * entry per channel this process attaches (three at the most: status, temoin
 * and ctl). */
static name_attach_t *g_attach[8];

int rl_channel(const char *name) {
  char path[128];
  if (rl_abs_name(path, sizeof path, name) != 0) return -1;
  /* name_attach() returns the attachment; the channel id lives inside it. */
  name_attach_t *att = name_attach(NULL, path, 0);
  if (att == NULL) return -1;
  for (size_t i = 0; i < sizeof g_attach / sizeof g_attach[0]; i++) {
    if (g_attach[i] == NULL) {
      g_attach[i] = att;
      return att->chid;
    }
  }
  return -1; /* no room left: the channel would not be usable anyway */
}

int rl_connect_local(const char *name, int *coid) {
  char path[128];
  if (rl_abs_name(path, sizeof path, name) != 0) return -1;
  int id = name_open(path, 0);
  if (id >= 0 && coid != NULL) *coid = id;
  return id;
}

/* INJ-07: the machine dies at an exact line of code by writing the
 * isa-debug-exit port, which makes QEMU quit. Needs I/O privileges, granted per
 * thread by ThreadCtl; if the policy refuses them, the bench kills the guest
 * from outside instead (spec 11, E4). */
void rl_qemu_exit(uint32_t code) {
  if (ThreadCtl(_NTO_TCTL_IO, NULL) == 0) out32((uint16_t)0x501u, code | 0x10u);
}

/* The service tool speaks text on /dev/relais/ctl: one command per line. */
bool rl_parse_command(const char *line, rl_command_msg *out) {
  if (line == NULL || out == NULL) return false;
  memset(out, 0, sizeof *out);
  out->h.type = RL_MSG_COMMAND;
  char word[16];
  char rest[64];
  memset(word, 0, sizeof word);
  memset(rest, 0, sizeof rest);
  int fields = sscanf(line, "%15s %63s", word, rest);
  if (fields < 1) return false;
  if (strcmp(word, "handover") == 0) {
    out->command = RL_CMD_HANDOVER;
    return true;
  }
  if (strcmp(word, "bias") == 0 && fields == 2) {
    out->command = RL_CMD_BIAS;
    out->value = (float)atof(rest);
    return true;
  }
  if (strcmp(word, "die") == 0 && fields == 2 && rest[0] == 'P') {
    out->command = RL_CMD_DIE;
    out->arg = (uint32_t)(rest[1] - '0');
    return true;
  }
  if (strcmp(word, "fault") == 0 && fields == 2) {
    out->command = RL_CMD_FAULT;
    out->arg = (uint32_t)atoi(rest);
    return true;
  }
  return false;
}
