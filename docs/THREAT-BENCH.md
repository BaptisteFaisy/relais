# Threat model of the bench (mission M20, idea 40)

This document states honestly what the Relais proofs do and do not cover when the
adversary controls the *bench* itself — the orchestrator, the host, the virtual
machines and the network between them — rather than a single process. It also
records the one byzantine frame that the current key model cannot attribute to
its real emitter.

Two separate adversaries are considered:

1. the **byzantine peer** (idea 16), a machine on the network that holds the link
   key and emits hostile but wire-valid frames;
2. the **compromised orchestrator** (idea 40), whoever runs the bench, the QEMU
   guests and the reporting pipeline.

The first is exercised by `host/byzantine/` and `core/tests/test_byzantine.c`.
The second is a limit of the method, not of the protocol, and is treated here.

---

## 1. The byzantine peer (idea 16)

`./build/run_byzantine` plays eleven attacks against the arbiter, in process,
through the real core, and prints the effect on the one value applied and on the
mandate. Reproduce with:

```
make byzantine          # or ./build/run_byzantine
```

Attacker model: an **insider** holding the shared link key (SEC-02). The result:

| Attack | Expected | Verdict |
| --- | --- | --- |
| order with a future mandate | SEC-04 reject | rejected, counted, no effect |
| order with a wrong (future/past) tick | SEC-03 reject | rejected, counted, no effect |
| replay of an authentic captured frame | SEC-03 reject | rejected by the tick tag |
| STATUT usurping the actuator | no effect | ignored: the arbiter never consumes a STATUT |
| corrupt but authenticated ETAT (stolen key) | no effect | ignored: the arbiter never consumes an ETAT |
| demande burst | SEC-06 no delay | holder order still applied, mandate not stolen |
| cession attributed to a non-holder | ignored | only the holder's own cession is honoured |
| **cession forged as the holder, emitted by B** | **should be rejected** | **EFFECT — see section 2** |
| order from a revoked node | idea 39 reject | rejected, counted (`revoked`) |
| order from an auth-banned node | idea 38 reject | rejected, counted (`auth_banned`) |
| frame forged without the link key | SEC-02 reject | never decodes (`check`), never reaches the arbiter |

Every attack is neutralised except the forged holder cession, which is a genuine
boundary of the key model and is described next rather than hidden.

---

## 2. Major finding: the forged holder CESSION

**File / function.** `core/src/arbiter.c`, `scan()`, case `RELAIS_MSG_CESSION`.

**Observed.** A cession is honoured when `sender == holder` and `mandate ==
current`. The sender field is authenticated by the MAC, but the MAC is computed
with a key that the actuator shares with **both** computers. A compromised
machine B can therefore emit a frame whose `sender` field says A (the holder),
sign it with the shared key, and the arbiter accepts it as A's own cession: at
the next tick (REL-06) the mandate is freed and, if B is the certified second,
handed to B with `mandate++`.

**Expected.** A cession attributed to A should be attributable to A alone.

**Requirement.** SEC-02 says the authenticator uses "a key specific to the
link". The bench actuator (`actuator/pc/relais-actuator.c`, `drain_socket`) and
the QNX transport (`target/net.c`, `pump_socket`) use **one** key for all peers
and never bind a decoded frame's `sender` to the link it arrived on. The core
cannot fix this: it receives frames already decoded and authenticated, with no
view of the transport.

**Mitigation (out of M20 core scope).** Either (a) one distinct key per
computer↔actuator link, so B cannot sign a frame that verifies on A's link, or
(b) a sender↔link binding at the transport: a frame decoded on B's socket whose
`sender` is not B is dropped as malformed before it reaches the arbiter. Both are
transport changes; the M20 revocation list (section 3) is the in-core stopgap an
operator can use once the imposture is noticed.

This is reported, not masked: `run_byzantine` marks it `EFFECT <-- major result`
and exits 0 so the bench stays usable while the finding stays visible.

---

## 3. In-core countermeasures added by M20

Both are **off by default**, so the proven behaviour of the protocol is unchanged
unless the operator asks for them (non-regression is tested in
`core/tests/test_byzantine.c`).

- **Revocation list (idea 39, MAN-11).** `relais_arb_revoke(arb, node)` puts a
  computer on a fixed-size list; from then on every one of its frames is rejected
  and counted (`RELAIS_REJ_REVOKED`) before any protocol rule, and it never
  refreshes the node's liveness. The list travels in the actuator snapshot, so it
  survives a restart. There is no un-revoke: clearing one is an operator act, done
  by restoring a snapshot taken before it. This is the response to section 2 once
  the impostor is identified.

- **Safe degraded mode on the authenticator (idea 38, DEG-09).** The transport
  reports each MAC failure with `relais_arb_auth_fail(arb, node)`; after
  `params.auth_strikes` consecutive failures for the same sender (0 disables it,
  the recommended value is 3) the sender is banned, its frames are rejected and
  counted (`RELAIS_REJ_AUTH_BANNED`), and a `AUTH_BANNED` event is journaled. A
  success clears the streak. The ban is sticky and survives a restart.

Wiring these two into the QNX transport (`relais-net`) and exposing a `relaisctl`
revocation command is a follow-up integration; M20 delivers the core API, its
unit tests and the bench that exercises it.

---

## 4. The compromised orchestrator (idea 40)

The bench is run by an orchestrator that starts the QEMU guests, freezes one with
QMP, cuts links, collects the journals and produces the report. An attacker who
controls the orchestrator controls the *evidence*. Concretely it can:

- **kill or freeze a guest** at a chosen instant, to stage or hide a handover;
- **cut or replay a link**, to manufacture the silence that revokes a mandate;
- **falsify a journal after the run**, rewriting the JSON-lines so the invariants
  appear satisfied when they were not;
- **report numbers that were never produced**, since it owns the pipeline that
  prints them.

The protocol cannot defend against its own bench: the arbiter is correct, but a
lying orchestrator can present a false transcript of it. What limits the damage:

1. **A chained / signed journal (mission M21).** If each journal line carries a
   hash of the previous one, or the run is signed, an after-the-fact rewrite
   breaks the chain and is detectable offline. This turns "trust the file" into
   "verify the file".

2. **Offline verification on an independent copy.** `relais-verify`
   (`host/verify/`) re-checks the invariants I1..I11 and DET-03/DET-09 from a
   journal, with no access to the run that produced it. Running it on a copy
   taken by a third party — not by the orchestrator — removes the orchestrator
   from the trust path of the *verification* step.

3. **Reproduction on independent hardware.** The only complete answer. The same
   seed, the same source and the same commands must give the same numbers on a
   machine the attacker does not control.

---

## 5. The honest limit

A compromised bench can lie about its results. No amount of in-process proof —
exhaustive exploration, seeded simulation, mutation score — detects an
orchestrator that fabricates the transcript, because all of it runs under that
orchestrator. The proofs in this repository establish that **the code is
correct**, not that **a given published run happened as claimed**. Only a third
party reproducing the run on its own hardware, verifying an independently copied
journal with `relais-verify`, and (from M21) checking the journal's chain, can
close that gap. Everything short of that is a claim, and is labelled as one.
