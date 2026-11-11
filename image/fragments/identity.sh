#!/bin/sh
# Relais — one image, two machines. The role comes from the MAC address, so the
# two computers run exactly the same bytes (CMD-03) and swapping the cables
# swaps the roles.
#
# Exports RELAIS_SELF (A or B), RELAIS_ADDR and RELAIS_PEER.

MAC=$(ifconfig en0 2>/dev/null | awk '/ether/{print $2}' | tr 'A-F' 'a-f')
[ -z "$MAC" ] && MAC=$(ifconfig em0 2>/dev/null | awk '/ether/{print $2}' | tr 'A-F' 'a-f')

case "$MAC" in
  52:54:00:12:35:0a)
    RELAIS_SELF=A; RELAIS_ADDR=10.9.0.2; RELAIS_PEER=10.9.0.3 ;;
  52:54:00:12:35:0b)
    RELAIS_SELF=B; RELAIS_ADDR=10.9.0.3; RELAIS_PEER=10.9.0.2 ;;
  *)
    # Unknown card: come up as a second that never asks for the mandate, so a
    # mis-built image cannot start commanding. The bench reports it.
    RELAIS_SELF=B; RELAIS_ADDR=10.9.0.3; RELAIS_PEER=10.9.0.2
    echo "identity.sh: unknown MAC '$MAC', coming up as B" > /dev/sloginfo ;;
esac

# The actuator: on the PC at level S, in the third guest at level C.
RELAIS_ACTUATOR=${RELAIS_ACTUATOR:-10.9.0.1}
export RELAIS_SELF RELAIS_ADDR RELAIS_PEER RELAIS_ACTUATOR
