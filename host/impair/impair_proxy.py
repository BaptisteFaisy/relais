#!/usr/bin/env python3
"""Relais — the impaired UDP relay of mission M38 (user space, no root).

Every network fault of the bench is injected inside the processes (the fault
model of host/simulate). The protocol, however, assumes no shared clock and
orders that may arrive out of order (spec 1.2, ROB-03): that assumption was
never played on a real transport. This relay is the missing piece: a seeded,
deterministic UDP impairment between the REAL bench processes (relais-actuator,
relais-node A, relais-node B).

IMPORTANT — this is NOT a kernel impairment. tc/netem needs root and cannot
run in this environment, so it was NOT executed here. The recipe for a real
two-board run is in docs/NETWORK-IMPAIRMENT.md, explicitly marked as not
executed. From the application's point of view the effect is the same: the
datagram the far side sees is the datagram the relay decided to deliver, with
the loss, the delay, the duplication, the reordering, the truncation and the
corruption it applied. Nothing else in the stack is touched, so the kernel
queuing, the MTU and the checksums keep their normal behaviour.

Impairments, per direction, with the exact parameters published in
profiles.json:
  - loss: random, or burst (Gilbert-Elliott two-state chain)
  - duplication
  - reordering (a hold buffer of depth N)
  - fixed delay and jitter
  - truncation and byte corruption
  - asymmetry: one direction degraded, the other clean

Determinism: every random decision is drawn from a per-direction generator
seeded by (seed, profile, direction), so the Nth datagram of a direction meets
the same decision whatever the other directions do. The arrival ORDER inside a
direction is the kernel's, not ours: the decisions are reproducible, the wall
clock is not.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import selectors
import signal
import socket
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

DEFAULT_HOST = "127.0.0.1"
RECV_SIZE = 65535
TICK = 0.001


def make_rng(seed: int, name: str) -> random.Random:
    """A generator that depends only on (seed, direction), never on the clock."""
    digest = hashlib.sha256(("%d|%s" % (seed, name)).encode("utf-8")).digest()
    return random.Random(int.from_bytes(digest[:8], "big"))


def load_profile(path: str, name: str) -> Dict[str, Any]:
    """Read profiles.json and return the profile called *name*.

    The direction list lives at the top level of the catalog; it is copied into
    the profile so resolve_impair sees one self-contained document.
    """
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    for profile in data.get("profiles", []):
        if profile.get("name") == name:
            profile.setdefault("directions", data.get("directions", []))
            return profile
    raise SystemExit("profile %s not found in %s" % (name, path))


def resolve_impair(profile: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    """Merge the defaults, the wildcard and the per-direction parameters."""
    resolved: Dict[str, Dict[str, Any]] = {}
    for direction in profile.get("directions", []):
        merged: Dict[str, Any] = {}
        merged.update(profile.get("defaults", {}))
        merged.update(profile.get("impair", {}).get("*", {}))
        merged.update(profile.get("impair", {}).get(direction, {}))
        resolved[direction] = merged
    return resolved


def parse_link(spec: str) -> Tuple[str, int, str, int]:
    """Parse DIR=ALIAS_PORT:HOST:PORT into its four parts."""
    name, _, rest = spec.partition("=")
    alias, _, dest = rest.partition(":")
    host, _, port = dest.rpartition(":")
    if not name or not alias or not host or not port:
        raise SystemExit("bad --link %s (want DIR=ALIAS:HOST:PORT)" % spec)
    return name, int(alias), host, int(port)


class Link:
    """One impaired direction: its alias port, its destination, its state."""

    def __init__(self, name: str, alias: int, host: str, port: int,
                 impair: Dict[str, Any], rng: random.Random) -> None:
        self.name = name
        self.alias = alias
        self.dest = (host, port)
        self.impair = impair
        self.rng = rng
        self.buffer: List[Tuple[float, bytes]] = []
        self.ge_state = "good"
        self.rx = 0
        self.tx = 0
        self.dropped = 0
        self.duplicated = 0
        self.corrupted = 0
        self.truncated = 0
        self.reordered = 0
        self.delayed = 0
        self.bytes_in = 0
        self.bytes_out = 0

    # --- impairments -----------------------------------------------------
    def is_lost(self) -> bool:
        """Loss: random, or burst (Gilbert-Elliott, two states)."""
        loss = self.impair.get("loss")
        if not loss:
            return False
        mode = loss.get("mode", "random")
        if mode == "random":
            return self.rng.random() < float(loss.get("rate", 0.0))
        if mode != "gilbert_elliott":
            raise SystemExit("unknown loss mode %s" % mode)
        to_bad = float(loss.get("p_good_to_bad", 0.0))
        to_good = float(loss.get("p_bad_to_good", 0.0))
        if self.ge_state == "good":
            if self.rng.random() < to_bad:
                self.ge_state = "bad"
        elif self.rng.random() < to_good:
            self.ge_state = "good"
        if self.ge_state == "good":
            rate = float(loss.get("loss_in_good", 0.0))
        else:
            rate = float(loss.get("loss_in_bad", 1.0))
        return self.rng.random() < rate

    def corrupt(self, data: bytes) -> bytes:
        """Flip a few bits: the MAC of the frame must catch it (SEC-01)."""
        spec = self.impair.get("corrupt")
        if not spec or self.rng.random() >= float(spec.get("rate", 0.0)):
            return data
        if not data:
            return data
        buffer = bytearray(data)
        count = min(int(spec.get("bytes_max", 1)), len(buffer))
        for _ in range(count):
            index = self.rng.randrange(len(buffer))
            buffer[index] ^= 1 << self.rng.randrange(8)
        self.corrupted += 1
        return bytes(buffer)

    def truncate(self, data: bytes) -> bytes:
        """Cut the datagram: the frame must be rejected, not applied."""
        spec = self.impair.get("truncate")
        if not spec or self.rng.random() >= float(spec.get("rate", 0.0)):
            return data
        keep_min = int(spec.get("keep_min", 1))
        if len(data) <= keep_min + 1:
            return data
        cut = self.rng.randrange(keep_min, len(data))
        self.truncated += 1
        return data[:cut]

    def delay_seconds(self) -> float:
        """Fixed delay plus jitter, drawn from the seeded generator."""
        spec = self.impair.get("delay")
        if not spec:
            return 0.0
        fixed = float(spec.get("fixed_ms", 0.0))
        jitter = float(spec.get("jitter_ms", 0.0))
        extra = self.rng.uniform(-jitter, jitter) if jitter > 0 else 0.0
        return max(0.0, (fixed + extra) / 1000.0)

    def stats(self) -> Dict[str, Any]:
        return {
            "rx": self.rx, "tx": self.tx, "dropped": self.dropped,
            "duplicated": self.duplicated, "corrupted": self.corrupted,
            "truncated": self.truncated, "reordered": self.reordered,
            "delayed": self.delayed, "bytes_in": self.bytes_in,
            "bytes_out": self.bytes_out, "destination": "%s:%d" % self.dest,
        }


class Relay:
    """The event loop: sockets in, impaired datagrams out."""

    def __init__(self, links: List[Link], host: str) -> None:
        self.links = links
        self.sel = selectors.DefaultSelector()
        self.out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.out.bind((host, 0))
        self.queue: List[Tuple[float, int, Link, bytes]] = []
        self.seq = 0
        for link in links:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind((host, link.alias))
            sock.setblocking(False)
            self.sel.register(sock, selectors.EVENT_READ, link)

    def close(self) -> None:
        for key in list(self.sel.get_map().values()):
            self.sel.unregister(key.fileobj)
            key.fileobj.close()
        self.sel.close()
        self.out.close()

    def schedule(self, link: Link, payload: bytes, delay_s: float) -> None:
        """Queue a datagram for release at now + delay."""
        self.seq += 1
        if delay_s > 0:
            link.delayed += 1
        self.queue.append((time.monotonic() + delay_s, self.seq, link, payload,
                           delay_s))

    def release(self, link: Link, payload: bytes) -> None:
        """A packet that survived the losses: it takes the delay."""
        self.schedule(link, payload, link.delay_seconds())

    def on_packet(self, link: Link, data: bytes) -> None:
        """The impairment pipeline, in a fixed and published order."""
        link.rx += 1
        link.bytes_in += len(data)
        data = link.corrupt(data)
        data = link.truncate(data)
        if link.is_lost():
            link.dropped += 1
            return
        spec = link.impair.get("duplicate")
        if spec and link.rng.random() < float(spec.get("rate", 0.0)):
            link.duplicated += 1
            copy_delay = float(spec.get("delay_ms", 5.0)) / 1000.0
            self.schedule(link, data, copy_delay)
        reorder = link.impair.get("reorder")
        if reorder and int(reorder.get("depth", 0)) > 0:
            link.buffer.append((time.monotonic(), data))
            if len(link.buffer) >= int(reorder.get("depth", 1)):
                self.pop_buffer(link)
            return
        self.release(link, data)

    def pop_buffer(self, link: Link) -> None:
        """Release one buffered packet, at random: that is the reordering."""
        index = link.rng.randrange(len(link.buffer))
        _, payload = link.buffer.pop(index)
        link.reordered += 1
        self.release(link, payload)

    def flush_buffers(self, now: float) -> None:
        """Release the packets held longer than hold_ms, even without traffic."""
        for link in self.links:
            spec = link.impair.get("reorder") or {}
            hold = float(spec.get("hold_ms", 50.0)) / 1000.0
            while link.buffer and now - link.buffer[0][0] >= hold:
                self.pop_buffer(link)

    def pump(self) -> None:
        """Send every packet whose release time is due."""
        now = time.monotonic()
        due = [item for item in self.queue if item[0] <= now]
        if not due:
            return
        self.queue = [item for item in self.queue if item[0] > now]
        for _, _, link, payload, _delay in sorted(
                due, key=lambda item: (item[0], item[1])):
            try:
                self.out.sendto(payload, link.dest)
            except OSError:
                continue
            link.tx += 1
            link.bytes_out += len(payload)

    def next_timeout(self) -> float:
        """How long we may sleep: the next release, the next flush, or 50 ms."""
        now = time.monotonic()
        candidates = [0.05]
        if self.queue:
            candidates.append(max(0.0, min(item[0] for item in self.queue) - now))
        for link in self.links:
            if link.buffer:
                candidates.append(max(0.0, link.buffer[0][0] + 0.05 - now))
        return max(0.0, min(candidates))

    def run(self, stop_at: Optional[float]) -> None:
        """The loop: read, impair, schedule, send."""
        while stop_at is None or time.monotonic() < stop_at:
            for key, _ in self.sel.select(self.next_timeout()):
                link = key.data
                try:
                    data, _addr = key.fileobj.recvfrom(RECV_SIZE)
                except OSError:
                    continue
                self.on_packet(link, data)
            self.flush_buffers(time.monotonic())
            self.pump()


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Relais impaired UDP relay (M38)")
    parser.add_argument("--profiles", required=True, help="profiles.json")
    parser.add_argument("--profile", required=True, help="profile name")
    parser.add_argument("--seed", type=int, default=None, help="override the seed")
    parser.add_argument("--link", action="append", default=[], help="DIR=ALIAS:HOST:PORT")
    parser.add_argument("--stats", default="", help="where to write the counters")
    parser.add_argument("--ready", default="", help="file created once bound")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds (0 = forever)")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--selftest", action="store_true",
                        help="no socket traffic: feed a fixed datagram sequence "
                             "through the pipeline twice and compare the digests")
    return parser.parse_args(argv)


def selftest_digest(profile: Dict[str, Any], seed: int) -> str:
    """One pass of a fixed synthetic sequence through the real pipeline.

    No socket is ever sent to: the digest covers every decision (payloads
    released with their drawn delay, packets still buffered, per-direction
    counters). Two passes with the same seed MUST give the same digest.
    """
    impair = resolve_impair(profile)
    links = [Link(name, 0, DEFAULT_HOST, 9, impair[name], make_rng(seed, name))
             for name in sorted(impair)]
    relay = Relay(links, DEFAULT_HOST)
    try:
        for i in range(300):
            payload = bytes([i % 251]) * (20 + i % 30)
            relay.on_packet(links[i % len(links)], payload)
        digest = hashlib.sha256()
        for _, seq, link, payload, delay_s in sorted(relay.queue,
                                                     key=lambda it: it[1]):
            digest.update(("%s|%d|%.9f|" % (link.name, seq, delay_s)).encode())
            digest.update(hashlib.sha1(payload).digest())
        for link in links:
            for _, payload in link.buffer:
                digest.update(("%s|buf|" % link.name).encode())
                digest.update(hashlib.sha1(payload).digest())
            digest.update(json.dumps(link.stats(), sort_keys=True).encode())
        return digest.hexdigest()
    finally:
        relay.close()


def selftest(profile: Dict[str, Any], seed: int) -> int:
    first = selftest_digest(profile, seed)
    second = selftest_digest(profile, seed)
    other = selftest_digest(profile, seed + 1)
    print("selftest profile=%s seed=%d" % (profile.get("name"), seed))
    print("  pass 1: %s" % first)
    print("  pass 2: %s" % second)
    print("  seed+1: %s" % other)
    if first != second:
        print("SELFTEST-ECHEC: same seed, different decisions")
        return 1
    if first == other:
        print("SELFTEST-ECHEC: the seed does not change the decisions")
        return 1
    print("SELFTEST-OK: decisions are a pure function of (seed, profile, direction)")
    return 0


def write_stats(path: str, profile: Dict[str, Any], seed: int,
                links: List[Link]) -> None:
    """The counters of the relay: the proof that the degradation happened.

    Written atomically (tmp file then rename): a reader never sees a half
    written document, even if this process dies mid-write.
    """
    counters = {link.name: link.stats() for link in links}
    totals = {key: sum(c[key] for c in counters.values())
              for key in ("rx", "tx", "dropped", "duplicated", "corrupted",
                          "truncated", "reordered", "delayed")}
    payload = {"profile": profile.get("name"), "seed": seed,
               "links": counters, "totals": totals}
    if path:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
        os.replace(tmp, path)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    profile = load_profile(args.profiles, args.profile)
    impair = resolve_impair(profile)
    seed = args.seed if args.seed is not None else int(profile.get("seed", 0))
    if args.selftest:
        return selftest(profile, seed)
    links = [Link(name, alias, host, port, impair.get(name, {}), make_rng(seed, name))
             for name, alias, host, port in (parse_link(s) for s in args.link)]
    relay = Relay(links, args.host)
    stopping = {"flag": False}

    def stop(_signum: int, _frame: Any) -> None:
        stopping["flag"] = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    if args.ready:
        with open(args.ready, "w", encoding="utf-8") as handle:
            handle.write("ready\n")
    stop_at = time.monotonic() + args.duration if args.duration > 0 else None
    try:
        while not stopping["flag"]:
            deadline = time.monotonic() + 0.5
            if stop_at is not None:
                deadline = min(deadline, stop_at)
            relay.run(deadline)
            if stop_at is not None and time.monotonic() >= stop_at:
                break
    finally:
        write_stats(args.stats, profile, seed, links)
        relay.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
