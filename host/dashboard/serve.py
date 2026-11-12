#!/usr/bin/env python3
"""Read-only live dashboard server for Relais journals (OBS-06, OBS-08).

It (a) serves a single self-contained page (index.html + app.js + style.css,
all local, no CDN), (b) tails a JSON-lines journal while it is being written
and pushes each new record over Server-Sent Events, (c) exposes /api/state
(last tick, holder, mandate, source, witness lamp, cause, position, command,
deviation from the fault-free twin), (d) reads only: every file is opened
with mode "r" and the server never writes into the bench directory.

The journal format is the one documented in CONTRACT.md ("Journal format
(authoritative)"): one JSON object per line, kinds "header", "tick", "event".

Run it with:
    python3 host/dashboard/serve.py --journal runs/r/actuator.jsonl \
        --twin runs/r/twin.jsonl --verify-log verify.log
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# OBS-08: the sources whose colours the dashboard must tell apart.
SOURCES = ("none", "holder", "shadow", "testament", "fallback")
# OBS-06: fields mirrored into /api/state even when absent from a record.
TICK_FIELDS = (
    "tick", "ns", "mandate", "holder", "source", "value", "pos", "vel",
    "setpoint", "misses", "lamp", "cause", "certified", "certified_node",
    "stop",
)


class Tailer(object):
    """Follows one JSON-lines file; yields each new object exactly once."""

    def __init__(self, path, kind="record"):
        self.path = path
        self.kind = kind
        self.offset = 0
        self._buf = ""

    def reset(self):
        """Drop buffered state (used after a truncation/rotation)."""
        self.offset = 0
        self._buf = ""

    def _read_new_lines(self):
        """Return the list of complete lines appended since the last poll."""
        if self.path is None:
            return []
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return []
        if size < self.offset:
            # File rotated or truncated: restart from the beginning.
            self.reset()
        with open(self.path, "r", encoding="utf-8", errors="replace") as fh:
            fh.seek(self.offset)
            chunk = fh.read()
        self.offset += len(chunk)
        self._buf += chunk
        lines = self._buf.split("\n")
        # The last element is the (possibly empty) trailing partial line.
        self._buf = lines.pop()
        return [ln for ln in lines if ln.strip()]

    def poll(self):
        """Parse the new complete lines into dict objects (tolerant)."""
        records = []
        for line in self._read_new_lines():
            try:
                obj = json.loads(line)
            except ValueError:
                continue  # one malformed line is counted by the exporters,
                # not by the live view
            if isinstance(obj, dict):
                obj.setdefault("__stream", self.kind)
                records.append(obj)
        return records


class Broadcaster(object):
    """Delivers every published record to each subscribed SSE queue."""

    def __init__(self):
        self._subs = []
        self._lock = threading.Lock()

    def subscribe(self):
        q = queue.Queue()
        with self._lock:
            self._subs.append(q)
        return q

    def unsubscribe(self, q):
        with self._lock:
            if q in self._subs:
                self._subs.remove(q)

    def publish(self, payload):
        with self._lock:
            subs = list(self._subs)
        for q in subs:
            q.put(payload)

    def close(self):
        with self._lock:
            subs = list(self._subs)
            self._subs.clear()
        for q in subs:
            q.put(None)


class State(object):
    """Thread-safe view of the journal tail: last tick state + recent events."""

    def __init__(self):
        self._lock = threading.Lock()
        self.header = {}
        self.last = {}
        self.events = []           # recent events, oldest first
        self.ticks_seen = 0
        self.events_seen = 0
        self.twin = {}             # tick -> twin applied value
        self.worst_dev = 0.0
        self.violations = 0
        self.verifier = None

    def on_header(self, rec):
        with self._lock:
            self.header = rec

    def on_tick(self, rec):
        with self._lock:
            self.ticks_seen += 1
            self.last = rec
            tick = rec.get("tick")
            value = rec.get("value")
            if tick in self.twin and isinstance(value, (int, float)):
                dev = abs(float(value) - float(self.twin[tick]))
                rec["twin_dev"] = round(dev, 6)
                self.worst_dev = max(self.worst_dev, dev)
            else:
                rec["twin_dev"] = None

    def on_twin(self, rec):
        with self._lock:
            self.twin[int(rec.get("tick", 0))] = rec.get("value")

    def on_event(self, rec):
        with self._lock:
            self.events_seen += 1
            self.events.append(rec)
            if len(self.events) > 60:
                self.events = self.events[-60:]

    def on_violation(self, count):
        with self._lock:
            self.violations = count

    def snapshot(self):
        """Return the JSON-safe current state (OBS-06)."""
        with self._lock:
            return {
                "ok": True,
                "scenario": self.header.get("scenario", ""),
                "params": self.header.get("params", {}),
                "ticks_seen": self.ticks_seen,
                "events_seen": self.events_seen,
                "violations": self.violations,
                "verifier": self.verifier,
                "worst_twin_dev": round(self.worst_dev, 6),
                "last": self.last,
                "events": self.events,
            }


def _feed(state, records):
    """Feed a batch of records into the state and the broadcaster.

    Twin ticks must be ingested before actuator ticks so the deviation of the
    current tick against the fault-free twin is available right away (both
    journals use `"kind": "tick"` on the wire; the __stream marker tells them
    apart).
    """
    broadcaster = _feed.broadcaster
    twin_recs = [r for r in records if r.get("__stream") == "twin"]
    act_recs = [r for r in records if r.get("__stream") != "twin"]
    for rec in twin_recs:
        rec.pop("__stream", None)
        if rec.get("tick") is not None:
            state.on_twin(rec)
    out = []
    for rec in act_recs:
        rec.pop("__stream", None)
        kind = rec.get("kind")
        if kind == "header":
            state.on_header(rec)
        elif kind == "tick":
            state.on_tick(rec)
            out.append(("record", rec))
        elif kind == "event":
            state.on_event(rec)
            out.append(("record", rec))
    for item in out:
        broadcaster.publish(item)


def tail_loop(stop, interval, state, broadcaster, tailers, verify_log):
    """Background thread: poll the tailers, update state, broadcast records."""
    _feed.broadcaster = broadcaster

    def verifier_violations(path):
        # OBS-06: report how many invariants a live verifier has flagged.
        if not path or not os.path.isfile(path):
            return None
        count = 0
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    if " violated at tick " in line:
                        count += 1
        except OSError:
            return None
        return count

    while not stop.is_set():
        records = []
        for t in tailers:
            records.extend(t.poll())
        if records:
            _feed(state, records)
        verifier = (verify_log if verify_log and os.path.isfile(verify_log)
                    else None)
        if state.verifier != verifier:
            with state._lock:
                state.verifier = verifier
        if verifier:
            count = verifier_violations(verify_log)
            if count is not None:
                state.on_violation(count)
        stop.wait(interval)


def _static_file(static_dir, rel):
    """Read one static asset from the dashboard directory, or None."""
    path = os.path.join(static_dir, rel)
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as fh:
        return fh.read()


class DashboardHandler(BaseHTTPRequestHandler):
    """Handles /, /app.js, /style.css, /api/state and /api/stream."""

    server_version = "relais-dashboard/1"
    STATE = None      # injected by build_server
    BROADCASTER = None
    STATIC_DIR = None

    def log_message(self, fmt, *args):
        pass  # quiet: the dashboard must not spam the bench console

    def _send(self, code, ctype, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        if extra:
            for k, v in extra:
                self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _serve_static(self, rel, ctype):
        body = _static_file(self.STATIC_DIR, rel)
        if body is None:
            return self._send(404, "text/plain", b"not found\n")
        self._send(200, ctype, body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            self._serve_static("index.html", "text/html; charset=utf-8")
        elif path == "/app.js":
            self._serve_static("app.js", "text/javascript; charset=utf-8")
        elif path == "/style.css":
            self._serve_static("style.css", "text/css; charset=utf-8")
        elif path == "/api/state":
            body = json.dumps(self.STATE.snapshot()).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
        elif path == "/api/stream":
            self._stream()
        else:
            self._send(404, "text/plain", b"not found\n")

    def _stream(self):
        """Push the current state, then every new record, as SSE."""
        q = self.BROADCASTER.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        wfile = self.wfile

        def emit(event, data):
            wfile.write(("event: %s\n" % event).encode("utf-8"))
            wfile.write(("data: %s\n\n" % data).encode("utf-8"))
            wfile.flush()

        try:
            emit("state", json.dumps(self.STATE.snapshot()))
            last_keepalive = time.time()
            while True:
                try:
                    item = q.get(timeout=0.5)
                except queue.Empty:
                    item = None
                if item is None:
                    break
                if time.time() - last_keepalive > 15.0:
                    wfile.write(b": keepalive\n\n")
                    wfile.flush()
                    last_keepalive = time.time()
                if item[0] == "record":
                    emit("record", json.dumps(item[1]))
                elif item[0] == "state":
                    emit("state", json.dumps(item[1]))
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.BROADCASTER.unsubscribe(q)


def build_server(journal, twin=None, verify_log=None, host="127.0.0.1",
                 port=8317, static_dir=None, poll_interval=0.25):
    """Build and start the tail thread; return the ready server and its parts."""
    if static_dir is None:
        static_dir = os.path.dirname(os.path.abspath(__file__))
    state = State()
    broadcaster = Broadcaster()
    tailers = [Tailer(journal, "actuator")]
    if twin:
        tailers.append(Tailer(twin, "twin"))
    stop = threading.Event()
    thread = threading.Thread(
        target=tail_loop,
        args=(stop, poll_interval, state, broadcaster, tailers, verify_log),
        daemon=True,
    )
    handler = DashboardHandler
    handler.STATE = state
    handler.BROADCASTER = broadcaster
    handler.STATIC_DIR = static_dir
    httpd = ThreadingHTTPServer((host, port), handler)
    httpd.daemon_threads = True
    thread.start()

    # Small helper object so tests (and main) can shut the server down cleanly.
    class _Server(object):
        pass

    box = _Server()
    box.httpd = httpd
    box.state = state
    box.broadcaster = broadcaster
    box._stop = stop

    def shutdown():
        stop.set()
        broadcaster.close()
        httpd.shutdown()
        httpd.server_close()

    box.shutdown = shutdown
    return box


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Read-only live dashboard for a running Relais journal.")
    parser.add_argument("--journal", required=True,
                        help="path to the actuator JSON-lines journal")
    parser.add_argument("--twin", default=None,
                        help="path to the fault-free twin journal")
    parser.add_argument("--verify-log", default=None,
                        help="path to a live verifier log (violation counter)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8317)
    parser.add_argument("--poll", type=float, default=0.25,
                        help="tail poll interval in seconds")
    args = parser.parse_args(argv)
    if args.port <= 1024:
        parser.error("port must be > 1024 (no administrator privileges)")
    server = build_server(args.journal, twin=args.twin,
                          verify_log=args.verify_log, host=args.host,
                          port=args.port, poll_interval=args.poll)
    url = "http://%s:%d/" % (args.host, args.port)
    print("relais-dashboard: serving %s (journal %s)" % (url, args.journal))
    try:
        server.httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())