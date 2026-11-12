# Relais — protocol reference (implementation level)

This document is the implementation-level description of the Relais protocol.
It is consistent with [CONTRACT.md](../CONTRACT.md) (frozen) and with the
headers in `core/include/relais/` — the headers are the contract. Narrative
context: [SPEC.md](SPEC.md) §4 and [SPEC-2.md](SPEC-2.md) §6.

Read it with one idea in mind: nothing in this protocol depends on a clock.
The actuator beats the tick, every frame names the tick it targets, and a frame
that arrives for any other tick is dropped. That is what makes a machine waking
up from a freeze harmless — its order is stale by construction, before anybody
has to decide whether it can be trusted. The trailer that authenticates a frame
is a CRC-32C at the base level and a keyed SipHash-2-4 MAC at the full level;
both are eight bytes, in the same place, so a receiver never has to guess which one it is reading.

Every integer on the wire is written byte by byte, most significant byte first,
so that a frame built on a little-endian PC is read identically by a big-endian
target: the encoding never depends on the host, and a capture can be decoded
with nothing but this table. The same rule holds for the journal the actuator
writes, which is JSON lines — one object per line, ASCII, floats with `%.6g` —
so that any tool can read it without a schema.

All integers on the wire are **big-endian**, written byte by byte (no aliasing,
no structure padding): the encoding is identical on PC and on QNX
(`core/include/relais/frame.h`).

---

## 1. Frame format, byte by byte

Every frame is a UDP datagram of fixed size per type, **512 bytes at most**
(`RELAIS_FRAME_MAX`). Header: 24 bytes (`RELAIS_HEADER_LEN`). Check trailer:
8 bytes (`RELAIS_CHECK_LEN`). Payload: at most 480 bytes (`RELAIS_PAYLOAD_MAX`).

| Offset | Size | Field | Type | Content |
|---:|---:|---|---|---|
| 0 | 4 | magic | u32 | `0x31534c52` ("RLS1", `RELAIS_MAGIC`) |
| 4 | 1 | version | u8 | `1` (`RELAIS_VERSION`); any other value → reject `RELAIS_REJ_VERSION` |
| 5 | 1 | type | u8 | Message type, see §2 (`RELAIS_MSG_*`) |
| 6 | 1 | sender | u8 | `0`=none, `1`=A, `2`=B, `3`=C, `4`=actuator (`relais_node`) |
| 7 | 1 | flags | u8 | Bitfield, see below |
| 8 | 8 | tick | u64 | Tick emitted (`STATUT`) or tick targeted (orders) |
| 16 | 4 | mandate | u32 | Current mandate (`STATUT`) or mandate believed by the sender |
| 20 | 2 | payload_len | u16 | Size of the payload |
| 22 | 2 | reserved | u16 | Zero |
| 24 | var | payload | bytes | Content per type (§2) |
| frame_end − 8 | 8 | check | u64 | CRC-32C (base level) or keyed MAC (full level), see §3 |

Flags (`frame.h`):

| Bit | Name | Meaning |
|---:|---|---|
| 0x01 | `RELAIS_FLAG_READY` | The sender is a certified second |
| 0x02 | `RELAIS_FLAG_CESSION` | Cession marker |
| 0x04 | `RELAIS_FLAG_TEST` | Bench frame, never applied |
| 0x08 | `RELAIS_FLAG_HAS_ORDER` | A `DEMANDE` carries its first order |

Encoding/decoding API: `relais_frame_encode()` / `relais_frame_decode()`
(returns a `relais_reject` reason on failure), and
`relais_frame_decode_unchecked()` for fuzzing the parser without the trailer
check (ROB-09).

## 2. The seven message types

`relais_msg_type` (`types.h`). One decoded message fits in a single
`relais_msg`; only the fields of its type are meaningful.

| Type | ID | Direction | When | Payload content |
|---|---:|---|---|---|
| `STATUT` | 1 | actuator → A, B | every tick | holder, source of the applied value, consecutive misses, certified second or not, lamp and cause, stop flag, measurements (`relais_meas`: `pos`, `vel`, `applied`, `setpoint`) |
| `ORDRE` | 2 | holder → actuator | every tick | `value` for the targeted tick + will: `testament[8]` (`RELAIS_TESTAMENT_MAX`), `testament_len` |
| `OMBRE` | 3 | second → actuator | every tick (full level) | `value` for the targeted tick, `replica_age` (REP-03) |
| `DEMANDE` | 4 | non-holder → actuator | on suspicion (base level) | `seen_mandate`, `reason`; optional first order when `RELAIS_FLAG_HAS_ORDER` (value + testament) |
| `CESSION` | 5 | holder → actuator and second | local fault, maintenance | `seen_mandate` (mandate ceded), `reason` |
| `ETAT` | 6 | holder → second | every tick | full control state `ctl_state[256]` (`RELAIS_CTL_STATE_MAX`, REP-01), `ctl_state_len`, `last_value` |
| `SUIVI` | 7 | second → holder | every tick | `replica_age`, `ready` flag |

Reject reasons counted and journaled (`relais_reject`, MAN-02, DET-04,
SEC-01): `NOT_HOLDER`, `MANDATE_STALE`, `MANDATE_FUTURE`, `TICK_MISSED`,
`CHECK`, `MALFORMED`, `RATE_LIMITED`, `NOT_CERTIFIED`, `VERSION`.

## 3. Integrity check

`core/include/relais/check.h`. The 8-byte trailer is either:

- **base level (S)**: a CRC-32C over the frame (`relais_crc32c`);
- **full level (C)**: a keyed authenticator — SipHash-2-4 truncated to 64
  bits (`relais_mac`), with a per-link 16-byte key (`relais_key`;
  `enabled = false` falls back to CRC-32C). SEC-02.

A frame whose check fails is treated as lost and counted (DET-04). Replay of a
captured authentic frame is rejected by the tick tag (SEC-03): an order is
applied only for the tick it names.

## 4. Parameters

`core/include/relais/params.h`; defaults per `relais_params_default()` and the
journal header of CONTRACT.md.

| Parameter | Field | Default | Meaning |
|---|---|---|---|
| T | `period_ms` | 5 ms (1–50) | Tick period |
| p | `patience` | 1 | Consecutive misses tolerated without deposition |
| N | `testament_len` | 8 ticks | Horizon of the will |
| Certification | `certify_ticks` | 3 ticks | Matching order/shadow ticks before a second is certified |
| Bail | `bail_ticks` | 4 ticks | `STATUT` silence after which a node deposes itself (MAN-09) |
| Request rate limit | `request_burst` / `request_window` | per defaults | `DEMANDE`s accepted per window (SEC-06) |
| ε | `epsilon` | 0.005 (0.5 % FS) | Order/shadow agreement tolerance |
| δ | `delta` | 0.02 (2 % FS) | Continuity tolerance at handover |
| Degraded duration | `degraded_ticks` | 6000 ticks (= 30 s at T = 5 ms) | Ticks without redundancy before "stop requested" (DEG-05) |
| Amplitude bound | `max_amplitude` | per defaults | ACT-05 |
| Slew bound | `max_slew` | per defaults | ACT-05, per tick |
| Slow motion | `slow_motion`, `slow_factor` | off, ×10 | CMD-07: only T changes, the logic is tick-based |

**Consistency rule, checked at start-up** (`relais_params_valid()`):

```
N ≥ p + 2 + 3
```

so that the will covers the handover even if three requests are lost. Overrides
parse as `"T=5,p=1,N=8"` (`relais_params_parse()`, unknown keys rejected).

## 5. The tick, actuator side: exact sequence

The actuator owns the tempo. During tick `n` it applies one value, then
publishes `STATUT(n)`. Machines compute on `STATUT(n)` and send frames
**targeting tick n+1**. One call to `relais_arb_step()` per tick, with the
frames received during the previous period (already decoded and
authenticated, at most `RELAIS_IN_MSG_MAX` = 8) and the measurements
(`relais_meas`: position and velocity at the end of the previous tick,
setpoint for the next one). The arbiter step runs in this order
(CONTRACT.md, `arbiter.h`):

1. find a valid `ORDRE` of the holder targeting `n`;
2. `misses = found ? 0 : misses + 1`;
3. if `misses > p` revoke (auto-switch to the certified second: mandate + 1);
4. process `CESSION`, then `DEMANDE` (grant only if vacant, mandate + 1);
5. pick the value: holder order, else certified shadow, else testament, else
   fallback law;
6. certification streak, lamp, degraded timer;
7. fill `STATUT(n)` and the journal events.

Output (`relais_arb_out`): the bounded value applied and its source, the
mandate state, the `STATUT` ready to send to both computers, and up to
`RELAIS_EVENTS_MAX` = 8 journal events. The arbiter also exposes
`relais_arb_snapshot()` / `relais_arb_restore()` so that the mandate number
survives a restart of the actuator (MAN-11).

With `p = 1` this yields the documented bounds: **3 periods between two fresh
orders at level S** (2 when the `DEMANDE` carries its first order), and
**zero** tick without a fresh order at level C when a second is certified.

## 6. The computer state machine

`core/include/relais/node.h`. Roles (`relais_role`): `NONE` (booting, not yet
a second), `SECOND`, `HOLDER`. The node never invents a mandate: it reads the
one the actuator publishes. Everything it wants to send is returned in
`relais_node_out` (at most `RELAIS_OUT_MSG_MAX` = 4 messages); the caller
(`relais-net` on QNX, the simulator on PC) owns the sockets.

### 6.1 Transitions

| From | To | Trigger | Frames emitted |
|---|---|---|---|
| NONE | SECOND | first `STATUT` received (`relais_node_on_status`) — learns holder and mandate | — |
| NONE/SECOND | HOLDER | `DEMANDE` granted (mandate vacant, MAN-05/06), or auto-switch after p+1 holder misses when certified (MAN-10), or `CESSION` of the holder (REL-06) | first `ORDRE` (possibly embedded in the `DEMANDE`, flag `HAS_ORDER`) |
| HOLDER | SECOND | reads in a `STATUT` that it is deposed or that the mandate passed to another (`deposed`); stops emitting orders **before the next tick** (MAN-08) | — |
| HOLDER | SECOND | local critical process died → cedes within the tick (`relais_node_on_local_fault`, DET-05), or voluntary handover (`relais_node_on_handover`, REL-09) | `CESSION` to actuator and second |
| HOLDER/SECOND | SECOND | no `STATUT` for `bail_ticks` → deposes itself (`relais_node_on_silence`, MAN-09; diagnostics only — safety rests on MAN-02) | — |

### 6.2 What each role emits per tick

- **HOLDER**, on `STATUT(n)` (`relais_node_on_status`, CMD-02): computes the
  value for tick n+1 with the control law (`relais_ctl_step`, pure function,
  CMD-03), then emits `ORDRE(n+1)` with its will of N values
  (`relais_testament_fill`) to the actuator, and `ETAT(n+1)` with its full
  control state to the second (REP-01). Values are bounded in amplitude and
  slew before emission (CMD-06).
- **SECOND**: on `ETAT` from the holder (`relais_node_on_etat`), replaces its
  state with the received one — an older `ETAT` is dropped (REP-02) — then
  computes on the same measurements. Emits `OMBRE(n+1)` with its replica age
  every tick at level C, and `SUIVI` to the holder (replica age, ready flag).
  On suspicion (no `ETAT` for one tick, or a miss announced in a `STATUT`,
  DET-02) it may emit a `DEMANDE` — a suspicion has **no effect** without the
  actuator's grant (REL-04). Without `ETAT`, it rebuilds a bump-free state
  from what the actuator publishes (`relais_ctl_reconstruct`, REP-04).
- Counters kept by the node: `orders_sent`, `shadows_sent`, `demandes_sent`,
  `cessions_sent`, `stale_dropped` (our own orders the actuator rejected);
  diagnostic text via `relais_node_status_text()` (OBS-02).
- Bench hooks: `relais_node_set_bias()` (INJ-10 value fault) and
  `relais_node_arm_death()` with a death point P1–P9 (`relais_death_point`,
  INJ-07 / spec §7.5); `relais_node_out.died` tells the caller to kill the
  machine.

## 7. Death of the holder, tick by tick

Worst case: A dies just after emitting its order for tick n. Defaults p = 1.

| Tick | Received by the actuator | Base (S): value applied | Full (C): value applied |
|---|---|---|---|
| n | `ORDRE(n)` from A | A's order (fresh) | A's order (fresh) |
| n+1 | nothing from A | will of A (`source=testament`), misses = 1 | `OMBRE(n+1)` from B (fresh, `source=shadow`), misses = 1 |
| n+2 | nothing from A; `DEMANDE` from B (base) | will of A; misses = 2 > p → mandate revoked, declared vacant, then granted to B (mandate + 1) | `OMBRE(n+2)` from B (fresh); misses = 2 > p → auto-switch, mandate to B (MAN-10) |
| n+3 | `ORDRE(n+3)` from B | B's order (fresh), new mandate | B's order (fresh) |

- **Base**: at most 3 periods between two fresh orders (15 ms at 200 Hz),
  bridged by the will. Variant: the second asks at the first miss → 2 periods.
- **Full**: no tick without a fresh order; the mandate change is a formality,
  2 periods after the last order of A.
- **Double fault** (B dies too): will, then fallback law at most N+1 ticks
  after the last fresh order (N = 8 → 45 ms), red lamp (I8, DEG-06).

## 8. The returning machine (freeze then thaw)

```
  time ────────────────────────────────────────────────────────────────►
  A   holder, mandate 7 │ frozen ░░░░░░░░░░░░░░░░░ │ thawed: sends ORDRE(mandate 7, old tick)
  B   second ────────────┴ substitutes, then holder of mandate 8 ────────────────────────────────
  ACT applies A          │ applies B; mandate 7 → 8            │ drops A; A reads the STATUT, becomes second
```

At thaw, A does not know that time has passed. Its order is dropped **twice
over**: it no longer comes from the holder (`RELAIS_REJ_NOT_HOLDER`,
`MANDATE_STALE`), and it targets a past tick (`RELAIS_REJ_TICK_MISSED`) —
invariant I3. A reads the next `STATUT`: holder = B, mandate = 8. It stops
emitting orders (MAN-08), resynchronises on the tempo and rejoins as second
(REL-07); it becomes eligible again only after a new certification (DEG-07).
A freeze shorter than p+1 ticks changes nothing: the substitution or the will
bridges, and A stays holder (DET-03).

## 9. Journal format (authoritative)

One JSON object per line, ASCII, no trailing comma, floats with `%.6g`
(CONTRACT.md; implementation `core/src/journal.c`, API
`core/include/relais/journal.h`). The journal of the actuator is the authority
the verifier reads.

```
{"kind":"header","version":1,"mutant":0,"scenario":"slalom-death-A","scenario_id":1,
 "params":{"T":5,"p":1,"N":8,"certify":3,"bail":4,"eps":0.005,"delta":0.02,"degraded":6000},
 "chain":"1da9e2d749c44f1c"}
{"kind":"tick","tick":41,"ns":0,"mandate":7,"holder":"A","source":"holder","value":0.125,
 "pos":0.4,"vel":0.01,"setpoint":0.5,"misses":0,"lamp":"off","cause":"none",
 "certified":true,"certified_node":"B","stop":false,"chain":"e6463b7efd919f9a"}
{"kind":"event","tick":42,"ns":0,"event":"mandate_granted","node":"B","mandate":8,
 "detail":0,"text":"demande","chain":"cd469cfd2cb2090b"}
```

`chain` is the last key of every record a current build writes: 16 hexadecimal
digits, the fingerprint of the previous record chained to this one
(`CONTRACT.md`, "The `chain` field"). It makes an edit, a deletion or an
insertion after the fact detectable, and it is **optional for readers**: a
journal without it — an older run, like the ones kept under `docs/figures/` —
is still read and still verified. `relais-verify --check-chain` recomputes the
chain and names the first break.

Enumerated values on the wire of the journal:

- `holder` / `node` / `certified_node`: `""`, `"A"`, `"B"`, `"C"`,
  `"actuator"`;
- `source`: `none|holder|shadow|testament|fallback`;
- `lamp`: `off|yellow|red`;
- `event`: snake_case of `RELAIS_EV_*`: `mandate_granted`, `mandate_revoked`,
  `mandate_auto_switch`, `request_denied`, `certified`, `decertified`,
  `order_rejected`, `disagreement`, `fallback_enter`, `fallback_exit`,
  `cession`, `lamp_change`, `stop_requested`, `forged`, `node_back`.

`ns` is the host timestamp, filled by the writer (0 if none). Parsing for
`relais-verify`: `relais_journal_parse_header/tick/event`,
`relais_journal_is_header/tick/event`.

## 10. Scenario file format (`scenarios/*.json`)

```json
{"id": 1, "name": "slalom-death-A", "trajectory": {"kind":"slalom","amp":0.6,"freq":0.5},
 "params": {"T":5,"p":1,"N":8}, "ticks": 4000, "seed": 12345,
 "faults": [{"at_tick": 500, "type": "death", "target": "A"},
            {"at_tick": 900, "type": "freeze", "target": "B", "ticks": 40},
            {"at_tick": 1200, "type": "cut", "link": "A-actuator", "direction": "both", "ticks": 60}]}
```

- `type`: one of `death|freeze|thaw|cut|restore|slow|bias|restart|crash`;
- `target`: `A|B|actuator`;
- `link`: `A-B|A-actuator|B-actuator` (the six half-links with `direction`);
- `direction`: `both|forward|reverse`;
- `crash` names a process: `relais-net|relais-core|relais-ctl`;
- `seed` fixes every draw of the simulator (delays, losses, fault details), so
  a run is exactly replayable (INJ-06).

Trajectory kinds (`relais_traj_kind`): `slalom`, `lane_change`, `step`,
`const`, `brake` (CMD-08). Trajectories and disturbances are functions of time
only: the fault-free twin and the faulted run share the same reference.
