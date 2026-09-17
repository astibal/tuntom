#!/usr/bin/env python3
"""Remote VIA over an encrypted UDP tunnel, without a remote switch or TUN."""
import os
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.dont_write_bytecode = True
from switch_ruleset_v2_integration_test import Harness
from switch_ruleset_integration_test import packet
from via_integration_test import tcp, decode, context, action

RULES = '''format 3
serial 1
port edge id 123
service smithproxy {
 client-side proxy-in*
 server-side proxy-out*
 relay proxy-link
 stickiness hash
 unavailable drop
}
exit exit
switch edge,[17,...] to exit,[99,...] via [smithproxy] allow bidir
'''


def connect(path, name):
    peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    peer.settimeout(3)
    peer.connect(str(path))
    name = name.encode()
    peer.sendall(b'TTP\x01' + bytes([len(name), 0, 0, 0]) + name)
    return peer


def run(binary, ctl, tunnel, root, expect_queue=False, expect_ipc_retry=False):
    sw = Harness(binary, ctl, root, RULES)
    processes, logs, peers = [], [], []
    try:
        edge, exit_port = sw.port('edge'), sw.port('exit')
        # Choose a currently unused tunnel UDP port.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for tid in range(230, 255):
            try:
                probe.bind(('127.0.0.1', 40000 + tid)); break
            except OSError:
                continue
        else:
            raise AssertionError('no free relay test tunnel ID')
        probe.close()
        remote = root / 'remote'
        def start(server):
            log = open(root / ('hub.log' if server else 'remote.log'), 'a+')
            logs.append(log)
            args = [tunnel, 'server' if server else 'client', str(tid), '-']
            if server:
                args += ['--relay-connect', str(sw.data), '--relay-port-id', 'proxy-link']
            else:
                args += ['127.0.0.1', '--relay-listen', str(remote)]
            control = root / ('hub.ctl' if server else 'remote.ctl')
            control.unlink(missing_ok=True)  # Previous test process has exited.
            args += ['--no-stats', '--control-socket', str(control)]
            process = subprocess.Popen(args, stdout=log, stderr=log,
                env={**os.environ, 'TUNTOM_SECRET': '0123456789abcdef0123456789abcdef'})
            processes.append(process)
            return process
        hub, distant = start(True), start(False)
        deadline = time.monotonic() + 5
        while not remote.exists():
            assert all(p.poll() is None for p in processes), 'tunnel startup failed'
            assert time.monotonic() < deadline
            time.sleep(.02)
        client = connect(remote, 'proxy-in0~via:c:smithproxy#0')
        server = connect(remote, 'proxy-out0~via:s:smithproxy#0')
        peers += [client, server]
        def offer(payload, timeout=8):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                edge.sendall(packet([17,42],payload))
                if select.select([client],[],[],.15)[0]:
                    labels, data = decode(client.recv(70000))
                    assert data == payload
                    assert context(labels)[1:] == (0,0,False)
                    return labels
                assert all(p.poll() is None for p in processes)
            raise AssertionError('remote service did not become available')
        labels = offer(tcp())
        server.sendall(packet(action(labels),tcp()))
        exit_labels, data = decode(exit_port.recv(70000))
        assert data == tcp() and context(exit_labels)[1:] == (65535,3,False)
        # Reverse handshake leg goes through the server side, then client side.
        reply = tcp(True,18)
        exit_port.sendall(packet(exit_labels,reply))
        reverse_labels, data = decode(server.recv(70000))
        assert data == reply and context(reverse_labels)[1:] == (0,0,True)
        client.sendall(packet(action(reverse_labels,True),reply))
        assert decode(edge.recv(70000)) == ([17,42],reply)
        # Full IP-size payload exercises 32-bit IPC reassembly lengths.
        huge = tcp(size=65535)
        large_labels = offer(huge)
        server.sendall(packet(action(large_labels),huge))
        assert decode(exit_port.recv(70000))[1] == huge
        if expect_queue:
            for name in ('hub', 'remote'):
                output = subprocess.check_output([ctl, str(root / (name + '.ctl')), 'show', 'stats'], text=True)
                stats = dict(line.split('=', 1) for line in output.splitlines())
                assert int(stats['udp_tx_queue_eagain']) >= 1, (name, stats)
                assert int(stats['udp_tx_queue_sent']) >= 1, (name, stats)
                for key in ('udp_tx_queue_capacity_drops', 'udp_tx_queue_expired', 'udp_tx_queue_error_drops'):
                    assert stats[key] == '0', (name, key, stats[key])
        # A duplicate local attachment cannot impersonate the relay service.
        duplicate = connect(sw.data,'proxy-in0~via:c:smithproxy#0')
        peers.append(duplicate)
        assert duplicate.recv(100) == b''
        # Loss of the peer expires availability; reconnect must re-register the
        # still-open adapter sockets after the hub creates a new session.
        if expect_ipc_retry:
            snapshots = [sw.stats()]
            for name in ('hub.ctl', 'remote.ctl'):
                raw = subprocess.check_output([ctl, str(root/name), 'show', 'stats'], text=True)
                snapshots.append(dict(line.split('=',1) for line in raw.splitlines() if '=' in line))
            for values in snapshots:
                assert sum(int(v) for k,v in values.items() if k.endswith('_retry_sent')) > 0, values
                assert sum(int(v) for k,v in values.items() if k.endswith(('_retry_capacity_drops','_retry_expired','_retry_error_drops'))) == 0, values
        hub.terminate(); hub.wait(timeout=5); processes.remove(hub)
        time.sleep(3.3)
        edge.sendall(packet([17,42],tcp(sport=23456)))
        assert not select.select([client,server,exit_port],[],[],.2)[0]
        hub = start(True)
        offer(tcp(sport=34567), timeout=30)
        assert int(sw.stats()['connections_current']) == 3, 'relay created per-channel switch sockets'
    except Exception:
        for log in logs:
            log.flush(); log.seek(0); print(log.read(),file=sys.stderr)
        raise
    finally:
        for peer in peers: peer.close()
        for process in processes:
            if process.poll() is None: process.terminate()
            process.wait(timeout=5)
        for log in logs: log.close()
        sw.stop()


if __name__ == '__main__':
    st, mp, ctl, tunnel = sys.argv[1:5]
    expect_queue = sys.argv[5:] == ['--expect-queue']
    for binary in (st,mp):
        with tempfile.TemporaryDirectory(prefix='tuntom-relay-') as root:
            run(binary,ctl,tunnel,Path(root),expect_queue,sys.argv[5:] == ['--expect-ipc-retry'])
        print('PASS remote IPC relay:',Path(binary).name)
