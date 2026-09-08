#!/usr/bin/env python3
"""Local deployment integration tests; real daemons, no root, SSH or host TUN.

Only privilege/account setup and `ip link show` are replaced. The adapter uses
the same real-loop TUN socket-pair fixture as switch_reconnect_test.py.
"""
import os
import pathlib
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def alive(pid):
    try:
        return pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1][0] not in "ZX"
    except FileNotFoundError:
        return False


def until(predicate, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("condition timed out")


def executable(path, content):
    path.write_text(content)
    path.chmod(0o700)


def main():
    with tempfile.TemporaryDirectory(prefix="tmk-") as tmp:
        directory = pathlib.Path(tmp)
        if len(sys.argv) == 4:
            switch, adapter, ctl = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
        else:
            switch, adapter, ctl = [directory / n for n in ("switch", "adapter", "ctl")]
            for output, source, extra in (
                (switch, "switch/main.cpp", []),
                (adapter, "adapter/main.cpp", [str(ROOT / "tests/adapter_tun_fixture.cpp"),
                                               "-Wl,--wrap=open", "-Wl,--wrap=ioctl"]),
                (ctl, "control/main.cpp", []),
            ):
                subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2",
                                str(ROOT / "src" / source), *extra, "-o", str(output)], check=True)

        env = os.environ.copy()
        env.update(TUNTOM_RUN_DIR=str(directory / "run"),
                   TUNTOM_STATE_DIR=str(directory / "state"),
                   TUNTOM_BIN_DIR=str(directory / "bin"),
                   TUNTOM_SOCKET_OWNER=f"{os.getuid()}:{os.getgid()}",
                   TEST_SWITCH=str(switch), TEST_ADAPTER=str(adapter), TEST_CTL=str(ctl),
                   TEST_EVENTS=str(directory / "events"))
        # Reuse freshly compiled binaries across restarts; compilation failures
        # are injected at the actual compiler command used by the bootstrap.
        compiler = directory / "compiler"
        executable(compiler, '''#!/usr/bin/env bash
set -euo pipefail
[[ "${FAIL_BUILD:-0}" != 1 ]] || exit 42
case "$*" in
  *src/switch/main.cpp*) input="$TEST_SWITCH" ;;
  *src/adapter/main.cpp*) input="$TEST_ADAPTER" ;;
  *src/control/main.cpp*) input="$TEST_CTL" ;;
  *) exit 90 ;;
esac
cp -- "$input" "${@: -1}"
''')
        env["CXX"] = str(compiler)
        ip = directory / "ip"
        executable(ip, '''#!/usr/bin/env bash
set -euo pipefail
[[ "$*" == "link show dev exit0" ]] || exit 91
[[ "${TEST_FOREIGN_TUN:-0}" == 1 ]] && exit 0
[[ -S "$TUNTOM_RUN_DIR/exit0.control" ]]
''')
        env["PATH"] = f"{directory}:{env['PATH']}"
        hook = directory / "lifecycle hook.sh"
        executable(hook, '''#!/usr/bin/env bash
set -euo pipefail
ready=0
[[ ! -S "$TUNTOM_CONTROL_SOCKET" ]] || ready=1
printf '%s %s/%s %s %s %s %s %s\n' "$TUNTOM_COMPONENT" "$TUNTOM_PHASE" \
  "$TUNTOM_ACTION" "$TUNTOM_SIDE" "$ready" "$TUNTOM_SWITCH_PORT_ID" \
  "$TUNTOM_MTU" "$TUNTOM_SWITCH_SOCKET" >> "$TEST_EVENTS"
if [[ "$TUNTOM_COMPONENT/$TUNTOM_PHASE/$TUNTOM_ACTION" == switch/pre/up ]]; then
    printf 'route a:17=b:83\n' >> "$TUNTOM_RULES_FILE"
    [[ "${FAIL_RULES:-0}" != 1 ]] || printf 'route invalid\n' >> "$TUNTOM_RULES_FILE"
    if [[ -n "${TEST_BLOCK:-}" ]]; then
        touch "$TEST_BLOCK.ready"
        while [[ ! -e "$TEST_BLOCK.release" ]]; do sleep 0.05; done
    fi
fi
[[ "$TUNTOM_PHASE/$TUNTOM_ACTION" != post/up || "${FAIL_POST:-0}" != 1 ]]
''')
        env.update(TUNTOM_SWITCH_PRE_HOOK=str(hook), TUNTOM_SWITCH_POST_HOOK=str(hook),
                   TUNTOM_ADAPTER_PRE_HOOK=str(hook), TUNTOM_ADAPTER_POST_HOOK=str(hook))
        tun, tun_peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        env["TUNTOM_TEST_TUN_FD"] = str(tun.fileno())
        harness = '''source "$1"
shift
local_require_root() { :; }
local_runtime_account() { mkdir -p -- "$run_dir"; }
main "$@"
'''

        def command(kind, *args):
            return ["bash", "-c", harness, "--", str(ROOT / f"mk_{kind}.sh"), *map(str, args)]

        def run(kind, *args, ok=True, extra=None):
            process = subprocess.Popen(command(kind, *args), env=env | (extra or {}),
                                       pass_fds=(tun.fileno(),), stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, start_new_session=True)
            try:
                stdout, stderr = process.communicate(timeout=45)
            except subprocess.TimeoutExpired as error:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate()
                raise AssertionError(f"Lifecycle timed out:\n{stdout}{stderr}") from error
            result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            return result

        def pid(kind, name):
            return int((directory / "state" / f"{kind}-{name}" / "pid").read_text())

        def events():
            path = directory / "events"
            return path.read_text().splitlines() if path.exists() else []

        def clear_events():
            (directory / "events").write_text("")

        sock = directory / "run/sw.sock"
        control = directory / "run/sw.control"
        adapter_args = ("exit0", "--switch-socket", sock, "--switch-port-id", "internet")
        children = []
        try:
            for kind, args in (
                ("switch", ("../bad",)), ("switch", ("sw", "--route")),
                ("switch", ("sw", "--remote", "example")),
                ("switch", ("sw", "--socket", "relative")),
                ("adapter", ("interface-too-long",)),
                ("adapter", ("exit0",)),
                ("adapter", (*adapter_args, "--mtu", "99999999999999999999")),
                ("adapter", (*adapter_args, "--l4-capacity", "0")),
            ):
                run(kind, *args, ok=False)
            assert not (directory / "state").exists()

            run("switch", "sw")
            first = pid("switch", "sw")
            print("PASS: local startup and argument validation", flush=True)
            assert alive(first)
            assert [e.split()[1:4] for e in events()] == [
                ["pre/up", "local", "0"], ["post/up", "local", "1"]], events()
            for path in (sock, control):
                info = path.stat()
                assert stat.S_IMODE(info.st_mode) == 0o660
                assert (info.st_uid, info.st_gid) == (os.getuid(), os.getgid())

            # The pre/up-generated flow is installed in the real switch.
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as a, \
                    socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as b:
                for peer, port in ((a, b"a"), (b, b"b")):
                    peer.connect(str(sock)); peer.settimeout(2)
                    peer.sendall(b"TTP\x01" + bytes([1, 0, 0, 0]) + port)
                time.sleep(0.05)
                a.sendall(struct.pack("!BBBBIQ", 1, 1, 0, 1, 20, 17) + b"test")
                assert b.recv(1024) == struct.pack("!BBBBIQ", 1, 1, 0, 1, 20, 83) + b"test"

            run("switch", "sw", ok=False, extra={"FAIL_BUILD": "1"})
            assert pid("switch", "sw") == first and alive(first)
            run("switch", "sw", ok=False, extra={"FAIL_RULES": "1"})
            assert pid("switch", "sw") == first and alive(first)
            run("switch", "sw", "--route", "a:17=c:99", ok=False)
            assert alive(first)  # Duplicate rule discovered by real parser.

            # A listener reached through an alias must never be unlinked.
            foreign_path = directory / "run/foreign.sock"
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as foreign:
                foreign.bind(f"{directory}/run/./foreign.sock"); foreign.listen()
                run("switch", "sw", "--socket", foreign_path, ok=False)
                assert foreign_path.exists() and alive(first)
            foreign_path.unlink()
            foreign_path.write_text("keep")
            run("switch", "sw", "--socket", foreign_path, ok=False)
            assert foreign_path.read_text() == "keep" and alive(first)
            foreign_path.unlink()
            foreign_path.symlink_to(sock)
            run("switch", "sw", "--socket", foreign_path, ok=False)
            assert foreign_path.is_symlink() and alive(first)
            foreign_path.unlink()

            # Build/prepare lock spans hooks; the child daemon must not retain it.
            print("PASS: generated flow, failed builds/rules and protected socket paths", flush=True)
            block = directory / "block"
            with tempfile.TemporaryFile() as output:
                task = subprocess.Popen(command("switch", "sw"), env=env | {"TEST_BLOCK": str(block)},
                                        pass_fds=(tun.fileno(),), stdout=output, stderr=output)
                children.append(task)
                until(lambda: pathlib.Path(f"{block}.ready").exists())
                result = run("switch", "sw", "--stop", ok=False)
                assert "Another mk_ process" in result.stderr and alive(first)
                pathlib.Path(f"{block}.release").touch()
                try:
                    assert task.wait(timeout=25) == 0
                except (subprocess.TimeoutExpired, AssertionError):
                    output.seek(0)
                    print(output.read().decode(), file=sys.stderr)
                    raise
            second = pid("switch", "sw")
            assert second != first and not alive(first)

            # Stale PID points to an unrelated process; the managed process is
            # also duplicated without appearing in the PID file.
            other = subprocess.Popen(["sleep", "60"])
            children.append(other)
            state = directory / "state/switch-sw"
            duplicate = subprocess.Popen([str(directory / "bin/switch-sw/main"),
                                          "--socket", str(directory / "dup.sock")])
            children.append(duplicate)
            until(lambda: (directory / "dup.sock").exists())
            (state / "pid").write_text(str(other.pid))
            clear_events()
            # --stop must use saved hooks, even if the caller now has no overrides.
            run("switch", "sw", "--stop", extra={"TUNTOM_SWITCH_PRE_HOOK": "/missing",
                                                   "TUNTOM_SWITCH_POST_HOOK": "/missing"})
            assert not alive(second) and duplicate.wait(timeout=2) == 0 and other.poll() is None
            assert [e.split()[1:4] for e in events()] == [
                ["pre/down", "local", "1"], ["post/down", "local", "0"]]
            assert not sock.exists() and not control.exists() and not (state / "pid").exists()
            run("switch", "sw", "--stop")

            run("switch", "sw")
            resistant = pid("switch", "sw")
            os.kill(resistant, signal.SIGSTOP)
            (state / "pid").unlink()
            run("switch", "sw", "--stop")  # TERM cannot run; KILL releases sockets.
            assert not alive(resistant) and not sock.exists()
            print("PASS: locking, saved hooks, orphan cleanup and KILL fallback", flush=True)
            run("switch", "sw", ok=False, extra={"FAIL_POST": "1"})
            assert not sock.exists() and not control.exists() and not (state / "endpoints").exists()

            # Restart after an abrupt crash removes stale sockets safely.
            run("switch", "sw")
            crashed = pid("switch", "sw")
            os.kill(crashed, signal.SIGKILL)
            until(lambda: not alive(crashed))
            assert sock.exists()
            run("switch", "sw")
            run("switch", "sw", "--stop")

            # Real adapter starts while the switch is absent. Its TUN is a
            # socket pair, and only `ip link show` is emulated.
            clear_events()
            run("adapter", *adapter_args, "--mtu", "1400", "--l4-capacity", "10")
            adapter_pid = pid("adapter", "exit0")
            assert alive(adapter_pid)
            assert [e.split()[1:6] for e in events()] == [
                ["pre/up", "local", "1", "internet", "1400"],
                ["post/up", "local", "1", "internet", "1400"]]
            run("switch", "sw")
            data_info = sock.stat()
            clear_events()
            run("adapter", "exit0", "--switch-socket", sock, "--switch-port-id", "new-port")
            assert not alive(adapter_pid)
            assert [e.split()[1:6] for e in events()] == [
                ["pre/down", "local", "1", "internet", "1400"],
                ["post/down", "local", "0", "internet", "1400"],
                ["pre/up", "local", "1", "new-port", "1500"],
                ["post/up", "local", "1", "new-port", "1500"]]
            assert sock.stat().st_ctime_ns == data_info.st_ctime_ns  # No upstream chown/chmod.
            run("adapter", "exit0", "--stop")
            run("adapter", *adapter_args, ok=False, extra={"FAIL_POST": "1"})
            assert not (directory / "run/exit0.control").exists()
            assert not (directory / "state/adapter-exit0/endpoints").exists()
            run("adapter", *adapter_args, ok=False, extra={"TUNTOM_TEST_TUN_FD": "9999"})
            assert not (directory / "state/adapter-exit0/pid").exists()
            run("adapter", *adapter_args, ok=False, extra={"TEST_FOREIGN_TUN": "1"})
            run("switch", "sw", "--stop")
            assert not list((directory / "bin").glob("stage.*"))

            # Upgrades must still stop a daemon using the former binary path.
            shutil.copy2(switch, state / "main")
            legacy = subprocess.Popen([str(state / "main"), "--socket", str(sock)])
            children.append(legacy)
            until(sock.exists)
            run("switch", "sw", "--stop")
            assert legacy.wait(timeout=2) == 0 and not sock.exists()

            # Use an existing writable noexec mount; never remount the host.
            noexec_mount = next((p for p in ("/run/lock", "/dev/shm", f"/run/user/{os.getuid()}")
                                 if os.path.isdir(p) and os.access(p, os.W_OK)
                                 and os.statvfs(p).f_flag & os.ST_NOEXEC), None)
            if noexec_mount:
                with tempfile.TemporaryDirectory(prefix="tmk-noexec-", dir=noexec_mount) as runtime:
                    storage_env = {"TUNTOM_STATE_DIR": runtime,
                                   "TUNTOM_BIN_DIR": str(directory / "storage-bin")}
                    try:
                        run("switch", "storage", extra=storage_env)
                        storage_pid = int((pathlib.Path(runtime) / "switch-storage/pid").read_text())
                        assert alive(storage_pid)
                        assert not (pathlib.Path(runtime) / "switch-storage/main").exists()
                        assert os.access(directory / "storage-bin/switch-storage/main", os.X_OK)
                        failed = run("switch", "storage", ok=False,
                                     extra=storage_env | {"TUNTOM_BIN_DIR": runtime})
                        assert "Cannot execute files" in failed.stderr
                        assert "TUNTOM_BIN_DIR" in failed.stderr
                        assert "[1] Compile" not in failed.stdout and alive(storage_pid)
                        run("switch", "storage", extra=storage_env)
                        assert not alive(storage_pid)
                        run("adapter", *adapter_args, extra=storage_env)
                        assert (pathlib.Path(runtime) / "adapter-exit0/pid").exists()
                    finally:
                        run("adapter", "exit0", "--stop", extra=storage_env)
                        run("switch", "storage", "--stop", extra=storage_env)
                print("PASS: noexec runtime storage, executable binaries and early noexec rejection", flush=True)
            else:
                print("SKIP: no writable noexec mount for storage regression", flush=True)
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait()
            for kind, name in (("adapter", "exit0"), ("switch", "sw")):
                run(kind, name, "--stop")
            tun.close(); tun_peer.close()
    print("PASS: local switch/adapter lifecycle, real flow rules, hooks, socket ownership, "
          "locks, failed builds/startup, orphan/crash cleanup and KILL fallback")


if __name__ == "__main__":
    main()
