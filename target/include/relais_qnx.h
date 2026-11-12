/* Relais — what the QNX processes share: priorities, device paths, and the
 * internal message layout between the three roles (spec 4.9).
 *
 * Nothing here holds protocol logic. The logic is in core/, compiled into the
 * same binaries; this header only names the pipes it travels through.
 */
#ifndef RELAIS_QNX_H
#define RELAIS_QNX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __QNX__
/* The real SDP 8.0 headers, in one place, so that every target file sees the
 * same declarations and nobody re-guesses an API (QNX-06).
 *   sched.h        SCHED_FIFO, struct sched_param (the fixed priority)
 *   sys/dispatch.h name_attach, name_open
 *   sys/mman.h     mlockall, MCL_CURRENT, MCL_FUTURE
 *   sys/neutrino.h the message passing and the timers
 *   sys/slog2.h    slog2c
 *   sys/trace.h    trace_logf
 *   hw/inout.h     out32, for the isa-debug-exit death points
 */
#include <sched.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>
#include <sys/trace.h>
#include <hw/inout.h>
#elif defined(RELAIS_HOST_STUB)
#include "../host_stub.h"
#else
#error "build for QNX, or define RELAIS_HOST_STUB for the syntax check"
#endif

#include "relais/frame.h"
#include "relais/node.h"
#include "relais/params.h"

/* Priorities are fixed and decreasing along the critical path, so that a
 * received STATUT is turned into an order without ever waiting behind a less
 * urgent thread (CMD-04, QNX-02, spec 1.4). */
#define RELAIS_PRIO_NET 62
#define RELAIS_PRIO_CORE 61
#define RELAIS_PRIO_CTL 60
#define RELAIS_PRIO_TOOL 10

#define RELAIS_DEV_ROOT "/dev/relais"
#define RELAIS_DEV_STATUS "/dev/relais/status"
#define RELAIS_DEV_TEMOIN "/dev/relais/temoin"
#define RELAIS_DEV_CTL "/dev/relais/ctl"

#define RELAIS_NET_CHANNEL "relais-net"
#define RELAIS_CTL_CHANNEL "relais-ctl"
#define RELAIS_SLOG_BUFFER "relais"
#define RELAIS_STATUS_LEN 256

/* Internal messages, one type each, fixed size: the whole protocol state fits
 * in a MsgSend round trip, no shared memory, no heap. */
typedef enum {
  RL_MSG_FRAME = 1,   /* a decoded frame: net -> core, or core -> net to send */
  RL_MSG_COMPUTE = 2, /* measurements: core -> ctl */
  RL_MSG_VALUE = 3,   /* the bounded command and the exported state: ctl -> core */
  RL_MSG_STATUS = 4,  /* status text: core -> a client of /dev/relais */
  RL_MSG_COMMAND = 5, /* handover / bias / die: relaisctl -> core */
  RL_MSG_ACK = 6
} rl_msg_type;

typedef struct {
  uint32_t type;
  uint32_t ioid; /* QNX resource-manager header: type then ioid */
} rl_hdr;

typedef struct {
  rl_hdr h;
  relais_msg msg;
} rl_frame_msg;

typedef struct {
  rl_hdr h;
  relais_meas meas;
  uint32_t tick;
  uint32_t role;
} rl_compute_msg;

typedef struct {
  rl_hdr h;
  float value;
  uint8_t state[RELAIS_CTL_STATE_MAX];
  uint8_t state_len;
} rl_value_msg;

typedef struct {
  rl_hdr h;
  char text[RELAIS_STATUS_LEN];
} rl_status_msg;

/* Commands accepted on /dev/relais/ctl, one text line at a time. */
typedef enum {
  RL_CMD_HANDOVER = 1, /* REL-09: cede the mandate for maintenance */
  RL_CMD_BIAS = 2,     /* INJ-10: add a bias to the emitted values */
  RL_CMD_DIE = 3,      /* INJ-07: fire a death point P1..P9 at a tick */
  RL_CMD_FAULT = 4     /* DET-05: report a local process death */
} rl_command;

typedef struct {
  rl_hdr h;
  uint32_t command;
  float value;
  uint32_t arg;
  uint32_t tick;
} rl_command_msg;

/* --- shared helpers, one implementation in qnx_util.c --- */
void rl_realtime(int priority);            /* QNX-03: fixed priority + locked memory */
void rl_log(int severity, const char *fmt, ...); /* OBS-01: slog2 */
void rl_trace(const char *fmt, ...);       /* OBS-03: user event in the kernel trace */
int rl_udp_bind(int port);                 /* non-blocking UDP socket, loopback ok */
int rl_udp_peer(const char *spec, void *addr_out, size_t cap);
int rl_channel(const char *name);          /* name_attach, returns the cid */
int rl_connect_local(const char *name, int *coid);
/* The isa-debug-exit death point: with I/O privileges, writing this port makes
 * QEMU quit, so a machine can die at an exact line of code (INJ-07, E4). */
void rl_qemu_exit(uint32_t code);
bool rl_parse_command(const char *line, rl_command_msg *out);

#endif /* RELAIS_QNX_H */
