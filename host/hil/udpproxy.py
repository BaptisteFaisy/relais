#!/usr/bin/env python3
"""Relais — UDP relay used to cut a link for real (mission M41, INJ-04).

The relay sits on the paths between the two computers and the actuator:

    node A --+  (orders, shadows)         +-- node A
             +-- relay :actuator-port -----+
    node B --+                             +-- node B
              (replicated state)
    node A --+  (ETAT / SUIVI)             +-- node A
             +-- relay :peer-port ---------+
    node B --+                             +-- node B
                        |
                    actuator

Two sockets, one process: :actuator carries the orders and the STATUTs,
:peer carries the replicated state between the computers. Killing the relay
(SIGKILL) severs BOTH paths, in both directions, for both computers, while
every socket stays open and every process stays unaware.

That is a REAL cut of the data path -- but it is a SOFTWARE cut, not an
electrical one: the conductors are not opened. The electrical cut is the job
of the relay adapter (host/hil/relay_adapter.h), which is BLOCKED(no relay
hardware) on this machine.

Usage:
  udpproxy.py --listen-actuator 7400 --listen-peer 7401 \
              --actuator 127.0.0.1:7300 \
              --node A=127.0.0.1:7301 --node B=127.0.0.1:7302
"""
import argparse
import select
import socket
import sys


def parse_addr(spec):
    host, _, port = spec.rpartition(":")
    return (host, int(port))


def parse_node(spec):
    name, _, addr = spec.partition("=")
    return name, parse_addr(addr)


def forward(sock, data, dests):
    for dest in dests:
        try:
            sock.sendto(data, dest)
        except OSError:
            pass


def serve_actuator(sock, actuator, nodes):
    """node -> actuator (orders, shadows, demands); actuator -> both nodes."""
    data, src = sock.recvfrom(65535)
    if src[1] == actuator[1]:
        forward(sock, data, [addr for _name, addr in nodes])
    else:
        forward(sock, data, [actuator])


def serve_peer(sock, nodes):
    """node -> the other node (ETAT, SUIVI)."""
    data, src = sock.recvfrom(65535)
    for name, addr in nodes:
        if addr[1] != src[1]:
            forward(sock, data, [addr])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--listen-actuator", type=int, required=True)
    parser.add_argument("--listen-peer", type=int, required=True)
    parser.add_argument("--actuator", required=True)
    parser.add_argument("--node", action="append", default=[])
    args = parser.parse_args()

    actuator = parse_addr(args.actuator)
    nodes = [parse_node(spec) for spec in args.node]
    act_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    act_sock.bind(("127.0.0.1", args.listen_actuator))
    peer_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer_sock.bind(("127.0.0.1", args.listen_peer))
    print("proxy up: actuator path on %d, peer path on %d, actuator %s, computers %s"
          % (args.listen_actuator, args.listen_peer, args.actuator, nodes), flush=True)
    while True:
        ready, _, _ = select.select([act_sock, peer_sock], [], [], 1.0)
        for sock in ready:
            if sock is act_sock:
                serve_actuator(sock, actuator, nodes)
            else:
                serve_peer(sock, nodes)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
