"""Fail-closed IPv4 return routing for generated TUN endpoints."""
from pathlib import Path
import shlex


def network_script(root, tunnel_id, index, suffix):
    """Reserve distinct routing marks/tables for parallel members."""
    member = str(tunnel_id) + (f'_{index}' if index else '')
    key = tunnel_id + 256 * index
    ident = member + suffix
    return f'''#!/bin/bash
set -euo pipefail
root={shlex.quote(root)}
export TUNTOM_ID={tunnel_id} TUNTOM_IF=ut{ident}
export TUNTOM_MARK={key << 16} TUNTOM_MARK_MASK=0xffff0000
export TUNTOM_TABLE={10000 + key} TUNTOM_CHAIN=TUNTOM_{key}{suffix}
export TUNTOM_SNAT=0
source "$root/runtime/tuntom-net.sh"
case "${{1:-}}" in
  up)
    # The process creates the TUN after ExecStart. Fail startup on timeout.
    for attempt in {{1..100}}; do
      if test -S "$root/run/{ident}.control" && ip link show dev "$TUNTOM_IF" >/dev/null 2>&1; then
        ip link set dev "$TUNTOM_IF" up
        tuntom_net_up
        exit 0
      fi
      sleep 0.1
    done
    echo "TUN/control socket not ready: {ident}" >&2
    exit 1
    ;;
  down) tuntom_net_down ;;
  *) echo 'expected up or down' >&2; exit 2 ;;
esac
'''


def extend_bundle(files, config, root):
    # VIA replaces the original TUN endpoint with a relay; it has no such TUN.
    if config.get('via'):
        return
    for side_name in ('side_a', 'side_b'):
        side = config[side_name]
        if side['attachment']['type'] != 'tun':
            continue
        prefix = side_name + '/'
        files[prefix + 'runtime/tuntom-net.sh'] = (Path(__file__).resolve().parents[1] / 'tuntom-net.sh').read_text()
        suffix = 'c' if side['role'] == 'initiator' else 's'
        for index in range(config['count']):
            member = str(config['tunnel_id']) + (f'_{index}' if index else '')
            ident = member + suffix
            network = root + '/runtime/network-' + ident + '.sh'
            files[prefix + 'runtime/network-' + ident + '.sh'] = network_script(root, config['tunnel_id'], index, suffix)
            if side['runtime'] == 'screen':
                # Keep cleanup in the runner, not only in the external stop command.
                path = prefix + 'runtime/run-' + ident + '.sh'
                before, command = files[path].rsplit('\nexec ', 1)
                files[path] = before + f'''
network={shlex.quote(network)}
child=
cleanup() {{
    local status=$?
    trap - EXIT INT TERM
    bash "$network" down || status=$?
    if [[ -n "$child" ]]; then
        kill "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
    exit "$status"
}}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
bash "$network" down
{command.strip()} &
child=$!
bash "$network" up
wait "$child"
'''
        key = prefix + 'scripts/10-install-dependencies.sh'
        files[key] = files[key].replace('iproute2 acl', 'iproute2 iptables acl')
        files[prefix + 'scripts/00-preflight.sh'] += 'command -v iptables >/dev/null\n'
        files[prefix + 'README.txt'] += '''
TUN return routing is fail-closed by default (IPv4). Startup brings the TUN up,
without assigning addresses, enabling forwarding or publishing ingress ports.
Each member reserves upper mark bits and a separate policy table. Stop retains
blackhole and reply-restoration guards; do not recycle these reservations
until old flows are retired. Verify compatibility with existing policy routing.
Remote session loss retains the TUN route, never a fallback to main.
'''
