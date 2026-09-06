#!/usr/bin/env python3
"""Exercise mk_tunnel process cleanup using disposable processes, without root/network."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
source = (root / 'mk_tunnel.sh').read_text()
functions = source[source.index('stop_matching_processes() {'):source.index('\nnet_down_local()')]
with tempfile.TemporaryDirectory(prefix='tuntom-stop-test-') as directory:
    directory = pathlib.Path(directory)
    c = directory / 'dummy.c'
    c.write_text('''#include <signal.h>
#include <unistd.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc > 4 && strcmp(argv[4], "ignore") == 0) signal(SIGTERM, SIG_IGN);
    for (;;) pause();
}
''')
    binary = directory / 'dummy'
    subprocess.run(['cc', str(c), '-o', str(binary)], check=True)
    children = []
    def spawn(role='client', tunnel='42', interface='ut42c', extra='normal', exe=None):
        child = subprocess.Popen([str(exe or binary), role, tunnel, interface, extra])
        children.append(child)
        return child
    def cleanup(remote=False, stale=None):
        pidfile = directory / 'instance.pid'
        if stale is not None:
            pidfile.write_text(str(stale))
        command = functions + '''
set -euo pipefail
root_cmd=()
local_bin="$1"; remote_bin="$1"; id=42
client_if=ut42c; server_if=ut42s
local_pid_file="$2"; remote_pid_file="$2"; remote=unused
# Exercise the remote stdin script without making an SSH connection.
ssh() { shift; bash -c "$*"; }
''' + ('stop_remote_process' if remote else 'stop_local_process')
        subprocess.run(['bash', '-c', command, '--', str(binary), str(pidfile)],
                       check=True, timeout=10)
        assert not pidfile.exists()
    try:
        other_id = spawn(tunnel='43')
        other_if = spawn(interface='ut_other')
        other_role = spawn(role='server')
        duplicate1, duplicate2 = spawn(), spawn()
        cleanup()  # Missing PID file must not leave either duplicate alive.
        for p in (duplicate1, duplicate2):
            p.wait(timeout=2)
        assert all(p.poll() is None for p in (other_id, other_if, other_role))
        orphan = spawn()
        cleanup(stale=other_id.pid)  # Stale PID must not kill an unrelated process.
        orphan.wait(timeout=2)
        assert other_id.poll() is None
        remote1, remote2 = spawn('server', interface='ut42s'), spawn('server', interface='ut42s')
        cleanup(remote=True)
        remote1.wait(timeout=2)
        remote2.wait(timeout=2)
        resistant = spawn(extra='ignore')
        cleanup()
        resistant.wait(timeout=2)
        assert resistant.returncode == -9
        cleanup()  # Idempotent stop.
    finally:
        for p in children:
            if p.poll() is None:
                p.kill()
            p.wait()
print('PASS: missing/stale PID files, duplicate/orphan cleanup, exact matching, remote stop, KILL fallback')
