#!/usr/bin/env python3
"""PF-05: bounded admission, descriptor reserve and live recovery, without TUN/root."""
import contextlib
import errno
import os
from pathlib import Path
import resource
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from switch_reconnect_test import snapshot, start, until
from switch_test import frame, register
from runtime_recovery_test import cpu_ticks
from switch_tunnel_test import wait_for


class Switch:
    def __init__(self, stack, binary, library, *, options=(), inherited=0, target="data", error=errno.EMFILE, once=False):
        self.directory = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="switch-pf05.")))
        self.path = str(self.directory / "data")
        self.ctl = str(self.directory / "control")
        self.accept_marker = self.directory / "accept-fault"
        self.poll_marker = self.directory / "poll-fault"
        self.alloc_marker = self.directory / "alloc-fault"
        self.stack = stack
        env = dict(os.environ, LD_PRELOAD=library,
                   TUNTOM_TEST_ACCEPT_MARKER=str(self.accept_marker),
                   TUNTOM_TEST_POLL_MARKER=str(self.poll_marker),
                   TUNTOM_TEST_ALLOC_MARKER=str(self.alloc_marker),
                   TUNTOM_TEST_ACCEPT_TARGET=str(self.directory / target),
                   TUNTOM_TEST_ACCEPT_ERRNO=str(error))
        if once:
            env["TUNTOM_TEST_ACCEPT_ONCE"] = "1"
        handles = [stack.enter_context(open("/dev/null", "rb")) for _ in range(inherited)]
        self.process = start(stack, [binary, "--socket", self.path, "--control-socket", self.ctl,
                                     "--default-back=on", *options], env=env,
                             preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64)),
                             pass_fds=tuple(handle.fileno() for handle in handles))
        wait_for([self.path, self.ctl], [self.process])
        self.stats()

    def stats(self):
        return {key: int(value) if value.isdecimal() else value for key, value in snapshot(self.ctl).items()}

    def connect(self, port=None, path=None):
        peer = self.stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
        peer.settimeout(2)
        peer.connect(path or self.path)
        if port is not None:
            register(peer, port)
        return peer

    def port(self, name):
        count = self.stats()["registrations_ok"]
        peer = self.connect(name)
        until(lambda: self.stats()["registrations_ok"] > count)
        return peer

    def fd_count(self):
        return len(list(Path(f"/proc/{self.process.pid}/fd").iterdir()))

    def rss_kib(self):
        fields = Path(f"/proc/{self.process.pid}/status").read_text().splitlines()
        return int(next(line for line in fields if line.startswith("VmRSS:")).split()[1])


def echo(peer, payload=b"live packet"):
    peer.sendall(frame(1, [7], payload))
    assert peer.recv(65536) == frame(2, [7], payload)


def closed(peer):
    try:
        assert peer.recv(1024) == b"", "unexpected data on rejected connection"
    except ConnectionResetError:
        pass


def cpu_fraction(process, before, began):
    return (cpu_ticks(process) - before) / os.sysconf("SC_CLK_TCK") / (time.monotonic() - began)


def capacity_case(binary, library, inherited):
    with contextlib.ExitStack() as stack:
        switch = Switch(stack, binary, library, inherited=inherited,
                        options=("--route", "a:11=b:12", "--route", "b:11=a:12"))
        a, b = switch.port("a"), switch.port("b")
        baseline = 5 + inherited + 2  # stdio, two listeners, inherited FDs, two ports
        # Receiving a snapshot can race its server-side client's RAII close.
        until(lambda: switch.fd_count() == baseline)
        stats = switch.stats()
        # Control snapshot temporarily owns one more FD, so count /proc outside it.
        assert stats["connections_limit_ports"] + stats["connections_limit_pending"] + baseline - 2 == 48, (stats, baseline)
        fillers = []
        for _ in range(100):
            peer = switch.connect()
            fillers.append(peer)
            if len(fillers) == 70:
                break  # 16 accepted plus <=65 queued: never block the test's connect.
        until(lambda: switch.stats()["connections_pending"] == 16)
        began, ticks = time.monotonic(), cpu_ticks(switch.process)
        while time.monotonic() - began < 1.1:
            for source, destination in ((a, b), (b, a)):
                source.sendall(frame(1, [11], b"fresh packet"))
                assert destination.recv(65536) == frame(1, [12], b"fresh packet")
            stats = switch.stats()
            assert stats["connections_total"] == 18 and stats["listener_accept_errors"] == 0
            assert switch.fd_count() <= 48
            time.sleep(0.025)
        assert cpu_fraction(switch.process, ticks, began) < 0.5, "capacity listener spin"
        for peer in fillers:
            peer.close()
        until(lambda: switch.stats()["connections_total"] == 2, timeout=5)
        until(lambda: switch.fd_count() == baseline)
        echo(switch.port("recovered"))
        print(f"PASS: 64-FD budget, {inherited} inherited FDs, live control/forwarding and recovery", flush=True)


def registration_case(binary, library):
    with contextlib.ExitStack() as stack:
        switch = Switch(stack, binary, library, options=("--max-ports", "1", "--max-pending", "2"))
        name = "r" * 63
        original = switch.port(name)
        baseline = 6  # stdio, two listeners, one port
        until(lambda: switch.fd_count() == baseline)
        denied = switch.connect("new-port")
        closed(denied)
        assert switch.stats()["registrations_capacity_rejected"] == 1
        echo(original)
        invalid = switch.connect()
        invalid.sendall(b"invalid registration")
        closed(invalid)
        assert switch.stats()["registrations_invalid"] == 1

        switch.alloc_marker.touch()
        failed = switch.connect(name)
        closed(failed)
        switch.alloc_marker.unlink()
        until(lambda: switch.stats()["runtime_allocation_errors"] >= 1)
        echo(original, b"old port survived failed replacement")
        replacement = switch.port(name)
        closed(original)
        echo(replacement)
        assert switch.stats()["connections_current"] == 1

        pending = [switch.connect(), switch.connect()]
        until(lambda: switch.stats()["connections_pending"] == 2)
        # No control queries or incoming DATA during expiry: the timer alone must wake poll.
        time.sleep(5.2)
        for peer in pending:
            closed(peer)
        stats = switch.stats()
        assert stats["registrations_timed_out"] == 2 and stats["connections_pending"] == 0
        echo(replacement, b"registered idle port survived")
        until(lambda: switch.fd_count() == baseline)
        print("PASS: full-port replacement, allocation rollback, idle expiry and registered idle port", flush=True)


def churn_case(binary, library):
    with contextlib.ExitStack() as stack:
        switch = Switch(stack, binary, library)
        live = switch.port("live")
        baseline = 6
        until(lambda: switch.fd_count() == baseline)
        accepted_before = switch.stats()["connections_accepted"]
        memory_before = switch.rss_kib()
        began, ticks = time.monotonic(), cpu_ticks(switch.process)
        # More attempts than the token bucket can admit; queued clients send
        # invalid registrations then disconnect. No unbounded test-side queue.
        for cycle in range(5):
            peers = [switch.connect() for _ in range(20)]
            for peer in peers:
                peer.sendall(b"invalid")
                peer.close()
            until(lambda: switch.stats()["connections_accepted"] >= accepted_before + 20 * (cycle + 1))
            echo(live)
        stats = switch.stats()
        elapsed = time.monotonic() - began
        assert stats["connections_accepted"] - accepted_before <= 16 + int(elapsed * 32) + 1
        assert stats["listener_accept_rate_limit_hits"] > 0
        assert cpu_fraction(switch.process, ticks, began) < 0.5
        until(lambda: switch.stats()["connections_total"] == 1)
        until(lambda: switch.fd_count() == baseline)
        assert switch.rss_kib() <= memory_before + 1024, "memory grew during bounded churn"
        echo(switch.port("after-churn"))
        print("PASS: repeated churn, bounded acceptance/CPU/FD and new port after flood", flush=True)


def fault_case(binary, library, target, error, once=False, recovery=False):
    with contextlib.ExitStack() as stack:
        switch = Switch(stack, binary, library, target=target, error=error, once=once)
        live = switch.port("live")
        if recovery:
            switch.poll_marker.touch()
            # Wake an existing successful main poll, then enter PF-02 recovery.
            switch.stats()
            time.sleep(0.12)
        switch.accept_marker.touch()
        began, ticks = time.monotonic(), cpu_ticks(switch.process)
        if target == "control":
            waiting = switch.connect(path=switch.ctl)
            waiting.sendall(b"show stats")
        else:
            waiting = switch.connect("waiting")
        while time.monotonic() - began < 2.2:
            assert switch.process.poll() is None
            if not recovery:
                echo(live, b"DATA while accept fails")
            if target == "data":
                switch.stats()
            time.sleep(0.03)
        assert cpu_fraction(switch.process, ticks, began) < 0.5, "accept/recovery spin"
        switch.accept_marker.unlink()
        if recovery:
            switch.poll_marker.unlink()
        if target == "control":
            waiting.settimeout(3)
            reply = waiting.recv(65536)
            assert b"component=switch" in reply
        else:
            until(lambda: switch.stats()["registrations_ok"] == 2)
            echo(waiting, b"new port after failure")
        stats = switch.stats()
        prefix = "control" if target == "control" else "listener"
        assert stats[prefix + "_accept_last_errno"] == error
        errors = stats[prefix + "_accept_errors"]
        assert (errors == 1 if once else 2 <= errors <= 3), (target, error, errors)
        if recovery:
            assert 1 <= stats["runtime_poll_errors"] <= 30
        echo(live, b"fresh DATA after recovery")
        print(f"PASS: {target} accept errno={error}, once={once}, PF-02 overlap={recovery}", flush=True)


def cli_case(binary):
    for option in ("--max-ports", "--max-pending"):
        for value in ("0", "-1", "+1", "65536", "1x", "999999999999999999999999"):
            result = subprocess.run([binary, option, value, "--help"], capture_output=True, timeout=2)
            assert result.returncode != 0, (option, value)
        for value in ("1", "65535"):
            result = subprocess.run([binary, option, value, "--help"], capture_output=True, timeout=2)
            assert result.returncode == 0, (option, value)


if __name__ == "__main__":
    binary, library = sys.argv[1:]
    cli_case(binary)
    for inherited in (0, 8):
        capacity_case(binary, library, inherited)
    registration_case(binary, library)
    churn_case(binary, library)
    for target in ("data", "control"):
        for error in (errno.EMFILE, errno.ENFILE, errno.ENOMEM, errno.ENOBUFS):
            fault_case(binary, library, target, error)
        fault_case(binary, library, target, errno.EMFILE, once=True)
    fault_case(binary, library, "control", errno.EMFILE, recovery=True)
