#!/usr/bin/env python3
"""Exercise bootstrap address policy without root, SSH or host network changes."""
import json
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / 'mk_tunnel.sh').read_text()
GROUP = (ROOT / 'tools/mk_tunnel_group.sh').read_text()
# Execute the real parser, hook implementations and endpoint setup. Build,
# account provisioning and previous-process cleanup are outside this fixture.
PREFIX = SOURCE[:SOURCE.index('local_bin=')]
HOOKS = SOURCE[SOURCE.index('run_hook_local() {'):
               SOURCE.index('# Run this function with the same privileges')]
START = (SOURCE[SOURCE.index('start_member() {'):
                SOURCE.index('\nsource "${script_dir}/tools/mk_tunnel_group.sh"')]
         + '\nstart_member\ntest_member\n')
STUBS = r'''
root_cmd=()
local_bin=unused-client
remote_bin=unused-server
local_pid_file="$TEST_DIR/client.pid"
remote_pid_file="$TEST_DIR/server.pid"
local_log="$TEST_DIR/client.log"
remote_log="$TEST_DIR/server.log"
local_stats_file="$TEST_DIR/client.stats"
remote_stats_file="$TEST_DIR/server.stats"
local_control_file="$TEST_DIR/client.control"
remote_control_file="$TEST_DIR/server.control"
stats_format=txt
export TEST_SIDE=local
record() { printf '%s %s\n' "$TEST_SIDE" "$*" >> "$TEST_DIR/commands"; }
ip() {
    record ip "$@"
    if [[ "$TEST_SIDE" == "${TEST_FAIL_LINK:-}" && "$1 $2" == 'link set' ]]; then
        return 1
    fi
}
kill() { record kill "$@"; [[ "$TEST_SIDE" != "${TEST_DEAD_SIDE:-}" ]]; }
ping() { record ping "$@"; [[ "${TEST_FAIL_PING:-0}" == 0 ]]; }
ping6() { ping "$@"; }
sleep() { :; }
# Only intercept daemon launches; actually evaluate all other remote shell
# commands, including address conditionals and the streamed hook environment.
ssh() {
    if [[ "$2" == *nohup* ]]; then
        read -r ignored_secret
        TEST_SIDE=remote record launch "$2"
        printf '%s\n' "$$" > "$remote_pid_file"
    else
        TEST_SIDE=remote bash -c "$2"
    fi
}
sh() { record launch "$*"; printf '%s\n' "$$" > "$local_pid_file"; }
net_up_local() { record net_up; }
net_up_remote() { TEST_SIDE=remote record net_up; }
export -f record ip kill ping ping6 sleep
if [[ -n "${TEST_MEMBER_INDEX:-}" ]]; then
    mark_override="" mask_override="" table_override="" chain_base="TUNTOM_$id"
    run_dir="$TEST_DIR"
    set_group_members
    select_member "$TEST_MEMBER_INDEX"
fi
'''
ADDRESS_VARS = ('LOCAL_IP', 'PEER_IP', 'CLIENT_IP', 'SERVER_IP',
                'CLIENT_IPV6', 'SERVER_IPV6')


def run(*args, settings=None, expected_rc=0):
    with tempfile.TemporaryDirectory(prefix='tuntom-no-address-') as tmp:
        directory = Path(tmp)
        hook = directory / 'hook.sh'
        hook.write_text('''python3 - <<'PY'
import json, os
from pathlib import Path
values = {k[7:]: v for k, v in os.environ.items() if k.startswith('TUNTOM_')}
with (Path(os.environ['TEST_DIR']) / 'hooks').open('a') as out:
    out.write(json.dumps(values) + '\\n')
PY
''')
        env = {'PATH': os.environ['PATH'], 'LC_ALL': 'C', 'TEST_DIR': tmp,
               'TUNTOM_SECRET': '00112233445566778899aabbccddeeff',
               'TUNTOM_PRE_HOOK': str(hook), 'TUNTOM_POST_HOOK': str(hook)}
        env.update(settings or {})
        # Also verify that teardown hooks get the same address context.
        down_hooks = '\nhook_pre_down_local\nhook_pre_down_remote\n'
        fixture = directory / 'mk_tunnel_fixture.sh'
        fixture.write_text(PREFIX + HOOKS + GROUP + STUBS + START + down_hooks)
        result = subprocess.run(
            ['bash', str(fixture), '42', 'router.example', *args],
            env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == expected_rc, result.stdout + result.stderr
        commands = directory / 'commands'
        hooks = directory / 'hooks'
        return (result.stdout,
                commands.read_text().splitlines() if commands.exists() else [],
                [json.loads(line) for line in hooks.read_text().splitlines()]
                if hooks.exists() else [])


def check_setup(args, *, no_address, tun_sides=('local', 'remote'), settings=None, index=0):
    instance = '42' + (f'_{index}' if index else '')
    if index:
        settings = (settings or {}) | {'TEST_MEMBER_INDEX': str(index)}
    output, commands, hooks = run(*args, settings=settings)
    for side, suffix, local_ip, peer_ip in (
            ('local', 'c', f'10.254.42.{4 * index + 1}', f'10.254.42.{4 * index + 2}'),
            ('remote', 's', f'10.254.42.{4 * index + 2}', f'10.254.42.{4 * index + 1}')):
        side_commands = [c for c in commands if c.startswith(side + ' ')]
        up_hooks = [h for h in hooks if h['SIDE'] == side and h['ACTION'] == 'up']
        if side in tun_sides:
            assert f'{side} ip link set dev ut{instance}{suffix} mtu 1500 up' in side_commands
            assert f'{side} net_up' in side_commands
            assert [h['PHASE'] for h in up_hooks] == ['pre', 'post'], up_hooks
            if not no_address:
                assert (f'{side} ip address add {local_ip} peer {peer_ip} '
                        f'dev ut{instance}{suffix}') in side_commands
                assert any(' ip -6 address add ' in c for c in side_commands)
                assert any(' ip -6 route replace ' in c for c in side_commands)
        else:
            assert not any(' ip ' in c or c.endswith(' net_up') for c in side_commands)
            assert not up_hooks
        for context in (h for h in hooks if h['SIDE'] == side):
            assert context['NO_ADDRESS'] == str(int(no_address)), context
            assert context['IF'] == f'ut{instance}{suffix}'
            assert context['INSTANCE'] == instance
            assert context['MEMBER_INDEX'] == str(index)
            if no_address:
                assert all(context[name] == '' for name in ADDRESS_VARS), context
            else:
                assert context['LOCAL_IP'] == local_ip, context
                assert context['PEER_IP'] == peer_ip, context
    if no_address:
        assert not any(' address ' in c or ' route ' in c or ' ping ' in c
                       for c in commands), commands
        assert '(--no-address; ping skipped)' in output, output
    if no_address or len(tun_sides) != 2:
        for side in ('local', 'remote'):
            assert any(c.startswith(f'{side} kill -0 ') for c in commands), commands
    else:
        assert len([c for c in commands if ' ping ' in c]) == 2, commands
        assert 'Tunnel is UP (IPv4 + IPv6)' in output


check_setup([], no_address=False)
check_setup(['--no-address'], no_address=True)
check_setup(['--count', '3'], no_address=False, index=2)
check_setup(['--count', '3', '--no-address'], no_address=True, index=2)
check_setup(['--no-address', '--no-stats'], no_address=True,
            settings={'TUNTOM_PREFIX16': 'unused-invalid-prefix', 'TEST_FAIL_PING': '1'})
for side in ('client', 'server'):
    switch = [f'--{side}-switch', '/tmp/unused-switch.sock', f'{side}-42', '17']
    check_setup(switch + ['--no-address'], no_address=True,
                tun_sides=('remote',) if side == 'client' else ('local',))
    check_setup(switch + [f'--{side}-switch-exit-node', '--no-address'], no_address=True)
check_setup(['--client-switch', '/tmp/unused-c.sock', 'c', '1',
             '--server-switch', '/tmp/unused-s.sock', 's', '2', '--no-address'],
            no_address=True, tun_sides=())
for side in ('local', 'remote'):
    output, _, _ = run('--no-address', settings={'TEST_DEAD_SIDE': side}, expected_rc=2)
    assert 'Tunnel process health check failed' in output
    for args in ([], ['--no-address']):
        output, _, _ = run(*args, settings={'TEST_FAIL_LINK': side}, expected_rc=1)
        assert 'Tunnel processes are UP' not in output
run(settings={'TEST_FAIL_PING': '1'}, expected_rc=2)
run(settings={'TUNTOM_PREFIX16': 'invalid'}, expected_rc=1)
print('PASS: mk_tunnel address policy, hooks, switch modes and process checks')
