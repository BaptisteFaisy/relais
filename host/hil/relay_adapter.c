/* Relais — physical cut adapter: implementation (see relay_adapter.h).
 *
 * The loopback backend runs anywhere; the gpio and usb backends need hardware
 * that is BLOCKED(no relay hardware) on this machine. Every backend honours
 * the same contract: relay_open leaves the contacts OPEN (link cut), and the
 * caller must explicitly energize to close them (fail-safe: a dead adapter
 * means a cut link, never a phantom link).
 */
/* cfmakeraw is a BSD/glibc extension: expose it alongside POSIX. */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "relay_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Default coil sequences of the common 1-channel "LC technology" USB relay. */
static const uint8_t kUsbOnDefault[] = {0xA0, 0x01, 0x00, 0xA2};
static const uint8_t kUsbOffDefault[] = {0xA0, 0x00, 0x01, 0xA1};

const char *relay_backend_name(relay_backend b) {
  switch (b) {
    case RELAY_BACKEND_LOOPBACK: return "loopback";
    case RELAY_BACKEND_GPIO: return "gpio";
    case RELAY_BACKEND_USB: return "usb";
  }
  return "?";
}

bool relay_backend_parse(const char *s, relay_backend *out) {
  if (s == NULL || out == NULL) return false;
  if (strcmp(s, "loopback") == 0) { *out = RELAY_BACKEND_LOOPBACK; return true; }
  if (strcmp(s, "gpio") == 0) { *out = RELAY_BACKEND_GPIO; return true; }
  if (strcmp(s, "usb") == 0) { *out = RELAY_BACKEND_USB; return true; }
  return false;
}

void relay_config_default(relay_config *cfg) {
  if (cfg == NULL) return;
  memset(cfg, 0, sizeof *cfg);
  cfg->backend = RELAY_BACKEND_LOOPBACK;
  cfg->gpio_root = "/sys/class/gpio";
  cfg->channel = 17;
  cfg->device = NULL;
  cfg->baud = 9600;
  cfg->on_cmd = kUsbOnDefault;  cfg->on_len = sizeof kUsbOnDefault;
  cfg->off_cmd = kUsbOffDefault; cfg->off_len = sizeof kUsbOffDefault;
}

static void set_why(relay_t *r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->why, sizeof r->why, fmt, ap);
  va_end(ap);
}

static bool write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return false;
  size_t len = strlen(text);
  ssize_t done = write(fd, text, len);
  close(fd);
  return done == (ssize_t)len;
}

/* --- loopback: mirror the contact state into a file ---------------------- */

static bool loopback_open(relay_t *r, const relay_config *cfg) {
  if (cfg->device == NULL) { set_why(r, "loopback needs --state <file>"); return false; }
  snprintf(r->device, sizeof r->device, "%s", cfg->device);
  r->fd = open(r->device, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (r->fd < 0) { set_why(r, "loopback state file: %s", strerror(errno)); return false; }
  if (write(r->fd, "0\n", 2) != 2) { set_why(r, "loopback write: %s", strerror(errno)); return false; }
  return true;
}

static bool loopback_set(relay_t *r, bool energized) {
  if (lseek(r->fd, 0, SEEK_SET) != 0) return false;
  if (ftruncate(r->fd, 0) != 0) return false;
  const char *v = energized ? "1\n" : "0\n";
  return write(r->fd, v, 2) == 2;
}

/* Read back through a SECOND open handle: the state really left the process. */
static bool loopback_get(relay_t *r, bool *energized) {
  FILE *f = fopen(r->device, "r");
  if (f == NULL) return false;
  int v = -1;
  bool ok = fscanf(f, "%d", &v) == 1;
  fclose(f);
  if (!ok) return false;
  *energized = (v == 1);
  return true;
}

/* --- gpio: Raspberry Pi sysfs, active-high coil drive --------------------- */

static bool gpio_export(const relay_config *cfg, char *why, size_t cap) {
  char path[256];
  snprintf(path, sizeof path, "%s/export", cfg->gpio_root);
  char num[12];
  snprintf(num, sizeof num, "%d", cfg->channel);
  if (write_file(path, num)) return true;
  int saved = errno;
  snprintf(path, sizeof path, "%s/gpio%d/direction", cfg->gpio_root, cfg->channel);
  if (access(path, F_OK) == 0) return true; /* already exported */
  (void)saved;
  snprintf(why, cap, "BLOCKED(no relay hardware): cannot export GPIO%d via %s (%s)",
           cfg->channel, cfg->gpio_root, strerror(errno));
  return false;
}

static bool gpio_open(relay_t *r, const relay_config *cfg) {
  char why[128];
  if (!gpio_export(cfg, why, sizeof why)) { snprintf(r->why, sizeof r->why, "%s", why); return false; }
  char path[256];
  snprintf(path, sizeof path, "%s/gpio%d/direction", cfg->gpio_root, cfg->channel);
  if (!write_file(path, "out")) {
    set_why(r, "BLOCKED(no relay hardware): %s not writable (%s)", path, strerror(errno));
    return false;
  }
  snprintf(r->device, sizeof r->device, "%s/gpio%d/value", cfg->gpio_root, cfg->channel);
  r->fd = open(r->device, O_RDWR);
  if (r->fd < 0) {
    set_why(r, "BLOCKED(no relay hardware): %s (%s)", r->device, strerror(errno));
    return false;
  }
  return true;
}

static bool gpio_set(relay_t *r, bool energized) {
  if (lseek(r->fd, 0, SEEK_SET) != 0) return false;
  const char *v = energized ? "1" : "0";
  return write(r->fd, v, 1) == 1;
}

static bool gpio_get(relay_t *r, bool *energized) {
  char buf[4] = {0};
  if (lseek(r->fd, 0, SEEK_SET) != 0) return false;
  ssize_t got = read(r->fd, buf, sizeof buf - 1);
  if (got < 1) return false;
  *energized = (buf[0] == '1');
  return true;
}

/* --- usb: serial relay board, vendor sequences ---------------------------- */

static bool usb_open(relay_t *r, const relay_config *cfg) {
  if (cfg->device == NULL) { set_why(r, "usb needs --device /dev/ttyUSBx"); return false; }
  snprintf(r->device, sizeof r->device, "%s", cfg->device);
  r->fd = open(r->device, O_RDWR | O_NOCTTY);
  if (r->fd < 0) {
    set_why(r, "BLOCKED(no relay hardware): %s (%s)", r->device, strerror(errno));
    return false;
  }
  struct termios tio;
  memset(&tio, 0, sizeof tio);
  if (tcgetattr(r->fd, &tio) != 0) { set_why(r, "tcgetattr: %s", strerror(errno)); return false; }
  cfmakeraw(&tio);
  speed_t speed = (cfg->baud == 115200) ? B115200 : B9600;
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);
  tio.c_cflag |= CLOCAL;
  if (tcsetattr(r->fd, TCSANOW, &tio) != 0) { set_why(r, "tcsetattr: %s", strerror(errno)); return false; }
  return true;
}

static bool usb_write_seq(relay_t *r, const uint8_t *cmd, size_t len) {
  if (len == 0 || len > sizeof r->on_cmd) return false;
  ssize_t done = write(r->fd, cmd, len);
  return done == (ssize_t)len;
}

/* --- one interface over the three backends -------------------------------- */

static void copy_sequences(relay_t *r, const relay_config *cfg) {
  r->on_len = cfg->on_len < sizeof r->on_cmd ? cfg->on_len : 0;
  r->off_len = cfg->off_len < sizeof r->off_cmd ? cfg->off_len : 0;
  if (cfg->on_cmd != NULL && r->on_len > 0) memcpy(r->on_cmd, cfg->on_cmd, r->on_len);
  if (cfg->off_cmd != NULL && r->off_len > 0) memcpy(r->off_cmd, cfg->off_cmd, r->off_len);
}

bool relay_open(relay_t *r, const relay_config *cfg) {
  if (r == NULL || cfg == NULL) return false;
  memset(r, 0, sizeof *r);
  r->backend = cfg->backend;
  r->fd = -1;
  copy_sequences(r, cfg);
  bool ok = false;
  switch (cfg->backend) {
    case RELAY_BACKEND_LOOPBACK: ok = loopback_open(r, cfg); break;
    case RELAY_BACKEND_GPIO: ok = gpio_open(r, cfg); break;
    case RELAY_BACKEND_USB: ok = usb_open(r, cfg); break;
  }
  if (!ok) {
    if (r->why[0] == '\0') set_why(r, "open failed (%s)", strerror(errno));
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    return false;
  }
  /* Fail-safe: open the contacts at init (relay_set(false) = link cut). */
  if (!relay_set(r, false)) { relay_close(r); return false; }
  r->open_ok = true;
  return true;
}

bool relay_set(relay_t *r, bool energized) {
  if (r == NULL || r->fd < 0) return false;
  bool ok = false;
  switch (r->backend) {
    case RELAY_BACKEND_LOOPBACK: ok = loopback_set(r, energized); break;
    case RELAY_BACKEND_GPIO: ok = gpio_set(r, energized); break;
    case RELAY_BACKEND_USB:
      ok = energized ? usb_write_seq(r, r->on_cmd, r->on_len)
                     : usb_write_seq(r, r->off_cmd, r->off_len);
      break;
  }
  if (ok) r->energized = energized;
  return ok;
}

bool relay_get(relay_t *r, bool *energized) {
  if (r == NULL || energized == NULL || r->fd < 0) return false;
  switch (r->backend) {
    case RELAY_BACKEND_LOOPBACK: return loopback_get(r, energized);
    case RELAY_BACKEND_GPIO: return gpio_get(r, energized);
    case RELAY_BACKEND_USB: *energized = r->energized; return true; /* no readback wire */
  }
  return false;
}

void relay_close(relay_t *r) {
  if (r == NULL) return;
  if (r->fd >= 0) {
    if (r->open_ok) (void)relay_set(r, false); /* leave the link cut */
    close(r->fd);
    r->fd = -1;
  }
  r->open_ok = false;
  r->energized = false;
}
