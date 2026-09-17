"""Tuntom SOCK_SEQPACKET client; also runs over SSH using Python's stdlib only."""
from __future__ import annotations

import json
from pathlib import Path
import re
import socket
import struct
import sys
import time

MAX_BODY = 1024 * 1024
MAX_FLOWS = 256 * 1024 * 1024
CHUNK = 16384


class ControlError(Exception):
    """A well-formed rejection returned by the daemon."""


def query(path, operation, body="", timeout=4, expected_pid=None, expected_start_ticks=None):
    if operation not in ("stats", "flows", "show", "check", "load"):
        raise ValueError("unknown control operation")
    if not isinstance(body, str):
        raise ValueError("rules must be text")
    payload = body.encode("utf-8")
    if len(payload) > MAX_BODY:
        raise ValueError("rules exceed 1 MiB")
    if operation in ("stats", "flows", "show") and payload:
        raise ValueError("unexpected control body")
    deadline = time.monotonic() + timeout
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        def remaining():
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("control request timed out")
            connection.settimeout(left)

        def receive(limit):
            remaining()
            data, _, flags, _ = connection.recvmsg(limit)
            if not data or flags & socket.MSG_TRUNC:
                raise OSError("incomplete or oversized control response")
            return data

        def send(data):
            remaining()
            if connection.send(data) != len(data):
                raise OSError("incomplete control request")

        remaining()
        connection.connect(path)
        if expected_pid is not None:
            peer_pid, _, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if peer_pid != expected_pid:
                raise OSError("control socket belongs to a different process; refresh discovery")
            if expected_start_ticks is not None:
                stat = Path(f"/proc/{peer_pid}/stat").read_text()
                if int(stat[stat.rindex(")") + 2:].split()[19]) != expected_start_ticks:
                    raise OSError("control process has restarted; refresh discovery")
        command = f"show {operation}" if operation in ("stats", "flows") else f"rules {operation} {len(payload)}"
        send(command.encode())
        for offset in range(0, len(payload), CHUNK):
            send(payload[offset:offset + CHUNK])
        if operation == "stats":
            response = receive(65536).decode("utf-8")
            if response.startswith("error="):
                raise ControlError(response.strip())
            return response
        header = receive(256)
        match = re.fullmatch(rb"(OK|ERROR) ([0-9]{1,9})\n?", header)
        if not match or int(match[2]) > (MAX_FLOWS if operation == "flows" else MAX_BODY):
            raise OSError("invalid framed control response")
        length = int(match[2])
        result = bytearray()
        while len(result) < length:
            result.extend(receive(min(CHUNK, length - len(result))))
        response = result.decode("utf-8")
        if match[1] == b"ERROR":
            raise ControlError(response.strip())
        return response


def main():
    try:
        request = json.loads(sys.stdin.buffer.read(MAX_BODY * 6 + 4096))
        result = {"ok": True, "text": query(**request)}
    except ControlError as error:
        result = {"ok": False, "kind": "rejected", "error": str(error)}
    except (OSError, ValueError, TypeError, UnicodeError) as error:
        result = {"ok": False, "kind": "transport", "error": str(error)}
    print(json.dumps(result))


if __name__ == "__main__":
    main()
