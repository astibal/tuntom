#!/usr/bin/env python3
"""Format-2 forwarding and reload over ST, MP inline, and MP mmap IPC."""
import select
import socket
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
from switch_ruleset_integration_test import Switch, packet
from switch_v2_integration_test import Peer


class Harness(Switch):
    def connect(self):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        peer.settimeout(3)
        peer.connect(str(self.data))
        self.peers.append(peer)
        return peer


class Endpoint:
    def __init__(self, sw, name, mmap):
        self.peer = Peer(sw, name, caps=3, batch=8) if mmap else None
        self.socket = self.peer.socket if self.peer else sw.port(name)

    def send(self, *frames):
        if self.peer:
            self.peer.send(frames)
        else:
            for frame in frames:
                self.socket.sendall(frame)

    def recv(self):
        return self.peer.receive() if self.peer else self.socket.recv(70000)

    def close(self):
        self.peer.close() if self.peer else self.socket.close()


def quiet(*ports):
    assert not select.select([p.socket for p in ports], [], [], .06)[0], "unexpected delivery"
    assert all(not p.peer or not p.peer.pending for p in ports), "unconsumed batch frame"


def config(serial, body, version=2):
    return f"format {version}\nserial {serial}\n{body}\n"


def exercise(binary, ctl, root, mmap=False):
    initial = config(1, "exit out*\n"
                     "switch src,[42,...] to out-block*,[99,&16,...] drop\n"
                     "switch src,[42,&16,...] to out*,[99,*,...] allow bidir [id=masked]")
    sw = Harness(binary, ctl, root, initial)
    endpoints = []
    try:
        endpoints = [Endpoint(sw, name, mmap) for name in ("src", "out-a", "out-b", "out-block")]
        src, a, b, blocked = endpoints
        serial = 1

        def load(body, version=2):
            nonlocal serial
            serial += 1
            assert "applied" in sw.command("load", config(serial, body, version))
            assert sw.stats()["ruleset_format"] == str(version)

        def exchange(labels, expected, target, opcode=1):
            src.send(packet(labels))
            assert target.recv() == packet(expected, opcode=opcode)
            quiet(*(p for p in endpoints if p is not target))

        assert sw.stats()["ruleset_format"] == "2"
        src.send(packet([42, 48, 7]))
        ready = select.select([a.socket, b.socket, blocked.socket], [], [], 3)[0]
        assert len(ready) == 1 and ready[0] != blocked.socket
        selected = a if ready[0] is a.socket else b
        assert selected.recv() == packet([99, 48, 7], opcode=2)
        src.send(*(packet([42, 48, 7]) for _ in range(8)))
        for _ in range(8):
            assert selected.recv() == packet([99, 48, 7], opcode=2)
        selected.send(packet([99, 48, 7]))
        assert src.recv() == packet([42, 48, 7])
        src.send(packet([42, 8]), packet([42, 32]), packet([42]))
        selected.send(packet([99, 8]))  # Inverse retains the bitmask predicate.
        quiet(*endpoints)
        shown = sw.command("show")
        assert "bidir" not in shown and "masked.reverse" in shown and "&16" in shown
        assert "unchanged" in sw.command("load", shown)
        assert "unchanged" in sw.command("load", initial)

        for body in ("switch src,&18446744073709551616 to out-a allow",
                     "switch src,[42,&16] to out-a,[42,99] allow bidir",
                     "switch src to out-a,&16 allow", "switch *,17 to out-a,99 allow",
                     "switch src,[] to out-a allow", "label src,17 to out-a,[99]"):
            for operation in ("check", "load"):
                sw.command(operation, config(serial + 1, body), ok=False)
                assert sw.command("show") == shown
        exchange([42,48,7], [99,48,7], selected, opcode=2)
        assert "checked" in sw.command("check", config(serial + 1, "switch drop"))
        exchange([42,48,7], [99,48,7], selected, opcode=2)

        load("switch src,[42,<90,99>] to out-a,[42,*,7] allow")
        for value in (90, 95, 99):
            exchange([42,value], [42,value,7], a)
        src.send(packet([42,89]), packet([42,100]), packet([42,95,7]), packet([42]))
        quiet(*endpoints)

        load("switch src,[42,...] to out-b,99 allow bidir")
        exchange([42,95,7], [99], b)
        b.send(packet([99]))
        assert src.recv() == packet([42])
        quiet(*endpoints)

        load("switch src,&24 to out-a allow")
        for value in (8, 16, 24):
            exchange([value], [value], a)
        src.send(packet([32]), packet([16,7]))
        quiet(*endpoints)

        load("switch src,[1,2,3,4,5,6,7,&9223372036854775808] to out-a,[*,*,*,*,*,*,*,*] allow")
        labels = [1,2,3,4,5,6,7,18446744073709551615]
        exchange(labels, labels, a)

        load('switch src,["ABCD",&b1000,<0x10,0x1f>,...] to out-a,["EXIT",*,*,...] allow bidir')
        labels = [0x4142434400000000, 24, 31, 7]
        output = [0x4558495400000000, 24, 31, 7]
        exchange(labels, output, a)
        a.send(packet(output))
        assert src.recv() == packet(labels)
        quiet(*endpoints)

        load("switch src,[42,...] to out-block*,[99,&16,...] drop\n"
             "switch src,[42,...] to out*,[99,16,...] allow\n"
             "switch src to out-a,77 allow")
        src.send(packet([42,8,7]))
        ready = select.select([a.socket,b.socket,blocked.socket], [], [], 3)[0]
        assert len(ready) == 1 and ready[0] != blocked.socket
        target = a if ready[0] is a.socket else b
        assert target.recv() == packet([99,16,7])
        quiet(*endpoints)

        load("switch src,[42,&16,...] drop\nswitch src to out-a allow")
        src.send(packet([42,16]))
        quiet(*endpoints)
        exchange([42,8], [42,8], a)
        load("switch src to out-a allow\nswitch src drop")
        exchange([42,16,7], [42,16,7], a)

        # Neither disconnected destinations nor failed rewrites try a later allow.
        load("switch src to disconnected,99 allow\nswitch src to out-a allow")
        src.send(packet([42]))
        quiet(*endpoints)
        assert int(sw.stats()["target_disconnected"]) > 0
        load("switch src,42 to out-a,[*,*] allow\nswitch src to out-b allow")
        src.send(packet([42]))
        quiet(*endpoints)
        assert int(sw.stats()["rewrite_drops"]) > 0

        # Both versions can replace one another atomically without reconnecting ports.
        load("switch src,17 to out-b,44 allow\nlabel src,17 to out-b,[44,...]", version=1)
        exchange([17,99], [44,99], b)
        load("switch src,17 to out-a,99 allow")
        src.send(packet([17,99]))
        quiet(*endpoints)
        exchange([17], [99], a)
        assert sw.stats()["connections_current"] == "4"
        final = sw.command("show")
        assert "unchanged" in sw.command("load", final)
    finally:
        for endpoint in endpoints:
            endpoint.close()
        sw.stop()
        sw.log.close()
    print("PASS:", Path(binary).name, "mmap" if mmap else "inline", "format-2 forwarding and reload", flush=True)


if __name__ == "__main__":
    for binary, mmap in ((sys.argv[1],False),(sys.argv[2],False),(sys.argv[2],True)):
        with tempfile.TemporaryDirectory(prefix="rules-v2-test-") as root:
            exercise(binary, sys.argv[3], Path(root), mmap)
