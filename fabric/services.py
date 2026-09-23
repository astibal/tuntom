"""Optional administrator knowledge about managed services.

This store enriches discovered runtime state.  It is never an authority for
forwarding or classifier state and can be removed without affecting the fabric.
"""
from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import threading
from urllib.parse import urlsplit


KINDS = {"external", "internal", "via", "network", "other"}


def default_services_path() -> Path:
    root = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state"))
    return root / "tuntom-fabric/services.sqlite"


def timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def normalize_label(value) -> str:
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise ValueError("labels must be uint64 decimal or hexadecimal values")
    text = str(value)
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,19}|0[xX][0-9a-fA-F]{1,16})", text):
        raise ValueError(f"invalid uint64 label: {text[:80]}")
    number = int(text, 0)
    if not 0 <= number <= 0xffffffffffffffff:
        raise ValueError(f"label is outside uint64: {text[:80]}")
    return str(number)


def validate(data: dict) -> dict:
    allowed = {"id", "generation", "name", "kind", "description", "labels", "peek_targets"}
    if not isinstance(data, dict) or set(data) - allowed:
        raise ValueError("invalid managed service fields")
    name = data.get("name")
    if not isinstance(name, str) or not 1 <= len(name.strip()) <= 96:
        raise ValueError("name must contain 1..96 characters")
    kind = data.get("kind", "other")
    if kind not in KINDS:
        raise ValueError("invalid managed service kind")
    description = data.get("description", "")
    if not isinstance(description, str) or len(description) > 4096:
        raise ValueError("description must be a string of at most 4096 characters")
    raw_labels = data.get("labels", [])
    if not isinstance(raw_labels, list) or len(raw_labels) > 128:
        raise ValueError("labels must be an array of at most 128 values")
    labels = list(dict.fromkeys(normalize_label(value) for value in raw_labels))
    raw_targets = data.get("peek_targets", [])
    if not isinstance(raw_targets, list) or len(raw_targets) > 32:
        raise ValueError("peek_targets must be an array of at most 32 targets")
    targets = []
    seen_urls = set()
    for item in raw_targets:
        if not isinstance(item, dict) or set(item) - {"url", "interval"}:
            raise ValueError("invalid Peek target fields")
        url, interval = item.get("url"), item.get("interval", 60)
        if not isinstance(url, str) or len(url) > 2048:
            raise ValueError("Peek target URL must be a string of at most 2048 characters")
        parsed = urlsplit(url)
        if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
            raise ValueError("Peek targets must be HTTPS URLs without credentials or fragments")
        if not isinstance(interval, int) or isinstance(interval, bool) or not 10 <= interval <= 86400:
            raise ValueError("Peek interval must be 10..86400 seconds")
        if url not in seen_urls:
            seen_urls.add(url)
            targets.append({"url": url, "interval": interval})
    service_id = data.get("id")
    if service_id is not None and (not isinstance(service_id, str) or not re.fullmatch(r"[0-9a-f]{32}", service_id)):
        raise ValueError("invalid managed service id")
    generation = data.get("generation")
    if generation is not None and (not isinstance(generation, int) or isinstance(generation, bool) or generation < 1):
        raise ValueError("invalid managed service generation")
    return {"id": service_id, "generation": generation, "name": name.strip(), "kind": kind,
            "description": description.strip(), "labels": labels, "peek_targets": targets}


class Services:
    def __init__(self, path: Path | str | None):
        self.path = str(path) if path is not None else ":memory:"
        if self.path != ":memory:":
            directory = Path(self.path).parent
            directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            try:
                directory.chmod(0o700)
            except OSError:
                pass
        self.lock = threading.RLock()
        self.db = sqlite3.connect(self.path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS service(
                id TEXT PRIMARY KEY, name TEXT NOT NULL, kind TEXT NOT NULL,
                description TEXT NOT NULL, generation INTEGER NOT NULL,
                created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS service_label(
                service_id TEXT NOT NULL REFERENCES service(id) ON DELETE CASCADE,
                label TEXT NOT NULL UNIQUE, PRIMARY KEY(service_id,label));
            CREATE TABLE IF NOT EXISTS peek_target(
                service_id TEXT NOT NULL REFERENCES service(id) ON DELETE CASCADE,
                url TEXT NOT NULL, interval INTEGER NOT NULL,
                PRIMARY KEY(service_id,url));
        """)
        if self.path != ":memory:":
            try:
                os.chmod(self.path, 0o600)
            except OSError:
                pass

    def close(self):
        with self.lock:
            self.db.close()

    def _one(self, row) -> dict:
        service_id = row["id"]
        labels = [item[0] for item in self.db.execute(
            "SELECT label FROM service_label WHERE service_id=? ORDER BY length(label),label", (service_id,))]
        targets = [{"url": item[0], "interval": item[1]} for item in self.db.execute(
            "SELECT url,interval FROM peek_target WHERE service_id=? ORDER BY url", (service_id,))]
        return {**dict(row), "labels": labels, "peek_targets": targets}

    def list(self) -> dict:
        with self.lock:
            rows = self.db.execute("SELECT * FROM service ORDER BY lower(name),id").fetchall()
            return {"services": [self._one(row) for row in rows],
                    "label_owners": {label: service_id for label, service_id in self.db.execute(
                        "SELECT label,service_id FROM service_label")}}

    def save(self, data: dict) -> dict:
        value = validate(data)
        with self.lock, self.db:
            current = self.db.execute("SELECT * FROM service WHERE id=?", (value["id"],)).fetchone() if value["id"] else None
            now = timestamp()
            if current:
                if value["generation"] != current["generation"]:
                    raise RuntimeError("managed service changed; reload before saving")
                service_id, generation, created = current["id"], current["generation"] + 1, current["created_at"]
            else:
                if value["id"] is not None:
                    raise KeyError("unknown managed service")
                service_id, generation, created = secrets.token_hex(16), 1, now
            try:
                self.db.execute("INSERT OR REPLACE INTO service VALUES(?,?,?,?,?,?,?)",
                    (service_id, value["name"], value["kind"], value["description"], generation, created, now))
                self.db.execute("DELETE FROM service_label WHERE service_id=?", (service_id,))
                self.db.execute("DELETE FROM peek_target WHERE service_id=?", (service_id,))
                self.db.executemany("INSERT INTO service_label(service_id,label) VALUES(?,?)",
                                    ((service_id, label) for label in value["labels"]))
                self.db.executemany("INSERT INTO peek_target(service_id,url,interval) VALUES(?,?,?)",
                                    ((service_id, item["url"], item["interval"]) for item in value["peek_targets"]))
            except sqlite3.IntegrityError as error:
                match = next((label for label in value["labels"] if self.db.execute(
                    "SELECT 1 FROM service_label WHERE label=? AND service_id<>?", (label, service_id)).fetchone()), None)
                raise ValueError(f"label {match or '?'} already belongs to another managed service") from error
            row = self.db.execute("SELECT * FROM service WHERE id=?", (service_id,)).fetchone()
            return self._one(row)

    def delete(self, service_id: str, generation) -> None:
        if not isinstance(service_id, str) or not re.fullmatch(r"[0-9a-f]{32}", service_id):
            raise KeyError("unknown managed service")
        with self.lock, self.db:
            current = self.db.execute("SELECT generation FROM service WHERE id=?", (service_id,)).fetchone()
            if not current:
                raise KeyError("unknown managed service")
            if generation != current["generation"]:
                raise RuntimeError("managed service changed; reload before deleting")
            self.db.execute("DELETE FROM service WHERE id=?", (service_id,))
