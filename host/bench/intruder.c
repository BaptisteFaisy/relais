/* Relais — the intruder: an adversarial process, written on purpose (SEC-01,
 * SEC-02, SEC-04, MAN-02, QNX-01, spec 8).
 *
 * This is the "opposed policy" demonstration of idea 35: instead of asserting
 * that the system refuses an attacker, an attacker is built and run, and the
 * refusal is printed as the actual result of the actual system call — or, where
 * the attack is a protocol lie rather than a file access, as the actual answer
 * of the actual core.
 *
 * On a PC it shows the refusal by absence: there is no /dev/relais, no channel
 * and nothing listening on the relay-core port, so every attempt fails with an
 * errno. The same binary is the one the QNX script target/intruder_test.sh runs
 * once the SDP is available, where the refusal comes from the typed security
 * policy (`on`) instead of from a missing path — see
 * docs/SECURITY-POLICY-DEMO.md for what each environment can and cannot prove.
 *
 * What is proven here, on any host:
 *   1. the four /dev/relais resources cannot be opened (QNX-01);
 *   2. a frame forged without the key is refused by the core, with its reason,
 *      before any state changes (SEC-02);
 *   3. a well-formed frame that claims a mandate it was never given is refused
 *      by the arbiter and its value is never applied (SEC-04, MAN-02, ACT-05);
 *   4. nothing answers on the relay-core port, so no forged frame is applied;
 *   5. (audit, not a refusal) the journal file, when it exists and is writable,
 *      accepts a forged record: the journal has no integrity protection, and
 *      that is written down in docs/ATTACK-SURFACE.md.
 *
 * Exit code: 0 when every attack was refused, 1 when one succeeded.
 *
 * Usage: relais-intruder [--port 7100] [--journal FILE] [--quiet]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "relais/arbiter.h"
#include "relais/frame.h"
#include "relais_qnx.h" /* the real device paths and channel names, not a copy */

/* The actuator's key and the intruder's guess: the point is that they differ. */
#define KEY_ACTUATOR "11111111111111111111111111111111"
#define KEY_GUESS "22222222222222222222222222222222"
#define FORGED_VALUE 1.0f /* full lock: the worst an order can ask for */
#define FORGED_MANDATE 9999u /* SEC-04: a node appointing itself */

static int g_refused;
static int g_granted;
static int g_audit; /* a gap found, reported but not counted as a breach */
static int g_quiet;

static void verdict(int refused, const char *what, const char *detail) {
  if (refused) {
    g_refused++;
    if (!g_quiet) printf("[REFUSED] %-44s %s\n", what, detail);
  } else {
    g_granted++;
    printf("[GRANTED] %-44s %s\n", what, detail);
  }
}

/* An audit finding is a real gap (the journal is not tamper-proof) but not a
 * breach of the opposed policy: it is reported, it does not fail the demo. */
static void audit(const char *what, const char *detail) {
  g_audit++;
  printf("[AUDIT  ] %-44s %s\n", what, detail);
}

static void attempt_open(const char *path) {
  char what[128];
  snprintf(what, sizeof what, "open %s O_RDWR", path);
  int fd = open(path, O_RDWR);
  if (fd >= 0) {
    close(fd);
    verdict(0, what, "the resource exists and is writable");
    return;
  }
  char detail[160];
  snprintf(detail, sizeof detail, "%s (%s)", strerror(errno),
           errno == ENOENT ? "no such resource on this host" : "access denied");
  verdict(1, what, detail);
}

/* A frame the intruder forges: right shape, key of its choice, full lock. */
static size_t forge(uint8_t *buf, size_t cap, const char *key_hex, relais_mandate mandate,
                    relais_node who, relais_tick tick) {
  relais_key key;
  relais_key_from_hex(&key, key_hex);
  relais_msg m;
  relais_msg_clear(&m);
  m.type = RELAIS_MSG_ORDRE;
  m.sender = who;
  m.tick = tick;
  m.mandate = mandate;
  m.value = FORGED_VALUE;
  m.has_order = true;
  m.testament_len = 2;
  return relais_frame_encode(&m, &key, buf, cap);
}

static void attempt_forged_frame(void) {
  relais_key real;
  relais_key_from_hex(&real, KEY_ACTUATOR);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t n = forge(buf, sizeof buf, KEY_GUESS, 1u, RELAIS_NODE_A, 42u);
  char what[128];
  snprintf(what, sizeof what, "forged ORDRE, %zu bytes, wrong key", n);
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;
  bool accepted = relais_frame_decode(buf, n, &real, &out, &why);
  char detail[160];
  snprintf(detail, sizeof detail, "core refuses it, reason=%s (SEC-02)",
           relais_reject_name(why));
  verdict(!accepted, what, detail);
}

/* Give the arbiter a real holder first, so the forged order is judged against a
 * live mandate and not against an empty state. */
static void arm_arbiter(relais_arb *a, const relais_params *p) {
  relais_arb_init(a, p);
  relais_msg demande;
  relais_msg_clear(&demande);
  demande.type = RELAIS_MSG_DEMANDE;
  demande.sender = RELAIS_NODE_A;
  demande.tick = 1;
  demande.reason = 1;
  relais_arb_out out;
  relais_arb_step(a, 1, &demande, 1, NULL, &out);
}

static bool rejected_as(const relais_arb_out *out, relais_reject why) {
  for (uint8_t i = 0; i < out->event_count; i++) {
    if (out->events[i].kind == RELAIS_EV_ORDER_REJECTED &&
        out->events[i].detail == (uint32_t)why)
      return true;
  }
  return false;
}

static void attempt_future_mandate(void) {
  relais_key real;
  relais_key_from_hex(&real, KEY_ACTUATOR);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t n = forge(buf, sizeof buf, KEY_ACTUATOR, FORGED_MANDATE, RELAIS_NODE_A, 2u);
  relais_msg forged;
  relais_reject why = RELAIS_REJ_NONE;
  char what[128];
  snprintf(what, sizeof what, "signed ORDRE claiming mandate %u", FORGED_MANDATE);
  if (!relais_frame_decode(buf, n, &real, &forged, &why)) {
    char detail[128];
    snprintf(detail, sizeof detail, "not even parseable, reason=%s", relais_reject_name(why));
    verdict(1, what, detail);
    return;
  }
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  arm_arbiter(&a, &p);
  relais_arb_out out;
  relais_arb_step(&a, 2, &forged, 1, NULL, &out);
  bool refused = rejected_as(&out, RELAIS_REJ_MANDATE_FUTURE) && out.value != FORGED_VALUE;
  char detail[192];
  snprintf(detail, sizeof detail,
           "arbiter rejects it (%s) and applies %.3f, not the forged %.1f (SEC-04)",
           relais_reject_name(RELAIS_REJ_MANDATE_FUTURE), (double)out.value,
           (double)FORGED_VALUE);
  verdict(refused, what, detail);
}

static void attempt_port(int port) {
  char what[128];
  snprintf(what, sizeof what, "send a frame to relay-core port %d", port);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    verdict(1, what, strerror(errno));
    return;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t n = forge(buf, sizeof buf, KEY_GUESS, 1u, RELAIS_NODE_A, 42u);
  char detail[160];
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    snprintf(detail, sizeof detail, "connect: %s", strerror(errno));
    verdict(1, what, detail);
    close(fd);
    return;
  }
  ssize_t sent = send(fd, buf, n, 0);
  if (sent < 0) {
    snprintf(detail, sizeof detail, "send: %s", strerror(errno));
    verdict(1, what, detail);
    close(fd);
    return;
  }
  char back[64]; /* a connected UDP socket reports the ICMP refusal here */
  ssize_t got = recv(fd, back, sizeof back, 0);
  if (got < 0) {
    snprintf(detail, sizeof detail, "sent %zd bytes, no core answers: %s", sent, strerror(errno));
    verdict(1, what, detail);
  } else {
    snprintf(detail, sizeof detail, "something answered (%zd bytes)", got);
    verdict(0, what, detail);
  }
  close(fd);
}

static void attempt_channel(void) {
  char what[128];
  snprintf(what, sizeof what, "reach the channel %s", RELAIS_CTL_CHANNEL);
#ifdef __QNX__
  verdict(0, what, "on QNX this needs the typed policy: see intruder_test.sh");
#else
  verdict(1, what, "no name_attach on this host: the channel does not exist");
#endif
}

/* Audit, not an attack: shows what the journal does NOT protect. */
static void attempt_journal(const char *path) {
  char what[128];
  snprintf(what, sizeof what, "append a forged record to %s", path);
  int fd = open(path, O_WRONLY | O_APPEND);
  if (fd < 0) {
    char detail[128];
    snprintf(detail, sizeof detail, "%s (nothing to forge)", strerror(errno));
    verdict(1, what, detail);
    return;
  }
  static const char line[] =
      "{\"kind\":\"tick\",\"tick\":1,\"mandate\":1,\"holder\":\"A\","
      "\"source\":\"holder\",\"certified\":false,\"stop\":false}\n";
  ssize_t w = write(fd, line, sizeof line - 1u);
  close(fd);
  char detail[160];
  snprintf(detail, sizeof detail,
           "wrote %zd bytes: the journal is NOT tamper-proof (ATTACK-SURFACE.md)", w);
  audit(what, detail);
}

int main(int argc, char **argv) {
  int port = 7100;
  const char *journal = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) journal = argv[++i];
    else if (strcmp(argv[i], "--quiet") == 0) g_quiet = 1;
    else {
      fprintf(stderr, "usage: %s [--port N] [--journal FILE] [--quiet]\n", argv[0]);
      return 2;
    }
  }

  printf("relais-intruder: an adversarial process, run on purpose (idea 35)\n");
  attempt_open(RELAIS_DEV_ROOT);
  attempt_open(RELAIS_DEV_STATUS);
  attempt_open(RELAIS_DEV_TEMOIN);
  attempt_open(RELAIS_DEV_CTL);
  attempt_channel();
  attempt_port(port);
  attempt_forged_frame();
  attempt_future_mandate();
  if (journal != NULL) attempt_journal(journal);

  printf("intruder: %d attempt(s) refused, %d granted, %d audit finding(s)\n", g_refused,
         g_granted, g_audit);
  if (g_granted > 0) {
    printf("POLICY-DEMO-FAILED: an attack got through\n");
    return 1;
  }
  printf("POLICY-DEMO-OK: every attack was refused\n");
  return 0;
}
