# Relais — frozen contract for contributors (humans and agents)

Read this before writing a line. The headers in `core/include/relais/` are the
contract: **implement them, do not change them**. If a change seems unavoidable,
write it in your report instead of editing the header.

## Ground rules (user rules, enforced by review)

- C11, compiled with `-std=c11 -O2 -Wall -Wextra -Werror`.
- `core/` is portable and pure: **no clock, no file, no socket, no allocation,
  no global mutable state**. Callers own every byte. `snprintf` and `libm` are
  allowed; `time`, `fopen`, `malloc` are not.
- QNX-only code lives in `target/` and `actuator/qnx/`; it is guarded by
  `#ifdef __QNX__` and never breaks the PC build.
- Every file **under 500 lines**, every function **under 50 lines**. Split.
- **No duplication**: one implementation per logic. Shared helpers go in
  `core/src/` or `core/tests/test_util.c`, never copy-pasted.
- Python: 3.x standard library only, no external dependency.
- Every requirement ID you satisfy (CMD-01, MAN-02, REL-01, ...) is cited in a
  comment above the code and in the matching test name.
- Tests are plain binaries: `build/test_xxx` returns 0 when green, prints one
  line per check, and a final `OK <n> checks` / `FAILED`.

## Build

`make` builds everything into `build/`; `make test` runs the unit tests;
`make explore` / `make simulate` / `make verify` run the proof levels;
`make mutants` builds and runs the eight mutants.

## Journal format (authoritative, `core/src/journal.c`)

One JSON object per line, ASCII, no trailing comma, floats with `%.6g`.

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

### The `chain` field (idea 34)

Every record a current build writes carries `"chain"` as its **last** key: 16
lower-case hexadecimal digits, the fingerprint of the record chained to the one
before it. It is computed in `core/src/journal.c`, in the pure core, so the
actuator that writes it and `relais-verify` that recomputes it cannot drift:

```
chain(0)   = 0000000000000000                       (genesis, before the header)
chain(i)   = SipHash-2-4(K, "relais-chain-v1" || 0 || chain(i-1) || body(i))
```

`body(i)` is the record exactly as formatted **without** the `,"chain":"..."`
suffix, newline excluded; the 8 bytes of the digest are written little-endian and
printed in order. `K` is the chain key: the built-in public key
`524c53636861696e3031323334353637` unless the writer was given one
(`--chain-key <32 hex>` on `relais-simulate`, `relais-actuator` and
`relais-verify`). Records are chained in file order, the header included, and the
events of a tick are chained before its record, like everything else.

A reader **must** treat `chain` as optional: a journal written before this field
exists has none and stays readable, and a reader that does not know the field
ignores it. `relais-verify --check-chain` recomputes the chain and names the
first break; a record that carries no fingerprint *after* one that did is a
break. The writers also print a `chain anchor` — the fingerprint of the last
record — which is the only way to notice a tail cut off the file, so it belongs
in whatever seals the run.

Names on the wire of the journal: `holder`/`node`/`certified_node` are `""`,
`"A"`, `"B"`, `"C"`, `"actuator"`; `source` is `none|holder|shadow|testament|fallback`;
`lamp` is `off|yellow|red`; `event` is the snake_case of `RELAIS_EV_*`
(`mandate_granted`, `mandate_revoked`, `mandate_auto_switch`, `request_denied`,
`certified`, `decertified`, `order_rejected`, `disagreement`, `fallback_enter`,
`fallback_exit`, `cession`, `lamp_change`, `stop_requested`, `forged`, `node_back`).

## Tick protocol (the sequencing every module must respect)

The actuator owns the tempo. During tick `n` it applies one value, then
publishes `STATUT(n)`. Machines compute on `STATUT(n)` and send frames
**targeting tick n+1**. The arbiter step for tick `n` runs in this order:

1. find a valid `ORDRE` of the holder targeting `n`;
2. `misses = found ? 0 : misses + 1`;
3. if `misses > p` revoke (auto-switch to the certified second: mandate + 1);
4. process `CESSION`, then `DEMANDE` (grant only if vacant, mandate + 1);
5. pick the value: holder order, else certified shadow, else testament, else
   fallback law;
6. certification streak, lamp, degraded timer;
7. fill `STATUT(n)` and the journal events.

With `p = 1` this yields the documented bounds: 3 periods between two fresh
orders at level S (2 when the demande carries its first order), and **zero**
tick without a fresh order at level C when a second is certified.

## Scenario file format (`scenarios/*.json`)

```json
{"id": 1, "name": "slalom-death-A", "trajectory": {"kind":"slalom","amp":0.6,"freq":0.5},
 "params": {"T":5,"p":1,"N":8}, "ticks": 4000, "seed": 12345,
 "faults": [{"at_tick": 500, "type": "death", "target": "A"},
            {"at_tick": 900, "type": "freeze", "target": "B", "ticks": 40},
            {"at_tick": 1200, "type": "cut", "link": "A-actuator", "direction": "both", "ticks": 60}]}
```

`type` is one of `death|freeze|thaw|cut|restore|slow|bias|restart|crash`,
`target` is `A|B|actuator`, `link` is `A-B|A-actuator|B-actuator`,
`direction` is `both|forward|reverse`, `crash` names a process
(`relais-net|relais-core|relais-ctl`).
