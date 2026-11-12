# Diversity bench (mission M37)

What is shared between the two computers is measured here, not assumed. The
curated result is [`docs/CCF.md`](../../docs/CCF.md); this directory holds the
tools that produce the raw numbers.

## What is here

| File | Role |
|---|---|
| `build_variants.sh` | builds every variant of the core this machine can produce, records the exact compile line, the binary digest and the source digest |
| `trace.c` | a driver that plays one simulator run and writes **every decision of every tick**, floats as raw IEEE-754 patterns |
| `law_alt.c`, `law_alt.h` | two control laws written apart from the built-in PID, plugged through CMD-09 (`relais_node_set_law`); `core/` is untouched |
| `compare.py` | plays one corpus on every variant and compares the decisions bit for bit, with **controls** that must diverge |
| `law_effect.py` | measures what a design-diverse law costs at the handover (certification, deviation from the twin, the will) |

## How to run

```sh
make diversity            # build the variants, compare, then measure the laws
```

or step by step:

```sh
host/diversity/build_variants.sh --jobs 3
python3 host/diversity/compare.py --ticks-cap 1500 --seeds 4
python3 host/diversity/law_effect.py
```

Artefacts land in `build/diversity/`:

- `toolchain.tsv` — every compiler probed on this machine, with the ones that
  are **absent** recorded as such;
- `variants.tsv` — one line per attempted build: status, role, compiler, flags,
  binary, digest, note;
- `source-digest.txt` — the digest of everything the variants share;
- `comparison.json` — the comparison, with the first divergent tick of every
  divergence;
- `law-effect.json`, `law-effect.md` — the measured effect of a diverse law.

## Rules of this bench

1. **No invented diversity.** A missing compiler is recorded ABSENT, with the
   command that was tried. A missing multilib is recorded BLOCKED. Nothing is
   simulated to look like a second compiler.
2. **The comparator must be sensitive.** Three controls are built — two injected
   defects (`-DRELAIS_MUTANT=1` and `=5`) and a forbidden floating point option
   (`-ffast-math`). If a control does **not** diverge, the bench reports itself
   as blind rather than as successful.
3. **Bit exact, not rounded.** The journal prints floats with `%.6g`, which
   hides a difference below one part in a million; the trace writes the raw
   patterns, so a comparison is exact and does not depend on the printf of the
   libc under test.
4. **`core/` is never touched.** The alternative laws go through the existing
   three-function interface of CMD-09.
