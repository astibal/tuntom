#!/usr/bin/env python3
"""V2 wire peer, independently encoded, against the real MP switch.

Only child processes and private temporary Unix sockets; no service/TUN changes.
Shared words use libatomic's native u64 ABI, also for Python test peers.
"""
import array
import ctypes
import fcntl
import mmap
import os
import select
import socket
import struct
import sys
import time
import tempfile
from pathlib import Path
sys.dont_write_bytecode = True
from switch_mp_integration_test import Switch, frame, until

ATOMIC = ctypes.CDLL("libatomic.so.1")
LOAD = getattr(ATOMIC, "__atomic_load_8")
LOAD.argtypes = [ctypes.c_void_p, ctypes.c_int]
LOAD.restype = ctypes.c_uint64
STORE = getattr(ATOMIC, "__atomic_store_8")
STORE.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int]
MAX_FRAME = 65607


def record(kind, body):
    return b"TTX\x02" + struct.pack("!BBH", kind, 0, len(body) + 8) + body


def unpack(data, kind, fmt):
    fmt = "!" + fmt
    assert data[:6] == b"TTX\x02" + bytes([kind, 0]), data[:32]
    assert struct.unpack_from("!H", data, 6)[0] == len(data)
    assert len(data) == 8 + struct.calcsize(fmt)
    return struct.unpack_from(fmt, data, 8)


class Pool:
    def __init__(self, fd, epoch, pool_id, slots, capacity, stride, size):
        assert os.fstat(fd).st_size == size
        assert fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
        assert fcntl.fcntl(fd, fcntl.F_GET_SEALS) & 7 == 7
        self.map = mmap.mmap(fd, size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE)
        os.close(fd)
        header = struct.unpack_from("<8sIIQIIQQ", self.map)
        assert header == (b"TTMMAP01", 1, pool_id, epoch, slots, capacity, stride, size), header
        assert not any(self.map[48:4096])
        self.address = ctypes.addressof(ctypes.c_char.from_buffer(self.map))
        self.slots, self.stride, self.capacity = slots, stride, capacity
        self.cursor = 0

    def token(self, slot):
        return LOAD(self.address + 4096 + slot * self.stride, 2)  # acquire

    def publish(self, slot, token):
        STORE(self.address + 4096 + slot * self.stride, token, 3)  # release

    def reserve(self, data):
        assert len(data) <= self.capacity
        for _ in range(self.slots):
            slot = self.cursor
            self.cursor = (slot + 1) % self.slots
            token = self.token(slot)
            if token & 1:
                continue
            offset = 4096 + slot * self.stride + 64
            self.map[offset:offset + len(data)] = data
            self.publish(slot, token + 1)
            return slot, len(data), token + 1
        return None

    def read(self, ref):
        slot, size, token = ref
        assert slot < self.slots and 17 <= size <= self.capacity and self.token(slot) == token
        offset = 4096 + slot * self.stride + 64
        data = self.map[offset:offset + size]
        self.publish(slot, token + 1)
        return data

    def close(self):
        self.map.close()


class Peer:
    def __init__(self, switch, name, caps=3, batch=8, slots=32, activate=True):
        self.socket = switch.connect()
        self.socket.sendall(record(1, struct.pack("!6I", caps, 1 if caps else 0, slots if caps else 0,
                                                MAX_FRAME, batch if caps else 1, 0)))
        (self.epoch, chosen, abi, selected_slots, maximum, chosen_batch, reserved) = unpack(
            self.socket.recv(1024), 2, "Q6I")
        assert self.epoch and chosen & ~caps == 0 and maximum == MAX_FRAME and reserved == 0, (self.epoch, chosen, abi, selected_slots, maximum, chosen_batch, reserved, caps)
        self.socket.sendall(b"TTP\x01" + bytes([len(name), 0, 0, 0]) + name.encode())
        data, ancillary, flags, _ = self.socket.recvmsg(1024, socket.CMSG_SPACE(8), socket.MSG_CMSG_CLOEXEC)
        assert not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
        epoch, self.caps, abi, count, capacity, stride, size, self.batch, reserved = unpack(data, 3, "Q4I2Q2I")
        assert epoch == self.epoch and self.caps & ~chosen == 0 and reserved == 0
        self.output = self.input = None
        self.pending = []
        if self.caps:
            assert len(ancillary) == 1 and ancillary[0][:2] == (socket.SOL_SOCKET, socket.SCM_RIGHTS)
            fds = array.array("i", ancillary[0][2])
            assert len(fds) == 2
            assert count == selected_slots and self.batch == chosen_batch
            self.output = Pool(fds[0], epoch, 1, count, capacity, stride, size)
            self.input = Pool(fds[1], epoch, 2, count, capacity, stride, size)
        else:
            assert not ancillary and not any((abi, count, capacity, stride, size)) and self.batch == 1
        if activate:
            self.activate()

    def activate(self, decline=False):
        caps = 0 if decline else self.caps
        self.socket.sendall(record(4, struct.pack("!QII", self.epoch, caps, int(decline))))
        assert unpack(self.socket.recv(1024), 5, "QII") == (self.epoch, caps, 0)
        self.caps = caps

    def refs(self, entries):
        if self.caps & 2:
            return record(17, struct.pack("!QIHH", self.epoch, 1, len(entries), 0) +
                          b"".join(struct.pack("!IIQ", *ref) for ref in entries))
        assert len(entries) == 1
        slot, size, token = entries[0]
        return record(16, struct.pack("!QIIQII", self.epoch, 1, slot, token, size, 0))

    def send(self, packets):
        pending = []
        for packet in packets:
            ref = self.output.reserve(packet) if self.caps and len(packet) <= self.output.capacity else None
            if ref is not None:
                pending.append(ref)
                if len(pending) == self.batch:
                    self.socket.sendall(self.refs(pending)); pending = []
            else:
                if pending:
                    self.socket.sendall(self.refs(pending)); pending = []
                self.socket.sendall(packet)
        if pending:
            self.socket.sendall(self.refs(pending))

    def receive(self):
        if not self.pending:
            data = self.socket.recv(70000)
            assert data, "unexpected disconnect"
            if data[0] == 1:
                return data
            if data[4] == 16:
                epoch, pool, slot, token, size, reserved = unpack(data, 16, "QIIQII")
                assert not reserved
                refs = [(slot, size, token)]
            else:
                assert data[:6] == b"TTX\x02\x11\x00"
                epoch, pool, count, reserved = struct.unpack_from("!QIHH", data, 8)
                assert not reserved and 1 <= count <= self.batch and len(data) == 24 + count * 16
                refs = [struct.unpack_from("!IIQ", data, 24 + i * 16) for i in range(count)]
            assert epoch == self.epoch and pool == 2
            self.pending = [self.input.read(ref) for ref in refs]
        return self.pending.pop(0)

    def close(self):
        self.socket.close()
        for pool in (self.input, self.output):
            if pool:
                pool.close()


def exchange(peer, payload=b"echo"):
    peer.send([frame([7], payload)])
    assert peer.receive() == frame([7], payload, opcode=2)


def variants(binary):
    for caps in (0, 1, 3):
        with Switch(binary, "--workers", "4", "--default-back=on") as switch:
            peer = Peer(switch, "a", caps=caps, batch=1 if caps != 3 else 8)
            try:
                assert peer.caps == caps
                packets = [frame([i + 1, 77], bytes([i]) * n)
                           for i, n in enumerate([1, 64, 1500, 9000, 65535, 3, 64, 1500])]
                peer.send(packets)
                for packet in packets:
                    expected = bytearray(packet); expected[1] = 2
                    assert peer.receive() == expected
                stats = switch.stats()
                assert stats["frames_rx"] == stats["frames_tx"] == 8, stats
                if caps == 3:
                    assert stats["port_0_ipc_rx_records"] == 3 and stats["port_0_ipc_rx_mapped_frames"] == 7
            finally:
                peer.close()
    with Switch(binary, "--workers", "2", "--default-back=on", "--ipc-mode", "inline") as switch:
        peer = Peer(switch, "inline")
        try:
            assert peer.caps == 0
            exchange(peer)
        finally:
            peer.close()
    with Switch(binary, "--workers", "2", "--default-back=on", "--ipc-memory-mib", "1", "--ipc-frame-capacity", "65607") as switch:
        peer = Peer(switch, "limited")
        try:
            assert peer.caps == 0
            exchange(peer)
            assert switch.stats()["ipc_mapping_bytes"] == 0
        finally:
            peer.close()


def replacement(binary):
    with Switch(binary, "--workers", "4", "--default-back=on", "--max-ports", "1") as switch:
        old = Peer(switch, "same")
        try:
            for action in ("abandon", "bad_ready", "timeout", "decline", "activate"):
                new = Peer(switch, "same", activate=False)
                try:
                    exchange(old, action.encode())
                    assert switch.stats()["registrations_ok"] >= 1
                    if action == "bad_ready":
                        new.socket.sendall(record(4, struct.pack("!QII", new.epoch + 1, new.caps, 0)))
                        assert new.socket.recv(1024) == b""
                        exchange(old)
                    elif action == "timeout":
                        time.sleep(5.1)
                        assert new.socket.recv(1024) == b""
                        exchange(old)
                    elif action in ("decline", "activate"):
                        new.activate(decline=action == "decline")
                        exchange(new)
                        assert old.socket.recv(1024) == b""
                        old.close(); old = new; new = None
                finally:
                    if new:
                        new.close()
            # Invalid HELLO carries no name and cannot replace a registered peer.
            bad = switch.connect(); bad.sendall(record(1, bytes(24)))
            assert bad.recv(1024) == b""
            exchange(old)
        finally:
            old.close()


def malformed(binary):
    with Switch(binary, "--workers", "4", "--default-back=on") as switch:
        for case in ("epoch", "pool", "slot", "length", "token", "duplicate", "rights", "late_setup"):
            peer = Peer(switch, case)
            try:
                ref = peer.output.reserve(frame([7], b"test"))
                wire = bytearray(peer.refs([ref]))
                offset = {"epoch": 8, "pool": 16, "slot": 24, "length": 28, "token": 32}.get(case)
                if offset is not None:
                    wire[offset] ^= 0x80
                elif case == "duplicate":
                    wire = peer.refs([ref, ref])
                elif case == "late_setup":
                    wire = record(4, struct.pack("!QII", peer.epoch, peer.caps, 0))
                if case == "rights":
                    fd = os.open("/dev/null", os.O_RDONLY)
                    try:
                        peer.socket.sendmsg([wire], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))])
                    finally:
                        os.close(fd)
                else:
                    peer.socket.sendall(wire)
                assert peer.socket.recv(1024) == b"", case
            finally:
                peer.close()
        # Inner V1 corruption releases a valid slot and is a frame drop, not a transport failure.
        peer = Peer(switch, "bad_frame")
        try:
            bad = bytearray(frame([7], b"bad")); bad[3] = 0
            peer.send([bad, frame([7], b"good")])
            assert peer.receive() == frame([7], b"good", opcode=2)
            assert switch.stats()["malformed_frames"] >= 1
        finally:
            peer.close()


def matrix(binary, adapters):
    options = ["--workers", "4", "--ipc-slots", "32", "--ipc-batch", "16"]
    for i in range(adapters):
        options += ["--exit-port", f"a{i}"]
    for i in range(20):
        options += ["--route", f"t{i}:7=a{i % adapters}:9"]
    with Switch(binary, *options) as switch:
        sinks = [Peer(switch, f"a{i}", batch=16) for i in range(adapters)]
        sources = [Peer(switch, f"t{i}", batch=16) for i in range(20)]
        try:
            before = switch.stats()
            assert before["connections_current"] == 20 + adapters
            assert all(before[f"port_{i}_ipc_mmap"] for i in range(20 + adapters))
            for iteration in range(12):
                expected = [set() for _ in sinks]
                for i, source in enumerate(sources):
                    packets = []
                    for j in range(16):
                        payload = struct.pack("!III", iteration, i, j) + bytes([i]) * (9000 if j % 2 else 64)
                        packets.append(frame([7], payload)); expected[i % adapters].add(payload)
                    source.send(packets)
                for i, sink in enumerate(sinks):
                    seen = set()
                    for _ in expected[i]:
                        data = sink.receive()
                        assert data[1] == 2 and struct.unpack_from("!Q", data, 8)[0] == 9
                        assert data[16:] in expected[i] and data[16:] not in seen
                        seen.add(data[16:])
                # Add/remove ports while sockets have already consumed batch records.
                extra = Peer(switch, "churn", batch=16)
                extra.close()
            stats = until(lambda: (s := switch.stats())["buffers_in_use"] == 0 and s)
            assert stats["frames_rx"] == stats["frames_tx"] == 3840, stats
            assert stats["queue_full_drops"] == stats["reconfiguration_drops"] == stats["send_errors"] == 0, stats
            assert sum(stats[f"port_{i}_ipc_tx_batches"] for i in range(stats["connections_current"])) > 0
        finally:
            for peer in sources + sinks:
                peer.close()


def cached_batch_migration(binary):
    with Switch(binary, "--workers", "4", "--pool-size", "2", "--queue-size", "128",
                "--exit-port", "sink", "--route", "source:7=sink:9") as switch:
        source = Peer(switch, "source", slots=128)
        sink = Peer(switch, "sink", slots=1)
        try:
            packets = [frame([7], struct.pack("!I", i) + bytes([i % 251]) * 8996) for i in range(128)]
            source.send(packets)  # 16 small socket records; all payloads have their own slots.
            stalled = until(lambda: (s := switch.stats())["pool_stalls"] and s["send_eagain"] and s)
            assert stalled["frames_tx"] < 128 and stalled["buffers_in_use"] == 2
            for _ in range(12):
                extra = Peer(switch, "migration")
                extra.close()
                until(lambda: switch.stats()["connections_current"] == 2)
            for expected in packets:
                expected = bytearray(expected); expected[1] = 2
                struct.pack_into("!Q", expected, 8, 9)
                assert sink.receive() == expected, "cached batch stranded/reordered after worker migration"
            stats = until(lambda: (s := switch.stats())["buffers_in_use"] == 0 and s)
            assert stats["frames_rx"] == stats["frames_tx"] == 128
            assert stats["reconfiguration_drops"] == stats["queue_full_drops"] == 0
        finally:
            source.close(); sink.close()


def fault_recovery(binary, faults):
    with tempfile.TemporaryDirectory(prefix="tuntom-v2-fault.") as directory:
        marker = Path(directory) / "fault"
        env = dict(os.environ, LD_PRELOAD=str(Path(faults).resolve()), TUNTOM_V2_FAULT_MARKER=str(marker))
        with Switch(binary, "--workers", "4", "--default-back=on", env=env) as switch:
            old = Peer(switch, "same")
            try:
                for mode in ("memfd", "fallocate", "mmap"):
                    marker.write_text(mode)
                    new = Peer(switch, "fallback")
                    try:
                        assert new.caps == 0, mode
                        exchange(new)
                    finally:
                        new.close(); marker.unlink()
                    exchange(old)
                for mode in ("active", "epoll"):
                    new = Peer(switch, "same", activate=False)
                    try:
                        marker.write_text(mode)
                        before = switch.stats()["registrations_ok"]
                        new.socket.sendall(record(4, struct.pack("!QII", new.epoch, new.caps, 0)))
                        for _ in range(8):
                            exchange(old)
                            assert switch.stats()["registrations_ok"] == before
                            time.sleep(.01)
                        if mode == "epoll":
                            assert new.socket.recv(1024) == b""
                        marker.unlink()
                        if mode == "active":
                            assert unpack(new.socket.recv(1024), 5, "QII") == (new.epoch, new.caps, 0)
                            exchange(new)
                            assert old.socket.recv(1024) == b""
                            old.close(); old = new; new = None
                        else:
                            exchange(old)
                    finally:
                        if new:
                            new.close()
                        marker.unlink(missing_ok=True)
            finally:
                old.close()


def main():
    binary = sys.argv[1]
    variants(binary); replacement(binary); malformed(binary); cached_batch_migration(binary)
    if len(sys.argv) > 2:
        fault_recovery(binary, sys.argv[2])
    for adapters in (1, 2, 3):
        matrix(binary, adapters)
    print("V2 real MP: variants, replacement, malformed peers, 20 tunnels + 1/2/3 adapters: OK")


if __name__ == "__main__":
    main()
