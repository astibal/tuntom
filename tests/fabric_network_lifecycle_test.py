#!/usr/bin/env python3
"""Real systemd lifecycle with TUNs in a disposable network namespace.

Run as root on a test host: python3 tests/fabric_network_lifecycle_test.py /path/to/tuntom
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'fabric'))
sys.path.insert(0, str(REPO / 'fabric/tests'))
from tunnel_deployments import render_bundle, validate
from test_tunnel_deployments import draft


def run(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True)


def main(binary):
    if os.geteuid() != 0 or not Path('/run/systemd/system').is_dir():
        print('SKIP: requires root and systemd'); return 77
    holder = subprocess.Popen(['unshare', '--net', 'sleep', '300'])
    created = []
    services = []
    with tempfile.TemporaryDirectory(prefix='tuntom-lifecycle-', dir='/var/tmp') as directory:
        root = Path(directory)
        root.chmod(0o755)
        try:
            time.sleep(0.2)
            assert holder.poll() is None
            def net(*args, check=True):
                return run('nsenter', '-t', str(holder.pid), '-n', *args, check=check)
            net('ip', 'link', 'set', 'lo', 'up')
            net('ip', 'link', 'add', 'fallback', 'type', 'dummy')
            net('ip', 'link', 'set', 'fallback', 'up')
            net('ip', 'addr', 'add', '192.0.2.1/24', 'dev', 'fallback')
            net('ip', 'route', 'add', 'default', 'dev', 'fallback')
            data = draft(); data['count'] = 2
            for side in ('side_a', 'side_b'): data[side]['runtime'] = 'systemd'
            config, _ = validate(data); config['deployment_id'] = uuid.uuid4().hex
            bundle = render_bundle(config, {config[s]['endpoint_id']: 'host' for s in ('side_a', 'side_b')}, 'unused', str(root))
            for name, content in bundle.items():
                if not name.startswith('side_b/'): continue
                path = root / name.removeprefix('side_b/')
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
                if path.suffix in ('.service', '.target', '.slice'):
                    if path.suffix == '.service':
                        content = content.replace('[Service]\n', f'[Service]\nNetworkNamespacePath=/proc/{holder.pid}/ns/net\n')
                        content = content.replace('Restart=on-failure', 'Restart=no')
                        services.append(path.name)
                    dest = Path('/run/systemd/system') / path.name
                    assert not dest.exists()
                    dest.write_text(content); created.append(dest)
            (root / 'bin').mkdir()
            shutil.copy2(binary, root / 'bin/tuntom')
            (root / 'secrets').mkdir()
            (root / 'secrets/master.env').write_text('TUNTOM_SECRET=' + '01' * 16 + '\n')
            run('systemctl', 'daemon-reload')
            def blocked(mark):
                result = net('ip', '-4', 'route', 'get', '198.18.0.10', 'mark', str(mark), check=False)
                assert result.returncode != 0, result.stdout
                assert 'fallback' in net('ip', '-4', 'route', 'get', '198.18.0.10').stdout
            for service in services: run('systemctl', 'start', service)
            for index in range(2):
                mark = (231 + 256 * index) << 16
                expected = 'ut231' + ('_1' if index else '') + 'c'
                assert expected in net('ip', '-4', 'route', 'get', '198.18.0.10', 'mark', str(mark)).stdout
            print('PASS: actual generated units start two TUNs with independent return routes; peer absent, no fallback')
            for service in services: run('systemctl', 'stop', service)
            for index in range(2): blocked((231 + 256 * index) << 16)
            print('PASS: normal stop leaves terminal policy guards')
            service = services[0]
            run('systemctl', 'start', service)
            run('systemctl', 'kill', '--kill-whom=main', '--signal=SIGKILL', service)
            for _ in range(100):
                state = run('systemctl', 'show', '-p', 'ActiveState', '--value', service).stdout.strip()
                if state == 'failed': break
                time.sleep(0.1)
            assert state == 'failed', state
            for index in range(2): blocked((231 + 256 * index) << 16)
            print('PASS: SIGKILL runs ExecStopPost and cannot route via main')
            run('systemctl', 'reset-failed', service)
            run('systemctl', 'start', service)
            run('systemctl', 'stop', service)
            shutil.copy2('/usr/bin/false', root / 'bin/tuntom')
            assert run('systemctl', 'start', service, check=False).returncode != 0
            for index in range(2): blocked((231 + 256 * index) << 16)
            print('PASS: failed process startup preserves guards')
        finally:
            for service in services:
                run('systemctl', 'stop', service, check=False)
                run('systemctl', 'reset-failed', service, check=False)
            for path in created: path.unlink(missing_ok=True)
            run('systemctl', 'daemon-reload', check=False)
            holder.terminate(); holder.wait()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
