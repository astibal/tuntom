#!/usr/bin/env python3
"""Remote ctl over authenticated UDP, without TUN/root requirements."""
import concurrent.futures
import contextlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from switch_ruleset_integration_test import Switch

binary, ctl, switch = sys.argv[1:4]


def run(enabled, authenticated=False):
    with tempfile.TemporaryDirectory(prefix='tuntom-remote-') as tmp, contextlib.ExitStack() as stack:
        root = Path(tmp)
        if authenticated:
            subprocess.run([ctl, 'auth-keygen', str(root/'authority.key'), str(root/'authority.pub'), '63', '7'], check=True)
            assert (root/'authority.key').stat().st_mode & 0o777 == 0o600
        sw = Switch(switch, ctl, root, 'format 1\nserial 1\nswitch server to client allow\n')
        stack.callback(sw.log.close)
        stack.callback(sw.stop)
        probe = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        for tid in range(240, 280):
            try:
                probe.bind(('::', 40000 + tid))
                break
            except OSError:
                continue
        else:
            raise AssertionError('no free tunnel ID')
        probe.close()
        children = []
        for role in ('server', 'client'):
            log = stack.enter_context(open(root / (role + '.log'), 'w+'))
            args = [binary, role, str(tid), '-'] + (['127.0.0.1'] if role == 'client' else [])
            args += ['--switch-socket', str(sw.data), '--switch-port-id', role,
                     '--switch-label', '1', '--control-socket', str(root / (role + '.ctl')),
                     '--no-stats', '--no-pmtud', '--transport-mtu', '600']
            if authenticated:
                args += (['--control-trust-key', str(root/'authority.pub'), '--control-require-level', '3'] if role == 'server' else
                         ['--control-authority-key', str(root/'authority.key')])
            if authenticated and role == 'server' and not enabled:
                args += ['--control-authority-key', str(root/'authority.key')]
            if role == 'client' or enabled:
                args += ['--allow-control-trusted' if authenticated else '--allow-control-all']
            child = subprocess.Popen(args, stdout=log, stderr=log, env={**os.environ, 'TUNTOM_SECRET': '0123456789abcdef0123456789abcdef'})
            children.append((child, log))
            stack.callback(lambda p: (p.terminate(), p.wait(timeout=5)), child)

        def local(role, *command):
            return subprocess.run([ctl, str(root / (role + '.ctl')), *command], capture_output=True, text=True, timeout=5)

        def remote(*command, integrated=False, routed=False):
            prefix = [binary, 'ctl'] if integrated else [ctl]
            return subprocess.run(prefix + ['remote', '--socket', str(root / 'client.ctl'),
                                 '--remote-wait', '100ms', '--remote-retries', '10', *(['--peer'] if routed else []), '---', *command],
                                 capture_output=True, text=True, timeout=15)

        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            for child, log in children:
                if child.poll() is not None:
                    log.seek(0)
                    raise AssertionError(log.read())
            if all((root / (role + '.ctl')).exists() for role in ('server', 'client')):
                if all('session_confirmed=1\n' in local(role, 'show', 'stats').stdout for role in ('server', 'client')):
                    break
            time.sleep(.05)
        else:
            raise AssertionError('session not ready')

        fields = dict(line.split('=', 1) for line in local('server', 'show', 'stats').stdout.splitlines() if '=' in line)
        assert int(fields['control_challenges_tx']) >= 1 if enabled else int(fields['control_challenges_tx']) == 0
        if not enabled:
            outgoing = subprocess.run([ctl, 'remote', '--socket', str(root/'server.ctl'), '---', 'show', 'stats'],
                                      capture_output=True, text=True, timeout=3)
            assert outgoing.returncode != 0 and 'disabled' in outgoing.stderr, outgoing
        routed_stats = remote('show', 'stats', routed=True)
        assert routed_stats.returncode == (0 if enabled else 1), (routed_stats.stdout, routed_stats.stderr)
        if enabled:
            assert 'session_confirmed=1' in routed_stats.stdout and len(routed_stats.stdout)>600
        stats = remote('show', 'stats', integrated=True)
        if not enabled:
            assert stats.returncode == 1, (stats.returncode, stats.stdout, stats.stderr)
            assert 'timed out' in stats.stderr
            assert local('server', 'show', 'stats').returncode == 0
            return
        assert stats.returncode == 0 and len(stats.stdout) > 600, stats.stderr
        assert 'session_confirmed=1' in stats.stdout
        request_id = stats.stderr.split('request_id=')[1].splitlines()[0]
        saved = remote('request', 'status', request_id)
        assert saved.returncode == 0 and saved.stdout == stats.stdout, saved.stderr
        # Input is opaque original text, including a filename resembling an option.
        body = 'format 1\n' + '# unmodified text č\n' * 2000 + 'classify ip4 proto tcp dport 443 to [7]\n'
        source = root / '--remote-wait'
        source.write_text(body)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            upload = pool.submit(remote, 'classifier', 'load', str(source))
            snapshots = [remote('show', 'stats') for _ in range(3)]
            loaded = upload.result()
        assert loaded.returncode == 0 and 'classifier_generation=1' in loaded.stdout, loaded.stderr
        assert all(s.returncode == 0 and 'session_confirmed=1' in s.stdout for s in snapshots)
        routed_loaded = remote('classifier', 'load', str(source), routed=True)
        assert routed_loaded.returncode == 0, routed_loaded.stderr
        shown = remote('classifier', 'show', routed=True)
        assert shown.returncode == 0 and shown.stdout == body, shown.stderr
        # A handler error differs from authorization/admission rejection.
        bad = root / 'bad'; bad.write_text('not a classifier\n')
        failed = remote('classifier', 'load', str(bad))
        assert failed.returncode == 1, failed.stderr
        assert remote('classifier', 'show').stdout == body
        assert remote('rules', 'show').returncode == 255
        # Repeated requests must not fill the sender's eight active slots.
        for _ in range(12):
            repeated = remote('show', 'stats')
            assert repeated.returncode == 0, (repeated.stdout, repeated.stderr)
        malformed = subprocess.run([ctl, 'remote', '--socket', str(root / 'client.ctl'), 'show', 'stats'], capture_output=True, timeout=3)
        assert malformed.returncode == 1


run(False)
run(True)
print('PASS: remote opt-in, CLI separator, multi-block stats/classifier, status, exit codes, local compatibility')

run(True, authenticated=True)
run(False, authenticated=True)
