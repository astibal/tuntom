"""Persistent SSH endpoint inventory and one-shot platform discovery."""
from __future__ import annotations
from deployment_diagnostics import safe_output

from datetime import datetime, timezone
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import subprocess
import tempfile
import threading


# Phase 0 intentionally uses an exact allowlist.  Add a platform only after its
# future deployment template has been tested on that exact release.
SUPPORTED_PLATFORMS = {("ubuntu", "26.04", "x86_64")}


def timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def validate_target(data: dict) -> tuple[str, int]:
    if not isinstance(data, dict) or set(data) - {"address", "port"}:
        raise ValueError("expected address and optional port")
    try:
        address = str(ipaddress.ip_address(data.get("address", "")))
    except ValueError as error:
        raise ValueError("address must be an IPv4 or IPv6 address") from error
    port = data.get("port", 22)
    if not isinstance(port, int) or isinstance(port, bool) or not 1 <= port <= 65535:
        raise ValueError("SSH port must be 1..65535")
    return address, port


class SSHDiscovery:
    def __init__(self, user="root", identity_file: Path | str | None = None, timeout=8):
        if not isinstance(user, str) or not re.fullmatch(r"[a-z_][a-z0-9_-]{0,31}", user):
            raise ValueError("invalid controlled-endpoint SSH user")
        self.user, self.identity_file, self.timeout = user, str(identity_file) if identity_file else None, timeout

    def scan(self, address: str, port: int) -> tuple[str, list[dict]]:
        try:
            result = subprocess.run(
                ["ssh-keyscan", "-T", str(self.timeout), "-p", str(port), address],
                capture_output=True, text=True, timeout=self.timeout + 2, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise OSError(f"SSH host-key scan failed: {error}") from error
        lines = [line.strip() for line in result.stdout.splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        if not lines:
            detail = result.stderr.strip().splitlines()[-1] if result.stderr.strip() else "no host key returned"
            raise OSError(f"SSH host-key scan failed: {detail}")
        keys = []
        for known_host in lines:
            try:
                output = subprocess.run(["ssh-keygen", "-lf", "-", "-E", "sha256"], input=known_host + "\n",
                    capture_output=True, text=True, timeout=3, check=True).stdout.strip()
            except (OSError, subprocess.SubprocessError) as error:
                raise OSError("could not fingerprint SSH host key") from error
            match = re.match(r"^(\d+)\s+(SHA256:[A-Za-z0-9+/]+)\s+.*\(([^()]+)\)$", output)
            if match:
                keys.append({"bits": int(match[1]), "fingerprint": match[2], "type": match[3], "known_host": known_host})
        if not keys:
            raise OSError("SSH host-key scan returned no usable keys")
        return keys

    def discover(self, address: str, port: int, known_hosts: str) -> dict:
        marker = "__TUNTOM_ARCH__"
        remote = f"cat /etc/os-release 2>/dev/null; printf '\\n{marker}'; uname -m"
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix="tuntom-known-hosts-") as known:
            os.chmod(known.name, 0o600)
            known.write(known_hosts); known.flush()
            command = ["ssh", "-p", str(port), "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
                "-o", "StrictHostKeyChecking=yes", "-o", f"UserKnownHostsFile={known.name}",
                "-o", f"ConnectTimeout={self.timeout}"]
            if self.identity_file:
                command += ["-i", self.identity_file]
            command += [f"{self.user}@{address}", remote]
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=self.timeout + 5, check=False)
            except (OSError, subprocess.TimeoutExpired) as error:
                raise OSError(f"SSH discovery failed: {error}") from error
        if result.returncode:
            detail = result.stderr.strip().splitlines()[-1] if result.stderr.strip() else f"ssh exited {result.returncode}"
            raise OSError(f"SSH discovery failed: {detail}")
        before, separator, architecture = result.stdout.partition(marker)
        if not separator:
            raise OSError("SSH discovery returned an invalid response")
        values = {}
        for line in before.splitlines():
            key, found, value = line.partition("=")
            if found and re.fullmatch(r"[A-Z][A-Z0-9_]*", key):
                values[key] = value.strip().strip('"')
        system = {"os": values.get("ID", "unknown"), "distribution": values.get("NAME", "Unknown"),
                  "version": values.get("VERSION_ID", "unknown"), "architecture": architecture.strip() or "unknown"}
        platform = (system["os"].lower(), system["version"], system["architecture"])
        supported = platform in SUPPORTED_PLATFORMS
        return {"system": system, "supported": supported,
                "support_reason": "supported platform" if supported else "no tested deployment template for this platform"}

    def command(self, address: str, port: int, known_hosts: str, remote_command: str,
                input_data: bytes | None = None, timeout=600) -> str:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix="tuntom-known-hosts-") as known:
            os.chmod(known.name, 0o600); known.write(known_hosts); known.flush()
            command = ["ssh", "-T", "-p", str(port), "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
                "-o", "StrictHostKeyChecking=yes", "-o", f"UserKnownHostsFile={known.name}",
                "-o", f"ConnectTimeout={self.timeout}"]
            if self.identity_file: command += ["-i", self.identity_file]
            command += [f"{self.user}@{address}", remote_command]
            try:
                result = subprocess.run(command, input=input_data, capture_output=True, timeout=timeout, check=False)
            except subprocess.TimeoutExpired as error:
                output = safe_output((error.stdout or b'') + b'\n' + (error.stderr or b''))
                raise OSError(f"SSH operation timed out after {timeout}s" + ("\n" + output if output else "")) from error
            except OSError as error:
                raise OSError(f"SSH operation failed: {safe_output(error)}") from error
        output = safe_output(result.stdout + b'\n' + result.stderr)
        if result.returncode:
            raise OSError(f"SSH operation failed (exit {result.returncode})" + ("\n" + output if output else ": remote command produced no output"))
        return result.stdout.decode(errors="replace")



class ControlledEndpoints:
    def __init__(self, path: Path | str | None, discovery: SSHDiscovery | None = None):
        self.path = str(path) if path is not None else ":memory:"
        self.discovery = discovery or SSHDiscovery()
        self.lock = threading.RLock()
        self.db = sqlite3.connect(self.path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("""
            CREATE TABLE IF NOT EXISTS controlled_endpoint(
                id TEXT PRIMARY KEY, address TEXT NOT NULL, port INTEGER NOT NULL,
                status TEXT NOT NULL, host_keys TEXT NOT NULL, snapshot TEXT,
                error TEXT, created_at TEXT NOT NULL, discovered_at TEXT,
                UNIQUE(address,port));
        """)

    def close(self):
        with self.lock: self.db.close()

    @staticmethod
    def _public(row) -> dict:
        value = dict(row)
        value["host_keys"] = [{key: item[key] for key in ("bits", "fingerprint", "type")}
                              for item in json.loads(value["host_keys"])]
        value["snapshot"] = json.loads(value["snapshot"]) if value["snapshot"] else None
        return value

    def list(self) -> dict:
        with self.lock:
            rows = self.db.execute("SELECT * FROM controlled_endpoint ORDER BY address,port").fetchall()
            return {"controlled_endpoints": [self._public(row) for row in rows]}

    def get(self, endpoint_id: str) -> dict:
        if not re.fullmatch(r"[0-9a-f]{32}", endpoint_id): raise KeyError("unknown controlled endpoint")
        with self.lock: row = self.db.execute("SELECT * FROM controlled_endpoint WHERE id=?", (endpoint_id,)).fetchone()
        if not row: raise KeyError("unknown controlled endpoint")
        return self._public(row)

    def run(self, endpoint_id: str, command: str, input_data: bytes | None = None, timeout=600) -> str:
        if not isinstance(command, str) or not command or len(command) > 4096:
            raise ValueError("invalid controlled command")
        if not re.fullmatch(r"[0-9a-f]{32}", endpoint_id): raise KeyError("unknown controlled endpoint")
        with self.lock: row = self.db.execute("SELECT * FROM controlled_endpoint WHERE id=?", (endpoint_id,)).fetchone()
        if not row: raise KeyError("unknown controlled endpoint")
        if row["status"] not in {"supported", "unsupported", "discovery_failed"}:
            raise RuntimeError("SSH host key is not confirmed")
        keys = json.loads(row["host_keys"])
        known_hosts = "\n".join(key["known_host"] for key in keys) + "\n"
        return self.discovery.command(row["address"], row["port"], known_hosts, command, input_data, timeout)

    def probe(self, data: dict) -> dict:
        address, port = validate_target(data)
        keys = self.discovery.scan(address, port)
        endpoint_id, created = secrets.token_hex(16), timestamp()
        with self.lock, self.db:
            if self.db.execute("SELECT 1 FROM controlled_endpoint WHERE address=? AND port=?", (address, port)).fetchone():
                raise RuntimeError("controlled endpoint already exists")
            self.db.execute("INSERT INTO controlled_endpoint VALUES(?,?,?,?,?,?,?,?,?)",
                (endpoint_id, address, port, "awaiting_host_key", json.dumps(keys), None, None, created, None))
            row = self.db.execute("SELECT * FROM controlled_endpoint WHERE id=?", (endpoint_id,)).fetchone()
        return self._public(row)

    def confirm(self, endpoint_id: str, fingerprint: str) -> dict:
        if not re.fullmatch(r"[0-9a-f]{32}", endpoint_id): raise KeyError("unknown controlled endpoint")
        with self.lock:
            row = self.db.execute("SELECT * FROM controlled_endpoint WHERE id=?", (endpoint_id,)).fetchone()
        if not row: raise KeyError("unknown controlled endpoint")
        stored = json.loads(row["host_keys"])
        if row["status"] != "awaiting_host_key": raise RuntimeError("host key was already confirmed")
        confirmed = [key for key in stored if key["fingerprint"] == fingerprint]
        if not confirmed:
            raise ValueError("confirmed SSH host-key fingerprint does not match")
        try:
            known_hosts = "\n".join(key["known_host"] for key in confirmed) + "\n"
            snapshot = self.discovery.discover(row["address"], row["port"], known_hosts)
            status, error, discovered = ("supported" if snapshot["supported"] else "unsupported"), None, timestamp()
        except OSError as exc:
            snapshot, status, error, discovered = None, "discovery_failed", str(exc)[:2048], timestamp()
        with self.lock, self.db:
            self.db.execute("UPDATE controlled_endpoint SET status=?,host_keys=?,snapshot=?,error=?,discovered_at=? WHERE id=?",
                (status, json.dumps(confirmed), json.dumps(snapshot) if snapshot else None, error, discovered, endpoint_id))
            result = self.db.execute("SELECT * FROM controlled_endpoint WHERE id=?", (endpoint_id,)).fetchone()
        return self._public(result)

    def refresh(self, endpoint_id: str) -> dict:
        if not re.fullmatch(r"[0-9a-f]{32}", endpoint_id): raise KeyError("unknown controlled endpoint")
        with self.lock:
            row = self.db.execute("SELECT * FROM controlled_endpoint WHERE id=?", (endpoint_id,)).fetchone()
        if not row: raise KeyError("unknown controlled endpoint")
        if row["status"] == "awaiting_host_key": raise RuntimeError("confirm the SSH host key first")
        keys = json.loads(row["host_keys"])
        known_hosts = "\n".join(key["known_host"] for key in keys) + "\n"
        try:
            snapshot = self.discovery.discover(row["address"], row["port"], known_hosts)
            status, error = ("supported" if snapshot["supported"] else "unsupported"), None
        except OSError as exc:
            snapshot, status, error = None, "discovery_failed", str(exc)[:2048]
        with self.lock, self.db:
            self.db.execute("UPDATE controlled_endpoint SET status=?,snapshot=?,error=?,discovered_at=? WHERE id=?",
                (status, json.dumps(snapshot) if snapshot else None, error, timestamp(), endpoint_id))
            result = self.db.execute("SELECT * FROM controlled_endpoint WHERE id=?", (endpoint_id,)).fetchone()
        return self._public(result)
