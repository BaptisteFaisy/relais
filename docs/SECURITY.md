# Relais — security model

This document states exactly what the protocol protects, against whom, with what
mechanisms, and — just as exactly — what it does **not** protect. Every claim
below names the file that implements it and a command that checks it.

Requirement ids are those of `docs/SPEC-2.md` section 5.10 (`SEC-01` … `SEC-07`)
and the journal chain is idea 34 of the annex.

---

## 1. Assets

| Asset | Where it lives | What it is worth |
|---|---|---|
| The applied steering value | the actuator, one per tick | the safety property itself |
| The mandate and the holder | the actuator's arbiter (`core/src/arbiter.c`) | who is allowed to command |
| The link key (pre-shared, then session) | RAM of `relais-net`, `relais-core`, the actuator | the only thing that makes a frame authentic |
| The journal | a file on the actuator | the evidence a campaign is judged on |

Nothing in `core/` is secret by construction: the core is pure C11 with no clock,
no file, no socket and no allocation, so it never fetches, stores or leaks a
secret on its own. Keys are handed in by the caller and wiped by
`relais_kx_clear()` / `relais_keyring_clear()` / `relais_key_clear()`.

## 2. Threat model

**The attacker controls the network between the three machines.** Concretely, it
can:

1. **Read** every frame (no confidentiality is claimed, see §5);
2. **Inject** arbitrary bytes on any link, at any rate;
3. **Modify** a frame in flight, including a single bit;
4. **Replay** a frame it captured, immediately or much later;
5. **Duplicate**, **delay**, **reorder** and **drop** frames;
6. **Edit the journal file** afterwards, if it ever gets write access to it.

**The attacker does not have:**

- the pre-shared key of a link, nor any key derived from it;
- write access to the machines' memory, to the QNX image, or to `/dev/relais`;
- the ability to make the actuator's clock or its tick counter lie.

The last two are the boundary of this model. QNX 8.0 process typing (`SEC-05`)
is what enforces them: only `relais-net` opens a socket, only `relais-core`
serves the device, and the test commands are absent from the production image.
A node whose software is compromised is outside the model — it holds the key and
can then say anything its role allows (see §5).

**Freshness anchor.** The actuator is the clock and the only writer of the
mandate number. Every frame names the tick it targets, and the actuator applies
exactly one value per tick, so "now" is not something an attacker can move.

## 3. Mechanisms

### 3.1 Frame integrity — SEC-01, SEC-02 (`core/src/check.c`)

A frame carries an 8-byte trailer: a CRC-32C at level S, a keyed SipHash-2-4
when the link holds a key. Control, version, size, sender and payload length are
checked before the payload is read (`core/src/frame.c`), and the trailer is
compared in constant time. A frame forged without the key is rejected, counted
(`DET-04`) and treated as a loss: the service continues on the shadow, the
testament or the fallback law.

```
./build/test_check      # 64 published SipHash vectors + 1000 single-bit alterations
./build/test_frame_fuzz
```

### 3.2 Anti-replay — SEC-03 (`core/src/anti_replay.c`, idea 33)

Two independent lines of defence:

- **the tick label** (already in the protocol): an order that does not name the
  tick being applied is refused (`RELAIS_REJ_TICK_MISSED`);
- **a sliding window per emitter** (this mission): the highest tick accepted
  from that sender, plus a 64-bit bitmap of the 63 ticks below it. A tick already
  recorded is refused as a replay; a tick that fell out of the window is refused
  as too old; a late tick inside the window is accepted and then remembered.

The window is what catches the two cases the tick label alone cannot: a frame
**duplicated** by the link inside the very tick it targets (`ROB-02`, the
simulator duplicates 2 % of frames), and a capture replayed while its tick is
still current. Its cost is constant — one 64-bit bitmap and one tick counter per
emitter — and its verdict does not depend on arrival order.

The window is a pure state machine: nothing allocates, nothing reads a clock, so
the same window can sit in `relais-net` on QNX and in the simulator on a PC.

```
./build/test_anti_replay   # includes 20000 draws against an independent model
```

### 3.3 Key agreement at start-up — SEC-07 (`core/src/keys.c`, idea 31)

A three-flight challenge/response over the pre-shared key (PSK) of the link:

```
flight 1   initiator -> responder   nonce_I || tag1
flight 2   responder -> initiator   nonce_R || tag2
flight 3   initiator -> responder   tag3

tagN = SipHash-2-4(PSK, "relais-kx-tag" || 0 || stage || id_I || id_R || nonces)
```

- `tag1` proves to the responder that the initiator holds the PSK.
- `tag2` proves the same to the initiator **and** binds `nonce_R`, so the
  initiator knows the session key is fresh and was not chosen by an attacker.
- `tag3` binds the whole transcript: replaying flights 1 and 2 of a captured
  session cannot complete it, because flight 3 cannot be produced without the
  PSK. This is what makes a captured exchange unusable as a shortcut into a new
  one.

Both ends then derive the same 128-bit session key from the finished transcript,
under two labels (`"relais-kx-key-0"` / `"-1"`), so the key does not rest on a
single 64-bit output. The node ids of the two ends are part of every derivation,
which binds a session key to one link: the same machine talking to another peer
gets another key.

Nonces are 32 bytes expanded from entropy the **caller** supplies (on QNX,
`/dev/random` read by `target/`, never by `core/`). `relais_kx_begin()` refuses
to start without entropy, because a predictable nonce would make every session
key predictable.

**Why this scheme and not X25519.** `SEC-07` asks for keys renewed at every
start-up by an authenticated exchange; it does not ask for forward secrecy. A
provisioned secret is already required for `SEC-02`, so the PSK is not a new
trust root — it is the one the deployment already has. Reusing SipHash-2-4,
which is already implemented and validated against its 64 published vectors,
keeps the safety core small and the primitive proven; re-implementing X25519
would add roughly 400 lines of unproven field arithmetic to a code base whose
every line has to be justified, for a property no requirement claims. The cost
is stated honestly in §5: **this scheme has no forward secrecy.**

```
./build/test_keys          # exchange, freshness, link binding, refusals, replay
./build/test_keyring       # rotation: window, promotion, no tick lost over 300 ticks
```

### 3.4 Key rotation in operation — SEC-07 (`core/src/keys.c`, idea 32)

A two-slot key set: `current` and `next`.

1. both ends stage the same `next` key and open a transition window of
   `RELAIS_KEYRING_WINDOW` (8) ticks;
2. the sender signs with `current`; the receiver accepts `current` **and**
   `next` while the window is open, so a peer that already switched loses
   nothing;
3. both ends count the window down in lockstep — one `relais_keyring_advance()`
   per tick — and when it closes `next` becomes `current` and the old key is
   wiped.

**No tick is lost by a rotation**, and this is a tested property, not an
intention: `test_sec07_rotation_loses_no_tick` runs 300 ticks of real frames
through a sender and a receiver keyring, rotates in the middle, and asserts that
all 300 are decoded, that the key really changed, and that the key which signed
before the rotation is refused afterwards.

`relais_keyring_decode()` is the receiving side of a rotating link: it tries the
current key then the staged one and decodes the frame in one call, so no caller
has to reimplement the acceptance rule.

### 3.5 Chained journal — idea 34 (`core/src/journal.c`)

Every record a current build writes carries, as its last key, the fingerprint of
the record before it chained to its own content:

```
chain(0) = 0000000000000000
chain(i) = SipHash-2-4(K, "relais-chain-v1" || 0 || chain(i-1) || body(i))
```

`body(i)` is the record exactly as formatted, without the `,"chain":"..."`
suffix and without the newline. The exact bytes and the hexadecimal form are
fixed in `CONTRACT.md`. The chain is computed in the pure core, so the writer
(the simulator, the PC actuator, the QNX actuator) and the reader
(`relais-verify`) cannot drift apart.

Because each fingerprint covers the previous one, editing a record, deleting it,
inserting one or reordering two of them breaks the chain **at the first record
that no longer follows**, and `relais-verify --check-chain` names that record and
prints both the expected and the found fingerprint.

**A cut tail is the one thing a hash chain cannot see**: a file whose last
records were removed is still a perfectly valid chain. The writers therefore
print an *anchor* — the fingerprint of the last record — and
`relais-verify --expect-chain <hex>` compares the file against it. The anchor is
only useful if it is recorded **outside** the journal (run log, signed report,
operator's record); that is a procedure, not a mechanism, and it is listed in §6.

```
./build/test_journal_chain                                   # pure, in-memory
./build/relais-simulate --ticks 300 --seed 7 --journal /tmp/a.jsonl --quiet
./build/relais-verify /tmp/a.jsonl --check-chain             # chain: OK
python3 -m unittest discover -s host/report -p 'test_*.py'   # edit/delete/insert/cut
```

### 3.6 What is wired today, and what is not

Being explicit about this is part of the threat model:

| Mechanism | In the running binaries? | Where |
|---|---|---|
| Frame CRC-32C / SipHash trailer | **yes** | every frame, `core/src/frame.c`, `--key` on the actuator and the node |
| Tick label (`SEC-03`) | **yes** | the arbiter refuses an order that does not name the tick being applied |
| Chained journal (idea 34) | **yes** | the simulator, the PC actuator and the QNX actuator all write it; `relais-verify --check-chain` reads it |
| Journal anchor | **yes** | printed by the simulator and the PC actuator; checked by `relais-verify --expect-chain` |
| Key exchange (`SEC-07`) | **not yet** | `core/src/keys.c` is a pure library, proven by `test_keys`; it still has to be called at start-up by `target/net.c` and `target/actuator_qnx.c`, which own the sockets |
| Key rotation (`SEC-07`) | **not yet** | same: `relais_keyring` is proven by `test_keyring`, and the send/receive paths have to be switched to it |
| Sliding window (idea 33) | **not yet** | `core/src/anti_replay.c` is proven by `test_anti_replay`; it belongs on the receive path, before the arbiter, whose own tick rule stays authoritative |

The three "not yet" rows are integration steps in `target/` and in the
actuators' receive path, not missing mechanisms: `SEC-07` is a level-X
requirement, the core is pure and cannot open a socket, and the arbiter is
another mission's file. Wiring them changes the runtime behaviour of a link
(which key is accepted, which duplicate is dropped), so it is a decision that
belongs to the integration of the protocol, and it is listed in §6.

### 3.7 What was already there

`SEC-04` (no machine appoints itself: a future mandate is refused), `SEC-05`
(QNX process typing) and `SEC-06` (at most `request_burst` demandes per
`request_window` ticks, so a flood of requests cannot delay the orders) are
implemented and tested elsewhere; see `docs/SAFETY.md` and `docs/RESULTS.md`.

## 4. Why it holds

The security of the link reduces to one secret: the PSK.

- Without it, an attacker cannot produce a valid trailer (`SEC-02`), cannot
  produce a flight tag, and therefore cannot complete an exchange or learn a
  session key (`SEC-07`). Its best play is to inject, replay, delay or drop —
  which the tick label, the window and the arbiter's own rules turn into a
  *loss*, never into a command: the actuator applies the shadow, the testament
  or the fallback law, and the invariants `I1`…`I11` are what makes that safe.
- With it, the attacker is a legitimate peer. Nothing in this design defends
  against that, and the honest statement is that the PSK is the whole trust
  root (§5).

Forgery resistance is 64 bits per attempt: SipHash-2-4 truncated to 64 bits.
That is a deliberate trade — the trailer is 8 bytes inside a frame of at most
512 — and it is acceptable only because `SEC-06` rate-limits what an attacker
can try and `DET-04` counts every refusal, so a brute-force attempt is visible
long before it is likely to succeed.

## 5. Limits (what this does **not** do)

1. **No confidentiality.** Frames are authenticated, not encrypted. Anyone on
   the link reads the setpoint, the position, the mandate and the holder. For a
   steering command inside one vehicle this is accepted; if it ever is not, the
   session key is already in place to feed a stream cipher, and that is a
   separate decision.
2. **No forward secrecy.** Session keys are derived from the PSK and public
   nonces. An attacker who records traffic and later obtains the PSK can
   recompute every past session key and forge past frames. Only a Diffie-Hellman
   exchange (X25519) would close this, at the cost stated in §3.3.
3. **No protection against a key holder.** A compromised node signs whatever it
   wants within its role. The arbiter still refuses a self-appointed mandate
   (`SEC-04`) and still applies one value per tick, so the *safety* invariants
   hold, but a compromised holder can command badly and only the disagreement
   and lamp logic will show it.
4. **No PKI, no identity beyond the key.** "A" is whoever holds A's key. There
   is no certificate, no revocation, no third party. Identities are the
   provisioned keys and the node ids in the frame header.
5. **The default journal chain key is public.** It is a constant in
   `core/src/journal.c` so that anybody can verify a journal. It detects
   corruption and naive editing, not an attacker who knows the algorithm and can
   rewrite the whole file. A secret `--chain-key` turns the fingerprint into a
   MAC and closes that; the option exists on the simulator, both actuators and
   the verifier, and provisioning it is a deployment task (§6).
6. **Truncation needs an anchor** (§3.5), and the anchor needs a place to live
   outside the journal.
7. **Entropy is the caller's job.** The core never reads a device. A deployment
   that feeds it weak or repeated entropy gets weak or repeated session keys;
   `relais_kx_begin()` refuses to run with no entropy at all, but it cannot
   judge the quality of what it is given.
8. **No side-channel defence beyond the constant-time comparisons.** Key
   handling is not blinded, and the process is not locked in memory.

## 6. What remains for a product

| Item | Why | Shape |
|---|---|---|
| Key provisioning | the PSK is the whole trust root | per-vehicle secret injected at production, never in the image or in source |
| HSM or secure element | a key in RAM can be read | PSK and chain key held by a TPM/SE; the core signs through it, `core/` stays pure |
| Key lifecycle | rotation exists, its *policy* does not | rotate on a schedule and on every maintenance access; revoke by re-provisioning; record which key signed which campaign |
| Wiring `SEC-07` and the window | §3.6: three mechanisms are proven but not yet called by the running processes | call `relais_kx_*` at start-up and route the send/receive paths through `relais_keyring`; put `relais_replay_set_accept()` on the receive path of `relais-net` and of the actuator |
| Chained journal with a secret key | §5 item 5 | pass `--chain-key` from the secure element, keep the key out of the run log |
| Anchor sealing | §5 item 6 | write the last fingerprint to a signed run summary, or to the next run's header |
| Forward secrecy | §5 item 2 | X25519 in `core/`, validated against the RFC 7748 vectors, only if a requirement ever asks for it |
| Auditing | refusals are counted but not exported | publish the per-reason counters (`relais_arb_rejected_total`, the window's `rejected_replay` / `rejected_old`) through `/dev/relais/status` and slog2 |
| Boot integrity | a modified image invalidates everything above | measured boot on the QNX target, out of scope of this repository |

## 7. Where to look

| Topic | File | Test |
|---|---|---|
| CRC-32C, SipHash-2-4, hex and little-endian helpers | `core/src/check.c` | `core/tests/test_check.c` |
| Key exchange and session key | `core/src/keys.c`, `core/include/relais/keys.h` | `core/tests/test_keys.c` |
| Key rotation on a live link | `core/src/keys.c` (`relais_keyring`) | `core/tests/test_keyring.c` |
| Sliding-window anti-replay | `core/src/anti_replay.c` | `core/tests/test_anti_replay.c` |
| Journal chain | `core/src/journal.c`, `actuator/journal_writer.c` | `core/tests/test_journal_chain.c`, `host/report/test_journal_chain.py` |
| Offline verification of the chain | `host/verify/relais-verify.c` (`--check-chain`) | `host/report/test_journal_chain.py` |
| Frame format and refusals | `core/src/frame.c`, `core/include/relais/frame.h` | `core/tests/test_frame.c`, `test_frame_fuzz.c` |
| Journal wire format | `CONTRACT.md` | — |
