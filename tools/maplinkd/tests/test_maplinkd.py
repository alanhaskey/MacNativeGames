#!/usr/bin/env python3
"""Contract tests for maplinkd. No game and no network: fixtures are written to a temp folder.

Run: python3 -m unittest discover -s tools/maplinkd/tests -v
"""
import importlib.util
import json
import os
import socket
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("maplinkd", HERE.parent / "maplinkd.py")
maplinkd = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(maplinkd)

SCHEMA = "macnativegames.maplink/1"


def sample(ok=True, age_ms=0, **extra):
    payload = {"schema": SCHEMA, "ok": ok, "game": "totk", "version": "1.4.3",
               "frame": 4242, "t": int(time.time() * 1000) - age_ms}
    if ok:
        payload.update({"x": 1234.5, "y": 123.4, "z": -567.8, "yaw": 90.0,
                        "headingSource": "actor", "layer": None})
    payload.update(extra)
    return payload


class BridgeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.file = Path(self.tmp.name) / "maplink.json"
        self.producer = maplinkd.Producer(str(self.file), stale_ms=1000, disconnect_ms=10000, poll_seconds=0.02)
        self.producer.start()
        self.server = maplinkd.build_server(self.producer, "127.0.0.1", 0, maplinkd.DEFAULT_ORIGINS)
        self.thread = threading.Thread(target=maplinkd.serve, args=(self.producer, self.server), daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(timeout=2)
        self.producer.stop()
        self.tmp.cleanup()

    # ---- helpers ----
    def write(self, payload):
        raw = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        tmp = self.file.with_suffix(".tmp")
        tmp.write_bytes(raw)
        os.replace(tmp, self.file)

    def get(self, path, headers=None):
        request = urllib.request.Request(self.base + path, headers=headers or {})
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, dict(response.headers), json.loads(response.read())
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), json.loads(e.read())

    def wait_for(self, predicate, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            state = self.get("/v1/state")[2]
            if predicate(state):
                return state
            time.sleep(0.02)
        self.fail("state never matched; last = %s" % json.dumps(self.get("/v1/state")[2]))

    # ---- cases ----
    def test_missing_producer(self):
        state = self.wait_for(lambda s: s["status"] == "disconnected" and s.get("reason") == "producer-missing")
        self.assertFalse(state["ok"])
        self.assertIsNone(state["ageMs"])
        status, _, body = self.get("/v1/health")
        self.assertEqual(status, 503)
        self.assertFalse(body["ok"])

    def test_live_state(self):
        self.write(sample())
        state = self.wait_for(lambda s: s["status"] == "live")
        self.assertTrue(state["ok"])
        self.assertEqual(state["x"], 1234.5)
        self.assertEqual(state["y"], 123.4)
        self.assertEqual(state["z"], -567.8)
        self.assertEqual(state["yaw"], 90.0)
        self.assertLess(state["ageMs"], 1000)
        status, _, body = self.get("/v1/health")
        self.assertEqual(status, 200)
        self.assertTrue(body["ok"])

    def test_not_in_world(self):
        self.write(sample(ok=False))
        state = self.wait_for(lambda s: s["status"] == "not-in-world")
        self.assertFalse(state["ok"])
        self.assertIsNone(state["x"])

    def test_stale(self):
        self.write(sample(age_ms=5000))
        state = self.wait_for(lambda s: s["status"] == "stale")
        self.assertGreaterEqual(state["ageMs"], 1000)
        status, _, _ = self.get("/v1/health")
        self.assertEqual(status, 503)

    def test_disconnected_when_silent(self):
        self.write(sample(age_ms=20000))
        # The last known position is kept on purpose; status says the producer went silent.
        state = self.wait_for(lambda s: s["status"] == "disconnected" and s.get("reason") == "producer-silent")
        self.assertEqual(state["status"], "disconnected")

    def test_invalid_json(self):
        self.write(b"{not json")
        state = self.wait_for(lambda s: s["status"] == "disconnected" and "producer-invalid" in (s.get("reason") or ""))
        self.assertFalse(state["ok"])

    def test_wrong_schema(self):
        self.write({"schema": "other/2", "ok": True, "t": int(time.time() * 1000)})
        state = self.wait_for(lambda s: "producer-invalid" in (s.get("reason") or ""))
        self.assertFalse(state["ok"])

    def test_meta(self):
        status, headers, body = self.get("/v1/meta")
        self.assertEqual(status, 200)
        self.assertEqual(body["schema"], SCHEMA)
        self.assertEqual(body["rateHz"], 10)
        self.assertEqual(body["capabilities"], {"trail": False, "layer": False, "reverse": False})
        self.assertIn("no-store", headers.get("Cache-Control", ""))

    def test_cors_allowed_origin(self):
        status, headers, _ = self.get("/v1/state", headers={"Origin": "http://localhost:3000"})
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Access-Control-Allow-Origin"), "http://localhost:3000")

    def test_cors_unknown_origin(self):
        _, headers, _ = self.get("/v1/state", headers={"Origin": "http://evil.example"})
        self.assertIsNone(headers.get("Access-Control-Allow-Origin"))

    def test_unknown_endpoint(self):
        status, _, body = self.get("/v2/nope")
        self.assertEqual(status, 404)
        self.assertEqual(body["error"], "not-found")

    def test_sse_hello_and_state(self):
        self.write(sample())
        self.wait_for(lambda s: s["status"] == "live")   # the bridge must already have picked the file up
        connection = socket.create_connection(("127.0.0.1", self.server.server_address[1]), timeout=5)
        connection.settimeout(5)
        connection.sendall(b"GET /v1/stream HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n")
        buffer = b""
        deadline = time.time() + 5
        events = []
        while len(events) < 2 and time.time() < deadline:
            buffer += connection.recv(4096)
            while b"\n\n" in buffer:
                block, buffer = buffer.split(b"\n\n", 1)
                lines = [line for line in block.decode().replace("\r\n", "\n").split("\n") if line]
                name = next((line.split(": ", 1)[1] for line in lines if line.startswith("event: ")), None)
                data = next((line.split(": ", 1)[1] for line in lines if line.startswith("data: ")), None)
                if name and data:
                    events.append((name, json.loads(data)))
        connection.close()
        self.assertGreaterEqual(len(events), 2)
        self.assertEqual(events[0][0], "hello")
        self.assertEqual(events[1][0], "state")
        self.assertEqual(events[1][1]["status"], "live")


if __name__ == "__main__":
    unittest.main()
