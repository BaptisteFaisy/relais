/* Relais M46 — failover harness STUB for the QNX guest.
 *
 * This is NOT the Relais protocol: it is a substitute binary that exercises
 * the guest-side harness mechanics end to end while target/ does not compile
 * with qcc yet (mission M45 owns that; see docs/QNX-RUN.md "BLOCKED"):
 *   - two processes talk over 127.0.0.1 UDP inside the guest;
 *   - the holder is killed FROM the guest (fault injection, INJ-01 shape);
 *   - the second detects the silence, takes over, and reports the takeover
 *     delay in microseconds on the console, where the host harness captures
 *     it into the transcript (OBS-04) and failover.json.
 *
 * Roles:
 *   rl-fail holder <port> <interval_ms>
 *       Sends an 8-byte monotonic-ns heartbeat to 127.0.0.1:<port> every
 *       interval, forever (until killed).
 *   rl-fail second <port> <silence_ms>
 *       Receives heartbeats; when none arrived for <silence_ms>, prints
 *       `rl-failover: takeover_us=<delay since last heartbeat> ...`,
 *       then `rl-failover: done rc=0` and exits 0.
 *
 * Build (SDP sourced):
 *   qcc -V gcc_ntox86_64 -std=c11 -O2 -Wall -Wextra -Werror -Wshadow \
 *       -Wconversion -Wno-sign-conversion -D_POSIX_C_SOURCE=200809L \
 *       host/bench/guest/qnx_failover_stub.c -lsocket -o build/qnx-vm-bin/rl-fail
 */
#include <arpa/inet.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD 8

static int64_t now_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    perror("clock_gettime");
    exit(1);
  }
  return (int64_t)ts.tv_sec * INT64_C(1000000000) + (int64_t)ts.tv_nsec;
}

static int udp_socket(void)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    exit(1);
  }
  return fd;
}

static void bind_local(int fd, in_port_t port)
{
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind");
    exit(1);
  }
}

static int run_holder(in_port_t port, long interval_ms)
{
  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  dest.sin_port = htons(port);
  int fd = udp_socket();
  printf("rl-failover: holder_up pid=%ld port=%u interval_ms=%ld\n",
         (long)getpid(), (unsigned)port, interval_ms);
  fflush(stdout);
  for (;;) {
    uint64_t stamp = (uint64_t)now_ns();
    unsigned char buf[PAYLOAD];
    for (unsigned i = 0U; i < PAYLOAD; i++) {
      buf[i] = (unsigned char)((stamp >> (8U * i)) & 0xffU);
    }
    ssize_t sent = sendto(fd, buf, sizeof(buf), 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
      perror("sendto");
      return 1;
    }
    struct timespec nap = {interval_ms / 1000L,
                           (interval_ms % 1000L) * 1000000L};
    (void)nanosleep(&nap, NULL);
  }
}

static int run_second(in_port_t port, long silence_ms)
{
  int fd = udp_socket();
  bind_local(fd, port);
  struct timeval tv = {.tv_sec = 0, .tv_usec = 1000};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
    perror("setsockopt");
    return 1;
  }
  printf("rl-failover: second_up port=%u silence_ms=%ld\n",
         (unsigned)port, silence_ms);
  fflush(stdout);
  unsigned char buf[PAYLOAD];
  uint64_t beats = 0U;
  int64_t last = 0;
  const int64_t limit = (int64_t)silence_ms * INT64_C(1000000);
  for (;;) {
    ssize_t got = recv(fd, buf, sizeof(buf), 0);
    int64_t now = now_ns();
    if (got > 0) {
      beats += 1U;
      last = now;
      continue;
    }
    if (last != 0 && now - last >= limit) {
      int64_t delay_us = (now - last) / 1000;
      printf("rl-failover: takeover takeover_us=%" PRId64
             " heartbeats=%" PRIu64 "\n", delay_us, beats);
      printf("rl-failover: done rc=0\n");
      fflush(stdout);
      return 0;
    }
  }
}

int main(int argc, char **argv)
{
  if (argc == 4 && strcmp(argv[1], "holder") == 0) {
    return run_holder((in_port_t)atoi(argv[2]), strtol(argv[3], NULL, 10));
  }
  if (argc == 4 && strcmp(argv[1], "second") == 0) {
    return run_second((in_port_t)atoi(argv[2]), strtol(argv[3], NULL, 10));
  }
  fprintf(stderr, "usage: %s holder|second <port> <ms>\n", argv[0]);
  return 2;
}
