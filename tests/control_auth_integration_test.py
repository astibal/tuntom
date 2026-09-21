#!/usr/bin/env python3
"""Pinned CONTROL on both switches and real adapters, without root or TUN."""
import contextlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

st, mp, ctl, adapter = sys.argv[1:]
for binary in (st, mp):
    with tempfile.TemporaryDirectory(prefix='tuntom-auth-') as tmp, contextlib.ExitStack() as stack:
        root = Path(tmp)
        for name in ('authority', 'other'):
            subprocess.run([ctl, 'auth-keygen', str(root/(name+'.key')), str(root/(name+'.pub')), '63', '7'], check=True, capture_output=True)
        generated = (root/'authority.key').read_bytes()
        refused = subprocess.run([ctl, 'auth-keygen', str(root/'authority.key'), str(root/'new.pub'), '63', '7'], capture_output=True)
        assert refused.returncode != 0 and (root/'authority.key').read_bytes() == generated and not (root/'new.pub').exists()
        assert (root/'authority.key').stat().st_mode & 0o777 == 0o600
        pub = (root/'authority.pub').read_text().split()[1]
        (root/'read.pub').write_text(f'x25519 {pub} 1 7\n')
        (root/'rules').write_text('format 1\nserial 1\n')
        children = []

        def start(args, name, fake_tun=False):
            env = os.environ.copy()
            inherited = ()
            if fake_tun:
                tun, peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                stack.callback(tun.close); stack.callback(peer.close)
                env['TUNTOM_TEST_TUN_FD'] = str(peer.fileno())
                inherited = (peer.fileno(),)
            log = stack.enter_context(open(root/(name+'.log'), 'w+'))
            child = subprocess.Popen(args, stdout=log, stderr=log, env=env, pass_fds=inherited)
            children.append((child, log))
            def stop():
                if child.poll() is None:
                    child.terminate(); child.wait(timeout=5)
            stack.callback(stop)

        args = [binary, '--socket', str(root/'data'), '--control-socket', str(root/'switch.ctl'),
                '--rules-file', str(root/'rules'), '--control-authority-key', str(root/'authority.key'),
                '--control-trust-key', str(root/'authority.pub'), '--control-require-level', '3', '--allow-control-trusted']
        if binary == mp:
            args += ['--workers', '2', '--pool-size', '16', '--queue-size', '16']
        start(args, 'switch')
        for name, public in (('target', 'read.pub'), ('wrong', 'other.pub')):
            args = [adapter, name, '--switch-socket', str(root/'data'), '--switch-port-id', name,
                    '--control-socket', str(root/(name+'.ctl')), '--l3-capacity', '16', '--l4-capacity', '16',
                    '--allow-control-trusted', '--control-trust-key', str(root/public), '--control-require-level', '3']
            if name == 'target':
                args += ['--control-require-authority', pub, '--control-authority-key', str(root/'authority.key')]
            start(args, name, True)

        def routed(source, port, *command):
            return subprocess.run([ctl, 'switch', str(root/(source+'.ctl')), '--port', port, '--remote-retries', '3',
                                   '--remote-wait', '50ms', '---', *command], capture_output=True, text=True, timeout=5)

        deadline = time.monotonic()+10
        while True:
            for child, log in children:
                if child.poll() is not None:
                    log.seek(0); raise AssertionError(log.read())
            result = routed('switch', 'target', 'show', 'stats')
            if result.returncode == 0:
                break
            assert time.monotonic()<deadline, result.stderr
            time.sleep(.05)
        assert 'component=adapter\n' in result.stdout
        metrics = dict(line.split('=',1) for line in result.stdout.splitlines() if '=' in line)
        assert metrics['control_access']=='allow-trusted' and metrics['control_trusted_keys']==pub, metrics
        assert metrics['control_authority_keys']==pub and metrics['control_discover_enabled']=='1', metrics
        # Adapters expose the same local routed API; the CLI's switch mode
        # intentionally requires component=switch, so exercise the raw API here.
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as reverse:
            reverse.settimeout(5); reverse.connect(str(root/'target.ctl'))
            reverse.sendall(b'routed 3 50 0206737769746368 show stats')
            assert reverse.recv(256).startswith(b'REQUEST ')
            header = reverse.recv(256).split()
            assert header[0] == b'OK', header
            data = b''
            while len(data) < int(header[1]):
                data += reverse.recv(16384)
            assert b'component=switch\n' in data
            assert b'control_access=allow-trusted\n' in data
            assert ('control_authority_keys='+pub+'\n').encode() in data
        wrong = routed('switch', 'wrong', 'show', 'stats')
        assert wrong.returncode != 0, 'untrusted authority accepted despite pinning'
        denied = routed('switch', 'target', 'classifier', 'disable')
        assert denied.returncode != 0, 'read-only key authorized modification in trusted mode'
        local = subprocess.run([ctl, str(root/'target.ctl'), 'show', 'stats'], capture_output=True, text=True, timeout=5)
        assert local.returncode == 0, 'local access must survive rejected remote auth'
        for _ in range(36):
            result = routed('switch', 'target', 'show', 'stats')
            assert result.returncode == 0, result.stderr
        discovery = routed('switch', 'target', 'discover')
        assert discovery.returncode == 0 and '\tFOUND\t' in discovery.stdout, discovery.stderr
        wrong_discovery = routed('switch', 'wrong', 'discover')
        assert '\tFOUND\t' not in wrong_discovery.stdout, 'discovery bypassed node pinning'
        fanout = subprocess.run([ctl, 'switch', str(root/'switch.ctl'), '---', 'discover'],
                                capture_output=True, text=True, timeout=5)
        assert fanout.returncode == 0 and 'port:target\tFOUND\t' in fanout.stdout, fanout
        assert 'port:wrong\tFOUND\t' not in fanout.stdout, 'untrusted branch disclosed node'
        print('PASS authenticated switch/adapter CONTROL, pinning, caps, keygen and session reclamation:', Path(binary).name)
