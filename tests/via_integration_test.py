#!/usr/bin/env python3
"""VIA chains over real ST/MP switches, independently encoded TCP and IPC frames."""
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.dont_write_bytecode = True
from switch_ruleset_v2_integration_test import Harness, Endpoint, quiet
from switch_ruleset_integration_test import packet


def rules(serial=1, permanent=True, policy="hash", unavailable="drop"):
    return f'''format 3
serial {serial}
port edge id 123
service smithproxy {{
 client-side proxy-in*
 server-side proxy-out*
 stickiness {policy}
 instances ["smithproxy#0", "smithproxy#1"]
 unavailable {unavailable}
}}
service capture {{
 client-side cap-in*
 server-side cap-out*
 stickiness failover
 instances ["capture#1", "capture#0"]
 unavailable pass
}}
exit exit
switch edge,[17,...] to exit,[99,...]{' via [smithproxy,capture]' if permanent else ''} allow bidir
'''


def tcp(reverse=False, flags=2, sport=12345, size=40):
    payload = bytearray(size)
    payload[0] = 0x45
    struct.pack_into('!H', payload, 2, size)
    payload[8:10] = bytes([64, 6])
    a, b = bytes([192, 0, 2, 1]), bytes([198, 51, 100, 1])
    payload[12:20] = b + a if reverse else a + b
    struct.pack_into('!HH', payload, 20, *( (443, sport) if reverse else (sport, 443) ))
    payload[32:34] = bytes([0x50, flags])
    return bytes(payload)


def decode(frame):
    count = frame[3]
    return list(struct.unpack('!' + 'Q' * count, frame[8:8 + 8 * count])), frame[8 + 8 * count:]


def context(labels):
    assert len(labels) == 7, labels
    header, ctx, origin = labels[2:5]
    assert (header >> 16) & 0xffffff == 0x564941
    assert header & 0xffff == 0x0105 and origin == 123 and labels[-2:] == [17, 42]
    assert all(chr(c).isalpha() and c < 128 for c in header.to_bytes(8, 'big')[:3])
    return (ctx >> 32, (ctx >> 16) & 65535, (ctx >> 1) & 7, bool(ctx & 1))


def action(labels, reverse=False, op=1):
    result = labels.copy()
    result[3] = (result[3] & ~255) | (op << 1) | int(reverse)
    return result


def received(endpoint, expected_payload, step, op=0, reverse=False):
    frame = endpoint.recv()
    labels, data = decode(frame)
    assert frame[1] == 2 and data == expected_payload
    chain, actual_step, actual_op, actual_reverse = context(labels)
    assert (actual_step, actual_op, actual_reverse) == (step, op, reverse), labels
    return labels


def selected(endpoints):
    ready = select.select([e.socket for e in endpoints], [], [], 3)[0]
    assert len(ready) == 1, ready
    return next(i for i, e in enumerate(endpoints) if e.socket is ready[0])


class ViaSwitch(Harness):
    def __init__(self, binary, ctl, root, adhoc=False, ipc=False):
        self.adhoc = adhoc
        self.inline = ipc == "inline"
        super().__init__(binary, ctl, root, rules(permanent=not adhoc))

    def start(self):
        args = [self.binary, '--socket', str(self.data), '--control-socket', str(self.control), '--rules-file', str(self.file)]
        if self.adhoc:
            config = self.root / 'divert.conf'
            config.write_text('format 3\nmatch edge,[17,...] via [smithproxy,capture]\n')
            args += ['--divert-file', str(config)]
        if 'mp' in Path(self.binary).name:
            args += ['--workers', '2', '--pool-size', '16', '--queue-size', '16']
            if self.inline: args += ['--ipc-mode', 'inline']
        self.process = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        for _ in range(200):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read())
            if self.control.exists():
                self.command('show')
                return
            time.sleep(.01)
        raise AssertionError('startup timeout')

    def divert(self, op):
        r = subprocess.run([self.ctl, str(self.control), 'divert', op], capture_output=True, text=True, timeout=5)
        assert r.returncode == 0, r.stderr
        return r.stdout


def exercise(binary, ctl, root, mmap=False, adhoc=False):
    sw = ViaSwitch(binary, ctl, root, adhoc, mmap)
    endpoints = []
    def connect(name):
        e = Endpoint(sw, name, mmap); endpoints.append(e)
        for _ in range(200):
            if int(sw.stats()['connections_current']) == len(endpoints): return e
            time.sleep(.01)
        raise AssertionError('registration publication timeout')
    def close(e):
        e.close(); endpoints.remove(e); sw.peers.remove(e.socket)
        for _ in range(200):
            if int(sw.stats()['connections_current']) == len(endpoints): return
            time.sleep(.01)
        raise AssertionError('port removal timeout')
    try:
        edge, exit = connect('edge'), connect('exit')
        if adhoc:
            edge.send(packet([17, 42], tcp()))
            assert exit.recv() == packet([99, 42], tcp(), opcode=2)
            assert sw.divert('enable') == 'divert_enabled=1\n'
        # A half-connected instance is never eligible, even in the legacy IPC path.
        c0 = connect('proxy-in0~via:c:smithproxy#0')
        edge.send(packet([17, 42], tcp()))
        quiet(*endpoints)
        foreign = subprocess.run([sys.executable, '-c',
            "import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET); s.settimeout(3); "
            "s.connect(sys.argv[1]); n=b'proxy-out0~via:s:smithproxy#0'; "
            "s.sendall(b'TTP\\x01'+bytes([len(n),0,0,0])+n); assert s.recv(100)==b''", str(sw.data)],
            capture_output=True, text=True, timeout=5)
        assert foreign.returncode == 0, foreign.stderr
        s0 = connect('proxy-out0~via:s:smithproxy#0')
        c1, s1 = connect('proxy-in1~via:c:smithproxy#1'), connect('proxy-out1~via:s:smithproxy#1')
        # Register lower-priority capture first; config order, not connection order, wins.
        cap_c0, cap_s0 = connect('cap-in0~via:c:capture#0'), connect('cap-out0~via:s:capture#0')
        cap_c1, cap_s1 = connect('cap-in1~via:c:capture#1'), connect('cap-out1~via:s:capture#1')
        clients, servers = [c0, c1], [s0, s1]
        shown = sw.command('show')
        assert 'unchanged' in sw.command('load', shown)
        edge.send(packet([17, 42], tcp()))
        chosen = selected(clients)
        offer = received(clients[chosen], tcp(), 0)
        # A proxy-generated SYNACK traverses backwards without a RETURN action.
        clients[chosen].send(packet(action(offer, True), tcp(True, 18)))
        assert edge.recv() == packet([17, 42], tcp(True, 18))
        # The upstream SYN traverses the second service, then the real exit.
        servers[chosen].send(packet(action(offer), tcp()))
        captured = received(cap_c1, tcp(), 1)
        cap_s1.send(packet(action(captured), tcp()))
        completed = received(exit, tcp(), 65535, op=3)
        assert completed[:2] == [99, 42] and context(completed)[0] == context(offer)[0]
        exit.send(packet(completed, tcp(True, 18)))
        reverse_capture = received(cap_s1, tcp(True, 18), 1, reverse=True)
        cap_c1.send(packet(action(reverse_capture, True), tcp(True, 18)))
        reverse_offer = received(servers[chosen], tcp(True, 18), 0, reverse=True)
        clients[chosen].send(packet(action(reverse_offer, True), tcp(True, 18)))
        assert edge.recv() == packet([17, 42], tcp(True, 18))
        # Flow hash is symmetric; HRW is independent of connection order and
        # losing one member only moves that member's flows.
        assignments = {}
        for sport in range(12000, 12024):
            edge.send(packet([17, 42], tcp(sport=sport)))
            member = selected(clients)
            received(clients[member], tcp(sport=sport), 0)
            assignments[sport] = member
            reply = tcp(True, 18, sport)
            exit.send(packet(completed, reply))
            cp = received(cap_s1, reply, 1, reverse=True)
            cap_c1.send(packet(action(cp, True), reply))
            received(servers[member], reply, 0, reverse=True)
        assert set(assignments.values()) == {0, 1}
        close(s1)
        for sport in assignments:
            edge.send(packet([17, 42], tcp(sport=sport)))
            received(c0, tcp(sport=sport), 0)
        # Even an old completed envelope resolves the service to the remaining ID.
        exit.send(packet(completed, tcp(True, 18)))
        cp = received(cap_s1, tcp(True, 18), 1, reverse=True)
        cap_c1.send(packet(action(cp, True), tcp(True, 18)))
        received(s0, tcp(True, 18), 0, reverse=True)
        s1 = connect('proxy-out1~via:s:smithproxy#1'); servers[1] = s1
        for sport, member in assignments.items():
            edge.send(packet([17, 42], tcp(sport=sport)))
            received(clients[member], tcp(sport=sport), 0)
        # Adapter admission BYPASS skips the entire chain.
        clients[chosen].send(packet(action(offer, op=2), tcp()))
        assert exit.recv() == packet([99, 42], tcp(), opcode=2)
        quiet(*endpoints)
        # Incorrect ingress, direction, unknown chain, and reserved CTX bits drop.
        clients[chosen].send(packet(action(offer), tcp()))
        servers[chosen].send(packet(action(offer, True), tcp()))
        edge.send(packet(action(offer), tcp()))
        broken = action(offer); broken[3] ^= 0x8000000000000000
        servers[chosen].send(packet(broken, tcp()))
        broken = action(offer); broken[3] |= 0x10
        servers[chosen].send(packet(broken, tcp()))
        edge.send(packet([17, 42, 9], tcp()))
        quiet(*endpoints)
        # Duplicate side registration must not disconnect or replace the live side.
        duplicate = sw.connect()
        name = ('proxy-in0~via:c:smithproxy#0' if chosen == 0 else 'proxy-in1~via:c:smithproxy#1').encode()
        duplicate.sendall(b'TTP\x01' + bytes([len(name), 0, 0, 0]) + name)
        assert duplicate.recv(100) == b''
        sw.peers.remove(duplicate); duplicate.close()
        edge.send(packet([17, 42], tcp()))
        assert received(clients[chosen], tcp(), 0) == offer
        # Check never publishes candidate chain IDs; successful reload retires old IDs.
        next_rules = rules(2, permanent=not adhoc, policy='failover')
        assert 'checked' in sw.command('check', next_rules)
        servers[chosen].send(packet(action(offer), tcp()))
        assert received(cap_c1, tcp(), 1) == captured
        assert 'applied' in sw.command('load', next_rules)
        servers[chosen].send(packet(action(offer), tcp()))
        exit.send(packet(completed, tcp(True, 18)))
        quiet(*endpoints)
        edge.send(packet([17, 42], tcp()))
        current = received(c0, tcp(), 0)
        assert context(current)[0] != context(offer)[0]
        # Losing one side removes the entire primary and failover picks the next.
        close(s0)
        edge.send(packet([17, 42], tcp()))
        current = received(c1, tcp(), 0)
        # Reverse traffic resolves the service again, never a recycled vector index.
        s1.send(packet(action(current), tcp()))
        cp = received(cap_c1, tcp(), 1)
        cap_s1.send(packet(action(cp), tcp()))
        done = received(exit, tcp(), 65535, op=3)
        close(cap_s1)
        exit.send(packet(done, tcp(True, 18)))
        cp = received(cap_s0, tcp(True, 18), 1, reverse=True)
        cap_c0.send(packet(action(cp, True), tcp(True, 18)))
        received(s1, tcp(True, 18), 0, reverse=True)
        # Primary recovery switches back according to the configured order.
        s0 = connect('proxy-out0~via:s:smithproxy#0')
        edge.send(packet([17, 42], tcp()))
        received(c0, tcp(), 0)
        # Payload sizes do not change label accounting; no payload copy in VIA core.
        for size in (1500, 9000, 65535):
            data = tcp(size=size)
            edge.send(packet([17, 42], data))
            received(c0, data, 0)
        if adhoc:
            assert sw.divert('stop') == 'divert_enabled=0\n'
            edge.send(packet([17, 42], tcp()))
            assert exit.recv() == packet([99, 42], tcp(), opcode=2)
            # Already offered frames may still continue after crude stop.
            s1.send(packet(action(current), tcp()))
            received(cap_c0, tcp(), 1)
            assert sw.divert('enable') == 'divert_enabled=1\n'
        # unavailable pass is the same policy for permanent and ad hoc services.
        close(s0); close(s1); close(cap_s0)
        assert 'applied' in sw.command('load', rules(3, permanent=not adhoc, unavailable='pass'))
        edge.send(packet([17, 42], tcp()))
        done = received(exit, tcp(), 65535, op=3)
        exit.send(packet(done, tcp(True, 18)))
        assert edge.recv() == packet([17, 42], tcp(True, 18))
        quiet(*endpoints)
        if adhoc:
            # Ad hoc interception precedes ordinary switch rules. Even a flow
            # that will be dropped on continuation must reach the service first.
            s0 = connect('proxy-out0~via:s:smithproxy#0')
            blocked = rules(4, permanent=False).split('exit exit')[0] + 'exit exit\nswitch edge drop\n'
            assert 'applied' in sw.command('load', blocked)
            edge.send(packet([17, 42], tcp()))
            offered = received(c0, tcp(), 0)
            before = int(sw.stats()['policy_drops'])
            s0.send(packet(action(offered), tcp()))
            quiet(*endpoints)
            assert int(sw.stats()['policy_drops']) == before + 1
        if not adhoc:
            # Downgrading the rules does not turn retired envelopes into user labels.
            assert 'applied' in sw.command('load', 'format 2\nserial 4\nexit exit\nswitch edge to exit allow\n')
            edge.send(packet(done, tcp()))
            exit.send(packet(done, tcp(True, 18)))
            quiet(*endpoints)
            edge.send(packet([17, 42], tcp()))
            assert exit.recv() == packet([17, 42], tcp(), opcode=2)
        print('PASS:', Path(binary).name, ('inline' if mmap == 'inline' else 'mmap' if mmap else 'v1'), 'adhoc' if adhoc else 'permanent', flush=True)
    finally:
        for e in endpoints: e.close()
        sw.stop(); sw.log.close()


if __name__ == '__main__':
    st, mp, ctl = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='tuntom-via-') as tmp:
        root = Path(tmp)
        for i, (binary, mmap, adhoc) in enumerate([(st, False, False), (mp, False, False), (mp, True, False), (mp, 'inline', False),
                                                 (st, False, True), (mp, True, True), (mp, 'inline', True)]):
            case = root / str(i); case.mkdir()
            exercise(binary, ctl, case, mmap, adhoc)
