# Relais in Rust — the arbiter ported (mission M32, idea 74)

**Status on this machine: BUILT, TESTED AND CONFRONTED FOR REAL.** `cargo` 1.99.1
is installed here, so nothing in this port is hypothetical:

```
make -C ports/rust check
```

| step | command | result here |
|---|---|---|
| build | `cargo build --offline` | OK |
| unit tests | `cargo test --offline` | **12 passed, 0 failed** |
| lint | `cargo clippy --offline --all-targets -- -D warnings` | **0 warning** |
| `no_std` | `cargo build --offline --no-default-features` | OK |
| C vs Rust | `python3 ports/rust/difftest/run_difftest.py --runs 200` | **201 sequences, 12 025 ticks, identical** |
| C vs Rust (long) | `--runs 2000 --seed 7 --ticks 120` | **2 001 sequences, 240 025 ticks, identical** |

No dependency at all: `Cargo.toml` has an empty `[dependencies]`, so `--offline`
always works and no registry is contacted.

## Scope: the arbiter, not the core

The point is a demonstration, not a second product. Ported: the mandate machine
and the tick decision of `core/src/arbiter.c` — grant, revocation on the
actuator's own evidence, auto-switch to a certified second, cession deferred one
tick, certification by agreement, disagreement detection, the will, the fallback
law, liveness, the lamp, the degraded timer, the rate limit on demandes, the
counters and the 16-byte restart snapshot (MAN-11).

Not ported: frames and their MAC (`frame.c`, `check.c`), the node state machine
(`node.c`), the plant model, the journal, the invariant checker. A Rust node
would be a second implementation of the *product*; the arbiter is where the
safety argument lives, so that is what gets a second opinion.

| file | lines | role |
|---|---|---|
| `src/lib.rs` | 29 | crate root, `#![forbid(unsafe_code)]`, `no_std` when the `std` feature is off |
| `src/types.rs` | 274 | the vocabulary of `types.h`, same integer values, plus `clamp_value` / `clamp_slew` / `testament_at` |
| `src/params.rs` | 86 | the parameter block of `params.c` and its consistency rule |
| `src/arbiter.rs` | 363 | the state, the scan, the mandate rules |
| `src/step.rs` | 288 | the tick pipeline: pick, certify, liveness, lamp, publish |
| `src/json.rs` | 208 | `std`-only JSON reader for the test bridge |
| `src/bin/bridge.rs` | 267 | the JSON-lines server, same protocol as `host/difftest/bridge.c` |
| `tests/arbiter_rules.rs` | 253 | 12 unit tests of the mandate rules |
| `difftest/run_difftest.py` | 111 | the C-vs-Rust driver |

Same rules as the core: **no allocation** (fixed arrays and counters, no `Vec` in
the library), **no clock**, **no I/O**, **no `unsafe`**.

## The differential test

`ports/rust/difftest/run_difftest.py` reuses the existing machinery instead of
inventing a second one: the sequence generators (`nominal_sequence`,
`random_sequence`), the persistent-child `Bridge` class and the recursive
comparator `diff_paths` are imported from `host/difftest/difftest.py`. The only
thing swapped is the executable, because the Rust bridge speaks the *same*
JSON-lines protocol as the C one:

```
{"cmd":"init","params":{...}}                     -> {"ok":true}
{"cmd":"step","tick":N,"meas":{...},"msgs":[...]} -> {"tick":N,"state":{...},"out":{...}}
```

Both children are fed the same frames; after every tick the **whole state** (36
fields, arrays included) and the **whole decision** (value, source, mandate,
holder, misses, lamp, cause, the STATUT and every journal event) are compared.
Integers, enums and booleans exactly; floats with the comparator's 1e-6 relative
tolerance. The C binary `build/relais-bridge` stays the reference — it is built
by `make all` from `core/`, unmodified.

## What the confrontation found

Two real bugs in this port, both caught by the comparator and not by reading:

1. **`shadow_misses`** — the C original writes `if (certified && ++shadow_misses
   > patience)`, so the counter moves *only* while there is a certification to
   lose. The first Rust version incremented it unconditionally. The very first
   tick of the nominal scenario disagreed (`state.shadow_misses 0 vs 1`).
2. **The NaN branch of `clamp_value`** — `!(v > -max)` is the point, not an
   oversight: it is what sends a NaN to `-max_amplitude`. Clippy flags it
   (`neg_cmp_op_on_partial_ord`) and the file carries an explicit `#[allow]` with
   the reason, so a future reader does not "fix" it into a divergence.

One observation about the C core, reported and **not** changed (rule 10 of the
mission): `relais_arb.have_order` and `relais_arb.have_shadow`
(`core/include/relais/arbiter.h`, lines 41-42) are declared, dumped by
`host/difftest/bridge.c` and never written anywhere in `core/src/arbiter.c` —
they are permanently `false`. Dead state, harmless, but a port that pretends to
be faithful has to reproduce it, which this one does.

## Reproduce

```bash
make -j4 all                                   # builds build/relais-bridge (the C reference)
make -C ports/rust check                       # build + tests + clippy + no_std + difftest
cd ports/rust && cargo test --offline          # the 12 Rust-side rules alone
python3 ports/rust/difftest/run_difftest.py --runs 2000 --seed 7 --ticks 120
```

If `cargo` is missing, `make -C ports/rust check` prints
`RUST-PORT-SKIPPED: cargo/rustc not installed` and exits 0; if the C reference is
missing, the difftest step prints `DIFFTEST-SKIPPED` and the rest still runs.

## What is *not* claimed

This is not a certified second implementation, and it is not a proposal to rewrite
the product in Rust. It is evidence for the central claim of the project — that
the arbiter is a *specification* rather than a C implementation detail: an
independent language, an independent type system, 240 025 ticks of random and
nominal traffic, and not one divergent decision.
