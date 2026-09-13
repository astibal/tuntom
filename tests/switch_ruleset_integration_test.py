#!/usr/bin/env python3
"""Format-1 rules, transactional control and unchanged registered peers on ST/MP."""
import concurrent.futures
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def packet(labels, payload=b"opaque packet", opcode=1):
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), 8 + 8 * len(labels) + len(payload)) + \
        b"".join(struct.pack("!Q", value) for value in labels) + payload


def config(serial, body):
    return f"format 1\nserial {serial}\n{body}\n"


class Switch:
    def __init__(self, binary, ctl, root, initial, fault_library=None):
        self.binary, self.ctl, self.root = binary, ctl, root
        self.data, self.control, self.file = root / "data", root / "control", root / "rules"
        self.file.write_text(initial)
        self.peers = []
        self.log = open(root / "log", "w+")
        self.process = None
        self.marker = root / "plan-fault"
        self.environment = os.environ.copy()
        if fault_library:
            self.environment.update(LD_PRELOAD=fault_library, TUNTOM_V2_FAULT_MARKER=str(self.marker))
        self.start()

    def start(self):
        args = [self.binary, "--socket", str(self.data), "--control-socket", str(self.control), "--rules-file", str(self.file)]
        if "mp" in Path(self.binary).name:
            args += ["--workers", "2", "--pool-size", "16", "--queue-size", "16"]
        self.process = subprocess.Popen(args, stdout=self.log, stderr=self.log, env=self.environment)
        for _ in range(200):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read())
            if self.control.exists():
                self.command("show")
                return
            time.sleep(.01)
        raise AssertionError("startup timeout")

    def stop(self):
        for peer in self.peers:
            peer.close()
        self.peers = []
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            self.process.wait(timeout=5)
            assert self.process.returncode == 0

    def command(self, operation, body=None, ok=True):
        args = [self.ctl, str(self.control), "rules", operation]
        if body is not None:
            args += ["-"]
        result = subprocess.run(args, input=body, text=True, capture_output=True, timeout=15)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        if not ok:
            assert result.stdout == "", result.stdout
            return result.stderr
        assert result.stderr == "", result.stderr
        return result.stdout

    def stats(self):
        result = subprocess.check_output([self.ctl, str(self.control), "show", "stats"], text=True, timeout=3)
        return dict(line.split("=", 1) for line in result.splitlines())

    def port(self, name):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        peer.settimeout(2)
        peer.connect(str(self.data))
        encoded = name.encode()
        peer.sendall(b"TTP\x01" + bytes([len(encoded), 0, 0, 0]) + encoded)
        self.peers.append(peer)
        expected = len(self.peers)
        for _ in range(100):
            if int(self.stats()["connections_current"]) == expected:
                return peer
            time.sleep(.01)
        raise AssertionError("registration timeout")

    def raw(self):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        peer.settimeout(2)
        peer.connect(str(self.control))
        return peer


def quiet(*peers):
    assert not select.select(peers, [], [], .10)[0], "unexpected forwarded packet"


def run(binary, ctl, root, fault_library=None):
    initial = config(1, """
# declarations stay wildcard patterns in show
exit out*
exit local-exit
switch block-*,* drop [id=blocked]
switch client*,* to out-block,* drop
switch client*,* to out*,1001 allow
label client*,* to out*, [1001, ...] # keep the lower labels
label client-1,17 to missing, [4]
""")
    sw = Switch(binary, ctl, root, initial, fault_library)
    try:
        a, b, c, blocked, source_blocked = [sw.port(p) for p in ("client-1", "out-a", "out-b", "out-block", "block-1")]
        original_show = sw.command("show")
        assert "#" not in original_show and "exit out*\n" in original_show
        assert original_show.index("label client*,*") < original_show.index("label client-1,17")
        assert "unchanged serial=1" in sw.command("load", original_show)
        assert "unchanged serial=1" in sw.command("load", original_show.replace("format 1", "format 01 # comment"))
        expected = packet([1001, 99], opcode=2)
        a.sendall(packet([17, 99]))
        ready = select.select([b, c, blocked], [], [], 2)[0]
        assert len(ready) == 1 and ready[0] != blocked
        selected = ready[0]
        assert selected.recv(70000) == expected
        for _ in range(4):
            a.sendall(packet([17, 99]))
            assert selected.recv(70000) == expected
        quiet(b, c, blocked)
        source_blocked.sendall(packet([17]))
        quiet(a, b, c, blocked)
        assert "same serial" in sw.command("load", config(1, "switch allow"), ok=False)
        assert "older" in sw.command("load", config(0, "switch allow"), ok=False)
        assert "line 3" in sw.command("load", config(2, "nonsense"), ok=False)
        assert "capture is not supported yet" in sw.command("check", config(2, "switch capture debug"), ok=False)
        assert sw.command("show") == original_show

        update = config(2, "switch client*,* to out-b,44 allow\nlabel client*,* to out-b, [44]")
        if fault_library and "mp" in Path(binary).name:
            sw.marker.write_text("epoll")
            try:
                for operation in ("check", "load"):
                    assert "Cannot prepare worker epoll" in sw.command(operation, update, ok=False)
                    assert sw.command("show") == original_show
                    a.sendall(packet([17, 99]))
                    assert selected.recv(70000) == expected
            finally:
                sw.marker.unlink()
        assert "checked serial=2" in sw.command("check", update)
        assert sw.command("show") == original_show
        assert "applied serial=2" in sw.command("load", update)
        assert sw.stats()["connections_current"] == "5"
        a.sendall(packet([17, 99]))
        assert c.recv(70000) == packet([44])
        quiet(b, blocked)
        if "mp" in Path(binary).name:
            stats = sw.stats()
            out_index = next(k[:-5] for k, v in stats.items() if k.endswith("_name") and v == "out-b")
            assert stats[out_index + "_kind"] == "tunnel"

        # A partial transaction must neither publish nor monopolize control/data.
        with sw.raw() as stalled:
            partial = config(3, "switch drop")
            stalled.sendall(f"rules load {len(partial)}".encode())
            stalled.sendall(partial[:8].encode())
            assert "serial 2\n" in sw.command("show")
            a.sendall(packet([17]))
            assert c.recv(70000) == packet([44])
        assert "serial 2\n" in sw.command("show")
        with sw.raw() as expired:
            expired.settimeout(7)
            expired.sendall(b"rules load 100")
            assert expired.recv(256) == b"", "incomplete request must expire"
        with sw.raw() as truncated:
            truncated.sendall(b"rules load 1")
            truncated.sendall(b"too much")
            assert truncated.recv(256).startswith(b"ERROR ")
        with sw.raw() as oversized:
            oversized.sendall(b"rules load 1048577")
            assert oversized.recv(256).startswith(b"ERROR ")
        assert "serial 2\n" in sw.command("show")

        # Expanded canonical response crosses the old 64 KiB single-record limit.
        large = config(3, "switch allow\nlabel client*,* to out-a, [keep,...]\n" +
                       "\n".join(f"label unused-{i},1 to unavailable, [1] [id=rule-{i}]" for i in range(1500)))
        assert len(large) > 65536
        sw.command("load", large)
        exported = sw.command("show")
        assert len(exported) > 65536
        sw.command("check", exported)
        assert "unchanged" in sw.command("load", exported)
        a.sendall(packet([18446744073709551615, 0, 4]))
        assert b.recv(70000) == packet([18446744073709551615, 0, 4])

        # Both first-policy and first-mapping selection terminate, including no live target.
        sw.command("load", config(4, "switch drop\nswitch allow\nlabel *,* to out*, [keep,...]"))
        a.sendall(packet([17]))
        quiet(a, b, c, blocked)
        assert int(sw.stats()["policy_drops"]) >= 1
        sw.command("load", config(5, "switch allow\nlabel *,* to disconnected, [keep,...]\nlabel client*,* to out-a, [keep,...]"))
        a.sendall(packet([17]))
        quiet(a, b, c, blocked)
        assert int(sw.stats()["target_disconnected"]) >= 1
        sw.command("load", config(6, "switch allow\nlabel *,* to out-a, [keep,keep,keep]"))
        a.sendall(packet([17]))
        quiet(a, b, c, blocked)
        assert int(sw.stats()["rewrite_drops"]) >= 1
        sw.command("load", config(7, "label *,* to out-a, [keep,...]"))
        a.sendall(packet([17]))
        quiet(a, b, c, blocked)

        # Concurrent commits cannot roll active serial backwards.
        def concurrent_load(serial):
            return subprocess.run([ctl, str(sw.control), "rules", "load", "-"],
                                  input=config(serial, "switch drop"), text=True, capture_output=True, timeout=10)
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            results = list(pool.map(concurrent_load, (8, 9)))
        assert results[1].returncode == 0
        assert results[0].returncode == 0 or "older" in results[0].stderr
        assert "serial 9\n" in sw.command("show")

        if "mp" in Path(binary).name:
            # A ruleset can fit now but exceed the preparation bound as ports
            # arrive. Reject that registration without killing the active plan.
            bounded = config(10, "switch allow\nlabel client*,17 to out-a,[keep,...]\n" +
                             "\n".join(f"label *,{i + 1000} to missing,[keep,...]" for i in range(4090)))
            sw.command("load", bounded)
            for i in range(11):
                sw.port(f"extra-{i}")
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as rejected:
                rejected.settimeout(5)
                rejected.connect(str(sw.data))
                rejected.sendall(b"TTP\x01\x08\x00\x00\x00rejected")
                assert rejected.recv(256) == b""
            assert sw.stats()["connections_current"] == "16"
            assert "serial 10\n" in sw.command("show")
            a.sendall(packet([17]))
            assert b.recv(70000) == packet([17])

        # Runtime loads do not overwrite the startup file. Export can become that file.
        sw.stop()
        sw.start()
        assert sw.command("show") == original_show
        sw.file.write_text(exported)
        sw.stop()
        sw.start()
        assert sw.command("show") == exported
        assert sw.command("show").startswith("format 1\nserial 3\n")
    finally:
        sw.stop()
        sw.log.close()
    print("PASS:", Path(binary).name, "ordered rules, ECMP, stack rewrite, export, reload, framing and restart", flush=True)


if __name__ == "__main__":
    for binary in sys.argv[1:3]:
        with tempfile.TemporaryDirectory(prefix="rules-test-") as root:
            run(binary, sys.argv[3], Path(root), sys.argv[4] if len(sys.argv) > 4 else None)
