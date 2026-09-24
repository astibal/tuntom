#!/usr/bin/env python3
"""Leased external HTTPS observer for the Tuntom Fabric management plane."""
from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import http.server
import http.client
import ipaddress
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import sqlite3
import tempfile
import threading
import time
from urllib.parse import urlsplit


MAX_BODY = 64 * 1024
MAX_TARGETS = 64
DEFAULT_TIMEOUT = 10.0
USER_AGENT = "Tuntom-Fabric-Peek/1"
DAY = 86400
SYSPIPER_PATHS = frozenset(('cpu','ram','disk','net','system','interfaces','filesystems','pressure','apt'))


class ProbeError(Exception):
    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind


class PeekHistory:
    def __init__(self, path: str, lease_days=7, retention_days=30):
        self.lock = threading.RLock(); self.lease_seconds=lease_days*DAY; self.retention_seconds=retention_days*DAY
        self.db=sqlite3.connect(path,check_same_thread=False); self.db.row_factory=sqlite3.Row
        self.db.executescript("""PRAGMA journal_mode=WAL;
        CREATE TABLE IF NOT EXISTS target(id TEXT PRIMARY KEY,url TEXT NOT NULL,interval INTEGER NOT NULL,lease_until REAL NOT NULL,next_probe REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS observation(seq INTEGER PRIMARY KEY AUTOINCREMENT,target_id TEXT NOT NULL,at REAL NOT NULL,payload TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS observation_target_at ON observation(target_id,at);""")
    def renew(self, targets):
        now=time.time()
        with self.lock,self.db:
            for item in targets:
                interval=item.get("interval",60)
                if not isinstance(interval,int) or isinstance(interval,bool) or not 10<=interval<=86400: raise ValueError("interval must be 10..86400 seconds")
                self.db.execute("INSERT INTO target VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET url=excluded.url,interval=excluded.interval,lease_until=excluded.lease_until",
                    (item["id"],item["url"],interval,now+self.lease_seconds,now+interval))
    def record(self, result):
        with self.lock,self.db:self.db.execute("INSERT INTO observation(target_id,at,payload) VALUES(?,?,?)",(result["id"],time.time(),json.dumps(result,separators=(",",":"))))
    def history(self, target_ids, limit=1000):
        with self.lock:
            return {target_id:[json.loads(row[0]) for row in reversed(self.db.execute("SELECT payload FROM observation WHERE target_id=? ORDER BY at DESC LIMIT ?",(target_id,limit)).fetchall())] for target_id in target_ids}
    def due(self):
        now=time.time()
        with self.lock,self.db:
            rows=self.db.execute("SELECT id,url,interval FROM target WHERE lease_until>? AND next_probe<=?",(now,now)).fetchall()
            for row in rows:self.db.execute("UPDATE target SET next_probe=? WHERE id=?",(now+row["interval"],row["id"]))
            self.db.execute("DELETE FROM target WHERE lease_until<=?",(now,));self.db.execute("DELETE FROM observation WHERE at<?",(now-self.retention_seconds,))
            return [dict(row) for row in rows]
    def release(self, ids):
        with self.lock,self.db:self.db.executemany("DELETE FROM target WHERE id=?",((item,) for item in ids))
    def status(self):
        now=time.time()
        with self.lock:
            active=self.db.execute("SELECT count(*) FROM target WHERE lease_until>?",(now,)).fetchone()[0]
            observations,oldest,newest=self.db.execute("SELECT count(*),min(at),max(at) FROM observation").fetchone()
        iso=lambda value:dt.datetime.fromtimestamp(value,dt.timezone.utc).isoformat(timespec="milliseconds") if value else None
        return {"active_targets":active,"observations":observations,"oldest_observation":iso(oldest),"newest_observation":iso(newest),"lease_days":self.lease_seconds//DAY,"retention_days":self.retention_seconds//DAY}
    def close(self):
        with self.lock:self.db.close()


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

    def __init__(self, address, token: str, workers: int, timeout: float, history=None, syspiper_key=None, syspiper_port=8181):
        super().__init__(address, PeekHandler)
        self.token = token
        self.workers = workers
        self.timeout = timeout
        self.executor = concurrent.futures.ThreadPoolExecutor(max_workers=workers, thread_name_prefix="peek")
        self.history=history;self.stopping=threading.Event()
        self.syspiper_key,self.syspiper_port=syspiper_key,syspiper_port
        self.scheduler=threading.Thread(target=self.schedule,name="peek-scheduler",daemon=True);self.scheduler.start()

    def schedule(self):
        while not self.stopping.wait(2):
            if not self.history:continue
            for target in self.history.due():
                future=self.executor.submit(probe,target,self.timeout)
                future.add_done_callback(lambda item:self.history.record(item.result()))

    def server_close(self):
        self.stopping.set();self.scheduler.join(timeout=3)
        self.executor.shutdown(wait=True, cancel_futures=True)
        if self.history:self.history.close()
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
            return self.respond(200, {"status": "ok", "time": utc_now(),"history":self.server.history.status() if self.server.history else None,
                "capabilities":{"syspiper_self":bool(self.server.syspiper_key)}})
        prefix="/syspiper/self/"
        if self.path.startswith(prefix):
            supplied=self.headers.get("Authorization","")
            if not secrets.compare_digest(supplied.encode(),("Bearer "+self.server.token).encode()):return self.respond(401,{"error":"unauthorized"})
            path=self.path[len(prefix):]
            if path not in SYSPIPER_PATHS or not self.server.syspiper_key:return self.respond(404,{"error":"not found"})
            connection=http.client.HTTPConnection("127.0.0.1",self.server.syspiper_port,timeout=15 if path=="apt" else 4)
            try:
                connection.request("GET","/"+path,headers={"X-API-Key":self.server.syspiper_key,"Accept":"application/json","Accept-Encoding":"identity"})
                response=connection.getresponse();raw=response.read(1024*1024+1)
                if len(raw)>1024*1024:return self.respond(502,{"error":"Syspiper response too large"})
                try:value=json.loads(raw)
                except (ValueError,UnicodeError):return self.respond(502,{"error":"invalid Syspiper response"})
                return self.respond(response.status,value)
            except (OSError,http.client.HTTPException):return self.respond(502,{"error":"local Syspiper unavailable"})
            finally:connection.close()
        self.respond(404, {"error": "not found"})

    def do_POST(self):
        if self.path not in ("/v1/probe","/v1/release"):
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
        if self.path=="/v1/release":
            ids=body.get("ids") if isinstance(body,dict) and set(body)=={"ids"} else None
            if not isinstance(ids,list) or not all(isinstance(item,str) for item in ids):return self.respond(400,{"error":"body must contain ids array"})
            if self.server.history:self.server.history.release(ids)
            return self.respond(200,{"released":len(ids)})
        if not isinstance(body, dict) or set(body) != {"targets"} or not isinstance(body["targets"], list):
            return self.respond(400, {"error": "body must contain only a targets array"})
        if not 1 <= len(body["targets"]) <= MAX_TARGETS:
            return self.respond(400, {"error": f"targets must contain 1..{MAX_TARGETS} items"})
        try:
            if self.server.history:self.server.history.renew(body["targets"])
        except (ValueError,KeyError) as error:return self.respond(400,{"error":str(error)})
        futures = [self.server.executor.submit(probe, item, self.server.timeout)
                   if isinstance(item, dict) else None for item in body["targets"]]
        results = [future.result() if future else {
            "id": None, "url": None, "observed_at": utc_now(), "ok": False, "available": False,
            "total_ms": 0, "error": {"kind": "invalid_target", "message": "target must be an object"},
        } for future in futures]
        if self.server.history:
            for result in results:self.server.history.record(result)
        history=self.server.history.history([item["id"] for item in body["targets"] if isinstance(item,dict) and isinstance(item.get("id"),str)]) if self.server.history else {}
        self.respond(200, {"observed_at": utc_now(), "results": results,"history":history})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8780)
    parser.add_argument("--token", default=os.environ.get("TUNTOM_PEEK_TOKEN"))
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--history-db", default=os.environ.get("TUNTOM_PEEK_HISTORY_DB"))
    parser.add_argument("--lease-days", type=int, default=7)
    parser.add_argument("--retention-days", type=int, default=30)
    parser.add_argument("--syspiper-key", default=os.environ.get("TUNTOM_PEEK_SYSPIPER_KEY"))
    parser.add_argument("--syspiper-port",type=int,default=8181)
    parser.add_argument("--once", metavar="HTTPS_URL", help="probe one URL and print JSON")
    args = parser.parse_args()
    if args.once:
        print(json.dumps(probe({"id": "once", "url": args.once}, args.timeout), ensure_ascii=False, indent=2))
        return
    if not args.token or len(args.token) < 24:
        parser.error("--token or TUNTOM_PEEK_TOKEN with at least 24 characters is required")
    if not 1 <= args.workers <= 32 or not 1 <= args.timeout <= 60 or not 0 <= args.port <= 65535:
        parser.error("workers must be 1..32, timeout 1..60 seconds and port 0..65535")
    if not 1<=args.lease_days<=365 or not args.lease_days<=args.retention_days<=3650:parser.error("lease/retention days are invalid")
    history=PeekHistory(args.history_db,args.lease_days,args.retention_days) if args.history_db else None
    server = PeekServer((args.host, args.port), args.token, args.workers, args.timeout,history,args.syspiper_key,args.syspiper_port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
