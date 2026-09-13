#!/usr/bin/env python3
"""Wildcard routes over real ST/MP sockets, including mixed V1/V2 peers."""
import select
import socket
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
from switch_mp_integration_test import Switch as MpSwitch, frame, until
from switch_v2_integration_test import Peer


class Switch(MpSwitch):
    def stats(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
            peer.settimeout(5)
            peer.connect(str(self.control))
            peer.sendall(b"show stats")
            data = peer.recv(1 << 20).decode()
        return {key: int(value) if value.isdigit() else value
                for key, value in (line.split("=", 1) for line in data.splitlines() if "=" in line)}


class Endpoint:
    def __init__(self, switch, name, caps=None):
        self.name = name
        self.peer = Peer(switch, name, caps=caps, batch=8 if caps == 3 else 1) if caps is not None else None
        self.socket = self.peer.socket if self.peer else switch.connect(name)

    def send(self, packet):
        if self.peer:
            self.peer.send([packet])
        else:
            self.socket.sendall(packet)

    def receive(self):
        return self.peer.receive() if self.peer else self.socket.recv(70000)

    def close(self):
        self.peer.close() if self.peer else self.socket.close()


def ip_packet(index, reverse=False, ipv6=False, protocol=17, payload=b"data"):
    src, dst = (b"\x0a\0\0\1", b"\x0a\0\0\2")
    sport, dport = 1024 + index, 443
    if reverse:
        src, dst, sport, dport = dst, src, dport, sport
    udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
    if ipv6:
        src, dst = b"\x20\x01" + b"\0" * 10 + src, b"\x20\x01" + b"\0" * 10 + dst
        return struct.pack("!IHBB16s16s", 6 << 28, len(udp), protocol, 64, src, dst) + udp
    return struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1, 0, 64, protocol, 0, src, dst) + udp


def no_packet(endpoint):
    assert not select.select([endpoint.socket], [], [], .06)[0], "Unexpected default-back / duplicate"


def exercise(binary, workers=None, mixed=False):
    options = ["--workers", str(workers)] if workers is not None else []
    options += ["--default-back=on", "--exit-port", "internet", "--exit-port", "edge-42_2"]
    for rule in ("internet:1001=edge-42*:44", "edge-42*:17=internet:1001",
                 "edge*:18=internet:300", "edge-42*:18=internet:200",
                 "edge-42_1:18=other:999", "*:19=internet:400"):
        options += ["--route", rule]
    with Switch(binary, *options) as switch:
        owned = []

        def connect(name, caps=None):
            endpoint = Endpoint(switch, name, caps)
            owned.append(endpoint)
            return endpoint

        internet = connect("internet", 3 if mixed else None)
        other, short = connect("other"), connect("edge-4")
        internet.send(frame([1001], ip_packet(0)))
        until(lambda: switch.stats()["target_disconnected"] == 1)
        no_packet(internet)
        members = {"edge-42": connect("edge-42")}

        def forward(payload, label=1001):
            internet.send(frame([label, 77], payload))
            sockets = [endpoint.socket for endpoint in members.values()]
            ready, _, _ = select.select(sockets + [other.socket, short.socket], [], [], 5)
            assert len(ready) == 1, "Missing or duplicated ECMP delivery"
            name = next((name for name, endpoint in members.items() if endpoint.socket == ready[0]), None)
            assert name is not None, "Prefix matched a wrong port"
            expected = frame([44, 77], payload, 2 if name == "edge-42_2" else 1)
            assert members[name].receive() == expected, "Payload, label stack or EXIT opcode changed"
            return name

        for index in range(8):
            assert forward(ip_packet(index)) == "edge-42"
        assert switch.stats()["ecmp_packets"] == 0
        members["edge-42_1"] = connect("edge-42_1", 0 if mixed else None)
        members["edge-42_2"] = connect("edge-42_2", 3 if mixed else None)
        baseline = []
        for index in range(192):
            ipv6 = bool(index % 2)
            name = forward(ip_packet(index, ipv6=ipv6))
            assert forward(ip_packet(index, reverse=True, ipv6=ipv6, payload=b"reply")) == name
            baseline.append(name)
        assert set(baseline) == set(members)

        # First and subsequent IPv4 fragments use the same L3 key.
        first, later = bytearray(ip_packet(5)), bytearray(ip_packet(500, payload=b"fragment"))
        struct.pack_into("!H", first, 6, 0x2000)
        struct.pack_into("!H", later, 6, 1)
        assert forward(first) == forward(later)
        assert forward(b"opaque one") == forward(b"opaque two")

        # Source wildcard, per-label precedence, and exact rules overriding prefixes.
        for endpoint, label, target, outlabel in (
                (members["edge-42"], 17, internet, 1001),
                (members["edge-42_2"], 17, internet, 1001),
                (members["edge-42_1"], 18, other, 999),
                (members["edge-42"], 18, internet, 200),
                (short, 18, internet, 300), (other, 19, internet, 400)):
            endpoint.send(frame([label, 99], b"return"))
            assert target.receive() == frame([outlabel, 99], b"return", 2 if target is internet else 1)

        # Replacing a member preserves its hash identity despite a new generation.
        replaced = members["edge-42_1"]
        members["edge-42_1"] = connect("edge-42_1", 0 if mixed else None)
        replaced.close()
        assert [forward(ip_packet(i, ipv6=bool(i % 2))) for i in range(192)] == baseline

        # Prefix semantics intentionally include edge-420. Only its new flows move.
        members["edge-420"] = connect("edge-420")
        expanded = [forward(ip_packet(i, ipv6=bool(i % 2))) for i in range(192)]
        assert "edge-420" in expanded
        assert all(new in (old, "edge-420") for old, new in zip(baseline, expanded))
        members.pop("edge-420").close()
        until(lambda: switch.stats()["connections_current"] == 6)
        assert [forward(ip_packet(i, ipv6=bool(i % 2))) for i in range(192)] == baseline

        members.pop("edge-42_1").close()
        until(lambda: switch.stats()["connections_current"] == 5)
        reduced = [forward(ip_packet(i, ipv6=bool(i % 2))) for i in range(192)]
        assert all(old == "edge-42_1" or new == old for old, new in zip(baseline, reduced))
        for endpoint in members.values():
            endpoint.close()
        until(lambda: switch.stats()["connections_current"] == 3)
        previous = switch.stats()["target_disconnected"]
        internet.send(frame([1001], ip_packet(0)))
        until(lambda: switch.stats()["target_disconnected"] == previous + 1)
        no_packet(internet)
        internet.send(frame([1234], b"miss"))
        assert internet.receive() == frame([1234], b"miss", 2)
        assert switch.stats()["ecmp_packets"] >= 192
        # Peer's mmap resources need explicit teardown; socket close is idempotent.
        for endpoint in owned:
            if endpoint.socket.fileno() >= 0:
                endpoint.close()
        return baseline


def invalid(binary):
    for rule in ("a**:1=b:2", "a*b:1=b:2", "a:1=b*c:2", "a:1=b**:2"):
        result = subprocess.run([binary, "--socket", "/unused-ecmp-test", "--route", rule],
                                capture_output=True, timeout=5)
        assert result.returncode != 0 and b"wildcard" in result.stderr, result.stderr


def main():
    st, mp = sys.argv[1:]
    invalid(st); invalid(mp)
    reference = exercise(st)
    assert exercise(mp, workers=1) == reference
    assert exercise(mp, workers=4, mixed=True) == reference
    print("PASS: wildcard ST/MP, symmetry, EXIT, reconnect, membership changes and mixed V1/V2")


if __name__ == "__main__":
    main()
