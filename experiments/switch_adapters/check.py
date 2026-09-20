#!/usr/bin/env python3
"""Explicit multi-adapter ownership and independent blocked-output recovery."""
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


def isolation(binary):
    rx = tx = 2
    with tempfile.TemporaryDirectory(prefix="tuntom-shared-check.") as directory:
        root = Path(directory)
        data, control, final = (root / name for name in ("data", "control", "final"))
        cmd = [binary, "--socket", str(data), "--control-socket", str(control),
               "--stats-file", str(final), "--rx-workers", str(rx), "--tx-workers", str(tx),
               "--pool-size", "16", "--queue-size", "4"]
        for name in ("a", "b", "c", "d"):
            cmd += ["--port", name]
        cmd += ["--route", "a:17=b:99", "--route", "c:17=d:99"]
        for side in ('rx','tx'):
            for name,owner in [('a',1),('b',0),('c',1),('d',0)]:
                cmd += [f'--{side}-owner',f'{name}:{owner}']
        cmd += ['--exit-port','b','--exit-port','d','--route','b:23=a:77','--route','d:23=c:77']
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
            stats = snapshot(control)
            for i,owner in enumerate([1,0,1,0]):
                for side in ('rx','tx'): assert stats[f'port_{i}_{side}_owner']==owner
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
                assert d.recv(70000) == frame([99],payload,2)
                d.sendall(frame([23],payload))
                assert c.recv(70000) == frame([77],payload)
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
                    if stats["frames_rx"] == accepted+80 and stats["buffers_in_use"] == 0:
                        break
            else:
                raise AssertionError("blocked output did not drain")
            assert received+80 == stats["frames_tx"]
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
    print("PASS: two adapters share RX/TX; one blocked adapter does not stall the other in either direction; POLLOUT resume, FIFO, idle wake, pool reclaim",flush=True)


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--draft',default='/tmp/tuntom-switch-adapters')
    args=p.parse_args()
    protocol(args.draft)
    pressure(args.draft)
    isolation(args.draft)
    for owners,expected in [(['a:0'],'explicit owners need all ports'),
                            (['a:0','b:2'],'invalid explicit owner'),
                            (['a:0','b:0'],'worker has no assigned ports'),
                            (['a:0','a:1'],'duplicate explicit owner')]:
        with tempfile.TemporaryDirectory(prefix='tuntom-owner-check.') as d:
            cmd=[args.draft,'--socket',str(Path(d)/'data'),'--port','a','--port','b','--rx-workers','2']
            for owner in owners:cmd+=['--rx-owner',owner]
            r=subprocess.run(cmd,capture_output=True,text=True,timeout=5)
            assert r.returncode!=0 and expected in r.stderr,(r.returncode,r.stderr)
            assert 'ThreadSanitizer' not in r.stderr,r.stderr
    print('PASS: incomplete, out-of-range, empty-worker and duplicate mappings rejected',flush=True)

if __name__=='__main__':main()
