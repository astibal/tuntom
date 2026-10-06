"""VIA relay deployment validation and runbooks for a remote service host."""
import re
import shlex


def validate_via(value, a, b, count, software):
    if value is None:
        return None
    allowed = {'mode', 'instance', 'service', 'in_port', 'out_port', 'in_tun', 'out_tun', 'namespace', 'trust_key', 'trust_public'}
    if not isinstance(value, dict) or set(value) - allowed:
        raise ValueError('invalid VIA fields')
    if a['attachment']['type'] != 'tun' or b['attachment']['type'] != 'switch':
        raise ValueError('VIA requires the service host on side A and switch on side B')
    if software['source'] != 'git_build':
        raise ValueError('VIA currently requires a Git build including tuntom-divert-adapter')
    mode = value.get('mode')
    if mode not in {'paired', 'split'}:
        raise ValueError('VIA mode must be paired or split')
    if count > 16 or (mode == 'split' and (count < 2 or count % 2)):
        raise ValueError('VIA supports up to 16 tunnels; split mode requires an even count, at least 2')
    result = {'mode': mode}
    for key in ('service', 'instance', 'in_port', 'out_port', 'in_tun', 'out_tun'):
        text = value.get(key)
        pattern = r'[A-Za-z0-9][A-Za-z0-9_.#-]{0,31}' if key == 'instance' else r'[A-Za-z0-9][A-Za-z0-9_.-]{0,31}'
        if not isinstance(text, str) or not re.fullmatch(pattern, text):
            raise ValueError(f'invalid VIA {key}')
        if key.endswith('_tun') and (len(text) > 15 or text in {'.', '..'}):
            raise ValueError('VIA TUN names must fit Linux IFNAMSIZ')
        result[key] = text
    if result['in_tun'] == result['out_tun'] or result['in_port'] == result['out_port']:
        raise ValueError('VIA needs distinct IN and OUT names')
    namespace = value.get('namespace', '')
    if not isinstance(namespace, str) or (namespace and not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}', namespace)):
        raise ValueError('invalid VIA network namespace')
    result['namespace'] = namespace
    trust = value.get('trust_key', '')
    if trust:
        from tunnel_deployments import clean_path
        clean_path(trust, 'via.trust_key')
    elif not isinstance(trust, str):
        raise ValueError('invalid VIA trust key path')
    result['trust_key'] = trust
    if value.get('trust_public'):
        result['trust_public'] = public_key_records(value['trust_public'])
    for port in (result['in_port'], result['out_port']):
        suffix = '#path-15' if mode == 'paired' else ''
        if len(f'{port}.15~via:c:{result["instance"]}{suffix}') > 63:
            raise ValueError('VIA registration exceeds the 63-byte IPC name limit')
    # Leave room for the side/path suffix on the physical switch port.
    if len(b['attachment']['port_id']) > 40:
        raise ValueError('VIA relay port prefix must be at most 40 characters')
    return result


def relay_port(base, index, count, via):
    if via['mode'] == 'split':
        half = count // 2
        return f'{base}-{"in" if index < half else "out"}_{index % half}'
    return base + (f'_{index}' if index else '')


def extend_bundle(files, config, root):
    from tunnel_deployments import shell, instance
    via = config['via']
    count = config['count']
    base = config['side_b']['attachment']['port_id']
    members = [instance(config['tunnel_id'], i) for i in range(count)]
    trust_path = f'{root}/config/control-trust.pub'
    control = ['--allow-control-trusted', '--control-trust-key', trust_path] if via.get('trust_key') or via.get('trust_public') else []
    if control:
        if not via.get('trust_public'): raise ValueError('resolve the CONTROL public key before generating a VIA runbook')
        for side in ('side_a', 'side_b'):
            files[side + '/config/control-trust.pub'] = public_key_records(via['trust_public'])
            files[side + '/scripts/30-install-config.sh'] += 'chmod 0755 "$root/config"\nchmod 0644 "$root/config/control-trust.pub"\n'
    for side_name in ('side_a', 'side_b'):
        side = config[side_name]
        suffix = 'c' if side['role'] == 'initiator' else 's'
        for index, member in enumerate(members):
            key = f'{side_name}/runtime/run-{member}{suffix}.sh'
            text = files[key]
            if side_name == 'side_a':
                text = text.replace(f' {member} ut{member}{suffix}', f' {member} -')
                text = text.rstrip() + ' ' + shell('--relay-listen', f'{root}/run/relay-{index}.sock') + '\n'
                # The instance lock excludes a competing generated runner.
                text = text.replace('source "$root/secrets/master.env"',
                    f'rm -f -- "$root/run/relay-{index}.sock"\nsource "$root/secrets/master.env"')
            else:
                attachment = side['attachment']
                old = shell('--switch-socket', attachment['switch_socket'], '--switch-port-id',
                            base + (f'_{index}' if index else ''), '--switch-label', attachment['label'])
                text = text.replace(old, shell('--relay-connect', attachment['switch_socket'],
                                              '--relay-port-id', relay_port(base, index, count, via)))
            if control: text = text.rstrip() + " " + shell(*control) + "\n"
            files[key] = text
    prefix = 'side_a/'
    software_key = prefix + 'scripts/20-install-software.sh'
    files[software_key] = files[software_key].replace('echo SOFTWARE_READY',
        'g++ -std=c++17 -pthread -O2 -Wall -Wextra -pedantic "$root/source/src/divert/main.cpp" -o "$root/bin/tuntom-divert-adapter.new"\n'
        'install -m 0755 "$root/bin/tuntom-divert-adapter.new" "$root/bin/tuntom-divert-adapter"\necho SOFTWARE_READY')
    if control:
        for side_name in ('side_a', 'side_b'):
            files[side_name + '/scripts/00-preflight.sh'] += shell('test', '-r', trust_path) + ' || { ' + shell('echo', 'deployment CONTROL public key is missing or unreadable: ' + trust_path) + ' >&2; exit 22; }\n'
    namespace = ['ip', 'netns', 'exec', via['namespace']] if via['namespace'] else []
    # Explicitly require unused TUN names in the selected namespace.
    preflight = ''.join(f'if {shell(*namespace, "ip", "link", "show", "dev", tun)} >/dev/null 2>&1; then echo "VIA TUN already exists" >&2; exit 21; fi\n'
                        for tun in (via['in_tun'], via['out_tun']))
    if namespace:
        preflight = shell(*namespace, 'true') + '\n' + preflight
    files[prefix + 'scripts/00-preflight.sh'] += preflight
    workers = range(count) if via['mode'] == 'split' else range(1)
    start, stop, verify = [], [], []
    for worker in workers:
        name = f'via-{worker}'
        args = namespace + [f'{root}/bin/tuntom-divert-adapter', via['in_tun'], via['out_tun'],
            '--via-instance', via['instance'], '--divert-in-port', via['in_port'],
            '--divert-out-port', via['out_port'], '--admission', 'immediate', '--mtu', '1500',
            '--control-socket', f'{root}/run/{name}.control']
        args += control
        paths = range(count)
        if via['mode'] == 'split':
            paths = [worker]
            args += ['--side', 'in' if worker < count // 2 else 'out', '--shared-flows', f'{root}/run/via.flows']
        for path in paths:
            path_id = path % (count // 2) if via['mode'] == 'split' else path
            args += ['--relay-path', f'{path_id}={root}/run/relay-{path}.sock']
        files[prefix + f'runtime/run-{name}.sh'] = f'''#!/bin/bash
set -euo pipefail
umask 0077
root={shlex.quote(root)}
exec 9>"$root/run/{name}.lock"
flock -n 9 || {{ echo 'VIA worker is already running' >&2; exit 1; }}
rm -f -- "$root/run/{name}.control"
exec {shell(*args)}
'''
        session = f'tuntom-{config["name"]}-{name}'
        start.append(f'screen -L -Logfile "$root/log/{name}.log" -dmS {shell(session)} "$root/runtime/run-{name}.sh"')
        stop.insert(0, f'screen -S {shell(session)} -p 0 -X stuff $\'\\003\' || true')
        verify.append(f'''for attempt in {{1..50}}; do
  timeout 1 "$root/bin/tuntomctl" "$root/run/{name}.control" show stats >/dev/null 2>&1 && break
  sleep 0.1
done
timeout 1 "$root/bin/tuntomctl" "$root/run/{name}.control" show stats >/dev/null
''')
    for side in ('side_a', 'side_b'):
        flag = '--relay-listen' if side == 'side_a' else '--relay-connect'
        check_script = f"""help=$("$root/bin/tuntom" --help 2>&1 || true)
grep -F -- {flag} <<< "$help" >/dev/null
"""
        if side == 'side_a':
            check_script += '''help=$("$root/bin/tuntom-divert-adapter" --help 2>&1 || true)
grep -F -- --relay-path <<< "$help" >/dev/null
'''
            if via['mode'] == 'split':
                check_script += 'grep -F -- --side <<< "$help" >/dev/null\n'
        files[side + '/scripts/30-install-config.sh'] += '\n' + check_script
    # Create relay listeners before adapter workers connect to them.
    ready = '\n'.join(f'''for attempt in {{1..50}}; do
  test -S "$root/run/relay-{index}.sock" && break
  sleep 0.1
done
test -S "$root/run/relay-{index}.sock"''' for index in range(count))
    files[prefix + 'scripts/40-start.sh'] = files[prefix + 'scripts/40-start.sh'].replace('echo STARTED', ready + '\n' + '\n'.join(start) + '\necho STARTED')
    files[prefix + 'scripts/90-stop.sh'] = files[prefix + 'scripts/90-stop.sh'].replace('set -euo pipefail\n', 'set -euo pipefail\n' + '\n'.join(stop) + '\n')
    files[prefix + 'scripts/50-verify.sh'] += '\n' + '\n'.join(verify)
    files[prefix + 'scripts/80-status.sh'] += '\n' + '\n'.join(verify)
    relay = (f'    client-relay {base}-in_*\n    server-relay {base}-out_*' if via['mode'] == 'split' else f'    relay {base}*')
    files['via-service.rules.txt'] = f'''# Merge into the existing format 3 rules; this is not a complete ruleset.
# Review relay selectors against all existing switch ports before loading.
service {via['service']} {{
    client-side {via['in_port']}.*
    server-side {via['out_port']}.*
{relay}
    stickiness hash
    unavailable drop
}}
# Add this service to the intended via [...] chain separately.
'''
    for side in ('side_a', 'side_b'):
        files[side + '/README.txt'] += '''
VIA relay deployment: process readiness does not imply an active service chain.
Review via-service.rules.txt and merge it into the switch's format 3 rules.
Namespace (if selected), IP addresses, routes, VRF and proxy configuration must
already be prepared independently. This deployment only owns its relay tunnels,
divert workers, generated files and keys. It does not change switch rules.
'''


def public_key_records(text):
    """Accept only public grant records, never private keys or arbitrary file data."""
    if not isinstance(text, str) or len(text) > 65536:
        raise ValueError('invalid CONTROL public key file')
    records = []
    for line in text.splitlines():
        if not line.strip() or line.startswith('#'): continue
        fields = line.split()
        if len(fields) != 4 or fields[0] != 'x25519' or not re.fullmatch('[0-9a-fA-F]{64}', fields[1]):
            raise ValueError('CONTROL key source must contain only x25519 public grants')
        values = []
        for field in fields[2:]:
            if not re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)', field):
                raise ValueError('invalid CONTROL public grant')
            value = int(field, 16 if field.lower().startswith('0x') else 10)
            if not 0 <= value < 2**64: raise ValueError('invalid CONTROL public grant')
            values.append(str(value))
        records.append(' '.join(['x25519', fields[1].lower(), *values]))
    if not 1 <= len(records) <= 128: raise ValueError('empty or oversized CONTROL public key file')
    return '\n'.join(records) + '\n'


def read_public_key(path):
    import os
    import stat
    from tunnel_deployments import clean_path
    clean_path(path, 'CONTROL public key source')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > 65536:
            raise ValueError('CONTROL key source must be a bounded regular public key file')
        with os.fdopen(fd, 'r', closefd=False) as stream:
            return public_key_records(stream.read(65537))
    finally: os.close(fd)
