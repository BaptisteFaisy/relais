/* Relais — QNX API stubs, for ONE purpose only: let a PC compiler syntax-check
 * the target layer when the SDP 8.0 is not installed (`make -C target syntax`).
 *
 * This file is NEVER included by a QNX build: target code includes it only
 * under `#ifdef RELAIS_HOST_STUB`, and the real headers come from the SDP.
 * Nothing here has behaviour; the constants are placeholders, the real values
 * come from <sys/neutrino.h>, <sys/dispatch.h> and <hw/inout.h>.
 */
#ifndef RELAIS_HOST_STUB_H
#define RELAIS_HOST_STUB_H

#include <signal.h> /* struct sigevent, same shape as the QNX one for our use */
#include <stdint.h>
#include <sys/types.h>

/* QNX's interval timer structure (sys/neutrino.h). Only the fields the target
 * layer touches are declared; the real structure is larger. */
struct _itimer {
  uint64_t it_value;
  uint64_t it_interval;
};

/* --- message passing (sys/neutrino.h) --- */
#define _NTO_CHF_COOKED 0x00000001u
#define _NTO_CHF_UNBLOCK 0x00000004u
#define _NTO_SIDE_FLAGS 0u
#define _NTO_NAME_PRIVATE 0x00000001u
#define _NTO_TIMER_MSEC 0x00000002u

/* The resource-manager message tags (sys/dispatch.h). Placeholder values: on a
 * QNX build the real ones come from the SDP header. */
#define _IO_OPEN 0x00000001u
#define _IO_READ 0x00000002u
#define _IO_WRITE 0x00000003u
#define _IO_CLOSE 0x00000004u
#define _IO_MSG 0x00000005u

struct _pulse {
  uint8_t type;
  uint8_t code;
  uint16_t tag;
  union {
    int32_t scount;
    uint32_t ucount;
    void *ptr;
  } value;
};

struct _sched_param {
  int sched_priority;
  int sched_curpriority;
  int sched_maxpriority;
};

int name_attach(const char *name, unsigned flags);
int name_detach(int cid, unsigned flags);
int ChannelCreate(unsigned flags);
int ChannelDestroy(int cid);
int MsgReceive(int chid, void *msg, int bytes, unsigned *id);
int MsgReply(unsigned id, long status, const void *msg, int bytes);
int MsgSend(int coid, const void *msg, int sbytes, void *reply, int rbytes);
int MsgSendPulse(int coid, int priority, int code, unsigned value);
int MsgError(unsigned id, int status);
int ConnectAttach(uint32_t nds, pid_t pid, int chid, unsigned index, unsigned flags);
int name_open(const char *name, unsigned flags);
int ConnectDetach(int coid);
int setprio(int which, int who, int prio);
int ThreadCtl(int cmd, void *arg);
int TimerCreate(int clockid, struct sigevent *evp, int *tid);
int TimerSettime(int tid, int flags, const struct _itimer *itime, struct _itimer *oitime);

#define _NTO_TCTL_IO 2
#define _NTO_TCTL_VIDEO 3

/* --- kernel tracing (sys/trace.h) --- */
int trace_logf(unsigned code, unsigned severity, const char *fmt, ...);

/* --- hardware ports, for the isa-debug-exit death points (hw/inout.h) --- */
uint32_t out32(uint32_t port, uint32_t value);
uint32_t in32(uint32_t port);

/* --- slog2 (sys/slog2.h) --- */
typedef struct {
  const char *buffer_name;
  uint16_t num_buffers;
  uint16_t severity;
  uint16_t buffer_size;
  uint16_t max_num_subbuffers;
} slog2_buffer_set_config_t;
typedef void *slog2_buffer_t;
int slog2_buffer_set_config(slog2_buffer_set_config_t *cfg, const char *name,
                            slog2_buffer_t **set);
int slog2c(slog2_buffer_t buf, uint16_t code, int severity, const char *fmt, ...);
#define SLOG2_DEBUG1 0x0100
#define SLOG2_INFO 0x0400
#define SLOG2_WARNING 0x0500
#define SLOG2_ERROR 0x0700

#endif /* RELAIS_HOST_STUB_H */
