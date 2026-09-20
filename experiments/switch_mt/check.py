#!/usr/bin/env python3
"""Draft-specific protocol/backpressure checks, plus real tuntom UDP integration."""
import argparse
from contextlib import contextmanager
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

from bench import parse_stats, snapshot, stop


def frame(labels, payload, opcode=1):
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), 8+8*len(labels)+len(payload)) + struct.pack("!"+"Q"*len(labels), *labels)+payload


@contextmanager
def fixture(binary, extra=()):
    with tempfile.TemporaryDirectory(prefix="tuntom-mt-check.") as temp:
        data, ctl, final = [str(Path(temp)/part) for part in ("data", "control", "final")]
        proc = subprocess.Popen([binary, "--socket", data, "--control-socket", ctl,
                                 "--stats-file", final, "--port", "a", "--port", "b",
                                 "--route", "a:17=b:99", "--route", "b:23=a:101", "--exit-port", "b",
                                 "--pool-size", "16", "--queue-size", "4", *extra],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        clients = []
        try:
            until = time.monotonic()+5
            while not Path(ctl).exists():
                if proc.poll() is not None or time.monotonic()>until:
                    raise RuntimeError("startup failed")
                time.sleep(.01)
            for name in (b"a", b"b"):
                peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                peer.settimeout(3)
                peer.connect(data)
                peer.sendall(b"TTP\x01"+bytes([len(name)])+b"\x00\x00\x00"+name)
                clients.append(peer)
            until = time.monotonic()+5
            while not snapshot(ctl)["workers_started"]:
                assert time.monotonic()<until
                time.sleep(.01)
            yield clients, ctl
        finally:
            stop(proc)
            for peer in clients:
                peer.close()
            assert proc.returncode == 0, proc.stderr.read()
            stats = parse_stats(Path(final).read_text())
            assert stats["buffers_in_use"] == 0, stats


def protocol(binary):
    with fixture(binary, ["--default-back=on"]) as ((a, b), ctl):
        for size in (1, 64, 1500, 9000, 65535):
            payload = bytes(i%251 for i in range(size))
            labels = [17, 2, 3, 4, 5, 6, 7, 8]
            a.sendall(frame(labels, payload))
            assert b.recv(70000) == frame([99, *labels[1:]], payload, 2)
            b.sendall(frame([23], payload))
            assert a.recv(70000) == frame([101], payload)
        a.sendall(frame([404, 8], b"default"))
        assert a.recv(70000) == frame([404, 8], b"default", 2)
        a.sendall(b"malformed")
        a.sendall(frame([17], b"wrong incoming opcode", 2))
        a.sendall(frame([17], b"after invalid"))
        assert b.recv(70000) == frame([99], b"after invalid", 2)
        stats = snapshot(ctl)
        assert stats["malformed_frames"] == 2 and stats["default_back"] == 1, stats
    print("PASS: v1 framing, 1/8 labels, complete payload through 65535 B, EXIT, default-back, malformed", flush=True)


def pressure(binary):
    with fixture(binary) as ((a, b), ctl):
        a.setblocking(False)
        accepted = 0
        payload = b"P"*9000
        until = time.monotonic()+.35
        while time.monotonic()<until:
            try:
                a.send(frame([17], struct.pack("!Q", accepted)+payload))
                accepted += 1
            except BlockingIOError:
                time.sleep(.0001)
        stats = snapshot(ctl)
        assert stats["send_eagain"]>0 and stats["queue_full_drops"]>0, stats
        assert stats["send_backpressure_drops"] == 0, stats
        # Reopen the receive path and verify order across EAGAIN + ring wrap.
        b.settimeout(.05)
        previous = -1
        received = 0
        until = time.monotonic()+5
        while time.monotonic()<until:
            try:
                wire = b.recv(70000)
                seq = struct.unpack_from("!Q", wire, 16)[0]
                assert seq>previous and wire[24:]==payload
                previous = seq
                received += 1
            except TimeoutError:
                stats = snapshot(ctl)
                if stats["frames_rx"]==accepted and stats["buffers_in_use"]==0:
                    break
        else:
            raise AssertionError("pressure did not drain")
        assert received == stats["frames_tx"], stats
        assert accepted-received == stats["queue_full_drops"], stats
        a.settimeout(3)
        a.sendall(frame([17], b"recovered"))
        assert b.recv(70000) == frame([99], b"recovered", 2)
    print("PASS: blocked output, bounded queue drops, EAGAIN retry, FIFO, recovery, all buffers released", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--draft", default="/tmp/tuntom-switch-mt")
    p.add_argument("--tuntom", default="/tmp/tuntom-mt-integration")
    p.add_argument("--ctl", default="/tmp/tuntom-mt-ctl")
    args = p.parse_args()
    protocol(args.draft)
    pressure(args.draft)
    root = Path(__file__).resolve().parents[2]
    subprocess.run(["python3", str(root/"tests/switch_tunnel_test.py"), args.tuntom, args.draft, args.ctl], check=True, timeout=40)
    print("PASS: production tuntom encrypted UDP, PFS, both directions, fragmentation and reassembly", flush=True)


if __name__ == "__main__":
    main()
