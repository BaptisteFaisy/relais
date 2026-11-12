# Attack surface

An exhaustive inventory of what enters the system and what leaves it, what an
attacker can do with each entry point, what protects it today, what is *not*
protected, and the consequence. It ends with the trust assumptions the whole
demonstration rests on.

Method: every `open`/`fopen`, every socket, every channel, every command-line
option and every file format in the tree was enumerated (`grep -rn 'fopen\|open(\|
socket\|bind\|MsgSend\|name_attach\|argv' core actuator host target`). Nothing
here is speculative: each line names the file that implements it.

The core library is not an attack surface by construction: no clock, no file, no
socket, no allocation, and every buffer access goes through a bounds-checked
cursor (`core/src/frame.c`). The surfaces are the glue around it.

## 1. The wire: every field of a frame

A frame is `magic 4 | version 1 | type 1 | sender 1 | flags 1 | tick 8 |
mandate 4 | payload_len 2 | reserved 2 | payload | check 8`, at most 512 bytes,
big-endian, written byte by byte (`core/src/frame.c`).

| field | attacker's power | protected by | not protected | consequence |
|---|---|---|---|---|
| `magic` (4) | must write `RLS1` | sanity filter only (`REJ_MALFORMED`) | — | none alone; it is a filter, not a defence |
| `version` (1) | must write `1` | `REJ_VERSION` | — | none; a future version needs a real compatibility rule |
| `type` (1) | chooses one of 7 | range checked, `REJ_MALFORMED` | — | selects which payload parser runs (that is what `fuzz_frame` walks) |
| `sender` (1) | claims to be A, B, C or the actuator | at **level C** the MAC binds it (SEC-02); range checked | at **level S** (CRC only) it is *not* authenticated | at level S anybody on the link can speak as the holder; level C is what makes MAN-02 hold |
| `flags` (1) | sets READY / CESSION / TEST / HAS_ORDER | MAC at level C; `TEST` frames are never applied (`RELAIS_FLAG_TEST`) | at level S, arbitrary | a forged `HAS_ORDER` makes a DEMANDE carry an order (MAN-05) |
| `tick` (8) | any 64-bit value | SEC-03: an order for another tick is `REJ_TICK_MISSED` | replay **inside** the same tick | a replayed frame is stale; a same-tick duplicate cannot change the applied value (exactly one value per tick, spec 4.4) |
| `mandate` (4) | any 32-bit value | MAN-02/SEC-04: `REJ_MANDATE_STALE` / `REJ_MANDATE_FUTURE`; the actuator is the only writer | — | a node cannot appoint itself. Demonstrated by `relais-intruder` (a signed frame claiming mandate 9999 is rejected and its value never applied) |
| `payload_len` (2) | any u16 | must equal the received length minus 32, and the fixed size of the type (`REJ_MALFORMED`) | — | the single most important parser guard: no length is ever trusted, so no read or write can leave the buffer (ROB-09, 24.1 M fuzz executions, 0 crash) |
| `reserved` (2) | any value | ignored | not validated | none today; a future use must add a rule now |
| payload STATUT (24) | holder, source, misses, certified, lamp, cause, stop, 4 floats | MAC at level C; STATUT goes to the computers, never to the arbiter (`scan()` ignores it) | at level S a forged STATUT can lie to a computer | a computer could depose itself early (bail_ticks) — degraded, not unsafe: the actuator still applies one value per tick |
| payload ORDRE (37) | value + testament (≤ 8 floats) | ACT-05 `relais_clamp_value` bounds the value and **catches NaN**; `testament_len` is clamped to 8 by the decoder; MAC at level C | — | the value is bounded before it reaches the plant (CMD-06); no NaN can enter the law |
| payload OMBRE (5) | value + replica_age | MAC; only used when the sender is the certified second (REL-05) | — | an uncertified shadow is ignored (`REJ_NOT_CERTIFIED` path) |
| payload DEMANDE (43) | seen_mandate, reason, has_order, value, testament | SEC-06 rate limit (`request_burst` per `request_window` ticks); granted only when the mandate is vacant | at level S, floods | a flood cannot delay the orders (SEC-06); fuzzed by `arbiter_hostile_flood.bin` |
| payload CESSION (1) | reason | accepted only from the current holder with the current mandate; takes effect at the **next** tick (REL-06) | — | a forged cession from a non-holder is ignored |
| payload ETAT (5..261) | `ctl_state_len` + up to 256 bytes — **the largest attacker-controlled blob** | `ctl_state_len + 5` must equal `payload_len`; the destination is a fixed 256-byte array; MAC at level C; `relais_ctl_import` refuses any state whose version or length is not exactly right (REP-02) | — | a hostile control state cannot be imported; the replica keeps what it had |
| payload SUIVI (2) | replica_age, ready | MAC; peer-to-peer only | — | readiness lies degrade certification, they do not command anything |
| `check` (8) | cannot compute it without the key | SEC-02: CRC-32C at level S, SipHash-2-4 truncated with a 16-byte key at level C; `REJ_CHECK` otherwise; a single flipped bit anywhere is refused (`test_frame_fuzz.c`, 8 × (n−8) cases) | **level S has no key**: integrity, not authentication | at level C a forged frame is impossible without the key; at level S it is trivial. Level C is the deployed assumption |

## 2. Network interfaces

| interface | who binds | protected | not protected | consequence |
|---|---|---|---|---|
| PC actuator UDP (`--port`, default 7100) | `actuator/pc/relais-actuator.c:open_socket` → **`INADDR_LOOPBACK`** | loopback only: unreachable from the network | any local process can send datagrams | a local attacker can flood; every frame is still MAC-checked and counted (`forged`, `rejected[]`) |
| PC node UDP (`--port`) | `host/node/relais-node.c` → **`INADDR_LOOPBACK`** | idem | idem | idem |
| QNX actuator UDP (`--port`) | `target/qnx_util.c:rl_udp_bind` → **`INADDR_ANY`** | the MAC (level C) and the QNX network stack | reachable from **every** interface of the target | on a real vehicle this is the main remote surface: at level C an attacker can only cause work (drop + count), at level S it can inject orders |
| QNX peer links (`--peer A=HOST:PORT`) | `target/net.c` | MAC | no mutual authentication of the IP endpoints | a man-in-the-middle can drop or delay (a fault the model covers), but cannot forge at level C |
| bench UDP helper | `host/bench/host_net.py:check_port` | refuses ports < 1024, so the bench never needs root; non-blocking sockets with timeouts | — | no privileged-port exposure from the bench |
| QMP monitor (QEMU) | `host/bench/qmp.py` — a **Unix socket**, JSON-lines | Unix socket permissions on the host; timeouts on every read | whoever can reach the socket controls the guest (stop/cont, screendump) | a local attacker on the bench host can freeze or resume the demonstration VM |

## 3. Local QNX interfaces

| interface | role | protected | not protected | consequence |
|---|---|---|---|---|
| `/dev/relais` | the resource manager root (QNX-01: open, read, write, nothing else — no `devctl`) | the QNX **typed security policy**: only processes with the right type can open it; `target/intruder_test.sh` prints `on /dev/relais` and the real errno | nothing in the code itself: the policy is a deployment artefact | if the policy is misconfigured, any process can read the arbiter state |
| `/dev/relais/status` | 256-byte status read by tools | idem | idem | information disclosure: holder, mandate, lamp, cause |
| `/dev/relais/temoin` | the lamp/witness | idem | idem | an attacker could read (not write) the degraded state |
| `/dev/relais/ctl` | **the most dangerous one**: `target/ctl.c:answer_import` imports a control-law state | REP-02 (`relais_ctl_import` refuses a wrong version or length) + the typed policy | the code cannot enforce the policy | without the policy, a local process could push a control state into the holder's law |
| channels `relais-net`, `relais-ctl` | `name_attach` + fixed-size `MsgSend` round trips, no heap, no shared memory | the typed policy; messages are fixed-size structs, and every length carried inside one is re-checked by the core | — | without the policy, a process could pose as `relais-net` and feed frames to the core |

## 4. Files read

| file | reader | protected | not protected | consequence |
|---|---|---|---|---|
| `scenarios/*.json` | `host/bench/relais_bench.py`, `host/report/journal_io.py` | parameters are re-validated in C (`relais_params_parse` rejects unknown keys, `relais_params_valid` enforces N ≥ p+2+3 and every bound) | the *content* is trusted: it chooses the ticks, the faults, the seed and the trajectory | a hostile scenario silently changes what is being proven — a false proof, not a crash |
| journal `*.jsonl` | `host/verify/relais-verify.c`, `host/report/*` | every line is classified then parsed by fixed field readers; `journal_io.py` never raises on bad content and counts malformed lines; `report.py` escapes the five HTML characters through one shared `esc()` (`host/report/charts.py:27`), so no stored XSS | **no integrity protection**: no hash chain, no signature | whoever can write the journal can forge the evidence the verifier reads and obtain a false "0 violation". Demonstrated by `relais-intruder --journal` (`[AUDIT]`). Planned fix: the chained journal of mission M21 |
| journal, unbounded growth | `host/verify/relais-verify.c:journal_load` uses `malloc`/`realloc` and doubles the array per line | lines are capped at `RELAIS_JOURNAL_LINE_MAX` | no cap on the number of records | a huge hostile journal exhausts the verifier's memory: denial of service **of the verification**, never of the actuator |
| actuator state file (`--state`) | `actuator/pc/relais-actuator.c:load_state` → `relais_arb_restore` | magic `RARB` + version + a minimum length; a bad snapshot is ignored (MAN-11 keeps the mandate) | **not authenticated**: 16 bytes, no MAC | a local attacker can set the mandate number and the holder, so the actuator restarts believing a node it never appointed. Fuzzed (`fuzz_snapshot`), not protected. Requires local write access |
| MAC key (`--key <32 hex>`) | every process that touches the wire | the key never leaves the process; `relais_key_from_hex` validates the length | **the key travels on the command line**, visible in `ps` and `/proc/<pid>/cmdline` to any local user | a local attacker reads the key and can then forge authenticated frames: a complete break of SEC-02. On QNX the typed policy and process isolation mitigate it; on PC nothing does. Planned fix: mission M21 (key files, rotation) |
| fuzz logs and crash cases | `tools/fuzz-report.py` | crash files are replayed through the target binary, never parsed as data | — | a hostile crash file cannot do more than make the target fail, which is the point |

## 5. Bench tools that accept external input

| tool | external input | notes |
|---|---|---|
| `host/bench/relais_bench.py` | `--scenario` (JSON), `--verify` (journal), `--out` (path), `--mode pc\|sim\|qemu` | orchestrates the processes; a path it is given is a path it writes |
| `host/bench/qemu.py`, `qemu_bench.py`, `qemu_config.py` | the QEMU command line and the QMP socket | the guest is the only thing that runs untrusted code by design |
| `host/bench/host_net.py`, `probes.py`, `orchestrator.py`, `faults.py`, `bench_env.py`, `bench_report.py` | loopback UDP, environment, run directories | no privileged port, no root, non-blocking with timeouts |
| `host/verify/relais-verify` | a journal path, `--twin` path | read-only consumer |
| `host/report/report.py` | journal + `scenario.json`, writes HTML | output escaped by one shared `esc()` |
| `host/explore/relais-explore` | `--budget`, `--faults`, an output path | writes a counter-example file; no external data parsed |
| `host/simulate/relais-simulate` | `--params`, `--trajectory`, `--fault`, `--seed`, `--loss`, `--delay`, `--journal` | every spec string is parsed and rejected when malformed |
| `host/bench/relais-intruder` | `--port`, `--journal` | **it is the attacker**; it exists to be run |

## 6. What leaves the system

| output | content | risk |
|---|---|---|
| STATUT frames on the wire | holder, mandate, misses, source, lamp, cause, certified, position, velocity, setpoint | **not encrypted, only authenticated**: an eavesdropper on the link reads the steering state tick by tick. Acceptable for a bench, a real decision on a vehicle |
| the journal file | the same, plus every event and every refusal, with the evidence | the proof artefact; its integrity is the weak point (above) |
| stdout of the actuator, node, simulator | counters, refusals, the invariant report | no secret, but it does reveal the key if the command line is echoed |
| `slog2` on QNX | the same, through the system log | readable per the QNX log policy |
| `/dev/relais/status` | 256 bytes of state | read is policy-controlled |
| the HTML report and the dashboard | aggregated journal content | escaped, self-contained, no external origin |

## 7. Trust assumptions

What this demonstration assumes honest. Each one is a place where the proof
stops, and each is stated so a reviewer can attack it deliberately:

1. **The orchestrator.** Whoever runs the bench chooses the scenario, starts the
   processes, collects the journal and runs the verifier. A hostile orchestrator
   can falsify every number in `docs/RESULTS.md`. Nothing in the tree detects it.
2. **The scenario file.** `scenarios/*.json` decides the parameters, the faults,
   the seed and the duration — that is, it decides *what is proven*. It is
   validated for consistency, never for honesty.
3. **The host clock.** The PC actuator and the simulator schedule ticks from the
   host clock. A stepped or frozen clock breaks the timing hypothesis
   (`2*delta + C + J <= T`), not the tick-counted logic (CMD-07: only T changes).
   The QNX target uses its own interval timer and is not affected by the host.
4. **Key distribution.** The 16-byte MAC key is assumed to be delivered out of
   band, secret, and identical on the three processes. The code only consumes it;
   today it travels on the command line (see §4), so on a PC this assumption is
   weaker than it looks.
5. **The QNX typed security policy.** `/dev/relais*`, the `relais-net` and
   `relais-ctl` channels are protected by a deployment artefact (`on` types,
   ownership, process manager configuration), not by the code. `target/
   intruder_test.sh` exists to print the real policy and the real errno rather
   than assert it.
6. **The toolchain and the runner.** The SDP, gcc, the CI runner and Mergify are
   assumed to build and merge what is in the tree. Reproducibility by a third
   party is the subject of mission M28, not of this document.
7. **The plant model.** The integrated plant model is the reference for the
   continuity invariant. A real actuator's faults (stiction, saturation, sensor
   noise beyond the model) are outside every proof here.
8. **The verifier and the report are read-only consumers.** They trust the
   journal's integrity. If the journal can be written by an attacker, the
   verification is worthless — which is why the chained journal (M21) is a
   prerequisite for treating the journal as evidence rather than as a log.
