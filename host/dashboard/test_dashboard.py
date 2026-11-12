#!/usr/bin/env python3
"""Unit tests for host/dashboard/serve.py (OBS-06).

Uses a fake journal that grows while the server runs: the SSE stream must emit
the new records, /api/state must reflect the current state, the server must
never write into the journal directory, and it must shut down cleanly.
"""
from __future__ import annotations

import http.client
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import serve  # noqa: E402


def tick_line(i, value=0.1, holder="A", source="holder", lamp="off"):
    rec = {"kind": "tick", "tick": i, "ns": 0, "mandate": 1, "holder": holder,
           "source": source, "value": value, "pos": value * 3.0, "vel": 0.0,
           "setpoint": 0.0, "misses": 0, "lamp": lamp, "cause": "none",
           "certified": True, "certified_node": "B", "stop": False}
    return json.dumps(rec)


def twin_line(i, value=0.1):
    return json.dumps({"kind": "tick", "tick": i, "ns": 0, "value": value})


class ServerThread(object):
    """Runs build_server(...).httpd.serve_forever() and shuts it down."""

    def __init__(self, journal, twin=None, poll=0.05):
        self.server = serve.build_server(
            journal, twin=twin, host="127.0.0.1", port=0,
            static_dir=HERE, poll_interval=poll)
        self.port = self.server.httpd.server_address[1]
        self.t = threading.Thread(target=self.server.httpd.serve_forever,
                                  daemon=True)
        self.t.start()

    def stop(self):
        self.server.shutdown()
        self.t.join(timeout=5)


def http_get(port, path):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", path)
    resp = conn.getresponse()
    body = resp.read()
    conn.close()
    return resp.status, body


def sse_connect(port):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    conn.request("GET", "/api/stream")
    resp = conn.getresponse()
    if resp.status != 200:
        conn.close()
        raise AssertionError("SSE returned %d" % resp.status)
    return conn, resp


def sse_next(resp):
    """Read one full SSE event; returns (event, data) or None on close."""
    event = None
    data = None
    while True:
        line = resp.readline().decode("utf-8")
        if line == "":
            return None
        line = line.rstrip("\r\n")
        if line == "":
            if event is not None:
                return (event, data or "")
        elif line.startswith(":"):
            continue  # keepalive / comment
        elif line.startswith("event: "):
            event = line[len("event: "):]
        elif line.startswith("data: "):
            data = line[len("data: "):]


class TestDashboard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="relais-dash-")
        self.journal = os.path.join(self.tmp, "actuator.jsonl")
        self.twin = os.path.join(self.tmp, "twin.jsonl")
        head = (json.dumps({"kind": "header", "scenario": "s"}) + "\n")
        with open(self.journal, "w", encoding="utf-8") as fh:
            fh.write(head + tick_line(1, 0.1) + "\n" + tick_line(2, 0.2) + "\n")
        with open(self.twin, "w", encoding="utf-8") as fh:
            fh.write(twin_line(1, 0.1) + "\n" + twin_line(2, 0.21) + "\n")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def append(self, i, value, **kw):
        with open(self.journal, "a", encoding="utf-8") as fh:
            fh.write(tick_line(i, value, **kw) + "\n")
            fh.flush()

    def test_static_pages(self):
        srv = ServerThread(self.journal, self.twin)
        try:
            code, body = http_get(srv.port, "/")
            self.assertEqual(code, 200)
            self.assertIn(b"mandate", body)
            self.assertEqual(http_get(srv.port, "/app.js")[0], 200)
            self.assertEqual(http_get(srv.port, "/style.css")[0], 200)
            self.assertEqual(http_get(srv.port, "/nope")[0], 404)
        finally:
            srv.stop()

    def test_api_state(self):
        srv = ServerThread(self.journal, self.twin)
        try:
            time.sleep(0.25)
            code, body = http_get(srv.port, "/api/state")
            self.assertEqual(code, 200)
            state = json.loads(body)
            self.assertEqual(state["last"]["tick"], 2)
            self.assertEqual(state["last"]["holder"], "A")
            self.assertEqual(state["last"]["source"], "holder")
            self.assertAlmostEqual(state["last"]["twin_dev"], 0.01)
            self.append(3, 0.33, holder="B", source="shadow")
            deadline = time.time() + 5
            while time.time() < deadline:
                _, body = http_get(srv.port, "/api/state")
                if json.loads(body)["last"]["tick"] == 3:
                    break
                time.sleep(0.05)
            state = json.loads(http_get(srv.port, "/api/state")[1])
            self.assertEqual(state["last"]["tick"], 3)
            self.assertEqual(state["last"]["holder"], "B")
            self.assertEqual(state["last"]["source"], "shadow")
        finally:
            srv.stop()

    def test_sse_emits_new_records(self):
        srv = ServerThread(self.journal, self.twin)
        conn, resp = sse_connect(srv.port)
        try:
            self.assertEqual(sse_next(resp)[0], "state")
            self.append(4, 0.44, holder="A", source="testament", lamp="yellow")
            found = False
            deadline = time.time() + 8
            while time.time() < deadline and not found:
                ev = sse_next(resp)
                if ev and ev[0] == "record":
                    rec = json.loads(ev[1])
                    if rec.get("tick") == 4:
                        self.assertEqual(rec["source"], "testament")
                        found = True
        finally:
            conn.close()
            srv.stop()
        self.assertTrue(found)

    def test_read_only(self):
        before_files = sorted(os.listdir(self.tmp))
        with open(self.journal, "rb") as fh:
            before_bytes = fh.read()
        srv = ServerThread(self.journal, self.twin)
        try:
            time.sleep(0.35)
            http_get(srv.port, "/api/state")
        finally:
            srv.stop()
        self.assertEqual(sorted(os.listdir(self.tmp)), before_files)
        with open(self.journal, "rb") as fh:
            self.assertEqual(fh.read(), before_bytes)

    def test_shutdown(self):
        srv = ServerThread(self.journal, self.twin)
        port = srv.port
        self.assertEqual(http_get(port, "/api/state")[0], 200)
        srv.stop()
        try:
            http_get(port, "/api/state")
            self.fail("server still answered after shutdown")
        except (http.client.HTTPException, OSError, ConnectionRefusedError):
            pass


if __name__ == "__main__":
    unittest.main()