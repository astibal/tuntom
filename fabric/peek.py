#!/usr/bin/env python3
"""Stateless external HTTPS probe for the Tuntom Fabric management plane."""
from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import http.server
import ipaddress
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import tempfile
import time
from urllib.parse import urlsplit


MAX_BODY = 64 * 1024
MAX_TARGETS = 64
DEFAULT_TIMEOUT = 10.0
USER_AGENT = "Tuntom-Fabric-Peek/1"


class ProbeError(Exception):
    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def milliseconds(seconds: float) -> float:
    return round(seconds * 1000, 2)


def public_addresses(host: str, port: int) -> list[tuple[int, tuple, str]]:
    try:
        answers = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    except socket.gaierror as error:
        raise ProbeError("dns", str(error)) from error
    result = []
    seen = set()
    for family, socktype, proto, _, sockaddr in answers:
        address = ipaddress.ip_address(sockaddr[0])
        if not address.is_global:
            raise ProbeError("address_not_public", f"{host} resolves to non-public address")
        key = (family, sockaddr)
        if key not in seen:
            seen.add(key)
            result.append((family, sockaddr, str(address)))
    if not result:
        raise ProbeError("dns", "no usable address")
    return result


def connect(addresses: list[tuple[int, tuple, str]], timeout: float):
    failures = []
    for family, sockaddr, address in addresses:
        sock = socket.socket(family, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        started = time.monotonic()
        try:
            sock.connect(sockaddr)
            return sock, address, time.monotonic() - started
        except OSError as error:
            failures.append(str(error))
            sock.close()
    raise ProbeError("connect", failures[-1] if failures else "connection failed")


def decode_certificate(der: bytes) -> dict:
    """Decode a certificate with CPython's stdlib helper; no CA decision is made here."""
    path = None
    try:
        with tempfile.NamedTemporaryFile("w", prefix="peek-cert-", suffix=".pem", delete=False) as output:
            path = output.name
            output.write(ssl.DER_cert_to_PEM_cert(der))
        return ssl._ssl._test_decode_cert(path)  # type: ignore[attr-defined]
    finally:
        if path:
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass


def name_fields(value) -> dict[str, str]:
    result = {}
    for rdn in value or ():
        for key, item in rdn:
            result[key] = item
    return result


def certificate_result(der: bytes, parsed: dict) -> dict:
    not_before = dt.datetime.fromtimestamp(ssl.cert_time_to_seconds(parsed["notBefore"]), dt.timezone.utc)
    not_after = dt.datetime.fromtimestamp(ssl.cert_time_to_seconds(parsed["notAfter"]), dt.timezone.utc)
    now = dt.datetime.now(dt.timezone.utc)
    return {
        "subject": name_fields(parsed.get("subject")),
        "issuer": name_fields(parsed.get("issuer")),
        "serial_number": parsed.get("serialNumber"),
        "not_before": not_before.isoformat(),
        "not_after": not_after.isoformat(),
        "days_remaining": round((not_after - now).total_seconds() / 86400, 2),
        "time_valid": not_before <= now <= not_after,
        "sha256": hashlib.sha256(der).hexdigest(),
        "subject_alt_names": [value for kind, value in parsed.get("subjectAltName", ()) if kind == "DNS"],
    }


def tls_connection(host: str, port: int, addresses, timeout: float):
    """Prefer a verified handshake; reconnect without verification to report bad certs."""
    verification_error = None
    for verified in (True, False):
        context = ssl.create_default_context() if verified else ssl._create_unverified_context()
        raw, address, connect_seconds = connect(addresses, timeout)
        started = time.monotonic()
        try:
            wrapped = context.wrap_socket(raw, server_hostname=host)
            handshake_seconds = time.monotonic() - started
            return wrapped, address, connect_seconds, handshake_seconds, verified, verification_error
        except ssl.SSLCertVerificationError as error:
            raw.close()
            verification_error = str(error)
            if not verified:
                raise ProbeError("tls", str(error)) from error
        except (ssl.SSLError, OSError) as error:
            raw.close()
            raise ProbeError("tls", str(error)) from error
    raise ProbeError("tls", verification_error or "TLS handshake failed")


def read_http_status(sock: ssl.SSLSocket, host_header: str, path: str) -> int:
    request = (f"HEAD {path} HTTP/1.1\r\nHost: {host_header}\r\nUser-Agent: {USER_AGENT}\r\n"
               "Accept: */*\r\nConnection: close\r\n\r\n").encode("ascii")
    sock.sendall(request)
    data = bytearray()
    while b"\r\n" not in data and len(data) < 8192:
        chunk = sock.recv(1024)
        if not chunk:
            break
        data.extend(chunk)
    first = bytes(data).split(b"\r\n", 1)[0]
    parts = first.split(b" ", 2)
    if len(parts) < 2 or not parts[0].startswith(b"HTTP/") or not parts[1].isdigit():
        raise ProbeError("http", "invalid HTTP response")
    return int(parts[1])


def probe(target: dict, timeout: float = DEFAULT_TIMEOUT) -> dict:
    started_at = utc_now()
    started = time.monotonic()
    target_id = target.get("id")
    url = target.get("url")
    base = {"id": target_id, "url": url, "observed_at": started_at}
    try:
        if not isinstance(target_id, str) or not target_id or len(target_id) > 128:
            raise ProbeError("invalid_target", "id must be a non-empty string of at most 128 characters")
        if not isinstance(url, str) or len(url) > 2048:
            raise ProbeError("invalid_target", "url must be a string of at most 2048 characters")
        parsed = urlsplit(url)
        if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
            raise ProbeError("invalid_target", "only https URLs without credentials or fragments are supported")
        try:
            port = parsed.port or 443
        except ValueError as error:
            raise ProbeError("invalid_target", str(error)) from error
        host = parsed.hostname.rstrip(".")
        addresses = public_addresses(host, port)
        sock, address, connect_s, handshake_s, trusted, verify_error = tls_connection(
            host, port, addresses, timeout)
        try:
            der = sock.getpeercert(binary_form=True)
            certificate = certificate_result(der, decode_certificate(der))
            path = parsed.path or "/"
            if parsed.query:
                path += "?" + parsed.query
            host_header = host if port == 443 else f"{host}:{port}"
            http_started = time.monotonic()
            status = read_http_status(sock, host_header, path)
            http_s = time.monotonic() - http_started
            result = {
                **base,
                "ok": True,
                "available": True,
                "address": address,
                "http_status": status,
                "connect_ms": milliseconds(connect_s),
                "tls_handshake_ms": milliseconds(handshake_s),
                "http_response_ms": milliseconds(http_s),
                "total_ms": milliseconds(time.monotonic() - started),
                "tls": {
                    "trusted": trusted,
                    "verification_error": verify_error,
                    "version": sock.version(),
                    "cipher": sock.cipher()[0] if sock.cipher() else None,
                    "certificate": certificate,
                },
            }
            return result
        finally:
            sock.close()
    except ProbeError as error:
        return {**base, "ok": False, "available": False, "total_ms": milliseconds(time.monotonic() - started),
                "error": {"kind": error.kind, "message": str(error)}}
    except (OSError, ValueError, KeyError) as error:
        return {**base, "ok": False, "available": False, "total_ms": milliseconds(time.monotonic() - started),
                "error": {"kind": "probe", "message": str(error)}}


class PeekServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, token: str, workers: int, timeout: float):
        super().__init__(address, PeekHandler)
        self.token = token
        self.workers = workers
        self.timeout = timeout
        self.executor = concurrent.futures.ThreadPoolExecutor(max_workers=workers, thread_name_prefix="peek")

    def server_close(self):
        self.executor.shutdown(wait=True, cancel_futures=True)
        super().server_close()


class PeekHandler(http.server.BaseHTTPRequestHandler):
    server_version = "TuntomPeek/1"

    def log_message(self, format, *args):
        pass

    def respond(self, status: int, value):
        body = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/healthz":
            return self.respond(200, {"status": "ok", "time": utc_now()})
        self.respond(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/v1/probe":
            return self.respond(404, {"error": "not found"})
        supplied = self.headers.get("Authorization", "")
        if not secrets.compare_digest(supplied.encode(), ("Bearer " + self.server.token).encode()):
            return self.respond(401, {"error": "unauthorized"})
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            return self.respond(413, {"error": "request body must be at most 64 KiB"})
        try:
            body = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return self.respond(400, {"error": "invalid JSON"})
        if not isinstance(body, dict) or set(body) != {"targets"} or not isinstance(body["targets"], list):
            return self.respond(400, {"error": "body must contain only a targets array"})
        if not 1 <= len(body["targets"]) <= MAX_TARGETS:
            return self.respond(400, {"error": f"targets must contain 1..{MAX_TARGETS} items"})
        futures = [self.server.executor.submit(probe, item, self.server.timeout)
                   if isinstance(item, dict) else None for item in body["targets"]]
        results = [future.result() if future else {
            "id": None, "url": None, "observed_at": utc_now(), "ok": False, "available": False,
            "total_ms": 0, "error": {"kind": "invalid_target", "message": "target must be an object"},
        } for future in futures]
        self.respond(200, {"observed_at": utc_now(), "results": results})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8780)
    parser.add_argument("--token", default=os.environ.get("TUNTOM_PEEK_TOKEN"))
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--once", metavar="HTTPS_URL", help="probe one URL and print JSON")
    args = parser.parse_args()
    if args.once:
        print(json.dumps(probe({"id": "once", "url": args.once}, args.timeout), ensure_ascii=False, indent=2))
        return
    if not args.token or len(args.token) < 24:
        parser.error("--token or TUNTOM_PEEK_TOKEN with at least 24 characters is required")
    if not 1 <= args.workers <= 32 or not 1 <= args.timeout <= 60 or not 0 <= args.port <= 65535:
        parser.error("workers must be 1..32, timeout 1..60 seconds and port 0..65535")
    server = PeekServer((args.host, args.port), args.token, args.workers, args.timeout)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
