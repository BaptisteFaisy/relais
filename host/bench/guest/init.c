/* Relais — minimal init for a QEMU guest running one computer of the pair.
 *
 * The point of this file is that there is nothing else in the guest: no
 * distribution, no root filesystem, no shell. A static binary is the init, it
 * mounts what the kernel needs, gives the virtual NIC the address the bench
 * expects (CONFIG_IP_PNP is off in stock kernels, so the `ip=` parameter cannot
 * do it), and execs relais-node.
 *
 * What this proves: the same core, in a real virtual machine, driven by the
 * bench through QMP -- stop, cont, quit, set_link -- with the actuator outside.
 * The guest is Linux, not QNX: there is no SDP here to build for QNX. The
 * protocol core is the same C11 either way; see target/ for the QNX processes.
 */
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>

#ifndef GUEST_IP
#define GUEST_IP "10.0.2.15"
#endif

static int set_addr(const char *ifname, const char *ip, const char *mask) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  struct ifreq req;
  memset(&req, 0, sizeof req);
  snprintf(req.ifr_name, sizeof req.ifr_name, "%s", ifname);
  struct sockaddr_in *sin = (struct sockaddr_in *)&req.ifr_addr;
  sin->sin_family = AF_INET;
  if (inet_pton(AF_INET, ip, &sin->sin_addr) != 1) { close(fd); return -1; }
  if (ioctl(fd, SIOCSIFADDR, &req) != 0) { close(fd); return -1; }
  memset(&req, 0, sizeof req);
  snprintf(req.ifr_name, sizeof req.ifr_name, "%s", ifname);
  struct sockaddr_in *nm = (struct sockaddr_in *)&req.ifr_netmask;
  nm->sin_family = AF_INET;
  inet_pton(AF_INET, mask, &nm->sin_addr);
  if (ioctl(fd, SIOCSIFNETMASK, &req) != 0) { close(fd); return -1; }
  memset(&req, 0, sizeof req);
  snprintf(req.ifr_name, sizeof req.ifr_name, "%s", ifname);
  req.ifr_flags = IFF_UP | IFF_RUNNING;
  int rc = ioctl(fd, SIOCSIFFLAGS, &req);
  close(fd);
  return rc;
}

/* The kernel drops boot arguments that contain ':' or '=' before handing them
 * to init, which is exactly what an address like 10.0.2.2:7100 looks like. The
 * raw command line survives in /proc/cmdline, so the arguments are read back
 * from there, starting at a marker the bench puts on the command line. */
#define MARKER "--self"
#define MAX_ARGS 24

static int build_argv(char *buf, char **argv) {
  char *at = strstr(buf, MARKER);
  if (at == NULL) return -1;
  int argc = 0;
  argv[argc++] = (char *)"/bin/relais-node";
  for (char *save = NULL, *tok = strtok_r(at, "\n", &save); tok != NULL && argc < MAX_ARGS - 1;
       tok = strtok_r(NULL, "\n", &save)) {
    for (char *s2 = NULL, *word = strtok_r(tok, " \t", &s2); word != NULL && argc < MAX_ARGS - 1;
         word = strtok_r(NULL, " \t", &s2))
      argv[argc++] = word;
  }
  argv[argc] = NULL;
  return argc;
}

int main(void) {
  mount("proc", "/proc", "proc", 0, NULL);
  mount("sysfs", "/sys", "sysfs", 0, NULL);
  mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
  set_addr("lo", "127.0.0.1", "255.0.0.0");
  if (set_addr("eth0", GUEST_IP, "255.255.255.0") != 0)
    printf("init: eth0 introuvable, le reseau ne marchera pas\n");
  printf("init: %s pret\n", GUEST_IP);

  static char cmdline[2048];
  int fd = open("/proc/cmdline", O_RDONLY);
  ssize_t got = fd >= 0 ? read(fd, cmdline, sizeof cmdline - 1) : -1;
  if (fd >= 0) close(fd);
  if (got > 0) cmdline[got] = '\0';
  else cmdline[0] = '\0';

  char *argv[MAX_ARGS];
  if (got <= 0 || build_argv(cmdline, argv) < 0) {
    printf("init: aucun argument '%s' sur la ligne de commande du noyau\n", MARKER);
    return 1;
  }
  for (int i = 0; argv[i] != NULL; i++) printf("init: argv[%d]=%s\n", i, argv[i]);
  fflush(stdout);
  execv("/bin/relais-node", argv);
  printf("init: exec /bin/relais-node impossible\n");
  return 1;
}
