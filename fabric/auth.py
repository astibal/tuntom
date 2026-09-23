"""Small file-backed SCRAM-style authentication for the Fabric web UI."""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import tempfile
import threading
import time

ITERATIONS = 600_000
ROLES = {"admin", "admin-ro"}
USER_RE = re.compile(r"[a-zA-Z0-9_.@-]{1,64}")


def b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def unb64(value: str, size: int | None = None) -> bytes:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise ValueError("invalid base64url value")
    data = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    if size is not None and len(data) != size:
        raise ValueError("invalid value length")
    return data


def password_record(password: str, *, salt: bytes | None = None, iterations: int = ITERATIONS) -> dict:
    """Used for bootstrap/testing; browsers derive the same record with WebCrypto."""
    if not isinstance(password, str) or not 1 <= len(password) <= 1024:
        raise ValueError("password must contain 1..1024 characters")
    salt = salt or secrets.token_bytes(16)
    salted = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations)
    client_key = hmac.digest(salted, b"Client Key", "sha256")
    return {"algorithm": "scram-sha-256", "iterations": iterations, "salt": b64(salt),
            "stored_key": b64(hashlib.sha256(client_key).digest()),
            "server_key": b64(hmac.digest(salted, b"Server Key", "sha256"))}


def validate_record(record: dict) -> dict:
    if not isinstance(record, dict) or record.get("algorithm") != "scram-sha-256":
        raise ValueError("unsupported password record")
    iterations = record.get("iterations")
    if not isinstance(iterations, int) or not 100_000 <= iterations <= 2_000_000:
        raise ValueError("invalid password work factor")
    unb64(record.get("salt", ""), 16)
    unb64(record.get("stored_key", ""), 32)
    unb64(record.get("server_key", ""), 32)
    return {key: record[key] for key in ("algorithm", "iterations", "salt", "stored_key", "server_key")}


class UserStore:
    def __init__(self, path: Path | None):
        self.path = Path(path) if path else None
        self.lock = threading.Lock()
        self.generation = 0
        self.users = {}
        self.load()

    def load(self):
        if not self.path or not self.path.exists():
            return
        if self.path.is_symlink() or not self.path.is_file():
            raise ValueError("users file must be a regular file")
        if self.path.stat().st_mode & 0o077:
            raise ValueError("users file must not be accessible by group or others")
        raw = self.path.read_bytes()
        if len(raw) > 1_048_576:
            raise ValueError("users file is too large")
        data = json.loads(raw)
        if data.get("format") != 1 or not isinstance(data.get("users"), list):
            raise ValueError("unsupported users file")
        users = {}
        for item in data["users"]:
            name = item.get("username") if isinstance(item, dict) else None
            role = item.get("role") if isinstance(item, dict) else None
            if not isinstance(name, str) or not USER_RE.fullmatch(name) or name in users or role not in ROLES:
                raise ValueError("invalid user entry")
            users[name] = {"username": name, "role": role, "enabled": item.get("enabled") is not False,
                           "password": validate_record(item.get("password")),
                           "created_at": str(item.get("created_at", "")), "created_by": str(item.get("created_by", ""))}
        self.generation = int(data.get("generation", 0))
        self.users = users

    def public(self):
        return [{k: user[k] for k in ("username", "role", "enabled", "created_at", "created_by")}
                for user in sorted(self.users.values(), key=lambda item: item["username"])]

    def save_user(self, username, role, enabled, record, actor):
        if not self.path:
            raise ValueError("user management is disabled; set --users-file")
        if not isinstance(username, str) or not USER_RE.fullmatch(username) or role not in ROLES or not isinstance(enabled, bool):
            raise ValueError("invalid user")
        record = validate_record(record) if record is not None else None
        with self.lock:
            existing = self.users.get(username)
            if record is None:
                if not existing:
                    raise ValueError("password is required for a new user")
                record = existing["password"]
            if existing and existing["role"] == "admin" and existing["enabled"] and (role != "admin" or not enabled) and sum(
                    user["role"] == "admin" and user["enabled"] for user in self.users.values()) == 1:
                raise ValueError("cannot disable or demote the last enabled admin")
            self.users[username] = {"username": username, "role": role, "enabled": enabled, "password": record,
                                    "created_at": existing["created_at"] if existing else time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                                    "created_by": existing["created_by"] if existing else actor}
            self.generation += 1
            self._write()

    def delete(self, username):
        with self.lock:
            if username not in self.users:
                raise KeyError(username)
            if self.users[username]["role"] == "admin" and self.users[username]["enabled"] and sum(
                    user["role"] == "admin" and user["enabled"] for user in self.users.values()) == 1:
                raise ValueError("cannot remove the last enabled admin")
            del self.users[username]
            self.generation += 1
            self._write()

    def _write(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        data = {"format": 1, "generation": self.generation, "users": list(self.users.values())}
        fd, name = tempfile.mkstemp(prefix=self.path.name + ".", dir=self.path.parent)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump(data, handle, ensure_ascii=False, indent=2)
                handle.write("\n"); handle.flush(); os.fsync(handle.fileno())
            os.replace(name, self.path)
        finally:
            if os.path.exists(name): os.unlink(name)


class AuthManager:
    def __init__(self, token: str, users_file: Path | None = None):
        self.users = UserStore(users_file)
        self.bootstrap = password_record(token, salt=secrets.token_bytes(16))
        self.fake = password_record(secrets.token_urlsafe(24), salt=secrets.token_bytes(16))
        self.challenges = {}
        self.sessions = {}
        self.lock = threading.Lock()

    @staticmethod
    def _message(username, client_nonce, combined_nonce, salt, iterations):
        return f"n={username}\nr={client_nonce}\ns={salt}\ni={iterations}\nr={combined_nonce}".encode()

    def challenge(self, username, client_nonce, bootstrap=False):
        if not isinstance(client_nonce, str) or not re.fullmatch(r"[A-Za-z0-9_-]{22,128}", client_nonce):
            raise ValueError("invalid client nonce")
        user = None if bootstrap else self.users.users.get(username)
        record = self.bootstrap if bootstrap else (user or {}).get("password")
        valid = bootstrap or bool(user and user["enabled"])
        if record is None:
            record = self.fake
        cid, server_nonce = secrets.token_urlsafe(24), secrets.token_urlsafe(24)
        combined = client_nonce + server_nonce
        message = self._message("bootstrap" if bootstrap else username, client_nonce, combined,
                                record["salt"], record["iterations"])
        with self.lock:
            self._prune()
            if len(self.challenges) >= 2048:
                self.challenges.pop(next(iter(self.challenges)))
            self.challenges[cid] = {"expires": time.time() + 60, "record": record, "valid": valid,
                                    "username": "bootstrap" if bootstrap else username, "auth_method":"bootstrap" if bootstrap else "account",
                                    "role": "admin" if bootstrap else (user or {}).get("role"), "message": message}
        return {"challenge_id": cid, "nonce": combined, "salt": record["salt"], "iterations": record["iterations"]}

    def login(self, challenge_id, proof):
        with self.lock:
            challenge = self.challenges.pop(challenge_id, None)
        if not challenge or challenge["expires"] < time.time():
            raise PermissionError("login challenge expired")
        record, message = challenge["record"], challenge["message"]
        try:
            supplied = unb64(proof, 32)
            stored = unb64(record["stored_key"], 32)
            signature = hmac.digest(stored, message, "sha256")
            client_key = bytes(a ^ b for a, b in zip(supplied, signature))
            okay = hmac.compare_digest(hashlib.sha256(client_key).digest(), stored)
        except (ValueError, TypeError):
            okay = False
        if not okay or not challenge["valid"]:
            raise PermissionError("invalid username or password")
        server_key = unb64(record["server_key"], 32)
        sid = secrets.token_urlsafe(32)
        session_key = hmac.digest(server_key, b"Session Key\0" + message, "sha256")
        with self.lock:
            self.sessions[sid] = {"key": session_key, "username": challenge["username"],
                                  "role": challenge["role"], "auth_method":challenge["auth_method"], "expires": time.time() + 12 * 3600, "nonces": {}}
        return {"session_id": sid, "username": challenge["username"], "role": challenge["role"], "auth_method":challenge["auth_method"],
                "expires_in": 12 * 3600, "server_signature": b64(hmac.digest(server_key, message, "sha256")),
                "users_enabled": self.users.path is not None}

    def verify(self, sid, timestamp, nonce, signature, method, path, body):
        with self.lock:
            self._prune()
            session = self.sessions.get(sid)
            if not session: raise PermissionError("invalid session")
            try: stamp = int(timestamp)
            except (TypeError, ValueError): raise PermissionError("invalid request timestamp")
            if abs(time.time() - stamp) > 30 or not re.fullmatch(r"[A-Za-z0-9_-]{16,128}", nonce or ""):
                raise PermissionError("stale or invalid request")
            if nonce in session["nonces"]: raise PermissionError("request replayed")
            canonical = f"{method}\n{path}\n{timestamp}\n{nonce}\n{hashlib.sha256(body).hexdigest()}".encode()
            expected = b64(hmac.digest(session["key"], canonical, "sha256"))
            if not hmac.compare_digest(expected, signature or ""): raise PermissionError("invalid request signature")
            session["nonces"][nonce] = stamp
            return {"username": session["username"], "role": session["role"], "auth_method":session["auth_method"]}

    def logout(self, sid):
        with self.lock:
            self.sessions.pop(sid, None)

    def revoke_user(self, username):
        with self.lock:
            for records in (self.sessions, self.challenges):
                for key, entry in list(records.items()):
                    if entry.get("username") == username and entry.get("auth_method") == "account":
                        del records[key]

    def _prune(self):
        now = time.time()
        self.challenges = {key: value for key, value in self.challenges.items() if value["expires"] >= now}
        self.sessions = {key: value for key, value in self.sessions.items() if value["expires"] >= now}
        for session in self.sessions.values():
            session["nonces"] = {key: stamp for key, stamp in session["nonces"].items() if stamp >= now - 60}
