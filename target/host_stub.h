/* Relais — QNX API stubs, for ONE purpose only: let a PC compiler syntax-check
 * the target layer when the SDP 8.0 is not installed (`make -C target syntax`).
 *
 * This file is NEVER included by a QNX build: target code includes it only
 * under `#ifdef RELAIS_HOST_STUB`, and the real headers come from the SDP.
 * Nothing here has behaviour; the constants are placeholders, the real values
 * come from <sys/neutrino.h>, <sys/dispatch.h> and <hw/inout.h>.
 *
 * Every prototype below is transcribed from the SDP 8.0 headers, so the stub
 * build checks the SAME shapes as the real qcc build:
 *   sys/neutrino.h   message passing, timers, thread control, _msg_info
 *   sys/dispatch.h   name_attach / name_open / name_attach_t
 *   sys/slog2.h      slog2c and the severities
 *   sys/trace.h      trace_logf
 *   hw/inout.h       out32
 *   sys/platform.h   struct _itimer (nsec / interval_nsec on QNX 8.0)
 * The prototypes that POSIX gives for free (setpriority, mlockall, close) are
 * NOT redeclared here: the system headers already declare them identically.
 */
#ifndef RELAIS_HOST_STUB_H
#define RELAIS_HOST_STUB_H

#include <sched.h>   /* SCHED_FIFO, struct sched_param */
#include <signal.h>  /* struct sigevent, same shape as the QNX one for our use */
#include <stdint.h>
#include <sys/types.h>
#include <time.h>    /* clockid_t, timer_t */

/* QNX's interval timer structure (sys/platform.h). Only the fields the target
 * layer touches are declared; the real structure is larger. On QNX 8.0 the two
 * fields are nsec and interval_nsec, NOT it_value and it_interval. */
struct _itimer {
  uint64_t nsec;
  uint64_t interval_nsec;
};

/* --- message passing (sys/neutrino.h) --- */
#define _NTO_CHF_FIXED_PRIORITY 0x00000001u
#define _NTO_CHF_UNBLOCK 0x00000002u
#define _NTO_CHF_THREAD_DEATH 0x00000004u
#define _NTO_SIDE_FLAGS 0u
#define _NTO_NAME_PRIVATE 0x00000001u
#define _NTO_TIMER_MSEC 0x00000002u

/* The resource-manager message tags (sys/iofd.h). Placeholder values: on a
 * QNX build the real ones come from the SDP header. */
#define _IO_OPEN 0x00000001u
#define _IO_READ 0x00000002u
#define _IO_WRITE 0x00000003u
#define _IO_CLOSE 0x00000004u
#define _IO_MSG 0x00000005u

/* QNX's pulse, as the kernel delivers it (sys/neutrino.h). */
struct _pulse {
  uint16_t type;
  uint16_t subtype;
  int8_t code;
  uint8_t zero[3];
  union sigval value;
  int32_t scoid;
};

/* The receive id is a 64-bit handle on a 64-bit target (sys/neutrino.h). */
typedef int64_t rcvid_t;

/* The message info the kernel fills in: on a 64-bit target it is _msg_info64. */
struct _msg_info {
  uint32_t nd;
  uint32_t srcnd;
  pid_t pid;
  int32_t tid;
  int32_t chid;
  int32_t scoid;
  int32_t coid;
  int16_t priority;
  int16_t flags;
  int64_t msglen;
  int64_t srcmsglen;
  int64_t dstmsglen;
  uint32_t type_id;
  uint32_t reserved;
};

/* --- names (sys/dispatch.h) --- */
typedef struct _dispatch dispatch_t;

typedef struct _name_attach {
  dispatch_t *dpp;
  int chid;
  int mntid;
  int zero[2];
} name_attach_t;

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags);
int name_detach(name_attach_t *attach, unsigned flags);
int name_open(const char *name, int flags);
int name_close(int fd);

/* --- channels and messages (sys/neutrino.h) --- */
int ChannelCreate(unsigned flags);
int ChannelDestroy(int cid);
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);
rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes);
long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes);
int MsgSendPulse(int coid, int priority, int code, long value);
int MsgReceivePulse(int chid, void *pulse, size_t bytes, struct _msg_info *info);
int MsgError(rcvid_t rcvid, int error);

/* --- the scheduler: QNX 8.0 has no setprio() at all (sys/neutrino.h) --- */
int SchedSet(pid_t pid, int tid, int algorithm, const struct sched_param *param);

/* --- timers and thread control (sys/neutrino.h) --- */
int ThreadCtl(int cmd, void *data);
int TimerCreate(clockid_t id, const struct sigevent *notify);
int TimerSettime(timer_t id, int flags, const struct _itimer *itime,
                  struct _itimer *oitime);

#define _NTO_TCTL_IO 14
#define _NTO_TCTL_VIDEO 15

/* --- kernel tracing (sys/trace.h) --- */
int trace_logf(int code, const char *fmt, ...);

/* --- hardware ports, for the isa-debug-exit death points (hw/inout.h) --- */
void out32(uint16_t port, uint32_t value);
uint32_t in32(uint16_t port);

/* --- slog2 (sys/slog2.h) --- */
typedef struct slog2_buffer_meta *slog2_buffer_t;
int slog2c(slog2_buffer_t buf, uint16_t code, uint8_t severity, const char *data);
#define SLOG2_SHUTDOWN 0
#define SLOG2_CRITICAL 1
#define SLOG2_ERROR 2
#define SLOG2_WARNING 3
#define SLOG2_NOTICE 4
#define SLOG2_INFO 5
#define SLOG2_DEBUG1 6
#define SLOG2_DEBUG2 7

#endif /* RELAIS_HOST_STUB_H */
