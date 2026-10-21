#!/usr/bin/env python3
"""maplinkd: a small local HTTP/SSE bridge for the TotK map link.

Reads the position file the game writes (maplink.json, 10 Hz, atomic rename) and serves it to any local
project over one stable contract. Read-only, loopback only, Python standard library only.

Endpoints (see tools/maplinkd/README.md and docs/maplink-interfaces.md):

    GET /v1/meta     bridge version, rate, capabilities
    GET /v1/state    the current snapshot (always 200; status says what is going on)
    GET /v1/stream   Server-Sent Events: hello, state (per sample), status (on change), heartbeat
    GET /v1/health   200 while the producer is fresh, 503 otherwise

Status values: live, not-in-world, stale, disconnected.
"""
import argparse
import http.server
import json
import math
import os
import signal
import sys
import threading
import time

SCHEMA = "macnativegames.maplink/1"
BRIDGE_VERSION = "1.0.0"
RATE_HZ = 10
DEFAULT_FILE = os.path.expanduser("~/Library/Application Support/MacNativeGames/TOTK/maplink.json")
DEFAULT_ORIGINS = ("http://localhost:3000", "http://127.0.0.1:3000")

EMPTY_STATE = {"schema": SCHEMA, "ok": False, "inWorld": False, "status": "disconnected", "ageMs": None,
               "frame": None, "t": None, "game": None, "version": None,
               "x": None, "y": None, "z": None, "yaw": None, "headingSource": "unknown",
               "layer": None}


def now_ms():
    return int(time.time() * 1000)


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def read_producer(path, cache):
    """(error, data, cache): re-read only when the file identity changed. error is a short reason or None."""
    try:
        st = os.stat(path)
    except FileNotFoundError:
        return "producer-missing", None, None
    except OSError as e:
        return "producer-unreadable: %s" % e.strerror, None, None
    identity = (st.st_mtime_ns, st.st_size, st.st_ino)
    if cache is not None and cache["identity"] == identity:
        return cache["error"], cache["data"], cache
    try:
        with open(path, "rb") as f:
            raw = f.read()
        data = json.loads(raw.decode("utf-8"))
        if not isinstance(data, dict):
            raise ValueError("not an object")
        schema = data.get("schema")
        if not isinstance(schema, str) or not schema.startswith("macnativegames.maplink/"):
            raise ValueError("unknown schema")
        major = schema.rsplit("/", 1)[-1].split(".")[0]
        if major != "1":
            raise ValueError("unsupported schema major %s" % major)
        if not is_number(data.get("t")):
            raise ValueError("missing t")
        error = None
    except (OSError, ValueError, UnicodeDecodeError) as e:
        data, error = None, "producer-invalid: %s" % e
    return error, data, {"identity": identity, "error": error, "data": data}


def compute_state(path, cache, stale_ms, disconnect_ms):
    """The public state for this moment (status/ageMs are recomputed even when the file did not change)."""
    error, data, cache = read_producer(path, cache)
    state = dict(EMPTY_STATE)
    if data is None:
        state["reason"] = error or "producer-missing"
        return state, cache
    age = max(0, now_ms() - int(data["t"]))
    state.update(t=int(data["t"]), game=data.get("game"), version=data.get("version"),
                 frame=data.get("frame") if isinstance(data.get("frame"), int) else None,
                 layer=data.get("layer"), ageMs=age, inWorld=bool(data.get("inWorld")))
    if data.get("ok"):
        if not all(is_number(data.get(k)) for k in ("x", "y", "z")):
            state.update(reason="producer-invalid: position")
            return state, cache
        state.update(ok=True, x=data["x"], y=data["y"], z=data["z"],
                     yaw=data["yaw"] if is_number(data.get("yaw")) else None,
                     headingSource=data.get("headingSource") or ("actor" if data.get("rot") else "unknown"))
        if age > disconnect_ms:
            state.update(status="disconnected", reason="producer-silent")
        elif age > stale_ms:
            state.update(status="stale")
        else:
            state.update(status="live")
    else:
        state.update(status="not-in-world")
        if age > disconnect_ms:
            state.update(status="disconnected", reason="producer-silent")
        elif age > stale_ms:
            state.update(status="stale")
    return state, cache


class Producer:
    """Polls the file and publishes snapshots to any number of SSE clients."""

    def __init__(self, path, stale_ms, disconnect_ms, poll_seconds=0.05):
        self.path = path
        self.stale_ms = stale_ms
        self.disconnect_ms = disconnect_ms
        self.poll_seconds = poll_seconds
        self.lock = threading.Lock()
        self.changed = threading.Condition(self.lock)
        self.state = dict(EMPTY_STATE, reason="starting")
        self.seq = 0
        self.cache = None
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.state, self.cache = compute_state(self.path, None, self.stale_ms, self.disconnect_ms)
        self.thread = threading.Thread(target=self._run, name="maplinkd-producer", daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1)

    def _run(self):
        while self.running:
            try:
                state, self.cache = compute_state(self.path, self.cache, self.stale_ms, self.disconnect_ms)
            except Exception as e:   # never let a bad file kill the bridge
                state = dict(EMPTY_STATE, reason="bridge-error: %s" % e)
            self._publish(state)
            time.sleep(self.poll_seconds)

    def _publish(self, state):
        with self.lock:
            if state == self.state:
                return
            self.state = state
            self.seq += 1
            self.changed.notify_all()

    def snapshot(self):
        with self.lock:
            return dict(self.state), self.seq

    def wait(self, seen_seq, timeout):
        with self.lock:
            self.changed.wait_for(lambda: self.seq != seen_seq, timeout=timeout)
            return dict(self.state), self.seq


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "maplinkd/" + BRIDGE_VERSION
    protocol_version = "HTTP/1.1"
    producer = None
    allowed_origins = DEFAULT_ORIGINS

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def log_message(self, fmt, *args):
        if getattr(self.server, "verbose", False):
            sys.stderr.write("[maplinkd] %s %s\n" % (self.address_string(), fmt % args))

    def cors(self):
        origin = self.headers.get("Origin")
        if origin and origin in self.allowed_origins:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Accept")

    def body(self, code, payload):
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.cors()
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.send_header("Access-Control-Max-Age", "600")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/v1/meta":
            return self.body(200, {"schema": SCHEMA, "bridge": BRIDGE_VERSION, "rateHz": RATE_HZ,
                                   "producer": "file",
                                   "capabilities": {"trail": False, "layer": False, "reverse": False},
                                   "uptimeMs": int((time.time() - self.server.started_at) * 1000)})
        if path == "/v1/state":
            return self.body(200, self.producer.snapshot()[0])
        if path == "/v1/health":
            state = self.producer.snapshot()[0]
            if state.get("status") == "live":
                return self.body(200, {"ok": True, "status": "live"})
            return self.body(503, {"ok": False, "status": state.get("status"), "reason": state.get("reason")})
        if path == "/v1/stream":
            return self.stream()
        return self.body(404, {"error": "not-found",
                               "endpoints": ["/v1/meta", "/v1/state", "/v1/stream", "/v1/health"]})

    def stream(self):
        state, seq = self.producer.snapshot()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.cors()
        self.end_headers()
        try:
            self.close_connection = True   # this response never ends by itself
            self._event("hello", {"schema": SCHEMA, "bridge": BRIDGE_VERSION, "rateHz": RATE_HZ})
            self._event("state", state)
            last_status, last_seq = state.get("status"), seq
            last_ping = time.time()
            while True:
                state, seq = self.producer.wait(last_seq, timeout=1.0)
                if seq != last_seq:
                    last_seq = seq
                    if state.get("status") != last_status:
                        last_status = state.get("status")
                        self._event("status", {"status": last_status, "ageMs": state.get("ageMs"),
                                               "reason": state.get("reason")})
                    self._event("state", state)
                if time.time() - last_ping >= 15:
                    last_ping = time.time()
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
            return

    def _event(self, name, payload):
        data = json.dumps(payload, separators=(",", ":"))
        self.wfile.write(("event: %s\ndata: %s\n\n" % (name, data)).encode("utf-8"))
        self.wfile.flush()


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.started_at = time.time()
        self.verbose = False


def build_server(producer, host, port, allowed_origins, verbose=False):
    handler = type("BoundHandler", (Handler,), {"producer": producer, "allowed_origins": tuple(allowed_origins)})
    server = Server((host, port), handler)
    server.verbose = verbose
    return server


def serve(producer, server):
    try:
        server.serve_forever()
    finally:
        server.server_close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default=os.environ.get("TOTK_MAPLINK_FILE", DEFAULT_FILE),
                    help="position file written by the game (default: %(default)s)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=17862)
    ap.add_argument("--stale-ms", type=int, default=1000, help="age after which a sample is stale (default 1000)")
    ap.add_argument("--disconnect-ms", type=int, default=10000, help="age after which the producer counts as gone")
    ap.add_argument("--cors-origin", action="append", default=[],
                    help="browser origin allowed to read the API (repeatable; defaults to localhost/127.0.0.1:3000)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    origins = tuple(args.cors_origin) if args.cors_origin else DEFAULT_ORIGINS
    producer = Producer(os.path.expanduser(args.file), args.stale_ms, args.disconnect_ms)
    producer.start()
    server = build_server(producer, args.host, args.port, origins, args.verbose)
    host, port = server.server_address[0], server.server_address[1]
    print("[maplinkd] %s on http://%s:%d (file %s, stale %d ms, disconnect %d ms)"
          % (BRIDGE_VERSION, host, port, producer.path, args.stale_ms, args.disconnect_ms),
          file=sys.stderr, flush=True)

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    http_thread = threading.Thread(target=serve, args=(producer, server), name="maplinkd-http", daemon=True)
    http_thread.start()
    try:
        while not stop.wait(0.5):
            pass
    finally:
        server.shutdown()
        producer.stop()
        print("[maplinkd] stopped", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
