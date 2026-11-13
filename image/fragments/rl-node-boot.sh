#!/proc/boot/ksh
# Relais M49 — the boot line that starts machine B, a real QNX 8.0 node.
#
# Launched IN THE BACKGROUND from ifs_start.custom, which mkqnximage runs
# BEFORE the system partition is mounted and before the network is up. It
# therefore waits for /system/bin/ifconfig to appear, then gives the guest
# its address, and finally execs the node so the "boot line" really is a
# boot-time thing and the node's PID is the shell's.
#
# The node is the exact core/ state machine of host/node/relais-node.c
# cross-compiled for QNX 8.0 with the SDP qcc (M48). It receives the STATUT
# on /proc/boot/rl-node's own port (7101) through the QEMU user-net
# (`hostfwd udp:127.0.0.1:7302-:7101` on the host side) and sends its ORDRE
# to the host actuator at 10.0.2.2:7100, the QEMU user-net gateway (spec 6.2,
# QNX-09, REP-01).
#
# The address is STATIC (10.0.2.100): the slirp gateway 10.0.2.2 is on the same
# subnet and this guest is alone on it, so no DHCP server is needed. Two
# reasons not to wait for a lease:
#   - some QEMU builds (>= 9) do not answer the QNX DHCP client at all: the
#     guest stays without an IPv4 and never reaches the actuator;
#   - the QNX startup shell does not always manage a pipe early in the boot
#     ("can't create pipe"), and a lease check needs one. The node sends to
#     the gateway, which is on the same subnet: no default route is required.
#
# usage: rl-node-boot.sh [PARAMS] [TICKS] [ACT_PORT] [PEER_PORT]
set -u

PARAMS="${1:-T=5,p=3,N=8}"
TICKS="${2:-20000}"
ACT_PORT="${3:-7100}"
PEER_PORT="${4:-7101}"
NODE_PORT=7101
ACT_HOST=10.0.2.2
GUEST_ADDR=10.0.2.100

# 1. Wait for the system partition (provides ifconfig), up to 90 s.
n=0
while [ "$n" -lt 90 ]; do
    if [ -x /system/bin/ifconfig ]; then
        break
    fi
    n=$((n+1))
    sleep 1
done

# 2. The address, by hand. The interface is up when the driver is up.
n=0
while [ "$n" -lt 30 ]; do
    if /system/bin/ifconfig vtnet0 10.0.2.100 >/dev/null 2>&1; then
        break
    fi
    n=$((n+1))
    sleep 1
done

# 3. The machine: same binary for QNX, different self, joining the host bench.
#    No --verbose here: the console of the guest is a serial line, and one
#    line per tick is enough to slow the node down below the tick period
#    (measured on a loaded host: the replies then arrive late and the
#    actuator counts misses). The journal of the actuator is the authority.
exec /proc/boot/rl-node --self B --port "$NODE_PORT" \
    --actuator "$ACT_HOST:$ACT_PORT" --peer "$ACT_HOST:$PEER_PORT" \
    --params "$PARAMS" --ticks "$TICKS"
