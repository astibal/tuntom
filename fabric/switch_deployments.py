"""Collector-local tunnel lifecycle. Accepts typed intents, never shell commands."""
from pathlib import Path
import json
import ipaddress
import socket
import os
import re
import stat
import subprocess
import threading

from tunnel_deployments import validate, render_bundle, check_switch_ports
from deployment_cleanup import undeploy
from deployment_diagnostics import safe_output

LOCK = threading.Lock()
BASE = Path('/opt/tuntom/deployments')
BINARIES = Path('/var/lib/tuntom-fabric/bin')


def trusted(path):
    """Privileged execution must not follow writable files or path components."""
    path = Path(path)
    for item in (path, *path.parents):
        info = item.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError(f'collector deployment requires a root-owned, non-writable path: {item}')
    return path


def inspect_switch(fabric, key):
    endpoint = fabric.endpoint(key)
    if endpoint.source != 'local' or endpoint.kind != 'switch' or not endpoint.switch_socket:
        raise ValueError('select a local observed switch with a socket')
    for field, namespace in (('mount_namespace', 'mnt'), ('net_namespace', 'net')):
        if getattr(endpoint, field) != os.readlink('/proc/self/ns/' + namespace):
            raise ValueError('switch must share the collector mount and network namespaces')
    if not stat.S_ISSOCK(os.stat(endpoint.switch_socket).st_mode):
        raise ValueError('observed switch socket is no longer available')
    directory = Path(endpoint.executable).parent
    binaries = {}
    for name in ('tuntom', 'tuntomctl'):
        for candidate in (BINARIES / name, directory / name):
            try:
                path = trusted(candidate)
                if not path.is_file() or not os.access(path, os.X_OK): continue
            except (OSError, ValueError):
                continue
            binaries[name] = str(path)
            break
        else:
            raise ValueError(f'install a root-owned {name} in {BINARIES} or next to the switch binary; all parent directories must be root-owned and non-writable by others')
    snapshot = fabric.snapshot()
    ports = set()
    for row in snapshot.get('endpoints', []):
        if row['id'] == key:
            ports.update(port['name'] for port in (row.get('switch_detail') or {}).get('ports', []) if port.get('name'))
        elif row.get('source', 'local') == 'local' and row.get('switch_socket') == endpoint.switch_socket and row.get('port_id'):
            ports.add(row['port_id'])
    return {'name': endpoint.name, 'switch_socket': endpoint.switch_socket, 'binaries': binaries, 'ports': sorted(ports)}


def run_script(root, step, timeout=120):
    # These files are generated here and live in a root-only deployment directory.
    result = subprocess.run(['/bin/bash', str(trusted(root / 'scripts' / step))], cwd=root,
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout, text=True, env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'HOME': '/root', 'LANG': 'C.UTF-8'})
    if result.returncode:
        raise RuntimeError(f'{step} failed (exit {result.returncode}): {safe_output(result.stdout)}')
    return safe_output(result.stdout, 16384)


def execute(fabric, body):
    if not isinstance(body, dict): raise ValueError('invalid switch deployment request')
    action = body.get('action')
    if action == 'inspect':
        if set(body) - {'action', 'switch_id', 'route_to', 'trust_key'} or not isinstance(body.get('switch_id'), str):
            raise ValueError('invalid switch inspection')
        result = inspect_switch(fabric, body['switch_id'])
        if body.get('trust_key'):
            from via_deployments import read_public_key
            result['trust_public'] = read_public_key(body['trust_key'])
        if 'route_to' in body:
            address = ipaddress.ip_address(body['route_to'])
            try:
                with socket.socket(socket.AF_INET6 if address.version == 6 else socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    probe.connect((str(address), 9))
                    result['transport_ip'] = probe.getsockname()[0]
            except OSError:
                result['transport_ip'] = ''
        return result
    if action not in {'prepare', 'start', 'rollback', 'undeploy', 'enable', 'status'} or set(body) != ({'action', 'intent', 'deployment_id', 'secret'} if action == 'prepare' else {'action', 'intent', 'deployment_id'}):
        raise ValueError('invalid switch deployment operation')
    ident = body['deployment_id']
    if not isinstance(ident, str) or not re.fullmatch('[0-9a-f]{32}', ident):
        raise ValueError('invalid deployment identity')
    config, _ = validate(body['intent'])
    if config['tunnel_id'] is None: raise ValueError('allocate a tunnel ID before deployment')
    side_name = next((key for key in ('side_a', 'side_b') if config[key].get('switch_id')), None)
    if side_name is None: raise ValueError('missing existing switch')
    side = config[side_name]
    if action == 'prepare' and (not isinstance(body['secret'], str) or not re.fullmatch('[0-9a-f]{32}', body['secret'])):
        raise ValueError('invalid deployment secret')
    # No caller paths, bundles or commands cross the privilege boundary.
    root = BASE / config['name']
    intent = json.dumps(body['intent'], sort_keys=True)
    with LOCK:
        if action == 'undeploy':
            if root.exists(): trusted(root)
            undeploy(root, ident, 'client' if side['role'] == 'initiator' else 'server',
                     [str(config['tunnel_id']) + (f'_{i}' if i else '') for i in range(config['count'])])
            return {'result': 'undeployed; deployment files and secrets removed'}
        if action in {'prepare', 'start'}:
            resolved = inspect_switch(fabric, side['switch_id'])
            check_switch_ports(side, config['count'], resolved.get('ports', []), config.get('via'))
        if action == 'prepare':
            if os.geteuid() != 0: raise ValueError('local deployment requires a root collector')
            BASE.mkdir(parents=True, exist_ok=True, mode=0o755)
            trusted(BASE)
            if root.exists():
                trusted(root)
                if (root / '.fabric-deployment-id').read_text().strip() != ident:
                    raise ValueError('deployment directory belongs to a different intent')
                previous = (root / '.fabric-intent').read_text()
                if previous != intent:
                    old_intent, new_intent = json.loads(previous), json.loads(intent)
                    new_public = new_intent.get('via', {}).pop('trust_public', None)
                    if not new_public or old_intent != new_intent:
                        raise ValueError('deployment directory belongs to a different intent')
                    # One-time migration: materialize the already-selected public
                    # key in failed drafts generated before key provisioning.
                    (root / '.fabric-intent').write_text(intent)
            else:
                root.mkdir(mode=0o700)
                (root / '.fabric-deployment-id').write_text(ident + '\n')
                (root / '.fabric-intent').write_text(intent)
            side['attachment']['switch_socket'] = resolved['switch_socket']
            side['local_binaries'] = resolved['binaries']
            config.update(schema=1, deployment_id=ident)
            addresses = {config[key]['endpoint_id']: 'collector' for key in ('side_a', 'side_b')}
            bundle = render_bundle(config, addresses, '', root=str(root))
            for name, content in bundle.items():
                if not name.startswith(side_name + '/'): continue
                path = root / name[len(side_name) + 1:]
                path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                trusted(path.parent)
                if path.exists(): trusted(path)
                path.write_text(content)
                path.chmod(0o700 if name.endswith('.sh') else 0o600)
            secret = root / 'secrets' / 'master.env'
            secret.parent.mkdir(exist_ok=True, mode=0o700)
            trusted(secret.parent)
            if secret.exists(): trusted(secret)
            fd = os.open(secret, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'w') as stream: stream.write('TUNTOM_SECRET=' + body['secret'] + '\n')
            for directory in ('run', 'log'):
                (root / directory).mkdir(exist_ok=True, mode=0o700)
            for step in ('00-preflight.sh', '10-install-dependencies.sh', '20-install-software.sh', '30-install-config.sh'):
                run_script(root, step)
        else:
            trusted(root)
            if (root / '.fabric-deployment-id').read_text().strip() != ident or (root / '.fabric-intent').read_text() != intent:
                raise ValueError('deployment identity no longer matches')
            if action == 'status':
                return {'output': run_script(root, '80-status.sh')}
            steps = {'start': ('40-start.sh', '50-verify.sh'), 'enable': ('60-enable.sh',), 'rollback': ('99-rollback.sh',)}
            for step in steps[action]:
                run_script(root, step)
    return {'result': action + ' completed'}
