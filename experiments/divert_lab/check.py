#!/usr/bin/env python3
"""Unprivileged protocol checks against the live experimental switch."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time


def frame(labels, payload, opcode=1):
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), 8 + 8 * len(labels) + len(payload)) + struct.pack("!" + "Q" * len(labels), *labels) + payload


def receive(sock):
    data = sock.recv(66000)
    version, opcode, flags, count, size = struct.unpack("!BBBBI", data[:8])
    assert version == 1 and flags == 0 and size == len(data)
    labels = list(struct.unpack("!" + "Q" * count, data[8:8 + 8 * count]))
    return opcode, labels, data[8 + 8 * count:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="/tmp/tuntom-divert-lab-build")
    args = parser.parse_args()
    build = Path(args.build).resolve()
    subprocess.run([build / "divert-unit"], check=True)
    with tempfile.TemporaryDirectory(prefix="tt-divert-check.") as tmp:
        root = Path(tmp)
        rules, path, trace = root / "rules", root / "switch.sock", root / "trace.jsonl"
        rules.write_text("format 2\nserial 1\nexit exit\nswitch edge, [17, 42] to exit, [99, 42] allow bidir\n")
        process = subprocess.Popen([build / "divert-switch", "--socket", path, "--rules", rules,
                                    "--cookie", "hTX", "--trace", trace,
                                    "--port", "edge=123", "--port", "exit=700", "--port", "dummy=999",
                                    "--port", "divert-in=401", "--port", "divert-out=402"], stderr=subprocess.PIPE)
        clients = {}
        def events():
            text = trace.read_text() if trace.exists() else ""
            return [json.loads(x) for x in text.splitlines() if x.endswith("}")]
        def wait(predicate):
            until = time.monotonic() + 5
            while time.monotonic() < until:
                assert process.poll() is None, process.stderr.read().decode()
                if predicate():
                    return
                time.sleep(0.01)
            raise AssertionError("switch readiness timeout")
        def connect(name):
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            sock.settimeout(2)
            sock.connect(str(path))
            sock.sendall(b"TTP\x01" + bytes([len(name), 0, 0, 0]) + name.encode())
            return sock
        try:
            wait(path.exists)
            # Origin 123 deliberately differs from every connection index.
            for name in ("divert-out", "dummy", "exit", "divert-in", "edge"):
                clients[name] = connect(name)
            wait(lambda: sum(r["event"] == "registered" for r in events()) == 5)
            packet = bytes.fromhex("4500002800000000400600000a0000010a0000029c4146a000000001000000005002ffff00000000")
            clients["edge"].sendall(frame([17, 42], packet))
            assert receive(clients["exit"]) == (2, [99, 42], packet)
            os.kill(process.pid, signal.SIGUSR1)
            wait(lambda: any(r["event"] == "enabled" for r in events()))
            clients["edge"].sendall(frame([17, 42], packet))
            opcode, offered, payload = receive(clients["divert-in"])
            assert opcode == 2 and payload == packet and offered[4:] == [123, 0, 17, 42]
            assert offered[2].to_bytes(8, "big") == b"hTX4\0\0\0\0"
            bypass = offered.copy()
            bypass[5] = 3
            clients["divert-in"].sendall(frame(bypass, packet))
            assert receive(clients["exit"]) == (2, [99, 42], packet)
            onward = offered.copy()
            onward[5] = 1
            clients["divert-out"].sendall(frame(onward, packet))
            _, exited, payload = receive(clients["exit"])
            assert exited[:2] == [99, 42] and exited[2:] == onward[2:] and payload == packet
            clients["exit"].sendall(frame(exited, packet))
            assert receive(clients["divert-out"]) == (2, exited, packet)
            returned = exited.copy()
            returned[5] = 2
            clients["divert-in"].sendall(frame(returned, packet))
            assert receive(clients["edge"]) == (1, [17, 42], packet)
            # Re-register edge while old service envelopes still carry ID 123.
            replacement = connect("edge")
            clients["edge"].close()
            clients["edge"] = replacement
            wait(lambda: sum(r["event"] == "registered" and r["from"] == "edge" for r in events()) == 2)
            clients["divert-in"].sendall(frame(returned, packet))
            assert receive(replacement) == (1, [17, 42], packet)
            unknown = onward.copy()
            unknown[4] = 124
            clients["divert-out"].sendall(frame(unknown, packet))
            truncated = onward.copy()
            truncated[2] = int.from_bytes(b"hTX:\0\0\0\0", "big")
            clients["divert-out"].sendall(frame(truncated, packet))
            wait(lambda: sum(r["event"] == "invalid_drop" for r in events()) == 2)
            clients["divert-out"].sendall(frame(onward, packet))
            assert receive(clients["exit"]) == (2, exited, packet)
            normal = [r for r in events() if r["event"] == "normal" and r["from"].startswith("divert-")]
            assert normal and all(r["logical"] == "edge" for r in normal)
            print("PASS: live switch resume, DVRT return, stable-ID reconnect, malformed/unknown-origin rejection")
        finally:
            for sock in clients.values():
                sock.close()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stderr.close()


if __name__ == "__main__":
    main()
