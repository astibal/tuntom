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


class ResponseTooLarge(OSError):
    """The announced reply exceeds the caller's bounded receive budget."""


def encode_route(route):
    """Encode a structured CONTROL v2 path; never accept a raw command."""
    if not isinstance(route, list) or len(route) > 16:
        raise ValueError("route must be a list of at most 16 hops")
    result = bytearray()
    for hop in route:
        if isinstance(hop, dict) and set(hop) == {"peer"} and hop["peer"] is True:
            result.extend((1, 0))
        elif isinstance(hop, dict) and set(hop) == {"port"} and isinstance(hop["port"], str) and re.fullmatch(r"[\x21-\x7e]{1,63}", hop["port"]):
            value = hop["port"].encode("ascii")
            result.extend((2, len(value)))
            result.extend(value)
        else:
            raise ValueError("invalid route hop")
    if len(result) > 1024:
        raise ValueError("route exceeds 1024 bytes")
    return result.hex() or "-"


def query(path, operation, body="", timeout=4, expected_pid=None, expected_start_ticks=None, max_response_bytes=None,
          route=None, on_accepted=None):
    if operation not in ("stats", "flows", "show", "check", "load", "discover", "classifier-show", "classifier-check", "classifier-load", "classifier-load-flush", "classifier-disable"):
        raise ValueError("unknown control operation")
    encoded_route = encode_route(route) if route is not None else None
    if operation == "discover" and route is None:
        raise ValueError("discover requires routed control")
    if not isinstance(body, str):
        raise ValueError("rules must be text")
    payload = body.encode("utf-8")
    if len(payload) > MAX_BODY:
        raise ValueError("rules exceed 1 MiB")
    if operation in ("stats", "flows", "show", "discover", "classifier-show", "classifier-disable") and payload:
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
        if operation.startswith("classifier-"):
            command = f"classifier {operation.removeprefix('classifier-')} {len(payload)}"
        if operation == "discover":
            command = "discover"
        if encoded_route is not None:
            command = f"routed 5 250 {encoded_route} {command}"
        send(command.encode())
        for offset in range(0, len(payload), CHUNK):
            send(payload[offset:offset + CHUNK])
        if encoded_route is not None:
            accepted = receive(256)
            match = re.fullmatch(rb"REQUEST ([0-9a-f]{32})\n?", accepted)
            if not match:
                raise ControlError("routed request was not accepted by daemon")
            if on_accepted is not None:
                on_accepted(match[1].decode("ascii"))
        if operation == "stats" and encoded_route is None:
            response = receive(65536).decode("utf-8")
            if response.startswith("error="):
                raise ControlError(response.strip())
            return response
        header = receive(256)
        match = re.fullmatch(rb"(OK|ERROR|REJECTED) ([0-9]{1,9})\n?", header)
        if not match or int(match[2]) > (MAX_FLOWS if operation == "flows" else MAX_BODY):
            raise OSError("invalid framed control response")
        length = int(match[2])
        if max_response_bytes is not None and length > max_response_bytes:
            raise ResponseTooLarge("control response exceeds configured receive limit")
        result = bytearray()
        while len(result) < length:
            result.extend(receive(min(CHUNK, length - len(result))))
        response = result.decode("utf-8")
        if match[1] != b"OK":
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
