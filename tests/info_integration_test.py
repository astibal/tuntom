#!/usr/bin/env python3
"""Rootless INFO test over real encrypted UDP and the production stats socket."""
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

binary, ctl = sys.argv[1:3]
rekey = '--rekey' in sys.argv[3:]


def run(root, enabled):
    probe = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    for tid in range(190, 230):
        try:
            probe.bind(('::', 40000 + tid))
            break
        except OSError:
            continue
    else:
        raise AssertionError('no free INFO test tunnel ID')
    probe.close()
    addresses = root / 'addresses'
    addresses.write_text('127.0.0.2\n10.0.0.2\n10.0.0.1\n')
    processes = []
    logs = []
    controls = {}
    try:
        for role in ('server', 'client'):
            log = open(root / (role + '.log'), 'w+')
            logs.append(log)
            control = root / (role + '.ctl')
            controls[role] = control
            args = [binary, role, str(tid), '-']
            if role == 'client':
                args += ['127.0.0.1']
                args += ['--info-field=site= Praha, centrum ', '--info-field', 'note=x=y',
                         '--info-field=unicode=č', '--info-field=empty=']
                if enabled:
                    args += ['--info-msg-enable']
            args += ['--relay-listen', str(root / (role + '.relay')),
                     '--control-socket', str(control), '--no-stats']
            processes.append(subprocess.Popen(args, stdout=log, stderr=log, env={
                **os.environ, 'TUNTOM_SECRET': '0123456789abcdef0123456789abcdef',
                'TUNTOM_TEST_INFO_ADDRESSES': str(addresses)}))

        def stats(role):
            assert all(p.poll() is None for p in processes), 'INFO process exited'
            if not controls[role].exists():
                return {}
            result = subprocess.run([ctl, str(controls[role]), 'show', 'stats'],
                                    capture_output=True, text=True, timeout=3, check=True)
            lines = result.stdout.splitlines()
            assert all('=' in line for line in lines)
            parsed = dict(line.split('=', 1) for line in lines)
            assert len(parsed) == len(lines), 'duplicate stats keys'
            return parsed

        def wait_for(role, predicate, timeout=8):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                snapshot = stats(role)
                if predicate(snapshot):
                    return snapshot
                time.sleep(.05)
            raise AssertionError(f'{role}: condition not reached: {stats(role)}')

        wait_for('client', lambda s: s.get('session_confirmed') == '1')
        wait_for('server', lambda s: s.get('session_confirmed') == '1')
        if enabled:
            received = wait_for('server', lambda s: s.get('peer_info_access') == '10.0.0.1,10.0.0.2')
            assert received['peer_info_site'] == 'Praha, centrum'
            assert received['peer_info_note'] == 'x=y'
            assert received['peer_info_unicode'] == '??'
            assert received['peer_info_empty'] == ''
            assert received['info_msg_enable'] == '0' , 'receiver must not need opt-in'
            assert received['info_msg_peer_received'] == '1'
            assert stats('client')['info_msg_peer_received'] == '0', 'server advertised without opt-in'
            if rekey:
                # The running spoke must enumerate again, rather than cache startup addresses.
                addresses.write_text('127.2.3.4\n192.0.2.10\n')
                fresh = wait_for('server', lambda s: s.get('peer_info_access') == '192.0.2.10', 135)
                assert int(fresh['rekey_completed']) >= 1
        else:
            time.sleep(.2)
            for role in controls:
                snapshot = stats(role)
                assert snapshot['info_msg_peer_received'] == '0'
                assert not any(key.startswith('peer_info_') for key in snapshot)
    except Exception:
        for log in logs:
            log.flush()
            log.seek(0)
            print(log.read(), file=sys.stderr)
        raise
    finally:
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for log in logs:
            log.close()


with tempfile.TemporaryDirectory(prefix='tuntom-info-') as temporary:
    for enabled in (False, True):
        root = Path(temporary) / str(enabled)
        root.mkdir()
        run(root, enabled)
print('PASS: INFO opt-in, UDP, loopback selection, show stats' + (', live rekey refresh' if rekey else ''))
