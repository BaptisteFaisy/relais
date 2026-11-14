#!/usr/bin/env bash
# Relais — REAL link cut between the computers and the actuator (M41, INJ-04).
#
# What is cut, exactly: the data path. A UDP relay (host/hil/udpproxy.py)
# holds the path between the two computers and the actuator; this script
# kills that relay with SIGKILL. From that instant the datagrams stop
# flowing in BOTH directions and for BOTH computers, while every socket
# stays open and every process stays unaware.
#
# What is NOT cut, and is stated here so nobody believes more than was done:
#   * the conductors are NOT opened: this is a software cut of the path, not
#     an electrical one. The electrical cut is the relay adapter
#     (host/hil/relay_adapter.h), BLOCKED(no relay hardware) here;
#   * the machines do not die: they detect the silence and dispose of
#     themselves (MAN-09), which is what a real link cut looks like.
# Both paths are cut at once: the computer-actuator path (orders, shadows,
# STATUT) and the computer-computer path (ETAT, SUIVI), each in both
# directions — faults F4 and F5 of the model, played on a real data path.
#
# Only the processes started here are ever signalled (their PIDs are kept).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
OUT="build/hil/linkcut"
JOURNAL="$OUT/actuator.jsonl"
mkdir -p "$OUT"
rm -f "$JOURNAL" "$OUT"/*.log

mapfile -t PORTS < <(python3 host/hil/freeports.py 5)
P_ACT="${PORTS[0]}"
P_A="${PORTS[1]}"
P_B="${PORTS[2]}"
P_PROXY="${PORTS[3]}"
P_PEER="${PORTS[4]}"
T_MS="${T_MS:-15}"
TICKS="${TICKS:-260}"
PRMS="T=$T_MS,p=3,N=8"

echo "link cut bench: actuator=127.0.0.1:$P_ACT nodeA=:$P_A nodeB=:$P_B proxy=:$P_PROXY"

# The relay and the computers come up first, the actuator last: the path
# exists and the two machines are listening when the tempo starts, so the
# run starts in a normal state (no startup gap to explain).
python3 host/hil/udpproxy.py --listen-actuator "$P_PROXY" \
  --listen-peer "$P_PEER" --actuator "127.0.0.1:$P_ACT" \
  --node "A=127.0.0.1:$P_A" --node "B=127.0.0.1:$P_B" > "$OUT/proxy.log" 2>&1 &
PROXY_PID=$!
disown "$PROXY_PID" 2>/dev/null || true
sleep 0.2

./build/relais-node --self A --port "$P_A" --actuator "127.0.0.1:$P_PROXY" \
  --peer "127.0.0.1:$P_PEER" --ticks $((TICKS + 100)) --params "$PRMS" \
  > "$OUT/node-A.log" 2>&1 &
NA_PID=$!
disown "$NA_PID" 2>/dev/null || true
./build/relais-node --self B --port "$P_B" --actuator "127.0.0.1:$P_PROXY" \
  --peer "127.0.0.1:$P_PEER" --ticks $((TICKS + 100)) --params "$PRMS" \
  > "$OUT/node-B.log" 2>&1 &
NB_PID=$!
disown "$NB_PID" 2>/dev/null || true
sleep 0.2

stdbuf -oL ./build/relais-actuator --port "$P_ACT" \
  --peer "A=127.0.0.1:$P_PROXY" --peer "B=127.0.0.1:$P_PROXY" \
  --ticks "$TICKS" --params "$PRMS" --journal "$JOURNAL" \
  --state "$OUT/arb.state" > "$OUT/actuator.log" 2>&1 &
ACT_PID=$!
disown "$ACT_PID" 2>/dev/null || true
sleep 1.0

LAST_LINE=$(tail -n 1 "$JOURNAL")
CUT_TICK="${LAST_LINE#*\"tick\":}"
CUT_TICK="${CUT_TICK%%,*}"
if [ -z "$CUT_TICK" ]; then
  echo "link cut bench: no journal line, aborting"
  kill -9 "$ACT_PID" "$NA_PID" "$NB_PID" "$PROXY_PID" 2>/dev/null
  exit 2
fi
echo "cutting the data path at tick $CUT_TICK (SIGKILL on the relay pid $PROXY_PID)"
kill -9 "$PROXY_PID" 2>/dev/null
sleep 2.0

kill -9 "$NA_PID" "$NB_PID" "$ACT_PID" 2>/dev/null
sleep 0.2

echo "--- what the protocol did after the cut ---"
python3 host/hil/linkreport.py "$JOURNAL" --cut-tick "$CUT_TICK" --t "$T_MS"
echo "--- invariants (disturbed channel: safety enforced, gap measured) ---"
./build/verify-hil "$JOURNAL" --lossy || true
echo "--- invariants (level C completeness, quiet channel) ---"
./build/verify-hil "$JOURNAL" --strict || true
echo "journal kept in $JOURNAL"
