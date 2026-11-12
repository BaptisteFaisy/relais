# GOLDEN — the behavioural fingerprint of the core (mission M35)

Two coming missions rewrite `core/` in depth: **M05** (three machines and a
vote) and **M06** (two paths of the actuator). Neither can tell, from the unit
tests alone, whether the SEQUENCE OF DECISIONS changed: the tests can stay
green while the core now hands the mandate over one tick later, or serves the
shadow instead of the holder. This file describes the fingerprint that makes
such a change visible before it merges.

## What the fingerprint is

* A **frozen game** of 26 scenarios — `tools/golden/scenarios.json`: fixed
  seeds, fixed faults, fixed parameters, fixed horizons.
* The **seeded simulator** of the repository (`build/relais-simulate`, spec
  7.4) replays them: no clock, no jitter, no socket, the very same core as
  the QNX target.
* The journal of every scenario is **normalised** (volatile fields
  neutralised) then hashed with **SHA-256**.
* The hashes live in the versioned reference `tools/golden/traces.json`.

The tool:

```
python3 tools/golden/capture.py     # capture: rewrites tools/golden/traces.json
python3 tools/golden/verify.py      # the guard: replays and compares, exit 1 on any divergence
python3 tools/golden/verify.py --update   # DELIBERATE refresh of the reference
make golden                         # = capture
make golden-verify                  # = verify (read-only: this is what CI runs)
make golden-update                  # = the deliberate refresh
```

## What the fingerprint guarantees

* **The decisions of the core do not change silently.** The SHA-256 covers the
  whole normalised journal: the tick, the mandate, the holder, the source of
  the value (`holder` / `shadow` / `testament` / `fallback`), the applied
  value, the position and the velocity, the misses, the lamp, the cause, the
  certification and the certification node (OBS-01, CMD-01..CMD-07).
* **The events do not change silently.** Mandate granted, revoked, auto
  switch, cession, certification, decertification, disagreement, lamp change,
  fallback, node back: every event line of the journal is inside the hash
  (MAN-04, DET-03, DET-09, REL-05, DEG-02, DEG-07).
* **The invariants still hold.** The report of the simulator (violations, max
  gap, max deviation, fallback run, lamp delay) is hashed too, and a
  scenario whose exit code is not 0 is refused by the capture.
* **The replay is reproducible.** Two consecutive runs of `verify.py` produce
  the same hashes: the simulator is seeded, the journal carries no wall clock
  and no identifier of the run.

## What the fingerprint does NOT guarantee

* **The real-time behaviour.** The simulator has no clock: the jitter, the
  scheduling of the period, the latency of the frames and the behaviour under
  QNX are outside the fingerprint (PERF-01, QNX-01..QNX-08). The bench of
  `host/bench/` and the QEMU layer cover those, not this file.
* **The behaviours that the game does not exercise.** The fingerprint pins
  what the 26 scenarios walk. A new behaviour (a third machine, a vote, a
  second actuator path) is not pinned until the game is extended and the
  reference refreshed.
* **A portability across toolchains.** The core formats its numbers with
  `%.6g`: a different compiler or a different `libm` can differ on the last
  digit. The reference is therefore pinned for the toolchain of the CI
  runner, not for every platform. A divergence on another platform is a
  question, not automatically a regression: read the first divergent tick
  before concluding.
* **The meaning of the scenarios.** The fingerprint says the decisions did not
  move; it does not say the behaviour is correct. Correctness is the job of
  the invariants, of the explorer and of the acceptance criteria.

## The normalisation, field by field

| Field | Rule | Why |
|---|---|---|
| `ns` | `"ns":<n>` -> `"ns":0` | nanosecond stamp of the record: on a real actuator it is the wall clock, the most volatile field of the journal |
| `pid` | `"pid":<n>` -> `"pid":0` | process identifier: volatile |
| `timestamp`, `ts`, `time_ms`, `wall_ms`, `epoch` | -> `0` | any wall-clock field a future version could add |
| absolute path | `"/..."` -> `"<path>"` | the run directory changes at every run |
| line endings | CRLF/CR -> LF, blank lines dropped, lines trimmed | the file must not depend on the platform |

Nothing else is touched: the floats written by the core, the names and the
tick numbers stay exactly as the core wrote them. The rules are idempotent,
and the count of normalised fields per scenario is stored in the reference
(`normalized_fields`): with the simulator of today, only `ns` is present, and
it is already `0` — the normalisation is there so that the guard survives the
day a real actuator journal is fed to it.

The reference also stores, per scenario, the SHA-256 of the invariant report
and a short digest per journal line (`line_digests`). The digests are not a
second guard (the whole-journal hash is); they localise the first divergent
line, and therefore its tick, when the guard bites.

## The frozen game

26 scenarios, all replayed at the `complet` level (shadow and certification
in play) unless stated otherwise:

| Behaviour | Scenarios |
|---|---|
| nominal | `published/slalom-nominal`, `golden/nominal-socle` (level S) |
| death of the holder | `published/slalom-death-A`, `published/slalom-double-death` |
| death of the second | `published/slalom-death-B` |
| freeze | `published/slalom-freeze-A-200ms`, `published/slalom-freeze-B-1s` |
| cut (machine -> actuator, between computers) | `published/slalom-cut-A-to-actuator`, `published/slalom-cut-between-computers`, `golden/cut-restore` |
| slow machine | `published/slalom-slow-machine` |
| double death | `published/slalom-double-death` |
| cession (voluntary handover) | `golden/cession-holder`, `published/rolling-update` |
| update in operation | `published/rolling-update` (REL-10, DEG-03, DEG-07) |
| dissimilar second | `golden/dissimilar-second` (DEG-08: the widened band, the dissimilar delta) |
| second computing wrong | `golden/mismatch-second` (DET-09: the disagreement is reported) |
| sweep of every phase of a handover | `golden/sweep-death-phase-0` .. `golden/sweep-death-phase-4` (the five phases of the period T = 5 ms) |
| forged order, stale replay | `golden/forge-order` (SEC-04), `golden/forge-stale` (ACT-09) |
| lossy channel with jitter | `golden/lossy-delay` (ROB-02, ROB-03) |
| crash of a process, restart of a machine | `golden/crash-proc` (DET-05, QNX-06), `golden/restart` (DEG-03, ROB-06) |

The `published/...` scenarios replay the scenarios of the repository
(`scenarios/*.json`) over a 2000-tick horizon (their faults land before tick
1000); the `golden/...` scenarios cover the behaviours the published set does
not reach. The manifest entry always overrides the published file, so the
game can be frozen independently of the campaign files.

## Refreshing the reference on purpose

A divergence is not a nuisance to be silenced: it is the signal the mission
was built for. The procedure is deliberate:

1. `make golden-verify` — read the scenario named by the guard and the first
   divergent tick.
2. Replay that scenario by hand to read what actually changed: the command
   line is printed by `python3 tools/golden/capture.py --list` (add
   `--journal <file>` to keep the journal, and diff it against a journal
   captured before the change).
3. If the change is the INTENDED one (the mission M05 or M06 rewrites the
   core on purpose), refresh the reference:
   `make golden-update`.
4. Commit the refreshed `tools/golden/traces.json` **with** the change, and
   say in the pull request what moved: the scenario, the tick, and the
   requirement that justifies it.
5. If the change is NOT intended, the divergence is a regression: fix it, do
   not refresh the reference.

`make golden` (capture) also rewrites the reference; it exists for the first
capture and for a local experiment. The CI never runs it: the workflow
`.github/workflows/golden.yml` only READS the reference.

## Why M05 and M06 depend on this

M05 (three machines and a vote) and M06 (two paths of the actuator) rewrite
the part of the core that decides everything: who holds the mandate, when it
is revoked, which value is applied, what the fallback does. Their own tests
will be written for the NEW behaviour — they cannot prove that the OLD
behaviour was preserved.

The golden fingerprint is the missing half of the proof: before the rewrite,
the behaviour is pinned; after the rewrite, `make golden-verify` must still
be green, and every divergence must be either an intended difference (the
mandate now goes to the third machine, the value now comes from the second
actuator path) or a regression. The workflow `.github/workflows/golden.yml`
runs on every pull request, so the check exists before the merge, not after.

## Reproducing the evidence

```
make -j4 all && make test && make explore   # the core is green
make golden                                  # capture the reference
python3 tools/golden/verify.py               # the guard is green
python3 tools/golden/verify.py               # and again: the replay is stable
python3 -m unittest discover -s tools/golden -p 'test_*.py'
```

To see the guard bite, change one parameter of the core in a COPY of the
tree (for instance the epsilon of the agreement band), rebuild the simulator
and run `python3 tools/golden/verify.py`: the guard names the scenario and
the first divergent tick, and exits with a non-zero code.
