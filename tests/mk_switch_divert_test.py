#!/usr/bin/env python3
"""Divert lifecycle through both helpers; real daemons and disposable Unix sockets."""
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
RULES = "format 2\nserial 1\nexit exit\nswitch edge,[17,42] to exit,[99,42] allow bidir\n"
DIVERT = "cookie hTX\nports divert-in divert-out\norigin edge 123\nmatch edge,[17,42]\n"


def frame(labels, opcode=1):
    payload = b"helper-divert-test"
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), 8 + 8 * len(labels) + len(payload)) + \
        struct.pack("!" + "Q" * len(labels), *labels) + payload


def exercise(kind, binaries):
    with tempfile.TemporaryDirectory(prefix="tmk-divert-") as tmp:
        directory = Path(tmp)
        env = os.environ | dict(zip(("TEST_SINGLE", "TEST_MP", "TEST_CTL", "TEST_PLANNER"), binaries))
        env.update(TUNTOM_RUN_DIR=str(directory / "run"), TUNTOM_STATE_DIR=str(directory / "state"),
                   TUNTOM_BIN_DIR=str(directory / "bin"), TUNTOM_SOCKET_OWNER=f"{os.getuid()}:{os.getgid()}",
                   TUNTOM_SWITCH_RULES_FILE="", TUNTOM_SWITCH_PRE_HOOK=str(directory / "no-pre-hook"),
                   TUNTOM_SWITCH_POST_HOOK=str(directory / "no-post-hook"))
        compiler = directory / "compiler"
        compiler.write_text('''#!/usr/bin/env bash
set -euo pipefail
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
        harness = '''source "$1"
shift
local_require_root() { :; }
local_runtime_account() { mkdir -p -- "$run_dir"; }
main "$@"
'''
        state = directory / "state/switch-sw"
        control = directory / "run/sw.control"
        rules, divert = directory / "rules with spaces", directory / "divert with spaces"
        rules.write_text(RULES)
        divert.write_text(DIVERT)
        options = ["--auto-pool", "--workers", "1"] if kind == "switch_mp" else []

        def run(*args, ok=True):
            command = ["bash", "-c", harness, "--", str(ROOT / f"mk_{kind}.sh"), "sw", *options, *map(str, args)]
            process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True, start_new_session=True)
            try:
                stdout, stderr = process.communicate(timeout=60)
            except subprocess.TimeoutExpired as error:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate()
                raise AssertionError(f"Helper timed out:\n{stdout}{stderr}") from error
            assert (process.returncode == 0) == ok, stdout + stderr
            return stdout + stderr

        def query(*args):
            return subprocess.check_output([binaries[2], str(control), *args], text=True, timeout=3)

        def pid():
            return int((state / "pid").read_text())

        peers = []
        try:
            run("--divert-file", ok=False)
            assert not state.exists()
            output = run("--rules-file", rules, "--divert-file", divert)
            if kind == "switch_mp":
                assert "configured.adapters=3\n" in output
            assert query("divert", "show") == "divert_enabled=0\n"
            assert (state / "divert").read_text() == DIVERT
            args = Path(f"/proc/{pid()}/cmdline").read_bytes().split(b"\0")
            assert args[args.index(b"--divert-file") + 1] == str(state / "divert").encode()
            assert not list((directory / "bin").glob("stage.*"))

            for name in (b"edge", b"exit", b"divert-in", b"divert-out"):
                peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                peers.append(peer)
                peer.settimeout(3)
                peer.connect(str(directory / "run/sw.sock"))
                peer.sendall(b"TTP\x01" + bytes([len(name), 0, 0, 0]) + name)
            deadline = time.monotonic() + 5
            while "connections_current=4\n" not in query("show", "stats"):
                assert time.monotonic() < deadline, "Ports did not register"
                time.sleep(.01)
            edge, exit_peer, divert_in, _ = peers
            edge.sendall(frame([17, 42]))
            assert exit_peer.recv(1024) == frame([99, 42], opcode=2)
            assert query("divert", "enable") == "divert_enabled=1\n"
            cookie = int.from_bytes(b"hTX4\0\0\0\0", "big")
            marker = int.from_bytes(b"DVRT\0\0\0\0", "big")
            expected = frame([17, 42, cookie, marker, 123, 0, 17, 42], opcode=2)
            edge.sendall(frame([17, 42]))
            assert divert_in.recv(1024) == expected

            # Invalid replacement must leave the active switch and saved config intact.
            first = pid()
            divert.write_text(DIVERT.replace("cookie hTX", "cookie invalid"))
            run("--rules-file", rules, "--divert-file", divert, ok=False)
            assert pid() == first and query("divert", "show") == "divert_enabled=1\n"
            assert (state / "divert").read_text() == DIVERT
            edge.sendall(frame([17, 42]))
            assert divert_in.recv(1024) == expected
            divert.write_text(DIVERT)
            rules.write_text("route edge:17=exit:99\nexit-port exit\n")
            rejected = run("--rules-file", rules, "--divert-file", divert, ok=False)
            assert "requires --rules-file" in rejected and pid() == first

            # A subsequent ordinary start does not implicitly reuse saved divert config.
            for peer in peers:
                peer.close()
            peers.clear()
            divert.unlink()
            run("--rules-file", rules)
            assert pid() != first
            assert b"--divert-file" not in Path(f"/proc/{pid()}/cmdline").read_bytes().split(b"\0")
            assert "divert_enabled=" not in query("show", "stats")
            print(f"PASS: {kind}: staged divert, activation, failed replacement and ordinary restart", flush=True)
        finally:
            for peer in peers:
                peer.close()
            run("--stop")
        assert not list((directory / "bin").glob("stage.*"))


if __name__ == "__main__":
    binaries = [str(Path(value).resolve()) for value in sys.argv[1:]]
    assert len(binaries) == 4
    for kind in ("switch", "switch_mp"):
        exercise(kind, binaries)
