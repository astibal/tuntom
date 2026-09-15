#!/usr/bin/env python3
"""Divert routing through real ST/MP processes, with independently encoded frames."""
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
from switch_ruleset_v2_integration_test import Harness, Endpoint, quiet
from switch_ruleset_integration_test import packet

COOKIE = int.from_bytes(b"hTX4\0\0\0\0", "big")
DVRT = int.from_bytes(b"DVRT\0\0\0\0", "big")


def labels(action, base=(17, 42), origin=123):
    return [*base, COOKIE, DVRT, origin, action, 17, 42]


class DivertSwitch(Harness):
    def start(self):
        divert = self.root / "divert.conf"
        divert.write_text("cookie hTX\nports divert-in divert-out\norigin edge 123\nmatch edge,[17,...]\n")
        args = [self.binary, "--socket", str(self.data), "--control-socket", str(self.control),
                "--rules-file", str(self.file), "--divert-file", str(divert)]
        if "mp" in Path(self.binary).name:
            args += ["--workers", "2", "--pool-size", "16", "--queue-size", "16"]
        self.process = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        for _ in range(200):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read())
            if self.control.exists():
                self.command("show")
                return
            time.sleep(.01)
        raise AssertionError("startup timeout")

    def divert(self, operation, ok=True):
        result = subprocess.run([self.ctl, str(self.control), "divert", operation],
                                capture_output=True, text=True, timeout=5)
        assert (result.returncode == 0) == ok, result.stderr
        return result.stdout if ok else result.stderr


def exercise(binary, ctl, root, mmap):
    initial = "format 2\nserial 1\nexit exit*\nswitch edge,[17,...] to exit,[99,...] allow bidir\n"
    sw = DivertSwitch(binary, ctl, root, initial)
    endpoints = []
    try:
        edge, exit = [Endpoint(sw, name, mmap) for name in ("edge", "exit")]
        endpoints += [edge, exit]
        assert sw.divert("show") == "divert_enabled=0\n"
        assert sw.divert("stop") == sw.divert("stop") == "divert_enabled=0\n"
        assert "both divert adapter ports" in sw.divert("enable", ok=False)
        # The new file by itself does not activate divert or reduce legacy stack capacity.
        full = [17, 2, 3, 4, 5, 6, 7, 8]
        edge.send(packet(full))
        assert exit.recv() == packet([99, *full[1:]], opcode=2)
        di, do = [Endpoint(sw, name, mmap) for name in ("divert-in", "divert-out")]
        endpoints += [di, do]
        assert sw.divert("enable") == sw.divert("enable") == "divert_enabled=1\n"
        shown = sw.command("show")
        edge.send(packet([17, 42]))
        assert di.recv() == packet(labels(0), opcode=2)
        quiet(edge, exit, do)
        # Existing connection resumes the original ingress and leaves no service labels.
        di.send(packet(labels(3)))
        assert exit.recv() == packet([99, 42], opcode=2)
        do.send(packet(labels(1)))
        assert exit.recv() == packet(labels(1, (99, 42)), opcode=2)
        exit.send(packet(labels(1, (99, 42))))
        assert do.recv() == packet(labels(1, (99, 42)), opcode=2)
        di.send(packet(labels(2, (99, 42))))
        assert edge.recv() == packet([17, 42])
        assert sw.command("show") == shown
        # Stop bypasses every subsequent unmarked packet, including the same flow.
        assert sw.divert("stop") == sw.divert("stop") == "divert_enabled=0\n"
        assert sw.divert("show") == "divert_enabled=0\n" and sw.stats()["divert_enabled"] == "0"
        edge.send(packet([17, 42]))
        assert exit.recv() == packet([99, 42], opcode=2)
        exit.send(packet([99, 42]))
        assert edge.recv() == packet([17, 42])
        edge.send(packet(full))
        assert exit.recv() == packet([99, *full[1:]], opcode=2)
        quiet(edge, di, do)
        # Frames already carrying DVRT can still finish their existing route.
        di.send(packet(labels(3)))
        assert exit.recv() == packet([99, 42], opcode=2)
        do.send(packet(labels(1)))
        assert exit.recv() == packet(labels(1, (99, 42)), opcode=2)
        exit.send(packet(labels(1, (99, 42))))
        assert do.recv() == packet(labels(1, (99, 42)), opcode=2)
        di.send(packet(labels(2, (99, 42))))
        assert edge.recv() == packet([17, 42])
        assert sw.command("show") == shown
        assert sw.divert("enable") == "divert_enabled=1\n"
        assert sw.stats()["divert_enabled"] == "1"
        for size in (1500, 9000, 65535):
            payload = bytes(range(256)) * (size // 256) + bytes(range(size % 256))
            edge.send(packet([17, 42], payload))
            assert di.recv() == packet(labels(0), payload, opcode=2)
            do.send(packet(labels(1), payload))
            assert exit.recv() == packet(labels(1, (99, 42)), payload, opcode=2)
        # No diversion loop; action and ingress side must agree.
        di.send(packet(labels(1)))
        do.send(packet(labels(2)))
        di.send(packet(labels(2, origin=987)))
        di.send(packet([17, 42]))
        malformed = labels(2)
        malformed[2] = int.from_bytes(b"hTX9\0\0\0\0", "big")
        di.send(packet(malformed))
        quiet(*endpoints)
        edge.send(packet([17, 42, 7]))
        quiet(*endpoints)
        assert int(sw.stats()["divert_overflow_drops"]) == 1
        # A normal rules reload is still transactional and also recompiles resumption.
        next_rules = "format 2\nserial 2\nexit exit*\nswitch edge,[17,42] to exit,[88,42] allow bidir\n"
        assert "checked" in sw.command("check", next_rules)
        do.send(packet(labels(1)))
        assert exit.recv() == packet(labels(1, (99, 42)), opcode=2)
        assert "applied" in sw.command("load", next_rules)
        do.send(packet(labels(1)))
        assert exit.recv() == packet(labels(1, (88, 42)), opcode=2)
        # Even with a disconnected origin the ID continues to name that port.
        edge.close()
        sw.peers.remove(edge.socket)
        endpoints.remove(edge)
        for _ in range(200):
            if sw.stats()["connections_current"] == "3":
                break
            time.sleep(.01)
        unrelated = Endpoint(sw, "unrelated", mmap)
        endpoints.append(unrelated)
        di.send(packet(labels(2)))
        quiet(*endpoints)
        edge = Endpoint(sw, "edge", mmap)
        endpoints.append(edge)
        di.send(packet(labels(2)))
        assert edge.recv() == packet([17, 42])
        quiet(*[p for p in endpoints if p is not edge])
        # Rewritten stacks that cannot retain the envelope drop without truncation.
        assert "applied" in sw.command("load", "format 2\nserial 3\nexit exit\nswitch edge to exit,[1,2,3] allow\n")
        do.send(packet(labels(1)))
        quiet(*endpoints)
        assert int(sw.stats()["divert_overflow_drops"]) == 2
        # Policies are evaluated with the original logical ingress, not divert-out.
        assert "applied" in sw.command("load", "format 2\nserial 4\nswitch edge drop\nswitch divert-out to exit allow\n")
        do.send(packet(labels(1)))
        quiet(*endpoints)
        assert int(sw.stats()["policy_drops"]) > 0
        assert int(sw.stats()["divert_invalid_drops"]) == 5
        other, blocked = [Endpoint(sw, name, mmap) for name in ("exit-other", "exit-blocked")]
        endpoints += [other, blocked]
        assert "applied" in sw.command("load", "format 2\nserial 5\nexit exit*\n"
            "switch edge,[17,42] to exit-blocked,[99,42] drop\n"
            "switch edge,[17,42] to exit*,[99,42] allow\n")
        do.send(packet(labels(1)))
        ready = select.select([exit.socket, other.socket, blocked.socket], [], [], 2)[0]
        assert len(ready) == 1 and ready[0] is not blocked.socket
        selected = exit if ready[0] is exit.socket else other
        assert selected.recv() == packet(labels(1, (99,42)), opcode=2)
        for _ in range(8):
            do.send(packet(labels(1)))
            assert selected.recv() == packet(labels(1, (99,42)), opcode=2)
        quiet(*endpoints)
        assert int(sw.stats()["ecmp_packets"]) >= 9
        # Stop must also work when a divert adapter port has disconnected.
        do.close()
        sw.peers.remove(do.socket)
        endpoints.remove(do)
        for _ in range(200):
            if int(sw.stats()["connections_current"]) == len(endpoints):
                break
            time.sleep(.01)
        assert int(sw.stats()["connections_current"]) == len(endpoints)
        assert sw.divert("stop") == "divert_enabled=0\n"
        assert "both divert adapter ports" in sw.divert("enable", ok=False)
        assert sw.divert("show") == "divert_enabled=0\n"
    finally:
        for endpoint in endpoints:
            endpoint.close()
        sw.stop()
        sw.log.close()
    print("PASS:", Path(binary).name, "mmap" if mmap else "inline", "divert, stop/re-enable, reload, stable origin and bounds", flush=True)


if __name__ == "__main__":
    for binary, mmap in ((sys.argv[1], False), (sys.argv[2], False), (sys.argv[2], True)):
        with tempfile.TemporaryDirectory(prefix="divert-test-") as root:
            exercise(binary, sys.argv[3], Path(root), mmap)
