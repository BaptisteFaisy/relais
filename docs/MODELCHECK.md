# Model checking — what is proved on the C itself (mission M36)

The exhaustive explorer (`host/explore/`) and the TLA+ models (`docs/tla/`)
prove properties of a **model** of the protocol. This document describes the
other half: what is proved **on the C code itself**.

There are two halves, and neither replaces the other:

| | Volet 1 — bounded exhaustion | Volet 2 — model checking |
|---|---|---|
| Where | `host/modelcheck/` | `host/modelcheck/cbmc/`, `host/modelcheck/wp/` |
| What runs **here**, today | the whole space, every case | nothing (the tools are absent) |
| What it proves | the properties, **on a finite published space** | the same properties, **for all inputs** |
| Tool | gcc, no dependency | CBMC, Frama-C/WP |
| Command | `make exhaustive` | `make modelcheck` |

The exhaustion is a *real* proof on a finite space, with published bounds.
The model checking is the *unbounded* counterpart, whose harnesses are ready
but whose tools are not installed on every machine. Neither one is a general
proof by itself: the exhaustion says nothing outside its bounds, and the
model checking is only as good as the contracts and the provers it is given.

---

## 1. Volet 1 — the bounded exhaustion (runs here)

`host/modelcheck/exhaustive.c` calls the **real** core functions —
`relais_frame_decode()`, `relais_arb_step()`, `relais_ctl_step()`,
`relais_testament_at()`, `relais_clamp_value()`, `relais_clamp_slew()` — on
every point of three finite spaces, and checks the safety properties on every
single case. No sampling, no random: the counts below are exhaustive.

```
make exhaustive
```

Two passes run: the plain build, then the **same** exhaustion under
AddressSanitizer + UndefinedBehaviorSanitizer (the parser and the arbiter must
not read or write outside their buffers, and must not trigger any undefined
behaviour). If the sanitizer runtime is missing, the second pass is skipped and
says so.

### 1.1 The measured result

```
MC-SPACE A-wire     cases=203511 rejected=202392 accepted=1110 time=0.15s
MC-SPACE B-arbiter  cases=387361 rejected=0      accepted=0    time=0.80s
MC-SPACE C-control  cases=19911  rejected=0      accepted=0    time=0.01s
MC-TOTAL cases=610783 time=0.93s
OK 9570930 checks
```

**610 783 cases exhausted, 9 570 930 property checks, 0 violation**, in about
**1 s** (plain, 0.5 s to 1.8 s over the runs) and **4 s** (sanitized, 3.4 s to
10.4 s) on the machine of the run. The number of cases is stable from one run
to the next; the wall time depends on the load of the machine.

### 1.2 Space A — the wire (`relais_frame_decode`)

| Axis | Bound | Why |
|---|---|---|
| message types | 7 (all of `PROTOCOL.md` §2) | every published type |
| declared payload length | 0 … 480 (`RELAIS_PAYLOAD_MAX`) | every length the header can carry |
| corrupted bytes | every position × every value 0…255 | single-byte corruption, exhaustively |
| truncations | every prefix of every valid frame | a short frame must be refused |
| extensions | +1 … +8 bytes | a long frame must be refused |
| header fields | type, sender, version, flags × 0…255 | the four discriminating bytes |
| tick / mandate extremes | 6 ticks, 4 mandates (0, 1, 2^31, 2^32−1) | the label and the number must round-trip |
| integrity level | 2 (level S: CRC-32C, level C: keyed SipHash) | both published trailers |

Properties: **WA-1** a frame is accepted exactly when its version, type,
sender and declared length are legal for the bytes present; **WA-2** any
single-byte corruption is rejected, or decodes to a message indistinguishable
from the original (a corruption is never observable by the arbiter); **WA-3**
every truncation and every extension is rejected; **WA-4** the tick label and
the mandate round-trip exactly (`SEC-03`: the tick tag is what makes a replay
stale); **WA-5** a corrupted frame is equivalent to a frame that was never
sent: the arbiter's decision does not change.

### 1.3 Space B — the arbiter (`relais_arb_step`)

Thirteen **published states**, each built by a deterministic prefix of ticks
(fresh and vacant; granted to A; A with the second certified; one miss;
revoked and vacant; cession recorded; will loaded; will exhausted (fallback);
auto-switch to the certified second; disagreement detected; degraded mode
past the stop request; patience p = 3; holder C). The enumeration crosses
them with:

| Axis | Bound |
|---|---|
| step tick | the state tick + {−1, 0, +1, +2, +3, +8, +64} (past and future ticks) |
| frame type | 7 (all of them, including the ones that never reach the actuator) |
| sender | 4 (A, B, C, actuator — the contract-legal set) |
| tick label of the frame | {tick−1, tick, tick+1} |
| mandate of the frame | {M−1, M, M+1, 0, 2^32−1} (stale, reused, future) |
| order value | 9 (−2, −1, −0.004, 0, +0.004, +1, +2, NaN, +∞) |
| two-frame sets | every ordered pair of a 16-frame vocabulary, for 5 tick offsets |
| sequences | every sequence of up to 3 frames, one tick each, 12-frame vocabulary |

Properties: **I1** exactly one value per tick, from one of the four published
sources, and the counter of that source is the only one that moves; **I2**
the mandate never decreases, moves by at most one step per tick, and two
machines never hold the same mandate number; **I3 / MAN-02** a value comes
from the holder only when a frame named this tick, came from the holder and
carried its mandate — a stale order is refused and counted with the published
reason; **ACT-05** the applied value is a finite number inside
`max_amplitude`.

The oracle is the **specification** (`docs/SPEC.md` 4.3/4.4,
`docs/PROTOCOL.md` §2) and the **published invariants** (I1, I2, I3 of
`core/src/invariants.c`), transcribed in `host/modelcheck/mc_arb.c` — not the
code under test.

### 1.4 Space C — the control law

| Axis | Bound |
|---|---|
| clamp inputs | 14 (−∞, −1e30, …, +1e30, +∞, NaN) × 3 amplitudes |
| slew pairs | every pair of the 14 values × 3 amplitudes |
| measurement grid | 9 × 9 × 9 (position, velocity, setpoint) × 2 applied × 3 parameter sets |
| corrupted measurements | the 14 extremes on the three fields × 3 parameter sets |
| will horizon | length 0…8 × 5 bases × 15 ticks |
| determinism run | 2000 ticks × 3 parameter sets |

Properties: **CC-1** the command is finite and inside `max_amplitude`, and the
change per tick is inside `max_slew`; **CC-2** the law is deterministic (same
state, same measurement, same bits); **CC-3** the exported state can be
imported back and continues the trajectory without a jump; **CC-4** the will
is played only inside its horizon; **CC-5** no corrupted measurement can make
the law emit a non-numeric or unbounded command.

### 1.5 What the exhaustion does **not** cover

These are bounds, not assumptions: outside them, nothing is claimed.

- **Space A**: corruptions of **two bytes or more** at once (only the
  single-byte corruption is exhausted; the CRC-32C and the SipHash cover the
  rest by construction, and `core/tests/test_frame_fuzz.c` fuzzes it), frames
  longer than 512 bytes, payloads above 480 bytes.
- **Space B**: more than two frames in a tick outside the vocabulary of the
  sequences, sequences longer than three ticks, parameter sets other than the
  three published ones (default, T = 1 ms / p = 3, T = 50 ms / DEG-08 band
  widened), and inputs outside the documented contract of `relais_arb_step()`
  (see §3.2).
- **Space C**: measurement values outside the 14 extremes and the 9-point grid,
  parameter sets other than the three published ones.

---

## 2. Volet 2 — the tooled half (ready, needs the tools)

`make modelcheck` always checks that the harnesses are valid C11, then runs
CBMC and Frama-C/WP **if they are installed**. When a tool is absent it says
exactly what is missing, gives the installation command, and returns **0**: an
absent tool is never a red build.

```
make modelcheck
```

### 2.1 CBMC — one file per property

`host/modelcheck/cbmc/` — the exact commands are in `host/modelcheck/cbmc/run.sh`.

| Property | File | Statement |
|---|---|---|
| P-FRAME-1 | `prop_frame_structure.c` | over an arbitrary buffer: a frame is accepted only if its magic, version, type, sender and declared length are legal (SEC-01, ROB-09) |
| P-FRAME-2 | `prop_frame_integrity.c` | a corrupted frame is refused by the integrity check (SEC-01, SEC-02) |
| P-ARB-1 | `prop_arbiter_acceptance.c` | the actuator applies an order only if it comes from the holder, carries the current mandate and targets the tick (MAN-02, SEC-03, SEC-04) |
| P-ARB-2 | `prop_arbiter_stale.c` | a stale order is never applied and is counted as rejected (I3, DET-04) |
| P-ARB-3 | `prop_arbiter_one_value.c` | exactly one value per tick, and only the counter of the source that served it moves (I1, ACT-01) |
| P-CTL-1 | `prop_ctl_bounded.c` | the law always returns a finite command inside the bounds (CMD-06, ACT-05) |

Command for one property (the others only change the file name):

```sh
cbmc host/modelcheck/cbmc/prop_arbiter_acceptance.c \
     core/src/arbiter.c core/src/control.c core/src/frame.c core/src/check.c \
     core/src/types.c core/src/params.c core/src/mutant.c \
     --function main --bounds-check --pointer-check --pointer-overflow-check \
     --div-by-zero-check --signed-overflow-check --unsigned-overflow-check \
     --conversion-check --unwinding-assertions --trace --timeout 240
```

Installation: `sudo apt-get install cbmc`, or a release of
<https://www.cprover.org/cbmc/> (no root needed). Then
`sh host/modelcheck/cbmc/run.sh`.

### 2.2 Frama-C / WP — the contracts, outside `core/`

`core/` is **not** modified: the ACSL contracts live in a specification
module, `host/modelcheck/wp/relais_core_spec.h`, and the harnesses are in
`host/modelcheck/wp/wp_*.c`.

| Function | Contract | Harness |
|---|---|---|
| `relais_frame_decode` | pre: the bytes and the key are readable; post: an accepted frame has a legal type and sender | `wp_frame.c` |
| `relais_arb_step` | post: the mandate never decreases and moves by at most one; one value is applied, from a known source, bounded; the published holder is the arbiter's | `wp_arbiter.c` |
| `relais_ctl_step` | post: the command is bounded and its change per tick is inside the rate bound | `wp_control.c` |
| `relais_clamp_value` | post: total and bounded | — |
| `relais_testament_at` | post: the will is served only inside its horizon | — |

Command (the exact commands are in `host/modelcheck/wp/run.sh`):

```sh
frama-c -kernel msgml -wp -wp-harness host/modelcheck/wp/wp_arbiter.c \
        -wp-fct relais_arb_step -wp-prover tip,alt-ergo,cvc4,z3 -wp-rte \
        -wp-timeout 60 -wp-print -Icore/include -Ihost/modelcheck/wp \
        host/modelcheck/wp/relais_core_spec.h \
        core/src/arbiter.c core/src/control.c core/src/frame.c core/src/check.c \
        core/src/types.c core/src/params.c core/src/mutant.c
```

Installation: `sudo apt-get install frama-c-base`, or `opam install frama-c`.
Then `sh host/modelcheck/wp/run.sh`.

**Known limitation of this half**: WP can only contract the **public** API of
the core. The static helpers of `core/src/arbiter.c` (`order_is_valid`,
`pick_value`, `scan`, `certify`) cannot receive a contract from outside the
translation unit; they are covered by the exhaustion of space B and by the
CBMC harnesses, not by an ACSL contract. Proving them with WP would require
annotating `core/src/arbiter.c` itself, which this mission does not do.

---

## 3. Observations (published, not failures)

The exhaustion prints three counters. They are **not** violations of the
properties: they locate the boundary of what the code guarantees.

### 3.1 A demand from the actuator is granted (6 622 cases)

`host/modelcheck/mc_arb.c` / `core/src/arbiter.c`, function
`handle_demande()`:

- **Expected** (`docs/PROTOCOL.md` §2): a `DEMANDE` is sent by a non-holder
  **computer** to the actuator; the actuator grants it only when the mandate
  is vacant.
- **Obtained**: the arbiter grants any demand whose sender is a computer **or
  the actuator itself** — it never checks that the sender is a computer. The
  decoder accepts a `DEMANDE` whose sender is the actuator (the sender field
  is legal for every type), so an authenticated frame with that sender makes
  the actuator **its own holder**.
- **Impact**: no double command (the actuator still applies exactly one
  value per tick, and the bound of §1.3 still holds), but the redundancy is
  lost and the two computers are both deposed: the actuator commands alone.
- **Requirement**: MAN-05 (a grant only when the mandate is vacant) is
  respected; the missing check is "the demander is a computer" — SEC-02
  (the frame must be authenticated) is the only barrier.
- **Reproduction**: `MC-OBSERVATIONS` of `make exhaustive`; every one of the
  6 622 cases is explained by a demand whose sender is the actuator (the two
  counters are equal).

Not fixed here: `core/` is outside the scope of this mission (rule 10 of the
common rules).

### 3.2 The contract boundary of the arbiter (13 cases)

`core/include/relais/arbiter.h` documents that the frames handed to
`relais_arb_step()` are "already decoded and authenticated". Space A proves
the decoder never produces a sender outside {A, B, C, actuator}, so the
precondition holds on every real path. With a frame that violates it (sender
`NONE`, which the decoder refuses), the arbiter applies an order while the
mandate is vacant: 13 published cases. The exhaustion reports them instead of
hiding them; the harness checks the contract-legal inputs only.

### 3.3 The rate clamp needs a finite previous value (225 cases)

`relais_clamp_slew()` bounds the change of the command, but it cannot bound a
non-finite previous value: with `previous = ±∞` it returns `±∞`. In the real
flow the previous value is the actuator's own output, always finite (the
amplitude clamp comes first), so the case does not occur; the harness
publishes the 225 cases where it would.

---

## 4. Reproduce

```sh
make -j4 all        # the core, the tests, the explorer (unchanged)
make test           # the unit tests (unchanged)
make explore        # the exhaustive explorer of the MODEL (unchanged)
make exhaustive     # the bounded exhaustion of the C (this mission)
make modelcheck     # the CBMC / WP harnesses: syntax always, tools when present
```

Requirements cited: MAN-02, MAN-03, MAN-04, MAN-05, DET-01, DET-04, SEC-01,
SEC-02, SEC-03, SEC-04, ACT-01, ACT-02, ACT-05, CMD-03, CMD-06, ROB-09, REP-01,
REP-02, I1, I2, I3, I6.
