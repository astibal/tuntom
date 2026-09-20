#!/usr/bin/env python3
"""IN/OUT-only workers over separate encrypted relays, using real ST/MP switches."""
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
    rules = RULES.replace('relay proxy-link', 'client-relay in-link*\n server-relay out-link*')
    sw = Harness(binary, ctl, root, rules)
    processes, logs, sockets = [], [], []
    names = ('in0', 'in1', 'out0', 'out1')
    workers, tuns, children = {}, {}, {}
    try:
        edge, exit_port = sw.port('edge'), sw.port('exit')

        def start(name, args, env=None, inherited=()):
            log = open(root / f'{name}.log', 'a+')
            logs.append(log)
            p = subprocess.Popen(args, stdout=log, stderr=log, pass_fds=inherited,
                env={**os.environ, 'TUNTOM_SECRET': '0123456789abcdef0123456789abcdef', **(env or {})})
            processes.append(p)
            return p

        def stop(p):
            p.kill()
            p.wait(timeout=5)
            processes.remove(p)

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

        tids = []
        for tid in range(230, 255):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                try:
                    probe.bind(('127.0.0.1', 40000 + tid))
                except OSError:
                    continue
                tids.append(tid)
            if len(tids) == len(names):
                break
        assert len(tids) == len(names)
        for name, tid in zip(names, tids):
            side, index = name[:-1], name[-1]
            start('hub-' + name, [tunnel, 'server', str(tid), '-', '--no-stats',
                '--relay-connect', str(sw.data), '--relay-port-id', f'{side}-link{index}'])
            start('remote-' + name, [tunnel, 'client', str(tid), '-', '127.0.0.1', '--no-stats',
                '--relay-listen', str(root / f'{name}.sock'), '--control-socket', str(root / f'remote-{name}.ctl')])
            a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            a.settimeout(3)
            b.settimeout(3)
            sockets.extend((a, b))
            tuns[name], children[name] = a, b

        def worker(name):
            control = root / f'{name}.ctl'
            control.unlink(missing_ok=True)
            return start(name, [adapter, 'di0', 'do0', '--side', name[:-1], '--via-instance', 'smithproxy#0',
                '--divert-in-port', 'proxy-in', '--divert-out-port', 'proxy-out',
                '--relay-path', f'{name[-1]}={root / f"{name}.sock"}', '--shared-flows', str(root / 'flows'),
                '--control-socket', str(control)],
                {'TUNTOM_TEST_TUN_FD': str(children[name].fileno()),
                 'TUNTOM_TEST_TUN_NAME': 'di0' if name.startswith('in') else 'do0'}, (children[name].fileno(),))

        def ready(active):
            return all((root / f'{name}.ctl').exists() and (root / f'remote-{name}.ctl').exists() and
                stats(name).get('tun_queues_active') == '1' and
                stats('remote-' + name).get('relay_acknowledged') == '1' and
                stats('remote-' + name).get('relay_channels') == '1' for name in active)

        # The IN group may start first, but incomplete instances must not receive offers.
        for name in names[:2]:
            workers[name] = worker(name)
        wait(lambda: ready(names[:2]))
        edge.sendall(packet([17, 42], tcp()))
        assert not select.select(list(tuns.values()) + [exit_port], [], [], .2)[0]
        for name in names[2:]:
            workers[name] = worker(name)
        wait(lambda: ready(names))

        # Side-qualified membership allows in:0 plus out:0, but never a duplicate
        # worker or a live mixture of paired and split workers in the same table.
        common = [adapter, 'di0', 'do0', '--via-instance', 'smithproxy#0',
                  '--divert-in-port', 'proxy-in', '--divert-out-port', 'proxy-out',
                  '--relay-path', f'0={root / "in0.sock"}', '--shared-flows', str(root / 'flows')]
        for side, error in (('in', 'worker ID is already active'), ('both', 'configuration or ABI mismatch')):
            result = subprocess.run([*common, '--side', side], capture_output=True, text=True, timeout=3)
            assert result.returncode == 1 and error in result.stderr, result.stderr

        def offer(side, data):
            available = select.select(list(tuns.values()), [], [], 3)[0]
            assert len(available) == 1, 'missing or duplicate TUN offer'
            name = next(name for name, sock in tuns.items() if sock is available[0])
            assert name.startswith(side), ('wrong TUN side', name, side)
            assert available[0].recv(70000) == data
            return name

        assignments = {'in': set(), 'out': set()}
        for port in range(30000, 30024):
            request, reply = tcp(sport=port), tcp(True, 18, sport=port)
            edge.sendall(packet([17, 42], request))
            chosen_in = offer('in', request)
            assignments['in'].add(chosen_in)
            # Force the kernel's hypothetical queue choice independently of ingress.
            # An OUT worker knows the flow before any upstream reply arrives.
            output = f'out{port % 2}'
            tuns[output].sendall(request)
            labels, data = decode(exit_port.recv(70000))
            assert data == request and context(labels)[1:] == (65535, 3, False)
            tuns['in1' if chosen_in == 'in0' else 'in0'].sendall(reply)
            assert decode(edge.recv(70000)) == ([17, 42], reply)
            exit_port.sendall(packet(labels, reply))
            assignments['out'].add(offer('out', reply))
            tuns[f'in{port % 2}'].sendall(reply)
            assert decode(edge.recv(70000)) == ([17, 42], reply)
        assert assignments == {'in': {'in0', 'in1'}, 'out': {'out0', 'out1'}}, assignments
        for name in names:
            result = stats(name)
            assert result['worker_side'] == name[:-1] and result['connected_paths'] == '1'
            assert result['connected_pairs'] == '0' and result['flow_entries'] == '24'
            assert result['divert_in_connected'] == str(int(name.startswith('in')))
            assert result['divert_out_connected'] == str(int(name.startswith('out')))
            for key in ('context_miss_drops', 'context_conflict_drops', 'invalid_drops', 'capacity_drops', 'tun_errors'):
                assert result[key] == '0', (name, key, result[key])

        # Remove one IN worker: both OUT workers remain usable without their namesake IN.
        stop(workers['in0'])
        wait(lambda: stats('remote-in0')['relay_channels'] == '0' and
             stats('remote-in0')['relay_acknowledged'] == '1')
        for port in (30000, 30001):
            request, reply = tcp(sport=port), tcp(True, 18, sport=port)
            edge.sendall(packet([17, 42], request))
            assert offer('in', request) == 'in1'
            tuns[f'out{port % 2}'].sendall(request)
            labels, data = decode(exit_port.recv(70000))
            assert data == request
            exit_port.sendall(packet(labels, reply))
            offer('out', reply)
            tuns['in1'].sendall(reply)
            assert decode(edge.recv(70000)) == ([17, 42], reply)
        workers['in0'] = worker('in0')
        wait(lambda: ready(names))
        tuns['in0'].sendall(tcp(True, 18, sport=30000))
        assert decode(edge.recv(70000)) == ([17, 42], tcp(True, 18, sport=30000))

        # Losing the entire OUT side makes the service unavailable, without bypass.
        for name in names[2:]:
            stop(workers[name])
        wait(lambda: all(stats('remote-' + name)['relay_channels'] == '0' and
             stats('remote-' + name)['relay_acknowledged'] == '1' for name in names[2:]))
        edge.sendall(packet([17, 42], tcp(sport=31000)))
        assert not select.select(list(tuns.values()) + [exit_port], [], [], .2)[0]
        # Rejoin with one OUT worker: no fixed pairing or equal group sizes required.
        workers['out1'] = worker('out1')
        wait(lambda: ready(('in0', 'in1', 'out1')))
        request = tcp(sport=30000)
        tuns['out1'].sendall(request)
        labels, data = decode(exit_port.recv(70000))
        assert data == request
        exit_port.sendall(packet(labels, tcp(True, 18, sport=30000)))
        assert offer('out', tcp(True, 18, sport=30000)) == 'out1'

        # CLI errors are rejected before creating a TUN or a shared flow group.
        for extra in (['--side', 'invalid'], ['--side', 'in'],
                      ['--side', 'out', '--shared-flows', str(root / 'bad')]):
            result = subprocess.run([adapter, 'di0', 'do0', *extra], capture_output=True, text=True, timeout=3)
            assert result.returncode == 1 and 'TUN' not in result.stderr, result.stderr
        assert not (root / 'bad').exists()
    except Exception:
        for log in logs:
            log.flush()
            log.seek(0)
            print(log.read(), file=sys.stderr)
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
        sw.log.close()


if __name__ == '__main__':
    st, mp, ctl, tunnel, adapter = sys.argv[1:]
    for binary in (st, mp):
        with tempfile.TemporaryDirectory(prefix='tt-relay-split-') as root:
            run(binary, ctl, tunnel, adapter, Path(root))
        print('PASS split-side relay:', Path(binary).name, flush=True)
