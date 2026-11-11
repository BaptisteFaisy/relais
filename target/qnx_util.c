/* Relais — helpers shared by every QNX process. One implementation, no
 * copy-paste between relais-net, relais-core, relais-ctl and relaisctl.
 *
 * Requirements covered: QNX-03 (fixed priority, memory locked at start),
 * OBS-01 (slog2), OBS-03 (user events in the kernel trace), INJ-07 (the
 * isa-debug-exit death point).
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

/* QNX-03: the critical path must never wait on a page fault, and its priority
 * must not drift. Called once at start-up by each process. */
void rl_realtime(int priority) {
#ifdef __QNX__
  setprio(PRIO_PROCESS, 0, priority);
  mlockall(MCL_NOWAIT);
#else
  (void)priority; /* nothing to raise or lock on the syntax-check host */
#endif
}

void rl_log(int severity, const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
#ifdef __QNX__
  slog2c(NULL, 0, (uint16_t)severity, "%s", line); /* OBS-01 */
#else
  fprintf(stderr, "relais[%d]: %s\n", severity, line);
#endif
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
#ifdef __QNX__
  trace_logf(0, 0, "%s", line);
#endif
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

int rl_channel(const char *name) {
#ifdef __QNX__
  return name_attach(name, 0);
#else
  (void)name;
  return -1;
#endif
}

int rl_connect_local(const char *name, int *coid) {
#ifdef __QNX__
  int id = name_open(name, 0);
  if (id >= 0 && coid != NULL) *coid = id;
  return id;
#else
  (void)name;
  (void)coid;
  return -1;
#endif
}

/* INJ-07: the machine dies at an exact line of code by writing the
 * isa-debug-exit port, which makes QEMU quit. Needs I/O privileges, granted per
 * thread by ThreadCtl; if the policy refuses them, the bench kills the guest
 * from outside instead (spec 11, E4). */
void rl_qemu_exit(uint32_t code) {
#ifdef __QNX__
  if (ThreadCtl(_NTO_TCTL_IO, NULL) == 0) out32(0x501u, code | 0x10u);
#endif
  (void)code;
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
