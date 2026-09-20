#!/usr/bin/env python3
"""Shared-worker correctness, especially isolation of blocked outputs."""
import argparse
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "switch_mt"))
from bench import snapshot, stop, parse_stats
from check import frame, protocol, pressure


def isolation(binary, rx, tx, dedicated=False):
    with tempfile.TemporaryDirectory(prefix="tuntom-shared-check.") as directory:
        root = Path(directory)
        data, control, final = (root / name for name in ("data", "control", "final"))
        cmd = [binary, "--socket", str(data), "--control-socket", str(control),
               "--stats-file", str(final), "--rx-workers", str(rx), "--tx-workers", str(tx),
               "--pool-size", "16", "--queue-size", "4"]
        for name in ("a", "b", "c", "d"):
            cmd += ["--port", name]
        cmd += ["--route", "a:17=b:99", "--route", "c:17=d:99"]
        if dedicated:
            cmd += ["--dedicated-rx", "a", "--dedicated-tx", "b"]
        process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        peers = []
        try:
            until = time.monotonic()+8
            while not control.exists():
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                assert time.monotonic() < until
                time.sleep(.01)
            for name in (b"a", b"b", b"c", b"d"):
                peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                peer.settimeout(2)
                peer.connect(str(data))
                peer.sendall(b"TTP\x01"+bytes([len(name)])+b"\x00\x00\x00"+name)
                peers.append(peer)
            until = time.monotonic()+8
            while not snapshot(control)["workers_started"]:
                assert time.monotonic() < until
                time.sleep(.01)
            a,b,c,d = peers
            a.setblocking(False)
            accepted = 0
            body = b"P"*9000
            until = time.monotonic()+.3
            while time.monotonic()<until:
                try:
                    a.send(frame([17], struct.pack("!Q",accepted)+body))
                    accepted += 1
                except BlockingIOError:
                    time.sleep(.0001)
            assert snapshot(control)["send_eagain"] > 0
            # b remains unreadable to its peer. c->d must continue even with
            # only one RX and one TX worker, including wake-after-idle cycles.
            for i in range(40):
                payload = struct.pack("!I",i)+b"healthy"
                c.sendall(frame([17],payload))
                assert d.recv(70000) == frame([99],payload)
                if i%5==0: time.sleep(.005)
            # Reopen b without sending more input to it. POLLOUT alone must
            # resume its pending frame and then drain the RX socket and queues.
            received, previous = 0, -1
            b.settimeout(.05)
            until = time.monotonic()+8
            while time.monotonic()<until:
                try:
                    wire = b.recv(70000)
                    seq = struct.unpack_from("!Q",wire,16)[0]
                    assert seq > previous and wire[24:] == body
                    previous = seq
                    received += 1
                except TimeoutError:
                    stats = snapshot(control)
                    if stats["frames_rx"] == accepted+40 and stats["buffers_in_use"] == 0:
                        break
            else:
                raise AssertionError("blocked output did not drain")
            assert received+40 == stats["frames_tx"]
            assert accepted-received == stats["queue_full_drops"]
            assert stats["send_backpressure_drops"] == 0
        finally:
            stop(process)
            for peer in peers: peer.close()
            error = process.stderr.read().decode()
            assert process.returncode == 0, error
            assert not error, error
            stats = parse_stats(final.read_text())
            assert stats["buffers_in_use"] == 0 and stats["shutdown_drops"] == 0
    print(f"PASS: {rx} RX / {tx} TX dedicated={dedicated}, blocked-output isolation, POLLOUT resume, FIFO, idle wake, pool reclaim",flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--draft", default="/tmp/tuntom-switch-workers")
    args = p.parse_args()
    protocol(args.draft)
    pressure(args.draft)
    for rx,tx,ded in ((1,1,False),(2,2,False),(3,1,False),(1,3,False),(2,2,True)):
        isolation(args.draft,rx,tx,ded)


if __name__ == "__main__":
    main()
