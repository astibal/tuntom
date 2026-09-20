"""Bounded, on-demand log reads. Never consume pipes, sockets or devices."""
from __future__ import annotations

import os
from pathlib import Path
import re
import select
import stat
import subprocess
import time

from discovery import stat_fields

MAX_LOG = 65536
MAX_LINES = 60


def redact(text):
    text = re.sub(r"(?i)((?:--|[\"'])?(?:cookie|password|passwd|secret|token|private[_-]?key)[\"']?\s*(?:[=:]\s*|\s+))"
                  r"(?:\"[^\"]*\"|'[^']*'|[^\s,;]+)", r"\1[redacted]", text)
    text = re.sub(r"(?i)(Bearer\s+)\S+", r"\1[redacted]", text)
    text = re.sub(r"-----BEGIN [^-]*PRIVATE KEY-----.*?(?:-----END [^-]*PRIVATE KEY-----|$)",
                  "[private key redacted]", text, flags=re.S)
    return re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", text)


def tail_fd(path):
    # O_NONBLOCK + fstat avoids hanging if a descriptor changes during opening.
    if not stat.S_ISREG(os.stat(path).st_mode):
        return None
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            return None
        offset = max(0, info.st_size - MAX_LOG)
        raw = os.pread(fd, MAX_LOG, offset)
        if offset:
            raw = raw.partition(b"\n")[2]
        lines = raw.decode("utf-8", "replace").splitlines()
        return {"text": redact("\n".join(lines[-MAX_LINES:])), "truncated": bool(offset or len(lines) > MAX_LINES),
                "identity": (info.st_dev, info.st_ino)}
    finally:
        os.close(fd)


def journal(endpoint):
    # Restrict records to this boot, PID and process lifetime (PID reuse).
    uptime = float(Path("/proc/uptime").read_text().split()[0])
    since = time.time() - uptime + endpoint.start_ticks / os.sysconf("SC_CLK_TCK")
    command = ["/usr/bin/journalctl", "--quiet", "--no-pager", "--output=short-iso-precise",
               f"--lines={MAX_LINES}", f"--since=@{since:.6f}",
               f"_BOOT_ID={endpoint.id.split(':')[0]}", f"_PID={endpoint.pid}"]
    raw = bytearray()
    deadline = time.monotonic() + 3
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"}) as process:
        try:
            while len(raw) <= MAX_LOG:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise OSError("journal read timed out")
                readable, _, _ = select.select([process.stdout], [], [], remaining)
                if not readable:
                    raise OSError("journal read timed out")
                chunk = os.read(process.stdout.fileno(), min(8192, MAX_LOG + 1 - len(raw)))
                if not chunk:
                    break
                raw.extend(chunk)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    return {"source": "journal", "text": redact(raw[:MAX_LOG].decode("utf-8", "replace")),
            "truncated": len(raw) > MAX_LOG}


def read_logs(endpoint):
    def verify():
        if int(stat_fields(Path(f"/proc/{endpoint.pid}/stat"))[19]) != endpoint.start_ticks:
            raise OSError("process identity changed; refresh discovery")
    verify()
    sources, seen, errors = [], set(), []
    for number in (1, 2):
        try:
            result = tail_fd(f"/proc/{endpoint.pid}/fd/{number}")
            if result and result["identity"] not in seen:
                seen.add(result.pop("identity"))
                sources.append({"source": "stdout" if number == 1 else "stderr", **result})
        except OSError as error:
            errors.append(str(error))
    if not sources:
        try:
            result = journal(endpoint)
            if result["text"].strip():
                sources.append(result)
        except OSError as error:
            errors.append(str(error))
    verify()
    return {"status": "ok" if sources else "unavailable", "sources": sources,
            "errors": errors, "max_lines": MAX_LINES, "max_bytes_per_source": MAX_LOG,
            "redaction": "best effort: named secrets and private-key blocks"}
