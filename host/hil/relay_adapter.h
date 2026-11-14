/* Relais — physical cut adapter (mission M41, INJ-04: cut and restoration of
 * a link, with and without loss of carrier).
 *
 * This adapter drives a REAL relay that physically opens the conductors of a
 * link (or the power feed of a computer). It is deliberately outside core/:
 * it touches files, devices and clocks, which the protocol library never does.
 *
 * Three backends, one interface:
 *
 *   loopback — no hardware: the commanded contact state is mirrored into a
 *              state file, and read back through a second handle. It proves
 *              the switching logic (open/close/readback/debounce) here, on
 *              this machine. This is the only backend that RUNS in CI.
 *
 *   gpio     — Raspberry Pi, sysfs GPIO, active-high drive of the relay coil.
 *              WIRING TABLE (Pi header -> relay board -> cut conductors):
 *                pin 2  (5V)     -> relay VCC
 *                pin 6  (GND)    -> relay GND
 *                pin 11 (GPIO17) -> relay IN1  (coil drive, active high)
 *                relay COM  -> conductor to cut (e.g. actuator TX line)
 *                relay NO   -> other side of the cut (normally open: the link
 *                              is CLOSED while the coil is energized)
 *              Energized = contacts COM-NO closed = link up.
 *              De-energized = contacts open = PHYSICAL cut (loss of carrier).
 *
 *   usb      — USB relay board on a serial port (default 9600 8N1), driven by
 *              configurable byte sequences (defaults match the common
 *              "LC technology" 1-channel board: ON  = A0 01 00 A2,
 *              OFF = A0 00 01 A1). Same COM/NO wiring as above.
 *
 * Hardware backends are BLOCKED(no relay hardware) on this machine: no Pi GPIO
 * and no USB relay are plugged. The exact commands that would run them:
 *   build/relais-relay --backend gpio --gpio-root /sys/class/gpio --channel 17
 *   build/relais-relay --backend usb  --device /dev/ttyUSB0 --baud 9600
 * (ROB-05, DEG-07: the cut this relay makes is electrical, not a socket
 * close — see docs/REAL-FAULTS.md for the honest real/simulated table.)
 */
#ifndef RELAIS_RELAY_ADAPTER_H
#define RELAIS_RELAY_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Exit codes of the self-test driver (relay_main.c). */
#define RELAY_EXIT_PASS 0
#define RELAY_EXIT_FAIL 1
#define RELAY_EXIT_BLOCKED 3 /* hardware absent: BLOCKED(no relay hardware) */

typedef enum {
  RELAY_BACKEND_LOOPBACK = 0,
  RELAY_BACKEND_GPIO = 1,
  RELAY_BACKEND_USB = 2
} relay_backend;

/* One opened relay. `energized` is the commanded coil state; for loopback it
 * is also the readable contact state. fd is device-specific (state file,
 * sysfs value file or serial port). */
typedef struct {
  relay_backend backend;
  bool energized;
  bool open_ok;
  int fd;
  char device[128];   /* resolved device/state path, for logs */
  char why[128];      /* human-readable failure or BLOCKED reason */
  uint8_t on_cmd[16];  size_t on_len;   /* coil sequences copied at open (usb) */
  uint8_t off_cmd[16]; size_t off_len;
} relay_t;

typedef struct {
  relay_backend backend;
  const char *gpio_root;  /* default "/sys/class/gpio" */
  int channel;            /* GPIO number, e.g. 17 */
  const char *device;     /* serial port for usb, state file for loopback */
  unsigned baud;          /* default 9600 */
  const uint8_t *on_cmd;  size_t on_len;   /* coil energize sequence */
  const uint8_t *off_cmd; size_t off_len;  /* coil de-energize sequence */
} relay_config;

const char *relay_backend_name(relay_backend b);
bool relay_backend_parse(const char *s, relay_backend *out);
void relay_config_default(relay_config *cfg);

/* Open the relay in the de-energized state (contacts OPEN = link cut). A
 * hardware backend with no device present returns false and fills why with
 * "BLOCKED(no relay hardware)" or the exact errno reason. */
bool relay_open(relay_t *r, const relay_config *cfg);

/* Command the coil. energized=true closes COM-NO (link up); false opens it
 * (physical cut). Loopback mirrors the state into the state file. */
bool relay_set(relay_t *r, bool energized);

/* Read the contact state back: loopback re-reads the state file through a
 * second open (proves the mirror), gpio reads the sysfs value file, usb has no
 * readback wire and returns the last commanded state. */
bool relay_get(relay_t *r, bool *energized);

void relay_close(relay_t *r);

#endif /* RELAIS_RELAY_ADAPTER_H */
