# Codelab — two machines, one master

*How to detect a failure in two periods, without ever choosing the wrong chief.*

Forty-five minutes, a Linux PC, no QNX licence, no root, no hardware. You will
run the real protocol, kill a machine, watch the other take over with no visible
bump on the physical curve, then prove that the bench you just trusted is not
lying to you.

---

## Step 0 — the problem in one paragraph

A steer-by-wire rack has no column: if the computer that commands it goes quiet,
the wheels go quiet. Doubling the computer does not remove the hard question —
**who commands, and how do you change hands without a bump and without two
masters?** The usual answer is a heartbeat and a timeout, and the timeout is a
guess: too short and you depose a healthy machine that was merely late, too long
and the actuator stays without a command. Relais answers differently: the
actuator itself decides, on evidence it gathers itself, and it gives the tempo.

## Step 1 — build and run the demonstration

```bash
git clone https://github.com/BaptisteFaisy/relais.git && cd relais
make -j$(nproc)          # gcc, make, python3: nothing else
make demo                # one command, no root, no QNX, no QEMU
```

`make demo` starts three real processes on your PC — the actuator and two
computers — over UDP, and plays the demonstration of the specification: a slalom,
then the second freezes, then the holder dies. It ends with the handover journal
and the verifier's verdict:

```
  tick     event                  node     mandate
  2        mandate_granted        A        1
  904      mandate_revoked        A        1
  904      mandate_auto_switch    B        2
ticks=1600 violations=0 max_gap=0 (bound 0) max_deviation=0 (bound 0.02)
```

`max_gap=0` is the whole promise: **not one tick went by without a fresh order**.
`max_deviation=0` compares every applied value with the *twin* — the same
scenario replayed with no fault at all. The curve did not move.

On a busy machine, prefer the deterministic mode (same core, same invariants, no
clock, no jitter): `make demo` with `--sim` —
`python3 host/bench/relais_bench.py --demo --sim`.

## Step 2 — read one line of the journal

The actuator writes one JSON record per tick. That file is the authority: the
verifier, the report and the acceptance criteria all read it, nothing else.

```json
{"kind":"tick","tick":902,"mandate":1,"holder":"A","source":"shadow","value":-0.031,
 "pos":0.598,"vel":-0.02,"setpoint":0.599,"misses":1,"lamp":"yellow",
 "cause":"node_missing","certified":true,"certified_node":"B","stop":false}
```

Read it out loud: at tick 902 the holder A sent nothing (`misses: 1`), so the
actuator applied the **shadow** of B, its certified second — the command stayed
fresh, the lamp went yellow, and the record says exactly why.

## Step 3 — kill a machine, watch the handover

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet \
    --fault 500:death:0 --journal /tmp/fault.jsonl --twin-journal /tmp/twin.jsonl
./build/relais-verify /tmp/fault.jsonl --level complet --twin /tmp/twin.jsonl
```

Now open `/tmp/fault.jsonl` around tick 500 and walk the four lines that matter:
the last order of A, the first shadow of B, the tick where the mandate moves
(exactly `p+1` ticks after A's last order), and the first order of B. Then do it
again at level S — drop `--level complet` — and watch the will (`"source":
"testament"`) cover the two ticks the second needs to ask and be granted.

## Step 4 — meet the revenant

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet --fault 800:freeze:0:40
```

A freezes for 200 ms. Two periods later the actuator has moved on. When A wakes
up it believes it still commands and sends an order — which is rejected **twice
over**: it does not carry the current mandate, and it does not target the next
tick. A reads the next `STATUT`, sees B holding mandate 2, and stands down as
second. No timeout, no vote, no split brain: the actuator never asked anyone's
opinion.

## Step 5 — cut the cable between the two computers

```bash
./build/relais-simulate --ticks 2000 --seed 7 --level complet --fault 600:cut-peer:0
```

Nothing happens, and that is the point. The second no longer receives the
replicated state, but it still receives the `STATUT` — so it keeps following,
rebuilds a state from what the actuator publishes, and **no handover is
triggered**. A machine that cannot hear its neighbour cannot promote itself:
that is the "dead or just disconnected?" dilemma, solved without a third
computer.

## Step 6 — run the proof

```bash
make test          # 5630 unit checks
make campaign      # 40 fault runs, both levels
make explore       # exhaustive exploration of the core (level 1)
python3 host/mutants/run_mutants.py
```

The last command is the one to look at. It builds the same core eight times,
each with one deliberate defect, and each defect faces a scenario **the healthy
core passes first**. If the bench does not flag a mutant, the bench is broken —
not the software. Result: 8 out of 8 flagged, each by the invariant it was meant
to break.

## Step 7 — read the core (30 lines that carry the design)

Open `core/src/arbiter.c` and read `relais_arb_step`. Seven steps, in this
order, once per tick:

1. find the holder's valid order for this tick;
2. count the miss;
3. revoke if the misses pass `p` — auto-switch to the certified second;
4. process cessions, then demandes;
5. pick the one value: order → shadow → will → fallback loop;
6. update certification and the lamp;
7. publish `STATUT` and the journal.

No clock, no file, no socket, no allocation, no global state. The same file
compiles into the QNX processes, the PC simulator and the exhaustive explorer —
which is why what is explored is what runs.

## Step 8 — build for QNX and run it under QEMU

1. Get the SDP through [QNX Everywhere](https://www.qnx.com/), target QNX 8.0,
   `qcc -V gcc_ntox86_64`.
2. Build the image with `mkqnximage` using the fragments in `image/`
   (`io-sock` with `devs-vtnet_pci.so`, the HAM supervisor, the typed security
   policy of SEC-05, and the two identities by MAC address — the same image
   boots both machines).
3. Start the bench: `python3 host/bench/relais_bench.py --mode qemu --scenario
   scenarios/slalom-death-A.json`. It launches two guests, the programmable
   cable between them, the pcap captures, and drives QEMU over QMP: `stop`,
   `cont`, `quit`, `system_reset`, `set_link`.
4. Run the feasibility probes first if this is your machine:
   `python3 host/bench/relais_bench.py --probe E1` (do two guests exchange
   datagrams without root, and at what latency?), then E2 to E6.

Without the SDP, `make -C target syntax` still checks the QNX layer compiles
against stubs — that is what the CI runs.

## Step 9 — make it yours

- **Your own control law**: implement the three functions of `core/include/relais/control.h`
  (`init`, `step`, `export`/`import`). The protocol does not change (CMD-09).
- **A second profile**: the braking demonstration is already there
  (`kind=brake`, CMD-08); add your own trajectory in `core/src/plant.c`.
- **Three machines and a vote**: the arbiter already tracks a third node
  (`RELAIS_NODE_C`); make it apply the value two machines agree on (REP-08).
- **An independent formal spec**: describe the arbiter in TLA+ and check that
  both explorations agree on the invariants.

## Pitfalls we paid for, so you don't have to

| Symptom | Cause | Fix |
|---|---|---|
| The second's shadow never matches the holder's order | the state was exported *after* the computation, so the replica was one tick ahead | export the state the holder had **before** computing (see `make_etat` in `core/src/node.c`) |
| The fallback loop arrives one tick late | the will was based on the tick of the order instead of the next one | `testament_base = tick + 1`, and the prediction advances the rack before each step |
| The lamp stays off during a substitution | the lamp looked at the holder's presence, not at the source of the value | `source != holder` is a loss of redundancy, whatever the presence flags say |
| The handover is judged too early | the invariant checker read the tick record before the events of that tick | feed the events first, then the tick record, then close the tick |
| The demo misses ticks on a laptop | a shared OS does not promise a 5 ms jitter | the PC demo runs in slow motion (T = 25 ms, p = 3); the 5 ms / 200 Hz figures belong to the QNX target under KVM |
