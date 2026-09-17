#!/usr/bin/env python3
"""Local Fabric API/dashboard. Run with --help; no daemon or root required."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict
from datetime import datetime, timezone
import difflib
import hashlib
import ipaddress
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
import os
from pathlib import Path
import re
import secrets
import socket
import sqlite3
import threading
import time
from urllib.parse import unquote, urlsplit, parse_qs

from control import ControlError, MAX_BODY, query
from errors import APIError
from discovery import discover, stat_fields
from logs import read_logs
from history import History, default_path
from telemetry import changes, health, switch_detail
from syspiper import Syspiper, add_arguments as syspiper_arguments, options as syspiper_options

STATIC = Path(__file__).parent / "static"
LOG = logging.getLogger("fabric")


def now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def parse_stats(text):
    # Keep uint64 counters/labels as strings; JavaScript numbers lose precision.
    values = {}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if not separator or not re.fullmatch(r"[a-zA-Z0-9_]+", key) or key in values:
            raise OSError("invalid stats response")
        values[key] = value
    if values.get("format") != "txt" or values.get("format_version") != "1":
        raise OSError("unsupported stats format")
    return values


def topology(endpoints):
    links = []
    switches = [e for e in endpoints if e["kind"] == "switch" and e["switch_socket"]]
    for endpoint in endpoints:
        if endpoint["kind"] == "switch" or not endpoint["switch_socket"]:
            continue
        matches = [e for e in switches if e["switch_socket"] == endpoint["switch_socket"] and
                   (not e["mount_namespace"] or not endpoint["mount_namespace"] or
                    e["mount_namespace"] == endpoint["mount_namespace"])]
        if len(matches) == 1:
            verified = bool(endpoint["mount_namespace"] and matches[0]["mount_namespace"])
            links.append({"source": endpoint["id"], "target": matches[0]["id"],
                          "port_id": endpoint["port_id"], "kind": "attachment",
                          "basis": "process arguments", "namespace_verified": verified})
    return links


class Fabric:
    def __init__(self, *, allow_write=False, interval=5, discover_fn=discover, history_path=None, syspiper=None):
        self.allow_write, self.interval = allow_write, interval
        self.discover = discover_fn
        self.endpoints = {}
        self.discovery_info = {"source": "procfs", "status": "pending"}
        self.action_lock = threading.Lock()
        self.mutex = threading.Lock()
        self.stop = threading.Event()
        self.wake = threading.Event()
        self.started_at = now()
        self.samples = {}
        self.baselines = {}
        self.thread = None
        self.history_store = History(history_path) if history_path else None
        self.history_error = None
        self.syspiper = Syspiper(history=self.history_store, **(syspiper or {}))

    def start(self):
        self.thread = threading.Thread(target=self._poll, name="fabric-poll", daemon=True)
        self.thread.start()
        self.syspiper.start()

    def close(self):
        self.stop.set()
        self.wake.set()
        if self.thread:
            self.thread.join()
        self.syspiper.close()
        if self.history_store:
            self.history_store.close()

    def control_query(self, endpoint, operation, body=""):
        if not endpoint.control:
            raise OSError("process has no --control-socket")
        if int(stat_fields(Path(f"/proc/{endpoint.pid}/stat"))[19]) != endpoint.start_ticks:
            raise OSError("process identity changed; refresh discovery")
        namespace = os.readlink("/proc/self/ns/mnt")
        if endpoint.mount_namespace and endpoint.mount_namespace != namespace:
            raise OSError("control socket is in a different mount namespace")
        return query(endpoint.control, operation, body, timeout=3,
                     expected_pid=endpoint.pid, expected_start_ticks=endpoint.start_ticks)

    def _sample(self, endpoint):
        try:
            if not endpoint.control:
                result = {"status": "process_only", "metrics": {}, "error": None}
            else:
                metrics = parse_stats(self.control_query(endpoint, "stats"))
                result = {"status": "reachable", "metrics": metrics, "error": None}
        except (OSError, ValueError, ControlError) as error:
            result = {"status": "unavailable", "metrics": {}, "error": str(error)}
        result["sampled_at"] = now()
        with self.mutex:
            tick = time.monotonic()
            baseline = self.baselines.pop(endpoint.id, None)
            elapsed = tick - baseline[0] if baseline else None
            previous = baseline[1] if baseline and elapsed <= max(15, self.interval * 3) else None
            result["changes"] = changes(result["metrics"], previous, elapsed if previous else None)
            if result["status"] == "reachable":
                self.baselines[endpoint.id] = (tick, result["metrics"])
            self.samples[endpoint.id] = result
        if self.history_store:
            try:
                self.history_store.record(endpoint, result)
                self.history_error = None
            except (sqlite3.Error, OSError, ValueError) as error:
                self.history_error = "history cache write failed"
                LOG.warning("History cache write failed: %s", error)
        return result

    def history(self, key, body=None):
        if not self.history_store:
            raise APIError(503, "history cache is disabled")
        body = {} if body is None else body
        if not isinstance(body, dict) or set(body) - {"after", "until"}:
            raise APIError(400, "invalid history query")
        try:
            return self.history_store.read(key, **body)
        except sqlite3.Error as error:
            raise APIError(503, "history cache unavailable") from error

    def _poll(self):
        with ThreadPoolExecutor(max_workers=8, thread_name_prefix="fabric-probe") as pool:
            while not self.stop.is_set():
                started = time.monotonic()
                self.wake.clear()
                try:
                    endpoints, info = self.discover()
                    with self.mutex:
                        for e in endpoints:
                            if e.id in self.endpoints and self.endpoints[e.id].control != e.control:
                                self.baselines.pop(e.id, None)
                        self.samples = {e.id: self.samples[e.id] if e.id in self.samples and
                                        self.endpoints[e.id].control == e.control else
                                        {"status": "pending", "sampled_at": None, "metrics": {}, "error": None}
                                        for e in endpoints}
                        self.endpoints = {e.id: e for e in endpoints}
                        self.baselines = {key: value for key, value in self.baselines.items() if key in self.endpoints}
                        self.discovery_info = {**info, "status": "ok", "scanned_at": now()}
                    self.syspiper.update(endpoints)
                    list(pool.map(self._sample, endpoints))
                    if self.history_store:
                        try:
                            self.history_store.prune()
                        except sqlite3.Error:
                            self.history_error = "history cache cleanup failed"
                except OSError as error:
                    with self.mutex:
                        self.endpoints, self.samples, self.baselines = {}, {}, {}
                        self.discovery_info = {"source": "procfs", "status": "error", "error": str(error)}
                    self.syspiper.update([])
                self.wake.wait(max(.2, self.interval - (time.monotonic() - started)))

    def snapshot(self):
        with self.mutex:
            endpoints = [{**asdict(endpoint), **self.samples[endpoint.id],
                          "health": health(endpoint, self.samples[endpoint.id]),
                          "switch_detail": switch_detail(self.samples[endpoint.id].get("metrics", {}),
                                                         self.samples[endpoint.id].get("changes", {}))
                          if endpoint.kind == "switch" else None}
                         for endpoint in self.endpoints.values()]
            info = dict(self.discovery_info)
        return {"api_version": 1, "generated_at": now(), "started_at": self.started_at,
                "poll_interval_seconds": self.interval, "allow_write": self.allow_write,
                "discovery": info, "endpoints": endpoints, "links": topology(endpoints),
                "collector": {"mode": "local", "uid": os.geteuid()},
                "syspiper": self.syspiper.snapshot(),
                "history": {"enabled": self.history_store is not None, "retention_seconds": 86400,
                            "error": self.history_error}}

    def logs(self, key):
        try:
            return {**read_logs(self.endpoint(key)), "sampled_at": now()}
        except (OSError, ValueError) as error:
            raise APIError(502, str(error)) from error

    def diagnostics(self, key):
        endpoint = self.endpoint(key)
        snapshot = self.snapshot()
        observed = next((e for e in snapshot["endpoints"] if e["id"] == key), asdict(endpoint))
        report = {"format": "tuntom-fabric-diagnostic-v1", "generated_at": now(), "endpoint": observed,
                  "links": [link for link in snapshot["links"] if key in (link["source"], link["target"])],
                  "logs": self.logs(key)}
        if endpoint.kind == "switch":
            try:
                report["active_rules"] = self.rules(key, "show")
            except APIError as error:
                report["rules_error"] = str(error)
        return report

    def endpoint(self, key):
        # Rediscover before an action: the process may have exited or exec'd
        # since the displayed snapshot, even if its PID has not changed.
        endpoints, _ = self.discover()
        for endpoint in endpoints:
            if endpoint.id == key:
                return endpoint
        raise APIError(404, "process no longer exists; refresh discovery")

    def rules(self, key, operation, body=None):
        endpoint = self.endpoint(key)
        if endpoint.kind != "switch":
            raise APIError(400, "rules are supported only by switches")
        if operation == "load" and not self.allow_write:
            raise APIError(403, "rule writes are disabled; restart with --allow-write")
        candidate = None
        if operation != "show":
            if not isinstance(body, dict) or set(body) - {"rules", "expected_sha256"}:
                raise APIError(400, "expected rules and optional expected_sha256")
            candidate = body.get("rules")
            if not isinstance(candidate, str) or not candidate.strip():
                raise APIError(400, "rules must be nonempty text")
            if len(candidate.encode()) > MAX_BODY:
                raise APIError(413, "rules exceed 1 MiB")
        with self.action_lock:
            try:
                current = self.control_query(endpoint, "show")
                revision = digest(current)
                if operation == "show":
                    return {"rules": current, "sha256": revision, "sampled_at": now()}
                if operation == "load":
                    if body.get("expected_sha256") != revision:
                        raise APIError(409, "active rules changed or expected_sha256 is missing; reload and review")
                checked = self.control_query(endpoint, "check", candidate)
                diff = "".join(difflib.unified_diff(current.splitlines(True), candidate.splitlines(True),
                                                    fromfile="active", tofile="candidate"))
                if operation == "check":
                    return {"result": checked, "sha256": revision, "diff": diff}
                result = self.control_query(endpoint, "load", candidate)
                self.wake.set()
                LOG.info(json.dumps({"event": "rules.load", "endpoint": key, "at": now(),
                                     "previous_sha256": revision, "candidate_sha256": digest(candidate)}))
                return {"result": result, "persistence": "runtime only"}
            except ControlError as error:
                raise APIError(422, str(error)) from error
            except (OSError, ValueError) as error:
                message = str(error)
                if operation == "load":
                    message += "; load outcome may be unknown: read active rules before retrying"
                    LOG.warning(json.dumps({"event": "rules.load.uncertain", "endpoint": key, "at": now()}))
                raise APIError(502, message) from error


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, fabric, token):
        self.fabric, self.token = fabric, token
        self.slots = threading.BoundedSemaphore(24)
        super().__init__(address, Handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class Handler(BaseHTTPRequestHandler):
    server_version = "TuntomFabric/1"

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def log_message(self, *_):
        pass  # Request URLs and bearer tokens never enter access logs.

    def respond(self, status, data, content_type="application/json; charset=utf-8"):
        if not isinstance(data, bytes):
            data = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; "
                         "img-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        self.end_headers()
        self.wfile.write(data)

    def guard(self):
        port = self.server.server_port
        # The accepted connection knows its concrete local destination even
        # when the listener is bound to 0.0.0.0. Do not trust arbitrary Host names.
        addresses = {"127.0.0.1", "localhost", self.connection.getsockname()[0]}
        hosts = {f"{address}:{port}" for address in addresses}
        if port == 80:
            hosts |= addresses
        if self.headers.get("Host") not in hosts:
            raise APIError(403, "invalid Host header")
        origin = self.headers.get("Origin")
        if origin and origin != "http://" + self.headers["Host"]:
            raise APIError(403, "cross-origin requests are not supported")
        if self.headers.get("Sec-Fetch-Site") == "cross-site":
            raise APIError(403, "cross-site requests are not supported")

    def authorize(self):
        supplied = self.headers.get("Authorization", "")
        if not secrets.compare_digest(supplied.encode(), ("Bearer " + self.server.token).encode()):
            raise APIError(401, "a valid bearer token is required")

    def body(self):
        if self.headers.get_content_type() != "application/json" or self.headers.get("Transfer-Encoding"):
            raise APIError(415, "send application/json with Content-Length")
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,8}", lengths[0]):
            raise APIError(400, "invalid Content-Length")
        length = int(lengths[0])
        if length > MAX_BODY * 6 + 4096:
            raise APIError(413, "request too large")
        raw = self.rfile.read(length)
        if len(raw) != length:
            raise APIError(400, "incomplete request")
        try:
            return json.loads(raw)
        except (ValueError, UnicodeError) as error:
            raise APIError(400, "invalid JSON") from error

    def route(self):
        self.guard()
        path = urlsplit(self.path).path
        if path == "/healthz" and self.command == "GET":
            return self.respond(200, {"status": "ok", "api_version": 1})
        if path.startswith("/api/"):
            self.authorize()
            if path == "/api/v1/snapshot" and self.command == "GET":
                return self.respond(200, self.server.fabric.snapshot())
            if path == "/api/v1/refresh" and self.command == "POST":
                self.server.fabric.wake.set()
                return self.respond(202, {"result": "discovery scheduled"})
            match = re.fullmatch(r"/api/v1/(?:endpoints|syspiper)/([^/]+)/history", path)
            if match and self.command == "GET":
                params = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
                if set(params) - {"after", "until"} or any(len(v) != 1 or not re.fullmatch(r"[0-9]{1,16}", v[0]) for v in params.values()):
                    raise APIError(400, "invalid history query")
                return self.respond(200, self.server.fabric.history(unquote(match[1]), {k:int(v[0]) for k,v in params.items()}))
            match = re.fullmatch(r"/api/v1/endpoints/([^/]+)/(logs|diagnostics)", path)
            if match and self.command == "GET":
                action = getattr(self.server.fabric, match[2])
                return self.respond(200, action(unquote(match[1])))
            match = re.fullmatch(r"/api/v1/endpoints/([^/]+)/rules(?:/(check|load))?", path)
            if match:
                operation = match[2] or "show"
                if self.command != ("GET" if operation == "show" else "POST"):
                    raise APIError(405, "method not allowed")
                result = self.server.fabric.rules(unquote(match[1]), operation,
                                                  self.body() if self.command == "POST" else None)
                return self.respond(200, result)
            raise APIError(404, "unknown API route")
        assets = {"/": ("index.html", "text/html; charset=utf-8"),
                  "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                  "/style.css": ("style.css", "text/css; charset=utf-8"),
                  "/favicon.svg": ("favicon.svg", "image/svg+xml")}
        if self.command == "GET" and path in assets:
            filename, mime = assets[path]
            return self.respond(200, (STATIC / filename).read_bytes(), mime)
        raise APIError(404, "not found")

    def dispatch(self):
        try:
            self.route()
        except APIError as error:
            self.respond(error.status, {"error": str(error)})
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass
        except Exception:
            LOG.exception("API request failed")
            self.respond(500, {"error": "internal server error"})

    do_GET = dispatch
    do_POST = dispatch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=ipaddress.IPv4Address, default=ipaddress.IPv4Address("0.0.0.0"),
                        help="IPv4 listen address (default: 0.0.0.0; use 127.0.0.1 for local access only)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--interval", type=float, default=5, help="minimum poll interval in seconds")
    parser.add_argument("--allow-write", action="store_true", help="enable runtime rule loads")
    parser.add_argument("--collector", help="use a separate Unix-socket collector")
    parser.add_argument("--golden-token", metavar="TOKEN", help="fixed lab access token; overrides TUNTOM_FABRIC_TOKEN (short tokens produce a warning)")
    parser.add_argument("--history-db", type=Path, default=default_path(), help="SQLite telemetry cache for local collection")
    parser.add_argument("--no-history", action="store_true", help="disable local telemetry cache")
    syspiper_arguments(parser)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    if not 1 <= args.interval <= 3600 or not 0 <= args.port <= 65535:
        parser.error("interval must be 1..3600 seconds; port must be 0..65535")
    try:
        token = args.golden_token if args.golden_token is not None else (os.environ.get("TUNTOM_FABRIC_TOKEN") or secrets.token_urlsafe(32))
        if not re.fullmatch(r"[a-zA-Z0-9_.~-]+", token):
            raise ValueError("access token must be non-empty and contain only URL-safe characters")
        if len(token) < 24:
            if args.golden_token is None:
                raise ValueError("TUNTOM_FABRIC_TOKEN must have at least 24 URL-safe characters")
            LOG.warning("WARNING: --golden-token is shorter than 24 characters; use a longer token outside the lab.")
        if os.geteuid() == 0:
            raise ValueError("run the HTTP server as a regular user; use collector.py for privileged reads")
        if args.collector:
            if args.syspiper_key is not None or args.syspiper_node or args.syspiper_port != 8181 or args.syspiper_interval != 30:
                raise ValueError("configure Syspiper on collector.py when using --collector")
            from collector import RemoteFabric
            fabric = RemoteFabric(args.collector, allow_write=args.allow_write)
        else:
            fabric = Fabric(allow_write=args.allow_write, interval=args.interval,
                            history_path=None if args.no_history else args.history_db, syspiper=syspiper_options(args))
        write_enabled = fabric.snapshot()["allow_write"]
        server = Server((str(args.host), args.port), fabric, token)
    except (ValueError, TypeError, OSError, sqlite3.Error, APIError) as error:
        parser.exit(1, f"Fabric: {error}\n")
    link_host = "127.0.0.1" if args.host.is_unspecified else str(args.host)
    print(f"Tuntom Fabric: http://{link_host}:{server.server_port}/#token={token}", flush=True)
    print(f"Listening: {args.host}:{server.server_port}", flush=True)
    if args.host.is_unspecified:
        print("Remote access: replace 127.0.0.1 in the link with this machine's IPv4 address.", flush=True)
    print(f"Collector: {args.collector or 'local /proc'}; rule writes {'enabled' if write_enabled else 'disabled'}", flush=True)
    fabric.start()
    try:
        server.serve_forever(poll_interval=.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        fabric.close()


if __name__ == "__main__":
    main()
