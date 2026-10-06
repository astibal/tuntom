"""Generate systemd runtime artifacts; screen remains the default runtime."""
from pathlib import Path
import hashlib
import json
import shlex
from deployment_cleanup import systemd_prefix


def unit_quote(value):
    # systemd performs its own escaping/specifier expansion, not shell parsing.
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%').replace('$', '$$') + '"'


def extend_bundle(files, config, root):
    for side_name in ('side_a', 'side_b'):
        side = config[side_name]
        if side['runtime'] != 'systemd': continue
        prefix = side_name + '/'
        ident = config['deployment_id']
        base = systemd_prefix(ident)
        target, slice_name = base + '.target', base + '.slice'
        runners = sorted(name[len(prefix + 'runtime/run-'):-3] for name in files if name.startswith(prefix + 'runtime/run-') and name.endswith('.sh'))
        services = {runner: base + '_' + runner.replace('-', '_') + '.service' for runner in runners}
        relays = [name for runner, name in services.items() if not runner.startswith('via-')]
        header = '# Fabric deployment: ' + ident + '\n'
        units = {}
        units[slice_name] = header + '[Unit]\nDescription=Tuntom deployment ' + config['name'] + ' slice\n\n[Slice]\nMemoryAccounting=yes\nTasksAccounting=yes\n'
        units[target] = header + f'''[Unit]
Description=Tuntom deployment {config['name']}
Wants=network-online.target {' '.join(services.values())}
After=network-online.target

[Install]
WantedBy=multi-user.target
'''
        for runner, unit in services.items():
            adapter = runner.startswith('via-')
            dependencies = ('Wants=' + ' '.join(relays) + '\nAfter=' + ' '.join(relays) + '\n') if adapter else ''
            units[unit] = header + f'''[Unit]
Description=Tuntom {config['name']} / {runner}
PartOf={target}
After=network-online.target
{dependencies}StartLimitIntervalSec=60
StartLimitBurst=10

[Service]
Type=exec
Slice={slice_name}
WorkingDirectory={root.replace("%", "%%")}
ExecStart=/bin/bash {unit_quote(root + '/runtime/run-' + runner + '.sh')}
Restart=on-failure
RestartSec=2s
KillMode=control-group
TimeoutStopSec=25s
UMask=0077
StandardOutput=journal
StandardError=journal
'''
            if adapter:
                # After= orders process starts; the socket itself can appear later.
                count = config['count']; via = config['via']; worker = int(runner.split('-')[1])
                paths = [worker] if via['mode'] == 'split' else range(count)
                ready = '\n'.join(f'''for attempt in {{1..100}}; do
  test -S "$root/run/relay-{index}.sock" && break
  sleep 0.1
done
test -S "$root/run/relay-{index}.sock" || {{ echo "Relay socket {index} not ready" >&2; exit 23; }}''' for index in paths)
                key = prefix + 'runtime/run-' + runner + '.sh'
                before, executable = files[key].rsplit('\nexec ', 1)
                files[key] = before + '\n' + ready + '\nexec ' + executable
        for name, content in units.items(): files[prefix + 'systemd/' + name] = content
        manifest = {'deployment_id': ident, 'target': target, 'slice': slice_name,
                    'units': {name: hashlib.sha256(content.encode()).hexdigest() for name, content in units.items()}}
        files[prefix + 'systemd/manifest.json'] = json.dumps(manifest, indent=2) + '\n'
        files[prefix + 'scripts/deployment-lifecycle.py'] = Path(__file__).with_name('deployment_cleanup.py').read_text()
        def command(action):
            return 'python3 "$root/scripts/deployment-lifecycle.py" systemd ' + action + ' "$root" ' + shlex.quote(ident)
        def script(action):
            return '#!/bin/bash\nset -euo pipefail\nroot=' + shlex.quote(root) + '\n' + command(action) + '\n'
        files[prefix + 'scripts/30-install-config.sh'] += '\n' + command('install') + '\n'
        files[prefix + 'scripts/40-start.sh'] = script('start')
        files[prefix + 'scripts/60-enable.sh'] = script('enable') if side.get('autostart') else '#!/bin/bash\nset -euo pipefail\necho AUTOSTART_DISABLED\n'
        files[prefix + 'scripts/90-stop.sh'] = script('stop')
        files[prefix + 'scripts/80-status.sh'] = script('status')
        verify = files[prefix + 'scripts/50-verify.sh']
        files[prefix + 'scripts/50-verify.sh'] = verify + '\n' + '\n'.join('systemctl is-active --quiet ' + unit for unit in services.values()) + '\n'
        files[prefix + 'scripts/00-preflight.sh'] = files[prefix + 'scripts/00-preflight.sh'].replace('command -v screen >/dev/null', 'command -v systemctl >/dev/null\ntest -d /run/systemd/system || { echo "systemd is not the host service manager" >&2; exit 24; }')
        key = prefix + 'scripts/10-install-dependencies.sh'
        files[key] = files[key].replace('make screen iproute2', 'make iproute2').replace('install -y screen iproute2', 'install -y iproute2').replace('command -v screen', 'command -v systemctl')
        files[prefix + 'README.txt'] += f'''
Runtime: systemd. Target: {target}
Slice: {slice_name}
Start: scripts/40-start.sh; state and recent journal: scripts/80-status.sh.
Start-on-boot: {'requested' if side.get('autostart') else 'disabled'}.
Run scripts/60-enable.sh only AFTER both sides pass scripts/50-verify.sh.
Stop/rollback also disables start-on-boot. Undeploy checks unit ownership,
disables the target, stops all its services, removes only its units, reloads
systemd and then erases deployment files and keys. Interrupted cleanup is retryable.
Switch, namespace and routing prerequisites must be available at boot separately.
'''
