"""Relais -- the reduced machine of docs/tla/Relais.tla, in Python.

A literal transcription of the TLA+ model: same state, same actions, same
invariants.  It exists so that host/verify/tla_crosscheck.py has a second,
independent path to confront with TLC and with the C code.  The reduction is
deliberate and bounded (two machines, small horizon, small patience, a bounded
number of ticks): the numeric values, the plant, the CRC and the MAC are not
here, because no safety invariant talks about them.
"""

import os
import re
import sys
from collections import deque

NONE = "none"
NODES = ("A", "B")
EMPTY_FRAME = ("EMPTY", NONE, 0, 0)

# Positions of the fields in the state tuple (the order of Relais.tla).
(TICK, MANDATE, HOLDER, MISSES, CERTIFIED, CERTNODE, STREAK, TBASE, TLEN,
 TRUN, SOURCE, LAMP, CESSPEND, CHANGES, APPFROM, APPSET, APPTICK, APPMAND,
 MANDHOLDERS, LASTMAND, LASTHOLDER, ALIVE, FROZEN, ROLE, BELIEF, SYNCED,
 PENDING, UPLINK, DOWNLINK, INBOX, LOSSES) = range(31)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JAR_CANDIDATES = (
    os.environ.get("TLA2TOOLS_JAR", ""),
    os.path.expanduser("~/.local/share/tla2tools/tla2tools.jar"),
    "/usr/local/share/tla2tools.jar",
    os.path.join(REPO, "tla2tools.jar"),
)


class Config(object):
    """The constants of the model, the same names as in Relais.tla."""

    def __init__(self, max_tick=2, max_mandate=2, n_horizon=1, patience=1,
                 certify_ticks=1):
        self.max_tick = max_tick
        self.max_mandate = max_mandate
        self.n_horizon = n_horizon
        self.patience = patience
        self.certify_ticks = certify_ticks

    def as_dict(self):
        return {"MaxTick": self.max_tick, "MaxMandate": self.max_mandate,
                "N": self.n_horizon, "P": self.patience,
                "CertifyTicks": self.certify_ticks}


def cap(value, ceiling):
    return ceiling if value > ceiling else value


def release(certified, certified_node, holder, mandate, cfg):
    """Relais.tla Release(): vacant, or straight to the certified second."""
    if certified and certified_node != NONE and certified_node != holder:
        return (certified_node, mandate + 1, False, NONE)
    return (NONE, mandate, False, NONE)


def init_state(cfg):
    return (
        0, 0, NONE, 0, False, NONE, 0, 0, 0, 0, "none", "off", False, 0,
        NONE, frozenset(), 0, 0, tuple(frozenset() for _ in range(cfg.max_mandate + 1)),
        0, NONE,
        (True, True), (False, False), ("none", "none"), (0, 0), (False, False),
        (EMPTY_FRAME, EMPTY_FRAME), (True, True), (True, True), frozenset(), 0,
    )


def apply_tick(st, cfg):
    """The actuator beat: scan, count the miss, revoke, grant, pick, certify."""
    if st[TICK] >= cfg.max_tick:
        return None
    tick = st[TICK] + 1
    if st[CESSPEND]:
        holder, mandate, certified, cnode = release(
            st[CERTIFIED], st[CERTNODE], st[HOLDER], st[MANDATE], cfg)
    else:
        holder, mandate, certified, cnode = st[HOLDER], st[MANDATE], st[CERTIFIED], st[CERTNODE]
    inbox = st[INBOX]
    has_order = any(f[0] == "ORDRE" and f[1] == holder and f[2] == mandate and f[3] == tick
                    for f in inbox)
    has_shadow = any(f[0] == "OMBRE" and f[1] == cnode and f[3] == tick for f in inbox)
    demandes = [f for f in inbox if f[0] == "DEMANDE"]
    has_cession = any(f[0] == "CESSION" and f[1] == holder and f[2] == mandate for f in inbox)
    misses = 0 if has_order else cap(st[MISSES] + 1, cfg.patience + 1)
    if holder != NONE and misses > cfg.patience:
        holder, mandate, certified, cnode = release(certified, cnode, holder, mandate, cfg)
    if holder == NONE and demandes and mandate < cfg.max_mandate:
        demander = "A" if any(f[1] == "A" for f in demandes) else "B"
        holder, mandate, certified, cnode = demander, mandate + 1, False, NONE
    source = pick_source(st, cfg, tick, holder, mandate, certified, cnode, has_order, has_shadow)
    streak, certified, cnode = certify(st, cfg, tick, holder, certified, cnode,
                                        has_order, inbox)
    will_len = cfg.n_horizon if source == "holder" else st[TLEN]
    will_base = tick + 1 if source == "holder" else st[TBASE]
    will_run = cap(st[TRUN] + 1, cfg.n_horizon) if source == "testament" else 0
    applied_from = holder if source == "holder" else (cnode if source == "shadow" else NONE)
    return build_state(st, cfg, tick, holder, mandate, misses, certified, cnode, streak,
                       will_base, will_len, will_run, source, has_cession,
                       applied_from, tick, mandate)


def pick_source(st, cfg, tick, holder, mandate, certified, cnode, has_order, has_shadow):
    """Spec 4.4: order, then shadow of the certified second, then will, then law."""
    if has_order:
        return "holder"
    if certified and has_shadow:
        return "shadow"
    if st[TLEN] > 0 and tick >= st[TBASE] and tick < st[TBASE] + st[TLEN]:
        return "testament"
    return "fallback"


def certify(st, cfg, tick, holder, certified, cnode, has_order, inbox):
    """REP-05: the second is certified after agreeing ticks."""
    if holder == NONE:
        return 0, False, NONE
    agrees = has_order and any(f[0] == "OMBRE" and f[1] == (NODES[1] if holder == "A" else "A")
                               and f[3] == tick for f in inbox)
    streak = cap(st[STREAK] + 1, cfg.certify_ticks) if agrees else 0
    if streak >= cfg.certify_ticks:
        return streak, True, (NODES[1] if holder == "A" else "A")
    if holder == NONE:
        return streak, False, NONE
    return streak, certified, cnode


def build_state(st, cfg, tick, holder, mandate, misses, certified, cnode, streak,
                will_base, will_len, will_run, source, has_cession, applied_from,
                applied_tick, applied_mandate):
    """Assemble the next state and publish the STATUT (MAN-07)."""
    holders = list(st[MANDHOLDERS])
    if applied_mandate > st[MANDATE] and applied_mandate <= cfg.max_mandate:
        holders[applied_mandate] = holders[applied_mandate] | {holder}
    roles, beliefs, synceds = [], [], []
    for i in range(2):
        hears = st[ALIVE][i] and not st[FROZEN][i] and st[DOWNLINK][i]
        if hears:
            roles.append("holder" if NODES[i] == holder else
                         ("none" if holder == NONE else "second"))
            beliefs.append(applied_mandate)
        else:
            roles.append(st[ROLE][i])
            beliefs.append(st[BELIEF][i])
        synceds.append(hears)
    return (
        tick, applied_mandate, holder, misses, certified, cnode, streak,
        will_base, will_len, will_run, source, lamp_of(source, holder, certified, st),
        has_cession, 1 if applied_mandate > st[MANDATE] else 0,
        applied_from, frozenset([applied_from]) - {NONE}, applied_tick,
        applied_mandate, tuple(holders), applied_mandate, holder,
        st[ALIVE], st[FROZEN], tuple(roles), tuple(beliefs), tuple(synceds),
        (EMPTY_FRAME, EMPTY_FRAME), st[UPLINK], st[DOWNLINK], frozenset(), 0,
    )


def lamp_of(source, holder, certified, st):
    """DEG-01: the lamp follows the cause."""
    if source == "fallback":
        return "red"
    if source != "holder":
        return "yellow"
    if holder == NONE:
        return "yellow"
    peer = 1 if holder == "A" else 0
    if not st[ALIVE][peer] or st[FROZEN][peer]:
        return "yellow"
    if not certified:
        return "yellow"
    return "off"


def send_frames(st, i):
    """The frames machine i may put in flight this period."""
    node = NODES[i]
    if not (st[ALIVE][i] and not st[FROZEN][i] and st[UPLINK][i]):
        return []
    if st[PENDING][i][0] != "EMPTY":
        return []
    tick = st[TICK] + 1
    frames = []
    if st[ROLE][i] == "holder" and st[HOLDER] == node:
        if st[BELIEF][i] == st[MANDATE]:
            frames.append(("ORDRE", node, st[BELIEF][i], tick))
        frames.append(("CESSION", node, st[MANDATE], tick))
    elif st[ROLE][i] == "second":
        frames.append(("OMBRE", node, st[MANDATE], tick))
    elif st[ROLE][i] == "none":
        frames.append(("DEMANDE", node, st[MANDATE], tick))
    return frames


def machine_actions(st, cfg, i):
    """Every action of machine i: send, deliver, lose, cut, kill, freeze."""
    out = []
    other = 1 - i
    pending = st[PENDING][i]
    for frame in send_frames(st, i):
        out.append(with_pending(st, replace_at(st[PENDING], i, frame)))
    if pending[0] != "EMPTY":
        cleared = replace_at(st[PENDING], i, EMPTY_FRAME)
        if st[UPLINK][i]:
            out.append(with_inbox(with_pending(st, cleared), st[INBOX] | {pending}))
        if st[LOSSES] < cfg.patience:
            lost = with_pending(st, cleared)
            out.append(lost[:LOSSES] + (st[LOSSES] + 1,))
    if st[UPLINK][i] or st[DOWNLINK][i]:
        out.append(with_links(st, replace_at(st[UPLINK], i, False),
                              replace_at(st[DOWNLINK], i, False)))
    if not st[UPLINK][i] or not st[DOWNLINK][i]:
        out.append(with_links(st, replace_at(st[UPLINK], i, True),
                              replace_at(st[DOWNLINK], i, True)))
    if st[ALIVE][i] and st[ALIVE][other] and not st[FROZEN][other]:
        out.append(with_alive(st, replace_at(st[ALIVE], i, False)))
    if not st[FROZEN][i] and st[ALIVE][other] and not st[FROZEN][other]:
        out.append(with_frozen(st, replace_at(st[FROZEN], i, True)))
    if st[FROZEN][i]:
        out.append(with_frozen(st, replace_at(st[FROZEN], i, False)))
    return out


def successors(st, cfg):
    """Relais.tla Next(): every action that is enabled in this state."""
    out = []
    nxt = apply_tick(st, cfg)
    if nxt is not None:
        out.append(nxt)
    for i in range(2):
        out.extend(machine_actions(st, cfg, i))
    return out


def replace_at(values, index, value):
    """Functional update of one slot of a per-machine tuple."""
    out = list(values)
    out[index] = value
    return tuple(out)


def with_pending(st, pending):
    return st[:PENDING] + (pending,) + st[PENDING + 1:]


def with_inbox(st, inbox):
    return st[:INBOX] + (inbox,) + st[INBOX + 1:]


def with_links(st, uplink, downlink):
    return st[:UPLINK] + (uplink, downlink) + st[UPLINK + 2:]


def with_alive(st, alive):
    return st[:ALIVE] + (alive,) + st[ALIVE + 1:]


def with_frozen(st, frozen):
    return st[:FROZEN] + (frozen,) + st[FROZEN + 1:]


def check_invariants(st, cfg):
    """The invariants of Relais.tla, checked on one state."""
    bad = []
    if st[TICK] > 0:
        if st[SOURCE] not in ("holder", "shadow", "testament", "fallback"):
            bad.append("I1")
        if st[SOURCE] == "holder" and st[HOLDER] == NONE:
            bad.append("I1")
        if st[SOURCE] == "shadow" and not (st[CERTIFIED] and st[CERTNODE] != NONE):
            bad.append("I1")
        if st[MANDATE] < st[LASTMAND]:
            bad.append("I2")
        if st[SOURCE] == "holder" and (st[APPFROM] != st[HOLDER] or
                                       st[APPTICK] != st[TICK] or
                                       st[APPMAND] != st[MANDATE]):
            bad.append("I3")
        if st[SOURCE] == "testament":
            if st[TLEN] > cfg.n_horizon or st[TRUN] > st[TLEN]:
                bad.append("I6")
            if st[TICK] >= st[TBASE] + st[TLEN]:
                bad.append("I6")
        if st[SOURCE] != "holder" and st[LAMP] == "off":
            bad.append("DEG-01")
    if st[CHANGES] > 1:
        bad.append("I2")
    for owners in st[MANDHOLDERS]:
        if len(owners) > 1:
            bad.append("I2")
    if len(st[APPSET]) > 1:
        bad.append("Safety")
    return sorted(set(bad))


def explore(cfg, max_states=400000):
    """Breadth-first exploration of the reduced machine (the 'model' path)."""
    start = init_state(cfg)
    seen = {start}
    queue = deque([start])
    pairs = set()
    sources = set()
    violations = []
    while queue:
        st = queue.popleft()
        pairs.add((st[MANDATE], st[HOLDER]))
        if st[TICK] > 0:            # the C journal has no tick 0
            sources.add(st[SOURCE])
        for name in check_invariants(st, cfg):
            violations.append((name, st[TICK], st[MANDATE], st[HOLDER], st[SOURCE]))
        if len(seen) >= max_states:
            continue
        for nxt in successors(st, cfg):
            if nxt not in seen:
                seen.add(nxt)
                queue.append(nxt)
    return {"states": len(seen), "pairs": pairs, "sources": sources,
            "violations": violations, "truncated": len(seen) >= max_states}


