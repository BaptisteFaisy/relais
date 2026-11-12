#!/bin/sh
# Relais M46 — guest-side failover scenario (STUB harness).
#
# Runs entirely INSIDE the QNX guest, driven over the serial console by
# host/bench/qnx_vm.py. Shape of the real banc (fault injected from inside
# the guest, takeover measured on the guest clock) with a substitute binary
# until target/ compiles with qcc (M45). All output goes to the console and
# lands in the host transcript — that IS the guest journal we recover.
#
# Timeline: second listens; holder heartbeats every 10 ms; after 1 s the
# holder is SIGKILLed; the second must declare takeover within the 40 ms
# silence window (p+1 = 4 missed beats at 10 ms, the banc's default p=3).
rl-fail second 41234 40 &
SECOND=$!
rl-fail holder 41234 10 &
HOLDER=$!
sleep 1
echo "rl-failover: killing holder_pid=$HOLDER"
kill -9 $HOLDER
wait $SECOND
RC=$?
echo "rl-failover: scenario_complete rc=$RC"
exit $RC
