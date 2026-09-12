#!/usr/bin/env python3
"""Single/MP handoff under one name; only disposable processes and local sockets."""
import fcntl
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def alive(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1][0] not in "ZX"
    except FileNotFoundError:
        return False


def until(check):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.02)
    raise AssertionError("Timed out")


def main():
    single, mp, ctl, planner = map(lambda p: Path(p).resolve(), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="tmr-") as tmp:
        directory = Path(tmp)
        env = os.environ | {"TUNTOM_RUN_DIR": str(directory / "run"),
                           "TUNTOM_STATE_DIR": str(directory / "state"),
                           "TUNTOM_BIN_DIR": str(directory / "bin"),
                           "TUNTOM_SOCKET_OWNER": f"{os.getuid()}:{os.getgid()}",
                           "TUNTOM_SWITCH_RULES_FILE": "", "TEST_SINGLE": str(single),
                           "TEST_MP": str(mp), "TEST_CTL": str(ctl), "TEST_PLANNER": str(planner),
                           "TEST_EVENTS": str(directory / "events")}
        compiler = directory / "compiler"
        compiler.write_text('''#!/usr/bin/env bash
set -euo pipefail
[[ "${FAIL_BUILD:-0}" != 1 ]] || exit 42
case "$*" in
  *src/switch/main.cpp*) input="$TEST_SINGLE" ;;
  *src/switch_mp/main.cpp*) input="$TEST_MP" ;;
  *src/control/main.cpp*) input="$TEST_CTL" ;;
  *tools/switch_mp_plan.cpp*) input="$TEST_PLANNER" ;;
  *) exit 90 ;;
esac
cp -- "$input" "${@: -1}"
''')
        compiler.chmod(0o700)
        env["CXX"] = str(compiler)
        hook = directory / "hook"
        hook.write_text('''#!/usr/bin/env bash
set -euo pipefail
printf '%s/%s %s %s\\n' "$TUNTOM_PHASE" "$TUNTOM_ACTION" "$TUNTOM_SWITCH_SOCKET" "$TUNTOM_BIN" >> "$TEST_EVENTS"
if [[ "$TUNTOM_PHASE/$TUNTOM_ACTION" == pre/up && -n "${TEST_BLOCK:-}" ]]; then
    touch "$TEST_BLOCK.ready"
    while [[ ! -f "$TEST_BLOCK.release" ]]; do sleep 0.05; done
fi
''')
        env.update(TUNTOM_SWITCH_PRE_HOOK=str(hook), TUNTOM_SWITCH_POST_HOOK=str(hook))
        harness = '''source "$1"
shift
local_require_root() { :; }
local_runtime_account() { mkdir -p -- "$run_dir"; }
main "$@"
'''
        state = directory / "state/switch-sw"
        legacy_state = directory / "state/switch-mp-sw"
        legacy_bin = directory / "bin/switch-mp-sw"
        data = directory / "run/sw.sock"
        control = directory / "run/sw.control"
        children = []

        def command(kind, name, *args):
            options = ["--workers", "2"] if kind == "switch_mp" else []
            if "--stop" not in args:
                options += ["--route", "a:17=b:83"]
            return ["bash", "-c", harness, "--", str(ROOT / f"mk_{kind}.sh"), name, *options, *map(str, args)]

        def run(kind, name="sw", *args, ok=True, extra=None):
            result = subprocess.run(command(kind, name, *args), env=env | (extra or {}),
                                    capture_output=True, text=True, timeout=60)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            return result

        def pid():
            return int((state / "pid").read_text())

        def stats(path=control):
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
                peer.settimeout(2)
                peer.connect(str(path)); peer.sendall(b"show stats")
                return dict(line.split("=", 1) for line in peer.recv(1024 * 1024).decode().splitlines())

        def flow(path=data, ctl_path=control):
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as a, socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as b:
                for peer, port in ((a, b"a"), (b, b"b")):
                    peer.settimeout(2); peer.connect(str(path))
                    peer.sendall(b"TTP\x01" + bytes([1, 0, 0, 0]) + port)
                until(lambda: int(stats(ctl_path)["connections_current"]) >= 2)
                a.sendall(struct.pack("!BBBBIQ", 1, 1, 0, 1, 20, 17) + b"test")
                assert b.recv(1024) == struct.pack("!BBBBIQ", 1, 1, 0, 1, 20, 83) + b"test"

        def seed_legacy(path, ctl_path):
            legacy_bin.mkdir(parents=True, exist_ok=True)
            legacy_state.mkdir(parents=True, exist_ok=True)
            # Atomic replacement also permits recreating a legacy instance.
            for source, name in ((mp, "main"), (ctl, "tuntomctl")):
                shutil.copy2(source, legacy_bin / (name + ".new"))
                (legacy_bin / (name + ".new")).replace(legacy_bin / name)
            child = subprocess.Popen([str(legacy_bin / "main"), "--workers", "1", "--socket", str(path),
                                      "--control-socket", str(ctl_path), "--route", "a:17=b:83"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            children.append(child)
            until(lambda: path.exists() and ctl_path.exists())
            record = [str(path), str(ctl_path), "", "1500", env["TUNTOM_SOCKET_OWNER"], str(hook), str(hook)]
            (legacy_state / "endpoints").write_bytes(b"\0".join(s.encode() for s in record) + b"\0")
            # Deliberately stale PID: executable identity is authoritative.
            (legacy_state / "pid").write_text(str(os.getpid()))
            return child

        try:
            run("switch")
            first = pid(); flow()
            run("switch_mp", ok=False, extra={"FAIL_BUILD": "1"})
            assert pid() == first and alive(first)
            run("switch_mp", "sw", "--route", "a:17=c:9", ok=False)
            assert pid() == first and alive(first)
            run("switch_mp", "sw", "--auto-pool", "--reserve-cpus", "65535", ok=False)
            assert pid() == first and alive(first)
            flow()
            print("PASS: build/config/planning failures preserve the single-thread switch", flush=True)

            block = directory / "block"
            with tempfile.TemporaryFile() as output:
                task = subprocess.Popen(command("switch_mp", "sw"), env=env | {"TEST_BLOCK": str(block)},
                                        stdout=output, stderr=output)
                children.append(task)
                until(lambda: Path(f"{block}.ready").exists())
                try:
                    for kind in ("switch", "switch_mp"):
                        rejected = run(kind, "sw", "--stop", ok=False)
                        assert "Another mk_ process" in rejected.stderr and alive(first)
                finally:
                    Path(f"{block}.release").touch()
                assert task.wait(timeout=60) == 0
            second = pid()
            assert second != first and not alive(first)
            assert stats()["implementation"] == "tomtom-switch-mp"
            assert not (legacy_state / "endpoints").exists()
            flow()
            with (legacy_state / "lock").open("r+") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                rejected = run("switch", "sw", "--stop", ok=False)
                assert "legacy MP lock" in rejected.stderr and alive(second)
            print("PASS: single -> MP handoff, shared lock, legacy lock and forwarding", flush=True)

            custom_data, custom_control = directory / "run/custom.sock", directory / "run/custom.control"
            run("switch", "sw", "--socket", custom_data, "--control-socket", custom_control)
            third = pid()
            assert not alive(second) and stats(custom_control).get("implementation") != "tomtom-switch-mp"
            assert not data.exists() and not control.exists()
            flow(custom_data, custom_control)
            run("switch_mp", "sw", "--stop")
            assert not alive(third) and not custom_data.exists() and not custom_control.exists()
            run("switch_mp", "other")
            other = int((directory / "state/switch-other/pid").read_text())
            run("switch_mp")
            fourth = pid()
            run("switch", "sw", "--stop")
            assert not alive(fourth) and alive(other)
            print("PASS: MP -> single handoff, cross-helper stop, saved sockets and name isolation", flush=True)

            legacy = seed_legacy(data, control)
            run("switch", ok=False, extra={"FAIL_BUILD": "1"})
            assert legacy.poll() is None
            run("switch")
            fifth = pid()
            assert legacy.wait(timeout=3) == 0 and not (legacy_state / "endpoints").exists()
            flow()

            alternate_data, alternate_control = directory / "run/old-mp.sock", directory / "run/old-mp.control"
            duplicate = seed_legacy(alternate_data, alternate_control)
            record = (legacy_state / "endpoints").read_bytes()
            (legacy_state / "endpoints").write_bytes(b"broken\0")
            run("switch_mp", ok=False)
            assert alive(fifth) and duplicate.poll() is None
            (legacy_state / "endpoints").write_bytes(record)
            run("switch_mp")
            assert not alive(fifth) and duplicate.wait(timeout=3) == 0
            assert not alternate_data.exists() and not alternate_control.exists()
            assert not (legacy_state / "pid").exists() and not (legacy_state / "endpoints").exists()
            assert stats()["implementation"] == "tomtom-switch-mp" and alive(other)
            events = (directory / "events").read_text().splitlines()
            assert any(line.startswith(f"pre/down {alternate_data} {legacy_bin}/main") for line in events)
            flow()
            run("switch", "sw", "--stop")
            run("switch", "other", "--stop")
            print("PASS: legacy MP migration, duplicate retirement, stale PID and saved legacy hooks", flush=True)
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait()
            for name in ("sw", "other"):
                run("switch", name, "--stop")
        assert not list((directory / "bin").glob("stage.*"))
    print("PASS: one managed switch per name across both helpers")


if __name__ == "__main__":
    main()
