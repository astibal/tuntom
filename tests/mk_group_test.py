#!/usr/bin/env python3
"""Exercise real group orchestration/state/hooks with disposable fake endpoints."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BOOTSTRAP = (ROOT / 'mk_tunnel.sh').read_text().split(
    'source "${script_dir}/tools/mk_tunnel_group.sh"')[0]
GROUP = (ROOT / 'tools/mk_tunnel_group.sh').read_text()
STUBS = r'''
root_cmd=()
run_dir="$TEST_DIR/run"
mk_lock_file="$run_dir/mk_${id}.lock"
export TEST_SIDE=local
record() { printf '%s\n' "$*" >> "$TEST_DIR/events"; }
ssh() {
    while [[ "$1" == -* ]]; do
        case "$1" in -o) shift 2;; *) shift;; esac
    done
    shift
    if [[ "${TEST_LOSE_LOCK:-0}" == 1 && "$*" == *server_42.lock* ]]; then
        TEST_SIDE=remote timeout 0.1 bash -c "$*"
    else
        TEST_SIDE=remote bash -c "$*"
    fi
}
ip() {
    if [[ "$1 $2" == 'link show' ]]; then
        [[ "${TEST_COLLISION:-}" == "$3" ]]
    elif [[ "$1 $2" == 'route show' ]]; then
        [[ "${TEST_TABLE_COLLISION:-}" != "$TEST_SIDE" ]] || echo occupied
    else
        echo 'Unexpected real networking operation' >&2; return 99
    fi
}
ss() { [[ -z "${TEST_PORT_COLLISION:-}" ]] || echo occupied; }
iptables() { [[ "${TEST_CHAIN_COLLISION:-}" == "$TEST_SIDE" ]]; }
member_process_exists() { [[ "${TEST_ORPHAN:-}" == "$TEST_SIDE" ]]; }
prepare_group_directories() { mkdir -p "$local_state" "$remote_state"; }
build_staging() {
    record build
    if [[ "${TEST_LOSE_LOCK:-0}" == 1 ]]; then sleep 0.3; fi
    [[ "${TEST_BUILD_FAIL:-0}" == 0 ]]
}
prepare_runtime() { :; }
net_down_local() { record net_down_local; }
net_down_remote() { record net_down_remote; }
install_companion_tools() { :; }
install_member_binary() { record "install $instance"; }
show_member() { :; }
start_member() {
    record "start $instance"
    mkdir -p "$TEST_DIR/running"
    touch "$TEST_DIR/running/$instance"
    if (( client_has_tun )); then
        run_hook_local "$tuntom_pre_hook" pre up local "$client_if" "$client_ip" "$server_ip"
        run_hook_local "$tuntom_post_hook" post up local "$client_if" "$client_ip" "$server_ip"
    fi
    if (( server_has_tun )); then
        run_hook_remote "$tuntom_pre_hook" pre up remote "$server_if" "$server_ip" "$client_ip"
        run_hook_remote "$tuntom_post_hook" post up remote "$server_if" "$server_ip" "$client_ip"
    fi
    [[ "$instance" != "${TEST_FAIL_MEMBER:-}" ]]
}
test_member() { record "check $instance"; }
check_member_processes() { record "health $instance"; }
stop_member() {
    local rc=0
    record "stop $instance"
    if (( client_has_tun )); then
        hook_pre_down_local || rc=1
        hook_post_down_local || rc=1
    fi
    if (( server_has_tun )); then
        hook_pre_down_remote || rc=1
        hook_post_down_remote || rc=1
    fi
    rm -f "$TEST_DIR/running/$instance"
    return "$rc"
}
export -f record ssh ip ss iptables
if [[ "${TEST_MAPPING:-0}" == 1 ]]; then
    mark_override="" mask_override="" table_override="" chain_base="TUNTOM_$id"
    validate_group
    write_group_manifest
else
    group_main
fi
'''
HOOK = r'''
record "hook $TUNTOM_SCOPE $TUNTOM_SIDE $TUNTOM_PHASE/$TUNTOM_ACTION ${TUNTOM_INSTANCE:-group} $TUNTOM_MEMBER_COUNT $TUNTOM_NO_ADDRESS"
if [[ "$TUNTOM_SCOPE" == group ]]; then
    test -f "$TUNTOM_GROUP_MANIFEST" || exit 90
    [[ -z "$TUNTOM_IF$TUNTOM_INSTANCE$TUNTOM_LOCAL_IP$TUNTOM_PEER_IP" ]] || exit 91
fi
if [[ "${TEST_HOOK_FAIL:-}" == "$TUNTOM_SCOPE/$TUNTOM_SIDE/$TUNTOM_PHASE/$TUNTOM_ACTION" ]]; then
    exit 92
fi
'''


class Fixture:
    def __init__(self, directory):
        self.directory = directory
        self.script = directory / 'mk_tunnel.sh'
        group = GROUP.replace('/var/lib/tuntom-mk', str(directory / 'state'))
        group = group.replace('/run/tuntom-mk', str(directory / 'locks'))
        self.script.write_text(BOOTSTRAP + '\n' + group + '\n' + STUBS)
        (directory / 'tuntom-net.sh').write_text('')
        self.hook = directory / 'hook.sh'
        self.hook.write_text(HOOK)
        self.env = {'PATH': os.environ['PATH'], 'TEST_DIR': str(directory), 'LC_ALL': 'C',
                    'TUNTOM_SECRET': '00112233445566778899aabbccddeeff'}
        for name in ('PRE', 'POST', 'GROUP_PRE', 'GROUP_POST'):
            self.env[f'TUNTOM_{name}_HOOK'] = str(self.hook)

    def run(self, *args, settings=None, ok=True, remote='router.example'):
        (self.directory / 'events').write_text('')
        result = subprocess.run(['bash', str(self.script), '42', remote, *args],
                                env=self.env | (settings or {}), capture_output=True,
                                text=True, timeout=20)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        return (self.directory / 'events').read_text().splitlines(), result

    def running(self):
        return sorted(p.name for p in (self.directory / 'running').glob('*'))

    def state(self, side='client'):
        return self.directory / 'state' / side / '42' / 'active'


def test_lock_shutdown(directory):
    lock_function = BOOTSTRAP[BOOTSTRAP.index('acquire_mk_lock() {'):
                              BOOTSTRAP.index('\nensure_runtime_account_local() {')]
    command = 'set -euo pipefail\n' + lock_function + r'''
root_cmd=()
run_dir="$1" id=42 remote=test
mk_lock_file="$run_dir/shutdown-local.lock"
release_fd="$2"
ssh() {
    while [[ "$1" == -* ]]; do
        case "$1" in -o) shift 2;; *) shift;; esac
    done
    shift
    bash -c "$*"
    # Hold SSH stdout open until Python has reaped the coordinator.
    IFS= read -r release <&"$release_fd"
}
acquire_mk_lock "exec 9>'$run_dir/shutdown-remote.lock'; flock -n 9; printf 'LOCKED\\n'; cat >/dev/null"
'''
    release_read, release_write = os.pipe()
    try:
        coordinator = subprocess.Popen(
            ['bash', '-c', command, '--', str(directory), str(release_read)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            pass_fds=(release_read,), text=True)
        try:
            assert coordinator.wait(timeout=5) == 0
        finally:
            os.write(release_write, b'release\n')
        stdout, stderr = coordinator.communicate(timeout=5)
        assert 'group lock: 42' in stdout, stdout + stderr
        assert not stderr, stderr
        for name in ('shutdown-local.lock', 'shutdown-remote.lock'):
            subprocess.run(['flock', '-n', str(directory / name), 'true'],
                           check=True, timeout=5)
    finally:
        os.close(release_read)
        os.close(release_write)


with tempfile.TemporaryDirectory(prefix='tuntom-group-test-') as tmp:
    test_lock_shutdown(Path(tmp))
    f = Fixture(Path(tmp))
    events, _ = f.run('--count', '3', '--no-address')
    assert f.running() == ['42', '42_1', '42_2']
    assert events.count('build') == 1
    assert events.index('hook group remote pre/up group 3 1') < events.index('start 42')
    assert events.index('check 42_2') < events.index('hook group local post/up group 3 1')
    manifest = (f.state() / 'manifest.tsv').read_text().splitlines()
    assert [line.split('\t')[:4] for line in manifest[1:]] == [
        ['42', '0', '42', '40042'], ['42_1', '1', '298', '40298'],
        ['42_2', '2', '554', '40554']]
    assert '00112233445566778899aabbccddeeff' not in (f.state() / 'config').read_text()
    assert (f.state() / 'owner').read_text() == (f.state('server') / 'owner').read_text()

    events, _ = f.run('--count', '4', settings={'TEST_BUILD_FAIL': '1'}, ok=False)
    assert events == ['build'], events
    assert f.running() == ['42', '42_1', '42_2']
    assert (f.state() / 'count').read_text().strip() == '3'
    f.run('--count', '4', settings={'TEST_LOSE_LOCK': '1'}, ok=False)
    assert f.running() == ['42', '42_1', '42_2']
    f.run('--count', '4', settings={'TEST_COLLISION': 'ut42_3c'}, ok=False)
    f.run('--count', '4', settings={'TEST_COLLISION': 'ut42_3s'}, ok=False)
    f.run('--count', '4', settings={'TEST_PORT_COLLISION': '1'}, ok=False)
    for side in ('local', 'remote'):
        f.run('--count', '4', settings={'TEST_TABLE_COLLISION': side}, ok=False)
        f.run('--count', '4', settings={'TEST_CHAIN_COLLISION': side}, ok=False)
        f.run('--count', '4', settings={'TEST_ORPHAN': side}, ok=False)
    f.run('--count', '4', settings={'TUNTOM_TABLE': '12345'}, ok=False)
    f.run('--no-address', remote='different.example', ok=False)
    assert f.running() == ['42', '42_1', '42_2']

    events, _ = f.run('--count', '2', '--no-address')
    assert f.running() == ['42', '42_1']
    assert events.index('hook group remote pre/down group 3 1') < events.index('stop 42_2')
    assert events.index('stop 42_2') < events.index('start 42')
    events, _ = f.run('--no-address')
    assert f.running() == ['42', '42_1']  # Omitted --count retains membership.
    assert 'hook group remote post/up group 2 1' in events

    f.hook.unlink()  # Stop must use the saved hook and address context.
    events, _ = f.run('--stop', settings={'TUNTOM_SECRET': ''})
    assert f.running() == []
    assert 'hook group remote pre/down group 2 1' in events
    assert not f.state().exists() and not f.state('server').exists()
    f.hook.write_text(HOOK)

    events, _ = f.run('--count', '3', '--no-address',
                      settings={'TEST_FAIL_MEMBER': '42_1'}, ok=False)
    assert f.running() == []
    assert 'start 42_2' not in events and 'stop 42_2' not in events
    assert 'hook group local post/up group 3 1' not in events
    assert 'hook group remote pre/down group 3 1' in events
    f.run('--stop', settings={'TUNTOM_SECRET': ''})

    events, _ = f.run('--count', '2', '--no-address', settings={
        'TEST_HOOK_FAIL': 'group/remote/post/up'}, ok=False)
    assert f.running() == []
    f.run('--stop', settings={'TEST_HOOK_FAIL': 'member/local/pre/down'}, ok=False)
    assert f.state().exists()  # Failed cleanup remains retryable.
    f.run('--stop', settings={'TUNTOM_SECRET': ''})

    f.run('--count', '2', '--no-address')
    (f.state('server') / 'owner').write_text('another-caller\n')
    events, _ = f.run('--stop', ok=False)
    assert not events and f.running() == ['42', '42_1']
    (f.state('server') / 'owner').write_text((f.state() / 'owner').read_text())
    f.run('--stop')

    # Both local and remote locks reject a second operation without touching members.
    for lock in (f.directory / 'run/mk_42.lock', f.directory / 'locks/server_42.lock'):
        holder = subprocess.Popen(['flock', '-x', str(lock), 'bash', '-c',
                                   'printf "LOCKED\\n"; read -r release'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            assert holder.stdout.readline().strip() == 'LOCKED'
            events, result = f.run('--count', '2', '--no-address', ok=False)
            assert not events and 'Cannot lock tunnel group' in result.stderr
        finally:
            holder.communicate('release\n', timeout=5)

    pure_switch = ['--client-switch', '/tmp/c.sock', 'client', '17',
                   '--server-switch', '/tmp/s.sock', 'server', '17']
    events, _ = f.run('--count', '2', '--no-address', *pure_switch)
    assert not any(event.startswith('hook member ') for event in events), events
    assert 'hook group remote post/up group 2 1' in events
    row = (f.state() / 'manifest.tsv').read_text().splitlines()[1].split('\t')
    assert row[18:] == ['/tmp/c.sock', '/tmp/s.sock', '0', '0', '1'], row
    events, _ = f.run('--stop', settings={'TUNTOM_SECRET': ''})
    assert not any(event.startswith('hook member ') for event in events), events

    _, mapping = f.run('--count', '64', '--client-switch', '/tmp/c.sock', 'edge', '17',
                       settings={'TEST_MAPPING': '1'})
    rows = [line.split('\t') for line in mapping.stdout.splitlines()[1:]]
    assert len(rows) == 64 and len({row[3] for row in rows}) == 64
    assert rows[-1][0] == '42_63' and rows[-1][6:8] == ['10.254.42.253', '10.254.42.254']
    assert rows[-1][8:10] == ['fd42::10:254:42:fd', 'fd42::10:254:42:fe']
    assert rows[1][14] == 'edge_1' and rows[1][16] == '17'
    for args in (['--count'], ['--count', '0'], ['--count', '65'], ['--count', '02']):
        f.run(*args, ok=False)
    f.run('--client-switch', '/tmp/bad\nsocket', 'client', '1', ok=False)

print('PASS: group lifecycle, saved hooks, resize, rollback, ownership and member mapping')
