#!/usr/bin/env python3
"""Shared VIA contexts across independent adapter processes and V5 relay paths."""
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
        assert len(tids) == 2

        def start(name, args, extra_env=None, inherited=()):
            log = open(root / (name + '.log'), 'a+')
            logs.append(log)
            p = subprocess.Popen(args, stdout=log, stderr=log, pass_fds=inherited,
                env={**os.environ, 'TUNTOM_SECRET': '0123456789abcdef0123456789abcdef', **(extra_env or {})})
            processes.append(p)
            return p

        def stats(name):
            result = subprocess.check_output([ctl, str(root / f'{name}.ctl'), 'show', 'stats'], text=True, timeout=3)
            return dict(line.split('=', 1) for line in result.splitlines())

        def wait(predicate, timeout=10):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                assert all(p.poll() is None for p in processes), 'child exited'
                if predicate():
                    return
                time.sleep(.03)
            raise AssertionError('readiness timeout')

        tuns, child_tuns = [], []
        for i in range(2):
            start(f'hub{i}', [tunnel, 'server', str(tids[i]), '-', '--no-stats',
                '--relay-connect', str(sw.data), '--relay-port-id', f'proxy-link{i}'])
            start(f'remote{i}', [tunnel, 'client', str(tids[i]), '-', '127.0.0.1', '--no-stats',
                '--relay-listen', str(root / f'remote{i}.sock'), '--control-socket', str(root / f'remote{i}.ctl')])
            pairs = [socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET) for _ in range(2)]
            tuns.append([pair[0] for pair in pairs])
            child_tuns.append([pair[1] for pair in pairs])
            for pair in pairs:
                sockets.extend(pair)
                for sock in pair:
                    sock.settimeout(3)

        def worker(i):
            return start(f'adapter{i}', [adapter, 'di0', 'do0', '--via-instance', 'smithproxy#0',
                '--divert-in-port', 'proxy-in', '--divert-out-port', 'proxy-out',
                '--relay-path', f'{i}={root / f"remote{i}.sock"}', '--shared-flows', str(root / 'flows'),
                '--control-socket', str(root / f'adapter{i}.ctl')],
                {'TUNTOM_TEST_TUN_FD': str(child_tuns[i][0].fileno()),
                 'TUNTOM_TEST_TUN_OUT_FD': str(child_tuns[i][1].fileno())},
                tuple(s.fileno() for s in child_tuns[i]))

        workers = [worker(i) for i in range(2)]

        def ready():
            return all((root / f'{name}{i}.ctl').exists() for name in ('adapter', 'remote') for i in range(2)) and all(
                stats(f'adapter{i}').get('tun_queues_active') == '1' and
                stats(f'remote{i}').get('relay_acknowledged') == '1' and
                stats(f'remote{i}').get('relay_channels') == '2' for i in range(2))

        wait(ready)

        def offer(side, data):
            readers = [pair[side] for pair in tuns]
            ready_sockets = select.select(readers, [], [], 3)[0]
            assert len(ready_sockets) == 1, 'missing or duplicate offer'
            assert ready_sockets[0].recv(70000) == data
            return readers.index(ready_sockets[0])

        assignments = {}
        for port in range(30000, 30016):
            request, reply = tcp(sport=port), tcp(True, 18, sport=port)
            edge.sendall(packet([17, 42], request))
            selected = offer(0, request)
            assignments[port] = selected
            other = 1 - selected
            # No replication delay or retry: immediately return SYN/ACK through
            # the other process, before either has seen a reverse OFFER.
            tuns[other][0].sendall(reply)
            assert decode(edge.recv(70000)) == ([17, 42], reply)
            tuns[other][1].sendall(request)
            labels, data = decode(exit_port.recv(70000))
            assert data == request and context(labels)[1:] == (65535, 3, False)
            exit_port.sendall(packet(labels, reply))
            assert offer(1, reply) == selected, 'asymmetric flow hash'
            tuns[other][0].sendall(reply)
            assert decode(edge.recv(70000)) == ([17, 42], reply)
        assert set(assignments.values()) == {0, 1}, 'both workers must receive flows'
        assert all(stats(f'adapter{i}')['flow_entries'] == '16' for i in range(2))

        for control in (sw.control, root / 'remote0.ctl', root / 'remote1.ctl'):
            empty = subprocess.run([ctl, str(control), 'show', 'flows'],
                                   capture_output=True, text=True, timeout=10, check=True).stdout
            assert 'tracking=none\n' in empty and 'flow_count=0\n' in empty
        def check_flow_dumps():
            for i in range(2):
                dumped = subprocess.run([ctl, str(root / f'adapter{i}.ctl'), 'show', 'flows'],
                                        capture_output=True, text=True, timeout=10, check=True).stdout
                assert 'tracking=shared_shards' in dumped and 'flow_count=16\n' in dumped
                assert dumped.count('table=shared_routes ') == 16
                assert 'client_saved_labels=[0x0000000000000011,0x000000000000002a]' in dumped

        check_flow_dumps()

        # Killing one worker must not lose the group cache. Return an existing
        # flow using the survivor, without relearning it from a new OFFER.
        workers[0].kill(); workers[0].wait(timeout=5); processes.remove(workers[0])
        port = next(port for port, worker_id in assignments.items() if worker_id == 0)
        reply = tcp(True, 18, sport=port)
        tuns[1][0].sendall(reply)
        assert decode(edge.recv(70000)) == ([17, 42], reply)
        (root / 'adapter0.ctl').unlink(missing_ok=True)
        workers[0] = worker(0)
        wait(ready)
        # A restarted process sees committed state before it learns any packet.
        tuns[0][0].sendall(reply)
        assert decode(edge.recv(70000)) == ([17, 42], reply)
        assert all(stats(f'adapter{i}')['flow_entries'] == '16' for i in range(2))
        check_flow_dumps()
        for i in range(2):
            result = stats(f'adapter{i}')
            assert result['shared_flows'] == '1' and result['connected_pairs'] == '1'
            for key in ('context_miss_drops', 'context_conflict_drops', 'invalid_drops', 'capacity_drops', 'tun_errors'):
                assert result[key] == '0', (i, key, result[key])
    except Exception:
        for log in logs:
            log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
        raise
    finally:
        for p in processes:
            if p.poll() is None:
                p.terminate()
            p.wait(timeout=5)
        for sock in sockets:
            sock.close()
        for log in logs:
            log.close()
        sw.stop()


if __name__ == '__main__':
    st, mp, ctl, tunnel, adapter = sys.argv[1:]
    for binary in (st, mp):
        with tempfile.TemporaryDirectory(prefix='tt-relay-shared-') as root:
            run(binary, ctl, tunnel, adapter, Path(root))
        print('PASS shared relay:', Path(binary).name, flush=True)
