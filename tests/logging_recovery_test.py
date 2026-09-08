#!/usr/bin/env python3
"""PF-03: live daemons forward packets/control while their logger cannot work."""
import contextlib
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from switch_reconnect_test import accept_port, listen, snapshot, until
from switch_tunnel_test import frame, terminate, wait_for


def start(stack, args, **kwargs):
    process = subprocess.Popen(args, stdout=subprocess.DEVNULL, **kwargs)
    stack.callback(terminate, process)
    return process


class Sink:
    def __init__(self, stack, directory, mode, library):
        self.mode = mode
        self.reader, self.writer = os.pipe()
        self.stack = stack
        stack.callback(self.close)
        os.set_blocking(self.reader, False)
        self.marker = Path(directory) / "stall"
        self.entered = Path(directory) / "entered"
        self.env = dict(os.environ, LD_PRELOAD=library, TUNTOM_TEST_LOG_FAULT=mode,
                        TUNTOM_TEST_LOG_STALL=str(self.marker),
                        TUNTOM_TEST_LOG_ENTERED=str(self.entered),
                        TUNTOM_SECRET="00112233445566778899aabbccddeeff")
        if mode == "full":
            os.set_blocking(self.writer, False)
            try:
                while True:
                    os.write(self.writer, b"x" * 4096)
            except BlockingIOError:
                pass
            os.set_blocking(self.writer, True)
        elif mode == "closed":
            os.close(self.reader)
            self.reader = -1
        elif mode == "stalled":
            self.marker.touch()

    def close(self):
        for fd in (self.reader, self.writer):
            if fd >= 0:
                os.close(fd)
        self.reader = self.writer = -1

    def attached(self):
        os.close(self.writer)
        self.writer = -1

    def drain(self):
        output = b""
        while True:
            try:
                data = os.read(self.reader, 65536)
                if not data:
                    break
                output += data
            except BlockingIOError:
                break
        return output

    def check(self, process, control, transfer, component):
        if self.mode in ("full", "stalled"):
            until(self.entered.exists)
        os.kill(process.pid, signal.SIGPIPE)
        # Every request transfers a fresh packet; control must respond during
        # the actual failure, not merely become healthy after unblocking stderr.
        for _ in range(100):
            transfer()
            fields = snapshot(control)
            assert process.poll() is None
        expected_threads = 1 if self.mode == "create" else 2
        assert len(list(Path(f"/proc/{process.pid}/task").iterdir())) == expected_threads
        assert fields["log_worker_started"] == ("0" if self.mode == "create" else "1")
        if self.mode == "create":
            assert fields["log_start_errors"] == "1"
            assert int(fields["log_dropped"]) > 0
        elif self.mode == "closed":
            until(lambda: int(snapshot(control)["log_write_errors"]) > 0)
            assert int(snapshot(control)["log_last_errno"]) == 32  # EPIPE
        if component == "tuntom":
            assert int(fields["log_dropped"]) > 0  # debug flood cannot grow memory
        if self.mode == "stalled" and component != "tuntom":
            # Ordinary graceful daemon shutdown may not join the blocked writer.
            process.terminate()
            assert process.wait(timeout=2) == 0
        elif self.mode in ("full", "stalled"):
            output = b""
            if self.mode == "full":
                # The writer may resume during this drain; retain its output.
                output = self.drain()
            else:
                self.marker.unlink()

            def recovered():
                nonlocal output
                output += self.drain()
                return b"\n" in output

            until(recovered)
            transfer()
            assert process.poll() is None
        print(f"PASS: {component} logger {self.mode}, live control and DATA", flush=True)


def switch_case(binary, library, mode):
    with tempfile.TemporaryDirectory(prefix="switch-pf03.") as directory, contextlib.ExitStack() as stack:
        sink = Sink(stack, directory, mode, library)
        path, control = directory + "/switch", directory + "/ctl"
        process = start(stack, [binary, "--socket", path, "--control-socket", control,
                                "--default-back=on"], stderr=sink.writer, env=sink.env)
        sink.attached()
        wait_for([path, control], [process])
        peer = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
        peer.settimeout(1)
        peer.connect(path)
        peer.sendall(b"TTP\x01\x04\x00\x00\x00test")
        until(lambda: snapshot(control)["registrations_ok"] == "1")
        wire = frame(1, b"fresh data")

        def transfer():
            peer.sendall(wire)
            assert peer.recv(65536) == bytes([1, 2]) + wire[2:]

        sink.check(process, control, transfer, "switch")


def adapter_case(binary, library, mode):
    with tempfile.TemporaryDirectory(prefix="adapter-pf03.") as directory, contextlib.ExitStack() as stack:
        sink = Sink(stack, directory, mode, library)
        path, control = directory + "/switch", directory + "/ctl"
        listener = listen(stack, path)
        tun, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.enter_context(tun)
        stack.enter_context(endpoint)
        tun.settimeout(1)
        sink.env["TUNTOM_TEST_TUN_FD"] = str(endpoint.fileno())
        process = start(stack, [binary, "test", "--switch-socket", path, "--switch-port-id", "test",
                                "--control-socket", control, "--l3-capacity", "16", "--l4-capacity", "16"],
                        stderr=sink.writer, env=sink.env, pass_fds=(endpoint.fileno(),))
        sink.attached()
        peer = accept_port(stack, listener, "test")
        wait_for([control], [process])
        packet = bytes.fromhex("4500001400000000400100000a0000010a000002")
        reply = packet[:12] + packet[16:20] + packet[12:16]

        def transfer():
            peer.sendall(bytes([1, 2]) + frame(7, packet)[2:])
            assert tun.recv(65536) == packet
            tun.sendall(reply)
            assert peer.recv(65536) == frame(7, reply)

        sink.check(process, control, transfer, "adapter")


def tunnel_case(binary, library, mode):
    with tempfile.TemporaryDirectory(prefix="tuntom-pf03.") as directory, contextlib.ExitStack() as stack:
        sink = Sink(stack, directory, mode, library)
        peers, controls, processes = {}, {}, {}
        for role in ("server", "client"):
            path, control = directory + f"/{role}", directory + f"/{role}.ctl"
            listener = listen(stack, path)
            env = sink.env if role == "server" else dict(os.environ, TUNTOM_SECRET=sink.env["TUNTOM_SECRET"])
            args = [binary, role, "242", "-"] + (["localhost"] if role == "client" else [])
            args += ["--debug" if role == "server" else "--quiet", "--no-pmtud",
                     "--switch-socket", path, "--switch-port-id", role, "--switch-label", "7",
                     "--control-socket", control]
            processes[role] = start(stack, args, env=env,
                                    stderr=sink.writer if role == "server" else subprocess.DEVNULL)
            if role == "server":
                sink.attached()
            peers[role] = accept_port(stack, listener, role)
            controls[role] = control
        wait_for(list(controls.values()), list(processes.values()))
        until(lambda: snapshot(controls["client"])["session_confirmed"] == "1")
        wire = frame(7, b"fresh data")

        def transfer():
            peers["client"].sendall(wire)
            assert peers["server"].recv(65536) == wire

        sink.check(processes["server"], controls["server"], transfer, "tuntom")


if __name__ == "__main__":
    tunnel, switch, adapter, library = sys.argv[1:]
    for mode in ("full", "closed", "stalled", "create"):
        for function, binary in ((switch_case, switch), (adapter_case, adapter), (tunnel_case, tunnel)):
            function(binary, library, mode)
