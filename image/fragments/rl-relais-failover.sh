#!/bin/sh
# Relais M48 — the real failover scenario, played INSIDE the QNX 8.0 guest.
#
# What it does, in order (the whole protocol is the real one: the same core/
# state machine, the same frames, the same arbitration and the same journal):
#
#   1. the actuator starts and writes its journal (the authority, ACT-07),
#   2. the two computers start: A and B, each one a node of the protocol,
#   3. the relay is normally established: A holds the mandate, B is certified,
#   4. the holder is killed with SIGKILL FROM INSIDE THE GUEST (INJ-01),
#   5. the takeover is measured on the GUEST clock and the verdict is read
#      from the actuator's journal, still inside the guest (OBS-04, PERF-02).
#
# Everything the host needs comes out as "rl-relais: ..." lines on the console.
#
# The ports are the ones the protocol uses on the bench: 7100 for the
# actuator, 7101 for A, 7202 for B (M46 found 7102 unusable in this guest).
#
# usage: rl-relais-failover.sh [actuator] [node] [probe] [ticks]
set -u

ACTUATOR=${1:-relais-actuator}
NODE=${2:-rl-node}
PROBE=${3:-rl-relais-failover}
TICKS=${4:-600}
# The parameters of the protocol: the library's defaults (T=5 ms, p=1, N=8),
# the same ones the PC campaigns use. The period matters: the replica of the
# second is one tick behind the holder (REP-01), and with a large T the law's
# step is large enough for that lag to leave the agreement band (eps) -- the
# PC bench shows exactly the same disagreement with T=20.
PARAMS=${5:-T=5,p=1,N=8}

ACT_PORT=7100
A_PORT=7101
B_PORT=7202
JOURNAL=/tmp/relais-actuator.jsonl
NODE_A_LOG=/tmp/relais-nodeA.log
NODE_B_LOG=/tmp/relais-nodeB.log

rm -f "$JOURNAL" "$NODE_A_LOG" "$NODE_B_LOG"

echo "rl-relais-failover: actuator=$ACTUATOR node=$NODE probe=$PROBE ticks=$TICKS params=$PARAMS"
uname -a

# 1. the actuator: the arbitrator, the tempo and the journal.
"$ACTUATOR" --port "$ACT_PORT" \
    --peer A=127.0.0.1:"$A_PORT" --peer B=127.0.0.1:"$B_PORT" \
    --params "$PARAMS" \
    --journal "$JOURNAL" --ticks "$TICKS" &
ACT_PID=$!
sleep 1

# 2. the two computers. A first, so that A takes the mandate (MAN-05).
"$NODE" --self A --port "$A_PORT" --actuator 127.0.0.1:"$ACT_PORT" \
    --peer 127.0.0.1:"$B_PORT" --params "$PARAMS" --ticks "$TICKS" --verbose \
    > "$NODE_A_LOG" 2>&1 &
A_PID=$!
sleep 1
"$NODE" --self B --port "$B_PORT" --actuator 127.0.0.1:"$ACT_PORT" \
    --peer 127.0.0.1:"$A_PORT" --params "$PARAMS" --ticks "$TICKS" --verbose \
    > "$NODE_B_LOG" 2>&1 &
B_PID=$!
sleep 1

# 3-5. wait for the established relay, kill the holder from the guest, measure
# the takeover on the guest clock and read the verdict in the journal.
"$PROBE" --journal "$JOURNAL" --second-log "$NODE_B_LOG" --victim-pid "$A_PID" \
    --ticks "$TICKS" --expect-holder A --expect-second "${6-B}" --timeout-ms 20000
RC=$?

# The journal, read in the guest, and the invariant check, also in the guest.
if [ -f "$JOURNAL" ]; then
  echo "rl-relais-failover: journal lines: $(grep -c . "$JOURNAL")"
  echo "rl-relais-failover: events:"
  grep '"kind":"event"' "$JOURNAL" | head -60
  tail -2 "$JOURNAL"
  if command -v rl-verify >/dev/null 2>&1; then
    for LEVEL in socle complet; do
      REPORT=$(rl-verify "$JOURNAL" --level $LEVEL 2>&1)
      echo "$REPORT" | head -2
      V=$(echo "$REPORT" | sed -n 's/.*violations=\([0-9][0-9]*\).*/\1/p')
      echo "rl-relais-failover: verify_$LEVEL violations=$V"
    done
  fi
fi
echo "rl-relais-failover: holder_pid=$A_PID second_pid=$B_PID actuator_pid=$ACT_PID"
echo "rl-relais-failover: node A (killed):"
tail -2 "$NODE_A_LOG"
echo "rl-relais-failover: node B (the second):"
tail -2 "$NODE_B_LOG"
echo "rl-relais-failover: node B, in the middle of the run (the replica age):"
sed -n '100,103p' "$NODE_B_LOG"
echo "rl-relais-failover: scenario_complete rc=$RC"
exit $RC
