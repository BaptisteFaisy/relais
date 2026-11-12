/* Relais — relaisctl, the service tool (priority 10).
 *
 * What an operator and the bench use to look at one machine and to poke it:
 *   relaisctl status            the role, the mandate, the replica age, the lamp
 *   relaisctl temoin            the lamp and its cause (DEG-01, DEG-02)
 *   relaisctl handover          cede the mandate for maintenance (REL-09)
 *   relaisctl bias <value>      inject a value fault (INJ-10)
 *   relaisctl die <P1..P9> <tick>   arm a death point (INJ-07)
 *   relaisctl fault <reason>    report a local process death (DET-05)
 *
 * These are test commands: they are compiled out of the production image
 * (SEC-05), and the typed security policy gives an untrusted process no path to
 * /dev/relais at all.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "relais_qnx.h"

static void usage(void) {
  fprintf(stderr,
          "usage: relaisctl status | temoin | handover | bias <value>\n"
          "              | die <P1..P9> <tick> | fault <reason> [--json]\n");
}

static int read_device(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t got = read(fd, buf, cap - 1);
  close(fd);
  if (got < 0) return -1;
  buf[got] = '\0';
  return 0;
}

static int write_device(const char *path, const char *line) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  ssize_t sent = write(fd, line, strlen(line));
  close(fd);
  return sent < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
  bool json = false;
  const char *cmd = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--json")) json = true;
    else if (cmd == NULL) cmd = argv[i];
  }
  if (cmd == NULL) {
    usage();
    return 2;
  }

  char buf[RELAIS_STATUS_LEN];
  char line[96];
  if (!strcmp(cmd, "status") || !strcmp(cmd, "temoin")) {
    const char *path =
        !strcmp(cmd, "status") ? RELAIS_DEV_STATUS : RELAIS_DEV_TEMOIN;
    if (read_device(path, buf, sizeof buf) != 0) {
      fprintf(stderr, "relaisctl: %s illisible (%s)\n", path, strerror(errno));
      return 1;
    }
    if (json) {
      /* One key, one value: enough for the bench, no parser dependency. */
      buf[strcspn(buf, "\n")] = '\0';
      printf("{\"device\":\"%s\",\"value\":\"%s\"}\n", cmd, buf);
    } else {
      fputs(buf, stdout);
      if (buf[0] != '\0' && buf[strlen(buf) - 1] != '\n') putchar('\n');
    }
    return 0;
  }

  /* Everything below writes one text line on /dev/relais/ctl; the grammar is
   * the one qnx_util.c parses, so there is a single definition of it. */
  if (!strcmp(cmd, "handover"))
    snprintf(line, sizeof line, "handover\n");
  else if (!strcmp(cmd, "bias") && argc > 2)
    snprintf(line, sizeof line, "bias %s\n", argv[2]);
  else if (!strcmp(cmd, "die") && argc > 3)
    snprintf(line, sizeof line, "die %s %s\n", argv[2], argv[3]);
  else if (!strcmp(cmd, "fault") && argc > 2)
    snprintf(line, sizeof line, "fault %s\n", argv[2]);
  else {
    usage();
    return 2;
  }
#ifndef RELAIS_PRODUCTION
  if (write_device(RELAIS_DEV_CTL, line) != 0) {
    fprintf(stderr, "relaisctl: %s refused (%s)\n", RELAIS_DEV_CTL, strerror(errno));
    return 1;
  }
  if (json) printf("{\"command\":\"%s\",\"result\":\"ok\"}\n", cmd);
  else printf("ok\n");
  return 0;
#else
  fprintf(stderr, "relaisctl: test commands are absent from a production image\n");
  return 1;
#endif
}
