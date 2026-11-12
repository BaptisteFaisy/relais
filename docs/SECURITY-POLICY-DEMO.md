# Security policy demonstration (idea 35: the opposed policy)

An assertion that "the system refuses an attacker" is worth nothing until an
attacker has actually tried. This document records what is demonstrated, where,
by which command, and — just as important — **what can only be demonstrated on
QNX** and is therefore *not* proven by a PC run.

Two artefacts:

- `host/bench/intruder.c` — the adversarial process. It attacks, and prints the
  real result of the real system call, or the real answer of the real core.
- `target/intruder_test.sh` — the same attack inside QNX 8.0, preceded by the
  real typed security policy (`on`) and followed by the kernel's own errno.

## On a PC: `make demo-policy`

```
$ make demo-policy
relais-intruder: an adversarial process, run on purpose (idea 35)
[REFUSED] open /dev/relais O_RDWR                      No such file or directory (no such resource on this host)
[REFUSED] open /dev/relais/status O_RDWR               No such file or directory (no such resource on this host)
[REFUSED] open /dev/relais/temoin O_RDWR               No such file or directory (no such resource on this host)
[REFUSED] open /dev/relais/ctl O_RDWR                  No such file or directory (no such resource on this host)
[REFUSED] reach the channel relais-ctl                 no name_attach on this host: the channel does not exist
[REFUSED] send a frame to relay-core port 7100         sent 69 bytes, no core answers: Connection refused
[REFUSED] forged ORDRE, 69 bytes, wrong key            core refuses it, reason=check (SEC-02)
[REFUSED] signed ORDRE claiming mandate 9999           arbiter rejects it (mandate_future) and applies -0.000, not the forged 1.0 (SEC-04)
intruder: 8 attempt(s) refused, 0 granted, 1 audit finding(s)
POLICY-DEMO-OK: every attack was refused
```

Exit code 0. Reproduce with `./build/relais-intruder --port 7100`.

What each line really proves:

| attempt | evidence | requirement |
|---|---|---|
| open the four `/dev/relais*` resources | `ENOENT` from `open(2)` | QNX-01 |
| reach the `relais-ctl` channel | no `name_attach` on this host | spec 4.9 |
| send a frame to the relay-core port | 69 bytes sent, then `ECONNREFUSED` on the connected UDP socket: the kernel answered with ICMP port-unreachable, so **no core ever received it** | SEC-01 |
| forge an ORDRE without the key | the core itself returns `reason=check` — the SipHash trailer does not match, the frame is dropped before any state changes | SEC-02 |
| sign an ORDRE that claims mandate 9999 | the frame is well formed and authenticated, so it *parses*; the arbiter then rejects it (`mandate_future`) and applies `-0.000` from its own fallback law instead of the forged `1.0` full lock | SEC-04, MAN-02, ACT-05 |

The last two lines are the interesting ones: they do not depend on the operating
system at all. They run the real `relais_frame_decode` and the real
`relais_arb_step` from `core/`, the same code that runs on the target. An
attacker who knows the wire format and even holds a valid key still cannot
appoint itself, and cannot make the actuator apply its value.

### The audit finding the same binary reports

```
$ ./build/relais-intruder --port 7100 --journal /var/tmp/m22-journal.jsonl
[AUDIT  ] append a forged record to /var/tmp/m22-journal.jsonl wrote 99 bytes: the journal is NOT tamper-proof (ATTACK-SURFACE.md)
intruder: 8 attempt(s) refused, 0 granted, 1 audit finding(s)
POLICY-DEMO-OK: every attack was refused
```

This is deliberately **not** counted as a breach of the opposed policy — the
policy concerns the interfaces the actuator exposes — but it is a real gap and
it is reported rather than hidden: the journal is a plain file with no hash
chain and no signature, so a local attacker who can write it can forge the
evidence the verifier reads. Consequence and planned fix (chained journal,
mission M21) are in `docs/ATTACK-SURFACE.md` §4. The demo still exits 0 because
the eight attacks were refused; the finding is printed on stdout for the record.

## What a PC run does NOT prove

The PC demonstration shows refusal **by absence**: there is no `/dev/relais`, no
channel, nothing listening. That is an honest result, and it is weaker than the
real thing. Specifically, a PC run cannot show:

1. **That the QNX typed security policy is what refuses the access.** On the
   target the errno must be `EACCES`/`EPERM` from the policy, not `ENOENT` from a
   missing path. Only `on /dev/relais` plus a denied `open` proves the type is
   assigned and enforced.
2. **That the resource manager itself is confined.** QNX-01 says `/dev/relais`
   supports open, read and write and *nothing else* — no `devctl`. That is a
   property of the resource manager's dispatch, visible only on the target.
3. **That a process without the `relais` type cannot `MsgSend` to `relais-ctl`
   or `relais-net`.** On a PC there is no message passing at all.
4. **That the priorities cannot be usurped.** `RELAIS_PRIO_NET 62 >
   RELAIS_PRIO_CORE 61 > RELAIS_PRIO_CTL 60 > RELAIS_PRIO_TOOL 10` (CMD-04,
   QNX-02) is enforced by the scheduler, not by the code.
5. **That `slog2` records are protected.** The journal goes to `slog2` on the
   target; its access policy is a deployment artefact.
6. **That the actuator's UDP socket is reachable only from the intended links.**
   On QNX `rl_udp_bind` binds `INADDR_ANY` (unlike the PC bench, which binds
   loopback), so on the target the network policy and the MAC are the only
   barriers. That difference is exactly what needs a real target to be opposed.

## On QNX: `target/intruder_test.sh`

Run it once the SDP 8.0 is available and the three roles are running:

```sh
make -C target                       # relais-core, relais-net, relais-ctl
qcc -V gcc_ntoaarch64le -O2 -Wall -Wextra -Werror -std=c11 \
    -Icore/include -Itarget/include -D__QNX__ \
    host/bench/intruder.c build/librelais.a -lm -o intruder
PORT=7100 JOURNAL=/var/log/relais/journal.jsonl INTRUDER=./intruder \
    sh target/intruder_test.sh | tee policy-demo.txt
```

The script prints, in order, and keeps the output verbatim:

1. `on /dev/relais`, `/dev/relais/status`, `/dev/relais/temoin`,
   `/dev/relais/ctl` — the security descriptor of each resource, i.e. the policy
   as the kernel states it;
2. `pidin -F '%a %n %t'` filtered on `relais|intruder` — the type each process
   actually runs under;
3. the intruder itself — **the same binary source as on the PC**, so the two
   demonstrations cannot drift apart;
4. `cat` on each `/dev/relais*` as a third-party process, with the kernel's own
   errno — this is where `EACCES` replaces the PC's `ENOENT`;
5. a forged frame on the wire, then `grep -E 'order_rejected|forged'` on the
   journal, so the refusal is also visible in the actuator's own record
   (DET-04);
6. `POLICY-DEMO-OK` or `POLICY-DEMO-FAILED`, and the matching exit code.

Until that run exists, the honest claim is the one printed above: **on this
machine the attacks are refused by absence of path, of channel and of listener,
and by the core's own checks; the typed security policy is designed and
documented but not yet opposed.** The intruder is written so that the day the
SDP is installed, the demonstration is one command away and produces the real
output rather than a promise.

## Relation to the other proofs

| question | answered by |
|---|---|
| can a hostile byte string break the parser? | `host/fuzz/` — 29.4 M executions under ASAN/UBSAN, 0 crash (`docs/FUZZING.md`) |
| can a node command without a mandate? | the exhaustive explorer and the campaign: I1…I11, 10 M states, 0 violation |
| can an attacker reach the interfaces at all? | this document, `relais-intruder` |
| what is left unprotected? | `docs/ATTACK-SURFACE.md` §4–§7, including the trust assumptions |
