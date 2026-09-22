#!/usr/bin/env python3
"""Separate /proc/control reader; optional root privileges, never an HTTP listener."""
from __future__ import annotations

import argparse
import json
import logging
import os
from pathlib import Path
import socket
import socketserver
import stat
import sqlite3
import struct
import threading

from errors import APIError
from server import Fabric
from history import default_path
from syspiper import add_arguments as syspiper_arguments, options as syspiper_options

MAX_FRAME = 8 * 1024 * 1024


def receive(sock):
    def exact(count):
        result = bytearray()
        while len(result) < count:
            chunk = sock.recv(min(65536, count - len(result)))
            if not chunk:
                raise OSError("incomplete collector response")
            result.extend(chunk)
        return bytes(result)
    length = struct.unpack("!I", exact(4))[0]
    if length > MAX_FRAME:
        raise OSError("collector frame exceeds 8 MiB")
    return json.loads(exact(length))


def send(sock, value):
    raw = json.dumps(value, ensure_ascii=False).encode()
    if len(raw) > MAX_FRAME:
        raise OSError("collector frame exceeds 8 MiB")
    sock.sendall(struct.pack("!I", len(raw)) + raw)


def peer_uid(sock):
    return struct.unpack("3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1]


class CollectorHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(12)
        try:
            if peer_uid(self.request) != self.server.allowed_uid:
                raise APIError(403, "collector peer UID is not allowed")
            request = receive(self.request)
            if not isinstance(request, dict) or set(request) - {"operation", "id", "body"}:
                raise APIError(400, "invalid collector request")
            operation, key = request.get("operation"), request.get("id")
            fabric = self.server.fabric
            if operation == "snapshot":
                result = fabric.snapshot()
                result["collector"] = {"mode": "separate", "uid": os.geteuid()}
            elif operation == "history" and isinstance(key, str):
                result = fabric.history(key, request.get("body"))
            elif operation == "request_submit" and isinstance(key, str):
                result = fabric.submit_request(key, request.get("body"))
            elif operation == "request_status" and isinstance(key, str):
                result = fabric.request_status(key)
            elif operation == "refresh":
                fabric.refresh()
                result = {"result": "discovery scheduled"}
            elif operation in {"classifier-show", "classifier-check", "classifier-load", "classifier-load-flush", "classifier-disable"} and isinstance(key, str):
                result = fabric.classifier(key, operation.removeprefix("classifier-"), request.get("body"))
            elif operation in {"show", "check", "load", "logs", "diagnostics", "flows"} and isinstance(key, str):
                # The caller supplies a discovered identity, never a path or command.
                if operation in {"logs", "diagnostics", "flows"}:
                    result = getattr(fabric, operation)(key)
                else:
                    result = fabric.rules(key, operation, request.get("body"))
            else:
                raise APIError(400, "unsupported collector operation")
            send(self.request, {"result": result})
        except APIError as error:
            send(self.request, {"status": error.status, "error": str(error)})
        except (OSError, ValueError, TypeError):
            try:
                send(self.request, {"status": 502, "error": "collector request failed"})
            except OSError:
                pass


class CollectorServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True
    request_queue_size = 16

    def __init__(self, path, fabric, allowed_uid):
        self.fabric, self.allowed_uid = fabric, allowed_uid
        self.slots = threading.BoundedSemaphore(8)
        # A fresh socket only. Never unlink a path supplied by a client.
        super().__init__(path, CollectorHandler, bind_and_activate=False)
        try:
            old_mask = os.umask(0o177)
            try:
                self.server_bind()
            finally:
                os.umask(old_mask)
            if os.geteuid() == 0:
                os.chown(path, allowed_uid, -1)
            os.chmod(path, 0o600)
            self.server_activate()
        except Exception:
            self.server_close()
            raise

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class RemoteFabric:
    def __init__(self, path, *, allow_write=False):
        self.path, self.allow_write = path, allow_write
        self.wake = self  # Same refresh interface as the in-process collector.

    def call(self, operation, key=None, body=None):
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(180 if operation.startswith("classifier-") else 12)
                sock.connect(self.path)
                if peer_uid(sock) not in {0, os.geteuid()}:
                    raise OSError("unexpected collector UID")
                send(sock, {"operation": operation, "id": key, "body": body})
                response = receive(sock)
                if "error" in response:
                    raise APIError(response.get("status", 502), response["error"])
                return response["result"]
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise APIError(502, f"collector unavailable: {error}") from error

    def start(self):
        pass

    def close(self):
        pass

    def set(self):
        self.call("refresh")

    def refresh(self):
        self.call("refresh")

    def snapshot(self):
        result = self.call("snapshot")
        result["allow_write"] = bool(result["allow_write"] and self.allow_write)
        return result

    def classifier(self, key, operation, body=None):
        if operation in {"load", "load-flush", "disable"} and not self.allow_write:
            raise APIError(403, "classifier writes are disabled; restart with --allow-write")
        return self.call("classifier-" + operation, key, body)

    def rules(self, key, operation, body=None):
        if operation == "load" and not self.allow_write:
            raise APIError(403, "rule writes are disabled; restart with --allow-write")
        return self.call(operation, key, body)

    def history(self, key, body=None):
        return self.call("history", key, body)

    def logs(self, key):
        return self.call("logs", key)

    def flows(self, key):
        return self.call("flows", key)

    def submit_request(self, key, body):
        return self.call("request_submit", key, body)

    def request_status(self, key):
        return self.call("request_status", key)

    def diagnostics(self, key):
        return self.call("diagnostics", key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="new Unix socket in an existing private or root-owned directory")
    parser.add_argument("--allow-uid", required=True, type=int, help="UID of the unprivileged web process")
    parser.add_argument("--interval", type=float, default=5)
    parser.add_argument("--allow-write", action="store_true", help="also permit manual runtime rule loads")
    parser.add_argument("--history-db", type=Path, default=default_path(), help="SQLite telemetry cache (24 hour retention)")
    parser.add_argument("--no-history", action="store_true", help="disable telemetry cache")
    syspiper_arguments(parser)
    args = parser.parse_args()
    if args.allow_uid < 0 or not 1 <= args.interval <= 3600:
        parser.error("invalid UID or interval")
    path = Path(args.socket).absolute()
    parent = path.parent.stat()
    if parent.st_uid != os.geteuid() or parent.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        parser.error("socket directory must belong to the collector UID and not be group/world writable")
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    try:
        fabric = Fabric(allow_write=args.allow_write, interval=args.interval,
                        history_path=None if args.no_history else args.history_db, syspiper=syspiper_options(args))
        server = CollectorServer(str(path), fabric, args.allow_uid)
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, f"Collector: {error}\n")
    identity = path.stat().st_ino
    print(f"Fabric collector: {path}; peer UID {args.allow_uid}; writes {'enabled' if args.allow_write else 'disabled'}", flush=True)
    fabric.start()
    try:
        server.serve_forever(poll_interval=.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        fabric.close()
        if path.exists() and path.stat().st_ino == identity:
            path.unlink()


if __name__ == "__main__":
    main()
