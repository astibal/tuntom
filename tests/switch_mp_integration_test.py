#!/usr/bin/env python3
"""Real SOCK_SEQPACKET tests for the independently built MP switch.

Only starts child processes and sockets inside its own temporary directory.
No TUN, privileges, network changes, installed binaries or services are used.
"""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def frame(labels, payload, opcode=1):
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), 8 + 8 * len(labels) + len(payload)) + b"".join(
        struct.pack("!Q", label) for label in labels) + payload


def until(check, timeout=8):
    deadline = time.monotonic() + timeout
    while True:
        result = check()
        if result:
            return result
        assert time.monotonic() < deadline, "condition timed out"
        time.sleep(.005)


class Switch:
    def __init__(self, binary, *options, affinity=None):
        self.directory = tempfile.TemporaryDirectory(prefix="tomtom-mp-test.")
        self.root = Path(self.directory.name)
        self.data, self.control = self.root / "data", self.root / "control"
        self.stderr = open(self.root / "stderr", "w+")
        self.peers = []
        self.process = subprocess.Popen(
            [binary, "--socket", str(self.data), "--control-socket", str(self.control), *options],
            stdout=subprocess.DEVNULL, stderr=self.stderr,
            preexec_fn=(lambda: os.sched_setaffinity(0, affinity)) if affinity else None)
        try:
            def ready():
                if self.process.poll() is not None:
                    self.stderr.seek(0)
                    raise AssertionError(self.stderr.read())
                return self.control.exists()
            until(ready)
            self.stats()
        except BaseException:
            self.close(check=False)
            raise

    def stats(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
            peer.settimeout(5)
            peer.connect(str(self.control))
            peer.sendall(b"show stats")
            wire = peer.recv(1 << 20).decode()
        result = {}
        for line in wire.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                result[key] = int(value) if value.isdigit() else value
        assert result.get("implementation") == "tomtom-switch-mp", result
        return result

    def connect(self, name=None):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        peer.settimeout(5)
        peer.connect(str(self.data))
        self.peers.append(peer)
        if name is not None:
            previous = self.stats()["registrations_ok"]
            value = name.encode()
            peer.sendall(b"TTP\x01" + bytes([len(value), 0, 0, 0]) + value)
            until(lambda: self.stats()["registrations_ok"] > previous)
        return peer

    def close(self, check=True):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("MP shutdown stuck")
        for peer in self.peers:
            peer.close()
        self.stderr.seek(0)
        errors = self.stderr.read()
        result = self.process.returncode
        self.stderr.close()
        removed = not self.data.exists() and not self.control.exists()
        self.directory.cleanup()
        if check:
            assert result == 0, errors
            assert removed, "owned socket paths not removed"
            assert "ThreadSanitizer" not in errors and "AddressSanitizer" not in errors, errors

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        self.close(check=kind is None)


def quiet(peer):
    peer.settimeout(.05)
    try:
        value = peer.recv(70000)
        raise AssertionError(f"unexpected frame: {value[:64]!r}")
    except TimeoutError:
        pass
    finally:
        peer.settimeout(5)


def owners(stats):
    return {stats[f"port_{i}_name"]: (stats[f"port_{i}_rx_owner"], stats[f"port_{i}_tx_owner"])
            for i in range(stats["connections_current"])}


def protocol(binary):
    with Switch(binary, "--workers", "8", "--default-back=on", "--exit-port", "adapter",
                "--exit-port", "adapter2", "--trunk-port", "trunk", "--route", "a:17=b:83",
                "--route", "a:18=adapter:84", "--route", "a:19=absent:85",
                "--route", "a:20=trunk:86") as switch:
        initial = switch.stats()
        assert initial["workers_active"] == min(2, initial["workers_pool"])
        assert initial["workers_pool"] <= initial["hardware_worker_limit"]
        a, b, adapter, adapter2, trunk = (switch.connect(name) for name in ("a", "b", "adapter", "adapter2", "trunk"))
        a.sendall(frame([17, 100, 200], b"payload"))
        assert b.recv(70000) == frame([83, 100, 200], b"payload")
        a.sendall(frame([18], b"adapter"))
        assert adapter.recv(70000) == frame([84], b"adapter", 2)
        a.sendall(frame([20], b"trunk"))
        assert trunk.recv(70000) == frame([86], b"trunk")
        a.sendall(frame([777], b"default"))
        assert a.recv(70000) == frame([777], b"default", 2)
        a.sendall(frame([19], b"missing target"))
        quiet(a)
        for payload in (b"invalid", frame([17], b"wrong opcode", 2), frame([17], b"bad length")[:-1]):
            a.sendall(payload)
        until(lambda: switch.stats()["malformed_frames"] == 3)
        a.sendall(frame([17], b"still usable"))
        assert b.recv(70000) == frame([83], b"still usable")
        new_b = switch.connect("b")
        a.sendall(frame([17], b"new generation"))
        assert new_b.recv(70000) == frame([83], b"new generation")
        assert b.recv(70000) == b""
        stats = switch.stats()
        assert stats["route_misses"] == 1 and stats["target_disconnected"] == 1
        assert stats["default_back"] == 1 and stats["exit_deliveries"] == 1 and stats["send_errors"] == 0
        mapping = owners(stats)
        assert mapping["adapter"] == mapping["adapter2"] == mapping["trunk"]
        if stats["workers_pool"] >= 4:
            assert set(mapping["adapter"]).isdisjoint(mapping["a"])
        assert stats["matrix_queues"] < stats["connections_current"] ** 2
    print("PASS: protocol, labels, EXIT/default, adapter sharing, trunk, reconnect", flush=True)


def live_migration(binary, workers):
    with Switch(binary, "--workers", str(workers), "--work-per-thread", "2",
                "--route", "a:17=b:99", "--exit-port", "adapter", "--trunk-port", "trunk") as switch:
        a, b = switch.connect("a"), switch.connect("b")
        baseline = switch.stats()
        stop = threading.Event()
        errors = []
        count = [0]

        def traffic():
            try:
                while not stop.is_set():
                    body = struct.pack("!Q", count[0]) + b"live" * 350
                    a.sendall(frame([17, 123], body))
                    assert b.recv(70000) == frame([99, 123], body)
                    count[0] += 1
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=traffic)
        thread.start()
        try:
            for _ in range(2):
                extras = [switch.connect(name) for name in ("c", "d", "e", "f", "adapter", "trunk")]
                expanded = switch.stats()
                assert expanded["workers_pool"] == baseline["workers_pool"]
                assert expanded["workers_active"] == min(8, expanded["workers_pool"])
                for peer in extras:
                    peer.close()
                until(lambda: switch.stats()["connections_current"] == 2)
                assert switch.stats()["workers_active"] == min(2, baseline["workers_pool"])
            until(lambda: count[0] >= 200)
        finally:
            stop.set()
            thread.join(timeout=6)
        assert not thread.is_alive() and not errors, errors
        until(lambda: switch.stats()["buffers_in_use"] == 0)
        final = switch.stats()
        assert final["frames_rx"] == final["frames_tx"] == count[0]
        assert final["queue_full_drops"] == final["reconfiguration_drops"] == 0
        assert final["scheduler_version"] > baseline["scheduler_version"] + 10
        until(lambda: all(switch.stats()[f"worker_{i}_version"] == final["scheduler_version"]
                          for i in range(final["workers_pool"])))
    print(f"PASS: workers={workers}, live split/merge, mixed roles, FIFO, version adoption", flush=True)


def blocked(binary, workers):
    with Switch(binary, "--workers", str(workers), "--work-per-thread", "2",
                "--pool-size", "16", "--queue-size", "4", "--route", "a:17=b:99",
                "--route", "c:17=d:99") as switch:
        a, b, c, d = (switch.connect(name) for name in ("a", "b", "c", "d"))
        a.setblocking(False)
        accepted = 0
        body = b"P" * 9000
        deadline = time.monotonic() + .15
        while time.monotonic() < deadline:
            try:
                a.send(frame([17], struct.pack("!Q", accepted) + body))
                accepted += 1
            except BlockingIOError:
                time.sleep(.0001)
        until(lambda: switch.stats()["send_eagain"] > 0)
        # Change worker ownership with a blocked pending TX, without draining it.
        extras = [switch.connect(name) for name in ("e", "f", "g")]
        for i in range(30):
            payload = struct.pack("!I", i) + b"healthy"
            c.sendall(frame([17], payload))
            assert d.recv(70000) == frame([99], payload)
            if i % 5 == 0:
                time.sleep(.002)
        for peer in extras:
            peer.close()
        until(lambda: switch.stats()["connections_current"] == 4)
        until(lambda: switch.stats()["frames_rx"] == accepted + 30)
        previous, received = -1, 0
        b.settimeout(.1)
        while True:
            try:
                value = b.recv(70000)
                sequence = struct.unpack_from("!Q", value, 16)[0]
                assert sequence > previous and value == frame([99], struct.pack("!Q", sequence) + body)
                previous, received = sequence, received + 1
            except TimeoutError:
                stats = switch.stats()
                if stats["buffers_in_use"] == 0:
                    break
        assert received + stats["queue_full_drops"] == accepted
        assert stats["frames_tx"] == received + 30 and stats["reconfiguration_drops"] == 0
        assert stats["send_backpressure_drops"] == 0
        # Fill again, wait until RX consumed all input, then replace the blocked
        # destination. Old-generation queued/pending buffers must be discarded.
        before = stats["frames_rx"]
        second = 0
        deadline = time.monotonic() + .1
        while time.monotonic() < deadline:
            try:
                a.send(frame([17], body))
                second += 1
            except BlockingIOError:
                time.sleep(.0001)
        until(lambda: switch.stats()["frames_rx"] == before + second)
        assert switch.stats()["buffers_in_use"] > 0
        new_b = switch.connect("b")
        quiet(new_b)
        until(lambda: switch.stats()["buffers_in_use"] == 0)
        assert switch.stats()["reconfiguration_drops"] > 0
        a.settimeout(5)
        a.sendall(frame([17], b"new"))
        assert new_b.recv(70000) == frame([99], b"new")
    print(f"PASS: workers={workers}, blocked-output isolation, pending migration, POLLOUT, reconnect reclamation", flush=True)


def limits(binary):
    with Switch(binary, "--workers", "65535", "--default-back=on", "--max-ports", "1", "--max-pending", "2",
                affinity={min(os.sched_getaffinity(0))}) as switch:
        assert switch.stats()["workers_pool"] == 1
        a = switch.connect("a")
        a.sendall(frame([7], b"single CPU"))
        assert a.recv(70000) == frame([7], b"single CPU", 2)
        rejected = switch.connect()
        rejected.sendall(b"TTP\x01\x01\0\0\0b")
        assert rejected.recv(100) == b""
        assert switch.stats()["registrations_capacity_rejected"] == 1
        replacement = switch.connect("a")
        assert a.recv(100) == b""
        replacement.sendall(frame([7], b"replacement at capacity"))
        assert replacement.recv(70000) == frame([7], b"replacement at capacity", 2)
        bad = switch.connect()
        bad.sendall(b"not a registration")
        assert bad.recv(100) == b""
        idle = switch.connect()
        until(lambda: switch.stats()["registrations_timed_out"] == 1)
        assert idle.recv(100) == b""
        assert switch.stats()["registrations_invalid"] == 1
    with tempfile.TemporaryDirectory(prefix="tomtom-mp-cli.") as directory:
        existing = Path(directory) / "existing"
        existing.write_text("keep this file")
        result = subprocess.run([binary, "--socket", str(existing)], capture_output=True, timeout=8)
        assert result.returncode != 0 and existing.read_text() == "keep this file"
        for args in (("--workers", "0"), ("--queue-size", "-1"), ("--exit-port", "x", "--trunk-port", "x"),
                     ("--route", "x:1=y:2", "--route", "x:1=z:3")):
            result = subprocess.run([binary, "--socket", str(Path(directory) / "data"), *args], capture_output=True, timeout=8)
            assert result.returncode != 0
    print("PASS: CPU affinity clamp, single-worker duplex, admission, timeout, socket ownership", flush=True)


def pool_exhaustion(binary):
    with Switch(binary, "--workers", "1", "--pool-size", "4", "--queue-size", "8",
                "--route", "a:1=b:2", "--route", "c:1=d:2") as switch:
        a, b, c, d = (switch.connect(name) for name in ("a", "b", "c", "d"))
        a.setblocking(False)
        deadline = time.monotonic() + .15
        while time.monotonic() < deadline:
            try:
                a.send(frame([1], b"X" * 9000))
            except BlockingIOError:
                time.sleep(.0001)
        until(lambda: switch.stats()["pool_stalls"] > 0)
        assert switch.stats()["buffers_in_use"] == 4
        c.sendall(frame([1], b"another pool"))
        assert d.recv(70000) == frame([2], b"another pool")
        # A full ingress pool must not hide its peer's HUP or prevent retiring
        # queues, including a buffer already popped by the destination TX.
        a.close()
        until(lambda: switch.stats()["connections_current"] == 3)
        until(lambda: switch.stats()["buffers_in_use"] == 0)
        assert switch.stats()["reconfiguration_drops"] == 4
        a = switch.connect("a")
        b.close()
        until(lambda: switch.stats()["connections_current"] == 3)
        b = switch.connect("b")
        a.sendall(frame([1], b"fresh pool"))
        assert b.recv(70000) == frame([2], b"fresh pool")
    print("PASS: exhausted pool isolation, ingress HUP, source generation reclamation", flush=True)


def readiness(binary):
    with Switch(binary, "--workers", "1", "--route", "a:1=b:2",
                "--route", "c:1=d:2") as switch:
        a, b, c, d = (switch.connect(name) for name in ("a", "b", "c", "d"))
        idle = [switch.connect(f"idle{i}") for i in range(20)]
        for i in range(8):
            a.sendall(frame([1], b"warm"))
            assert b.recv(70000) == frame([2], b"warm")
        before = switch.stats()
        began = time.monotonic()
        for i in range(200):
            body = struct.pack("!Q", i)
            a.sendall(frame([1], body))
            assert b.recv(70000) == frame([2], body)
        after = switch.stats()
        assert after["recv_calls"] - before["recv_calls"] <= 4 * 200 + 20, after
        assert after["cpu_samples"] - before["cpu_samples"] <= 15 * (time.monotonic()-began) + 5

        # More than one RR turn must be consumed from a single readiness edge.
        for i in range(64):
            a.sendall(frame([1], struct.pack("!Q", i)))
        for i in range(64):
            assert b.recv(70000) == frame([2], struct.pack("!Q", i))

        # Discover a previously idle input even while another input keeps work available.
        stop = threading.Event()
        failures = []
        def hot_path():
            try:
                while not stop.is_set():
                    a.sendall(frame([1], b"hot"))
                    assert b.recv(70000) == frame([2], b"hot")
            except BaseException as error:
                failures.append(error)
        thread = threading.Thread(target=hot_path)
        thread.start()
        try:
            for i in range(30):
                body = struct.pack("!Q", i) + b"new readiness"
                c.sendall(frame([1], body))
                assert d.recv(70000) == frame([2], body)
                time.sleep(.001)
        finally:
            stop.set()
            thread.join(timeout=6)
        assert not thread.is_alive() and not failures, failures
        until(lambda: switch.stats()["buffers_in_use"] == 0)
        assert switch.stats()["queue_full_drops"] == 0
        assert len(idle) == 20
    print("PASS: sparse RX syscalls, retained ET readiness, new input fairness, sampled CPU stats", flush=True)


def paired_start(binary):
    with Switch(binary, "--workers", "8", "--exit-port", "adapter",
                "--route", "a:1=adapter:2", "--route", "adapter:1=a:2") as switch:
        a, adapter = switch.connect("a"), switch.connect("adapter")
        initial = switch.stats()
        if initial["workers_pool"] >= 2:
            mapping = owners(initial)
            assert initial["workers_active"] == 2
            assert mapping["a"][0] == mapping["adapter"][1]
            assert mapping["adapter"][0] == mapping["a"][1]
        stop = threading.Event()
        failures, count = [], [0]
        def traffic():
            try:
                while not stop.is_set():
                    body = struct.pack("!Q", count[0]) + b"paired" * 200
                    a.sendall(frame([1], body))
                    assert adapter.recv(70000) == frame([2], body, 2)
                    adapter.sendall(frame([1], body))
                    assert a.recv(70000) == frame([2], body)
                    count[0] += 1
            except BaseException as error:
                failures.append(error)
        thread = threading.Thread(target=traffic)
        thread.start()
        try:
            for _ in range(3):
                extra = switch.connect("extra")
                assert switch.stats()["workers_active"] == min(4, initial["workers_pool"])
                extra.close()
                until(lambda: switch.stats()["connections_current"] == 2)
                assert switch.stats()["workers_active"] == min(2, initial["workers_pool"])
            until(lambda: count[0] >= 200)
        finally:
            stop.set()
            thread.join(timeout=6)
        assert not thread.is_alive() and not failures, failures
        final = switch.stats()
        assert final["frames_rx"] == final["frames_tx"] == 2 * count[0]
        assert final["queue_full_drops"] == final["reconfiguration_drops"] == 0
    print("PASS: paired startup, duplex forwarding and live transition to/from split roles", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    protocol(binary)
    for workers in (1, 2, 8):
        live_migration(binary, workers)
        blocked(binary, workers)
    pool_exhaustion(binary)
    readiness(binary)
    paired_start(binary)
    limits(binary)


if __name__ == "__main__":
    main()
