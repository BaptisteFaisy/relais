#!/usr/bin/env python3
"""Relais — one machine in a real virtual machine, one on the host (spec 7.1, level 3).

    python3 host/bench/vm_demo.py [--ticks 60] [--period-ms 200] [--out runs/vm]

What runs:
  * the actuator on the host — it owns the tempo, the mandate and the rack;
  * machine A on the host, machine B inside a QEMU guest (Linux kernel booted
    with a hand-made initramfs that contains nothing but a static relais-node);
  * the fault is injected through QMP on the guest: `stop` freezes the whole
    virtual machine — CPU, timers and network — exactly like a computer that
    hangs, and `cont` wakes it up (spec 4.11).

Evidence produced in --out:
  actuator.jsonl     the journal, which is the authority
  guest-console.log  what machine B printed, before and after the freeze
  guest-frozen.png   screendump of the guest while it is frozen
  guest-after.png    screendump once it is back and has stood down as second
  qmp.log            every QMP command and answer
  proof.svg          the command curve through the freeze, the source of each
                     tick, the mandates, and the verifier's verdict

Honesty: the guest is Linux, not QNX — there is no SDP on this machine to build
for QNX (see target/README.md). The protocol core is the same C11 either way;
what this proves is the bench mechanics and the protocol across a real virtual
machine boundary, with a real freeze of a real guest.
"""
import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KERNEL_CANDIDATES = ["/boot/vmlinuz-6.14.0-37-generic", "/boot/vmlinuz"]
HOST_ACTUATOR_PORT = 7100
HOST_NODE_A_PORT = 7101
GUEST_NODE_PORT = 7101
GUEST_FWD_PORT = 7302
GUEST_IP = "10.0.2.15"
HOST_IN_GUEST = "10.0.2.2"


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def build_guest(workdir):
    """Static node + minimal init, packed into an initramfs."""
    os.makedirs(workdir, exist_ok=True)
    node = os.path.join(workdir, "relais-node")
    init = os.path.join(workdir, "init")
    proc = run(["gcc", "-std=c11", "-O2", "-static", "-D_POSIX_C_SOURCE=200809L",
                "-Icore/include", "host/node/relais-node.c", "build/librelais.a",
                "-lm", "-o", node])
    if proc.returncode != 0:
        sys.exit("build relais-node statique impossible:\n" + proc.stderr[-800:])
    # subprocess ne passe pas par un shell : les guillemets du littéral C font
    # partie de l'argument, et rien d'autre ne doit les entourer.
    proc = run(["gcc", "-std=c11", "-O2", "-static", "-D_GNU_SOURCE",
                '-DGUEST_IP="%s"' % GUEST_IP, "host/bench/guest/init.c",
                "-o", init])
    if proc.returncode != 0:
        sys.exit("build init impossible:\n" + proc.stderr[-800:])
    root = os.path.join(workdir, "initrd")
    for sub in ("bin", "proc", "sys", "dev", "tmp"):
        os.makedirs(os.path.join(root, sub), exist_ok=True)
    shutil.copy(init, os.path.join(root, "init"))
    shutil.copy(node, os.path.join(root, "bin", "relais-node"))
    for path in (os.path.join(root, "init"), os.path.join(root, "bin", "relais-node")):
        os.chmod(path, 0o755)
    image = os.path.join(workdir, "initrd.cpio.gz")
    with open(image, "wb") as out:
        find = subprocess.Popen(["find", "."], cwd=root, stdout=subprocess.PIPE)
        cpio = subprocess.Popen(["cpio", "-o", "-H", "newc", "--quiet"], cwd=root,
                                stdin=find.stdout, stdout=subprocess.PIPE)
        find.stdout.close()
        gzip = subprocess.Popen(["gzip", "-9"], stdin=cpio.stdout, stdout=out)
        cpio.stdout.close()
        gzip.wait()
        cpio.wait()
        find.wait()
    return image


class Qmp:
    """QMP over a unix socket: hello, capabilities, then commands."""

    def __init__(self, path, log):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise RuntimeError("QMP injoignable : %s" % path)
        self.log = log
        self._read()                      # the greeting
        self._send({"execute": "qmp_capabilities"})

    def _read(self):
        data = b""
        self.sock.settimeout(10)
        while b"\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        for line in data.decode("utf-8", "replace").splitlines():
            if line.strip():
                self.log.write(line + "\n")
        self.log.flush()
        return json.loads(data.decode("utf-8", "replace").splitlines()[0]) if data else {}

    def _send(self, payload):
        self.sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
        return self._read()

    def cmd(self, name, **args):
        answer = self._send({"execute": name, "arguments": args} if args
                            else {"execute": name})
        self.log.write("> %s %s\n" % (name, args or ""))
        self.log.flush()
        return answer


def first_holder(journal):
    """Which machine got the first mandate."""
    with open(journal, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if '"mandate_granted"' in line:
                try:
                    return json.loads(line).get("node", "?")
                except ValueError:
                    return "?"
    return "?"


def wait_armed(journal, timeout_s):
    """Wait until the actuator has granted a mandate."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if os.path.exists(journal):
            with open(journal, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    if '"mandate_granted"' in line:
                        return True
        time.sleep(0.3)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ticks", type=int, default=60)
    ap.add_argument("--period-ms", type=int, default=200)
    ap.add_argument("--freeze-at", type=int, default=20, help="tick of the freeze")
    ap.add_argument("--freeze-ticks", type=int, default=12)
    ap.add_argument("--slow", type=int, default=4,
                    help="facteur de ralenti (CMD-07) : la physique ne change pas, "
                         "seul le temps mur s'étire — indispensable sans KVM")
    ap.add_argument("--out", default="runs/vm")
    args = ap.parse_args()

    kernel = next((k for k in KERNEL_CANDIDATES if os.path.exists(k)), None)
    if kernel is None:
        sys.exit("aucun noyau lisible dans /boot : pas de démonstration possible")
    if not os.path.exists(os.path.join(ROOT, "build", "relais-actuator")):
        sys.exit("build/ manquant : lance `make` d'abord")

    out = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    journal = os.path.join(out, "actuator.jsonl")
    guest_log = os.path.join(out, "guest-console.log")
    qmp_log = open(os.path.join(out, "qmp.log"), "w", encoding="utf-8")
    qmp_path = os.path.join(out, "qmp.sock")

    initrd = build_guest(os.path.join(out, "guest-build"))
    # CMD-01 borne T a 1..50 ms : le ralentissement passe par la logique,
    # pas par la periode (CMD-07). p=3 laisse 4 periodes a un invite emule
    # sans KVM, qui n'a pas la regularite d'une cible reelle.
    params = "T=%d,p=3,N=8,slow=%d" % (args.period_ms, args.slow)
    traj = "kind=slalom amp=0.6 freq=0.5"
    procs = []

    actuator = subprocess.Popen(
        [os.path.join(ROOT, "build/relais-actuator"), "--port", str(HOST_ACTUATOR_PORT),
         "--peer", "A=127.0.0.1:%d" % HOST_NODE_A_PORT,
         "--peer", "B=127.0.0.1:%d" % GUEST_FWD_PORT,
         "--ticks", str(args.ticks), "--params", params, "--trajectory", traj,
         "--journal", journal],
        cwd=ROOT, stdout=open(os.path.join(out, "actuator.log"), "w"),
        stderr=subprocess.STDOUT)
    procs.append(("actuator", actuator))

    guest_cmdline = ("console=ttyS0 console=tty0 rdinit=/init --self B --port %d "
                     "--actuator %s:%d --peer %s:%d --ticks %d --params %s --verbose"
                     % (GUEST_NODE_PORT, HOST_IN_GUEST, HOST_ACTUATOR_PORT,
                        HOST_IN_GUEST, HOST_NODE_A_PORT, args.ticks, params))
    guest = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-initrd", initrd,
         "-append", guest_cmdline, "-m", "512", "-smp", "1", "-no-reboot",
         "-vga", "std", "-display", "none", "-serial", "file:%s" % guest_log,
         "-qmp", "unix:%s,server,nowait" % qmp_path,
         "-netdev", "user,id=n0,hostfwd=udp:127.0.0.1:%d-:%d" % (GUEST_FWD_PORT,
                                                                 GUEST_NODE_PORT),
         "-device", "virtio-net-pci,netdev=n0"],
        cwd=ROOT, stdout=open(os.path.join(out, "qemu.log"), "w"),
        stderr=subprocess.STDOUT)
    procs.append(("qemu-guest", guest))

    def start_node_a():
        proc = subprocess.Popen(
            [os.path.join(ROOT, "build/relais-node"), "--self", "A",
             "--port", str(HOST_NODE_A_PORT),
             "--actuator", "127.0.0.1:%d" % HOST_ACTUATOR_PORT,
             "--peer", "127.0.0.1:%d" % GUEST_FWD_PORT,
             "--ticks", str(args.ticks), "--params", params],
            cwd=ROOT, stdout=open(os.path.join(out, "node-A.log"), "w"),
            stderr=subprocess.STDOUT)
        procs.append(("node-A", proc))
        return proc

    try:
        qmp = Qmp(qmp_path, qmp_log)
        print("QMP: %s" % qmp.cmd("query-status").get("return", {}))
        if not wait_armed(journal, 180):
            sys.exit("le mandat n'a pas été attribué (voir %s)" % out)
        holder = first_holder(journal)
        print("mandat attribué à %s ; la machine A démarre et devient second" % holder)
        start_node_a()
        time.sleep(6 * args.period_ms * args.slow / 1000.0)

        wall = args.period_ms * args.slow / 1000.0
        freeze_at = args.freeze_at * wall
        time.sleep(max(0.0, freeze_at - (time.time() - _started)))
        print("== QMP stop : gel complet de la machine virtuelle B ==")
        qmp.cmd("stop")
        print("   statut: %s" % qmp.cmd("query-status").get("return", {}))
        qmp.cmd("screendump", filename=os.path.join(out, "guest-frozen.png"))
        time.sleep(args.freeze_ticks * wall)
        print("== QMP cont : dégel ==")
        qmp.cmd("cont")
        time.sleep(6 * wall)
        qmp.cmd("screendump", filename=os.path.join(out, "guest-after.png"))
        remaining = args.ticks * wall - (time.time() - _started)
        if remaining > 0:
            time.sleep(min(remaining, 60))
        make_proof(out, journal, args)
    finally:
        for name, proc in procs:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for name, proc in procs:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        qmp_log.close()
    print("run: %s" % out)
    return 0


def make_proof(out, journal, args):
    """One image: the command curve through the freeze, the source of every
    tick, the mandates, and the verifier's verdict."""
    sys.path.insert(0, os.path.join(ROOT, "host", "report"))
    import charts
    from journal_io import read_journal

    jrn = read_journal(journal)
    ticks = [t.tick for t in jrn.ticks]
    values = [t.value for t in jrn.ticks]
    sources = [t.source for t in jrn.ticks]
    holders = [t.holder or "-" for t in jrn.ticks]
    mandates = [t.mandate for t in jrn.ticks]
    events = [(e.tick, e.event) for e in jrn.events
              if "mandate" in e.event or e.event in ("cession", "fallback_enter")]
    curve = charts.series_svg(ticks, values, None, title="Command applied by the actuator")
    strip = charts.source_strip_svg(sources)
    mandate = charts.mandate_strip_svg(holders, mandates, events)
    verdict = run([os.path.join(ROOT, "build/relais-verify"), journal,
                   "--level", "socle"])
    text = (verdict.stdout or "").strip().replace("&", "&amp;").replace("<", "&lt;")
    height = 300 + 90 + 95 + 90
    svg = ("<svg xmlns='http://www.w3.org/2000/svg' width='900' height='%d'>"
           "<rect width='900' height='%d' fill='white'/>"
           "<text x='16' y='24' font-family='sans-serif' font-size='15' "
           "font-weight='bold'>Relais — machine B inside QEMU, frozen with QMP "
           "`stop`, then `cont`</text>"
           "<g transform='translate(0,32)'>%s</g>"
           "<g transform='translate(0,332)'>%s</g>"
           "<g transform='translate(0,427)'>%s</g>"
           "<text x='16' y='%d' font-family='monospace' font-size='11'>%s</text>"
           "</svg>" % (height, height, curve, strip, mandate, height - 20,
                      text.splitlines()[0] if text else ""))
    with open(os.path.join(out, "proof.svg"), "w", encoding="utf-8") as fh:
        fh.write(svg)
    print("proof: %s" % os.path.join(out, "proof.svg"))


_started = time.time()

if __name__ == "__main__":
    sys.exit(main())
