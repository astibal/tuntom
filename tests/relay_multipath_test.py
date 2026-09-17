#!/usr/bin/env python3
"""Two encrypted relay tunnels and the real divert loop, with socket-backed TUNs."""
import os
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
from relay_integration_test import RULES
from switch_ruleset_v2_integration_test import Harness
from switch_ruleset_integration_test import packet
from via_integration_test import tcp, decode, context


def run(binary, ctl, tunnel, adapter, root):
    sw = Harness(binary, ctl, root, RULES.replace('relay proxy-link', 'relay proxy-link*'))
    processes, logs, sockets = [], [], []
    try:
        edge, exit_port = sw.port('edge'), sw.port('exit')
        tids = []
        for tid in range(230, 255):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                try:
                    probe.bind(('127.0.0.1', 40000 + tid))
                except OSError:
                    continue
                tids.append(tid)
            if len(tids) == 2:
                break
        assert len(tids) == 2, 'no free relay test UDP ports'

        def start(name, args, extra_env=None, inherited=()):
            log = open(root / (name + '.log'), 'a+')
            logs.append(log)
            process = subprocess.Popen(args, stdout=log, stderr=log, pass_fds=inherited,
                env={**os.environ, 'TUNTOM_SECRET': '0123456789abcdef0123456789abcdef', **(extra_env or {})})
            processes.append(process)
            return process

        def hub(i):
            return start(f'hub{i}', [tunnel, 'server', str(tids[i]), '-', '--no-stats',
                '--relay-connect', str(sw.data), '--relay-port-id', f'proxy-link{i}'])

        def remote(i):
            return start(f'remote{i}', [tunnel, 'client', str(tids[i]), '-', '127.0.0.1', '--no-stats',
                '--relay-listen', str(root / f'remote{i}.sock'), '--control-socket', str(root / f'remote{i}.ctl')])

        def stats(name):
            text = subprocess.check_output([ctl, str(root / (name + '.ctl')), 'show', 'stats'], text=True, timeout=3)
            return dict(line.split('=', 1) for line in text.splitlines())

        def wait(predicate, timeout=8):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                assert all(p.poll() is None for p in processes), 'child exited'
                if predicate():
                    return
                time.sleep(.03)
            raise AssertionError('readiness timeout')

        hubs, remotes = [], []
        for i in range(2):
            hubs.append(hub(i))
            remotes.append(remote(i))
        tun_in, child_in = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        tun_out, child_out = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        sockets += [tun_in, child_in, tun_out, child_out]
        for sock in sockets:
            sock.settimeout(3)
        # The wrapper only substitutes /dev/net/tun and interface ioctls. The
        # adapter's IPC negotiation, flow cache, polling and reconnects are real.
        start('adapter', [adapter, 'di0', 'do0', '--via-instance', 'smithproxy#0',
            '--divert-in-port', 'proxy-in', '--divert-out-port', 'proxy-out',
            '--relay-path', f'a={root / "remote0.sock"}', '--relay-path', f'b={root / "remote1.sock"}',
            '--control-socket', str(root / 'adapter.ctl')],
            {'TUNTOM_TEST_TUN_FD': str(child_in.fileno()), 'TUNTOM_TEST_TUN_OUT_FD': str(child_out.fileno())},
            (child_in.fileno(), child_out.fileno()))
        wait(lambda: all((root / f'{name}.ctl').exists() for name in ('adapter', 'remote0', 'remote1')))
        wait(lambda: stats('adapter').get('connected_pairs') == '2' and
             all(stats(f'remote{i}').get('relay_acknowledged') == '1' and
                 stats(f'remote{i}').get('relay_channels') == '2' for i in range(2)))
        assert int(sw.stats()['connections_current']) == 4, 'expected one hub IPC connection per tunnel'

        def rx(s, path, side='in'):
            return int(s[f'relay_path_{path}_{side}_ipc_rx_inline_frames'])

        def tx(s, path, side='out'):
            return int(s[f'relay_path_{path}_{side}_ipc_tx_inline_frames'])

        def exchange(sport, expected=None):
            before = stats('adapter')
            request, reply = tcp(sport=sport), tcp(True, 18, sport=sport)
            edge.sendall(packet([17, 42], request))
            assert tun_in.recv(70000) == request
            offered = stats('adapter')
            selected = [p for p in ('a', 'b') if rx(offered, p) == rx(before, p) + 1]
            assert len(selected) == 1, selected
            path = selected[0]
            if expected is not None:
                assert path == expected, (path, expected)
            # A proxy may answer SYN before it has received any reverse OFFER.
            tun_in.sendall(reply)
            assert decode(edge.recv(70000)) == ([17, 42], reply)
            tun_out.sendall(request)
            labels, data = decode(exit_port.recv(70000))
            assert data == request and context(labels)[1:] == (65535, 3, False)
            exit_port.sendall(packet(labels, reply))
            assert tun_out.recv(70000) == reply
            tun_in.sendall(reply)
            assert decode(edge.recv(70000)) == ([17, 42], reply)
            after = stats('adapter')
            assert rx(after, path, 'out') == rx(before, path, 'out') + 1, 'asymmetric flow selection'
            assert tx(after, path) == tx(before, path) + 1, 'forward TUN used another path'
            assert tx(after, path, 'in') == tx(before, path, 'in') + 2, 'reply TUN used another path'
            other = 'b' if path == 'a' else 'a'
            assert tx(after, other) == tx(before, other) and tx(after, other, 'in') == tx(before, other, 'in')
            return path, labels

        assignments, labels_by_port = {}, {}
        for sport in range(30000, 30016):
            assignments[sport], labels_by_port[sport] = exchange(sport)
        assert set(assignments.values()) == {'a', 'b'}, 'flow hash did not use both paths'
        for sport, path in assignments.items():
            exchange(sport, path)  # Payload/direction changes cannot move an unchanged flow.

        hubs[0].terminate(); hubs[0].wait(timeout=5); processes.remove(hubs[0])
        time.sleep(3.3)  # The adapter sockets remain connected; the relay lease expires.
        migrated = next(port for port, path in assignments.items() if path == 'a')
        exchange(migrated, 'b')
        # Also exercise migration learned first from a reverse packet.
        reverse_first = next(port for port, path in assignments.items() if path == 'a' and port != migrated)
        reply = tcp(True, 18, sport=reverse_first)
        before = stats('adapter')
        exit_port.sendall(packet(labels_by_port[reverse_first], reply))
        assert tun_out.recv(70000) == reply
        tun_in.sendall(reply)
        assert decode(edge.recv(70000)) == ([17, 42], reply)
        tun_out.sendall(tcp(sport=reverse_first))
        assert decode(exit_port.recv(70000))[1] == tcp(sport=reverse_first)
        after = stats('adapter')
        assert tx(after, 'b') == tx(before, 'b') + 1 and tx(after, 'b', 'in') == tx(before, 'b', 'in') + 1
        for sport in assignments:
            exchange(sport, 'b')
        assert int(stats('adapter')['flow_entries']) == len(assignments), 'migration allocated extra flow state'

        hubs[0] = hub(0)
        wait(lambda: stats('remote0').get('relay_acknowledged') == '1', timeout=30)
        for sport, path in assignments.items():
            exchange(sport, path)  # Stable IDs recover the original rendezvous mapping.
        # Losing the remote process also closes exactly one adapter socket pair.
        remotes[0].terminate(); remotes[0].wait(timeout=5); processes.remove(remotes[0])
        wait(lambda: stats('adapter').get('connected_pairs') == '1')
        time.sleep(3.3)
        exchange(migrated, 'b')
        # SIGTERM may leave socket files. The listener deliberately refuses to
        # replace them; remove only this test process's paths after wait().
        (root / 'remote0.ctl').unlink(missing_ok=True)
        (root / 'remote0.sock').unlink(missing_ok=True)
        remotes[0] = remote(0)
        wait(lambda: (root / 'remote0.ctl').exists())
        wait(lambda: stats('adapter').get('connected_pairs') == '2' and
             stats('remote0').get('relay_acknowledged') == '1' and
             stats('remote0').get('relay_channels') == '2', timeout=30)
        for sport, path in assignments.items():
            exchange(sport, path)
        assert int(stats('adapter')['flow_entries']) == len(assignments)
        for process in hubs:
            process.terminate(); process.wait(timeout=5); processes.remove(process)
        time.sleep(3.3)
        edge.sendall(packet([17, 42], tcp(sport=31000)))
        assert not select.select([tun_in, tun_out, exit_port], [], [], .2)[0], 'all-down service did not drop'
        result = stats('adapter')
        for key in ('context_miss_drops', 'context_conflict_drops', 'invalid_drops', 'capacity_drops', 'tun_errors'):
            assert result[key] == '0', (key, result[key])
        assert result['relay_path_a_in_ipc_version'] == result['relay_path_b_in_ipc_version'] == '2'
    except Exception:
        for log in logs:
            log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
        raise
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        for sock in sockets:
            sock.close()
        for log in logs:
            log.close()
        sw.stop()


if __name__ == '__main__':
    st, mp, ctl, tunnel, adapter = sys.argv[1:]
    for binary in (st, mp):
        with tempfile.TemporaryDirectory(prefix='tt-relay-mp-') as root:
            run(binary, ctl, tunnel, adapter, Path(root))
        print('PASS multipath relay:', Path(binary).name, flush=True)
