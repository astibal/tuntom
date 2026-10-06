#!/usr/bin/env python3
"""Real namespace TUN regression for exit and two-sided divert adapters."""

import os
import grp
import pwd
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
SKIP = 77


def skip(reason):
    print(f"SKIP: {reason}")
    raise SystemExit(SKIP)


def prerequisites():
    if sys.platform != "linux" or os.geteuid() != 0:
        skip("requires Linux root privileges")
    for command in ("ip", "nsenter", "unshare", "sysctl"):
        if not shutil.which(command):
            skip(f"requires {command}")
    device = Path("/dev/net/tun")
    if not device.exists() or not stat.S_ISCHR(device.stat().st_mode):
        skip("requires /dev/net/tun")
    if subprocess.run(["unshare", "--net", "true"], capture_output=True).returncode:
        skip("requires permission to create and enter network namespaces")
    try:
        user = pwd.getpwnam("tuntom")
        group = grp.getgrnam("tuntom")
    except KeyError:
        skip("requires the tuntom user and group")
    return user.pw_uid, group.gr_gid


def checksum(data):
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    total = (total & 0xffff) + (total >> 16)
    total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def udp_packet(src, dst, sport, dport, payload, ttl=64):
    source = socket.inet_aton(src)
    destination = socket.inet_aton(dst)
    udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
    header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1234,
                                   0, ttl, socket.IPPROTO_UDP, 0, source, destination))
    struct.pack_into("!H", header, 10, checksum(bytes(header)))
    return bytes(header) + udp


def frame(labels, payload, opcode=2):
    size = 8 + 8 * len(labels) + len(payload)
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), size) + \
        b"".join(struct.pack("!Q", value) for value in labels) + payload


def decode(packet):
    assert len(packet) >= 8 and packet[0] == 1
    count = packet[3]
    size = struct.unpack_from("!I", packet, 4)[0]
    assert size == len(packet)
    labels = list(struct.unpack_from(f"!{count}Q", packet, 8)) if count else []
    return packet[1], labels, packet[8 + 8 * count:]


def via_labels(reverse=False):
    saved = [17, 42]
    cookie = int.from_bytes(b"VIA", "big")
    header = (cookie << 40) | 0x5649410000 | 0x100 | (3 + len(saved))
    context = (1 << 32) | (len(saved) << 8) | int(reverse)
    return [17, 42, header, context, 123, *saved]


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def ns(holder, *arguments, **kwargs):
    kwargs.setdefault("check", True)
    return subprocess.run(["nsenter", f"--net=/proc/{holder.pid}/ns/net", *arguments], **kwargs)


class IpcServer:
    def __init__(self, path, uid, gid):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.socket.bind(str(path))
        os.chown(path, uid, gid)
        os.chmod(path, 0o660)
        self.socket.listen(4)
        self.socket.settimeout(8)
        self.clients = []

    def accept(self):
        client, _ = self.socket.accept()
        client.settimeout(5)
        registration = client.recv(1024)
        assert registration[:4] == b"TTP\x01" and len(registration) >= 8
        size = registration[4]
        name = registration[8:8 + size].decode()
        self.clients.append(client)
        return name, client

    def close(self):
        for client in self.clients:
            client.close()
        self.socket.close()


def wait_interface(holder, name, process, log):
    for _ in range(300):
        if process.poll() is not None:
            log.flush()
            raise RuntimeError(Path(log.name).read_text(errors="replace"))
        result = ns(holder, "ip", "link", "show", "dev", name,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        if result.returncode == 0:
            return
        time.sleep(.02)
    raise RuntimeError(f"{name} was not created in target namespace")


def wait_privilege_drop(process, uid, gid, log):
    for _ in range(300):
        if process.poll() is not None:
            log.flush()
            raise RuntimeError(Path(log.name).read_text(errors="replace"))
        fields = {}
        for line in Path(f"/proc/{process.pid}/status").read_text().splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                fields[key] = value.split()
        if (fields.get("Uid") == [str(uid)] * 4 and
                fields.get("Gid") == [str(gid)] * 4 and
                fields.get("NoNewPrivs") == ["1"] and
                "0" not in fields.get("Groups", [])):
            return
        time.sleep(.02)
    raise RuntimeError(f"process {process.pid} did not securely drop privileges")


def exit_case(binary, root, uid, gid, external_namespace):
    holder = process = responder = None
    server = None
    log = open(root / "exit.log", "w+")
    try:
        holder = subprocess.Popen(["unshare", "--net", "sleep", "300"])
        suffix = "external" if external_namespace else "current"
        socket_path = root / f"exit-{suffix}.sock"
        server = IpcServer(socket_path, uid, gid)
        command = [binary, "ex0", "--switch-socket", str(socket_path),
            "--switch-port-id", "exit", "--switch-ipc", "v1",
        ]
        if external_namespace:
            command += ["--tun-netns", f"pid:{holder.pid}"]
        else:
            command = ["nsenter", f"--net=/proc/{holder.pid}/ns/net", *command]
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=log)
        name, client = server.accept()
        assert name == "exit"
        wait_interface(holder, "ex0", process, log)
        wait_privilege_drop(process, uid, gid, log)
        ns(holder, "ip", "link", "set", "lo", "up")
        ns(holder, "ip", "address", "add", "10.65.0.2/32", "dev", "lo")
        ns(holder, "ip", "route", "add", "10.65.0.1/32", "dev", "ex0")
        responder_code = (
            "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); "
            "s.bind(('10.65.0.2',23456)); d,a=s.recvfrom(1000); "
            "assert d==b'exit-netns'; s.sendto(b'exit-reply',a)")
        responder = subprocess.Popen([
            "nsenter", f"--net=/proc/{holder.pid}/ns/net", sys.executable, "-c", responder_code])
        time.sleep(.05)
        request = udp_packet("10.65.0.1", "10.65.0.2", 12345, 23456, b"exit-netns")
        client.sendall(frame([77], request))
        opcode, labels, reply = decode(client.recv(70000))
        assert opcode == 1 and labels == [77]
        assert reply[12:20] == socket.inet_aton("10.65.0.2") + socket.inet_aton("10.65.0.1")
        assert reply[28:] == b"exit-reply"
        assert responder.wait(timeout=3) == 0
    finally:
        stop(responder); stop(process); stop(holder)
        if server: server.close()
        log.close()


def divert_case(binary, root, uid, gid):
    holder = process = None
    server = None
    log = open(root / "divert.log", "w+")
    try:
        holder = subprocess.Popen(["unshare", "--net", "sleep", "300"])
        server = IpcServer(root / "divert.sock", uid, gid)
        process = subprocess.Popen([
            binary, "di0", "do0", "--switch-socket", str(root / "divert.sock"),
            "--via-instance", "smithproxy#0", "--admission", "immediate",
            "--switch-ipc", "v1", "--tun-netns", f"pid:{holder.pid}",
        ], stdout=subprocess.DEVNULL, stderr=log)
        connections = dict(server.accept() for _ in range(2))
        input_client = connections["divert-in~via:c:smithproxy#0"]
        output_client = connections["divert-out~via:s:smithproxy#0"]
        wait_interface(holder, "di0", process, log)
        wait_interface(holder, "do0", process, log)
        wait_privilege_drop(process, uid, gid, log)
        ns(holder, "sysctl", "-qw", "net.ipv4.ip_forward=1")
        for key in ("all", "default", "di0", "do0"):
            ns(holder, "sysctl", "-qw", f"net.ipv4.conf.{key}.rp_filter=0")
        ns(holder, "ip", "route", "add", "10.66.0.1/32", "dev", "di0")
        ns(holder, "ip", "route", "add", "10.66.0.2/32", "dev", "do0")

        request = udp_packet("10.66.0.1", "10.66.0.2", 12000, 443, b"divert-forward")
        input_client.sendall(frame(via_labels(False), request))
        opcode, labels, forwarded = decode(output_client.recv(70000))
        assert opcode == 1 and forwarded[8] == 63 and forwarded[28:] == b"divert-forward"
        assert labels[0:2] == [17, 42] and labels[3] & 0xff == 2  # onward, forward

        reply = udp_packet("10.66.0.2", "10.66.0.1", 443, 12000, b"divert-reverse")
        output_client.sendall(frame(via_labels(True), reply))
        opcode, labels, returned = decode(input_client.recv(70000))
        assert opcode == 1 and returned[8] == 63 and returned[28:] == b"divert-reverse"
        assert labels[0:2] == [17, 42] and labels[3] & 0xff == 3  # onward, reverse
    finally:
        stop(process); stop(holder)
        if server: server.close()
        log.close()


def split_multiqueue_case(binary, root, uid, gid):
    holder = None
    processes = []
    servers = {}
    logs = []
    try:
        holder = subprocess.Popen(["unshare", "--net", "sleep", "300"])
        shared = root / "split-flows"
        clients = {}
        for side, path_id in (("in", "i"), ("out", "o")):
            socket_path = root / f"split-{side}.sock"
            server = IpcServer(socket_path, uid, gid)
            servers[side] = server
            log = open(root / f"split-{side}.log", "w+")
            logs.append(log)
            process = subprocess.Popen([
                binary, "mdi0", "mdo0", "--side", side,
                "--via-instance", "smithproxy#mq", "--admission", "immediate",
                "--relay-path", f"{path_id}={socket_path}",
                "--shared-flows", str(shared), "--switch-ipc", "v1",
                "--tun-netns", f"pid:{holder.pid}",
            ], stdout=subprocess.DEVNULL, stderr=log)
            processes.append(process)
            name, client = server.accept()
            assert name == (f"divert-in.{path_id}~via:c:smithproxy#mq" if side == "in"
                            else f"divert-out.{path_id}~via:s:smithproxy#mq")
            clients[side] = client

        wait_interface(holder, "mdi0", processes[0], logs[0])
        wait_interface(holder, "mdo0", processes[1], logs[1])
        for process, log in zip(processes, logs):
            wait_privilege_drop(process, uid, gid, log)
        ns(holder, "sysctl", "-qw", "net.ipv4.ip_forward=1")
        for key in ("all", "default", "mdi0", "mdo0"):
            ns(holder, "sysctl", "-qw", f"net.ipv4.conf.{key}.rp_filter=0")
        ns(holder, "ip", "route", "add", "10.67.0.1/32", "dev", "mdi0")
        ns(holder, "ip", "route", "add", "10.67.0.2/32", "dev", "mdo0")

        request = udp_packet("10.67.0.1", "10.67.0.2", 13000, 443, b"split-multiqueue")
        clients["in"].sendall(frame(via_labels(False), request))
        opcode, labels, forwarded = decode(clients["out"].recv(70000))
        assert opcode == 1 and forwarded[8] == 63 and forwarded[28:] == b"split-multiqueue"
        assert labels[3] & 0xff == 2

        reply = udp_packet("10.67.0.2", "10.67.0.1", 443, 13000, b"split-return")
        clients["out"].sendall(frame(via_labels(True), reply))
        opcode, labels, returned = decode(clients["in"].recv(70000))
        assert opcode == 1 and returned[8] == 63 and returned[28:] == b"split-return"
        assert labels[3] & 0xff == 3

        # Closing the live IPC channel makes the unprivileged worker detach its
        # multiqueue fd.  Its reconnect then attaches the same fd again.  A
        # second packet proves both privileged-looking ioctls remain usable by
        # the descriptor owner after dropping UID/GID.
        clients["in"].close()
        name, clients["in"] = servers["in"].accept()
        assert name == "divert-in.i~via:c:smithproxy#mq"
        request = udp_packet("10.67.0.1", "10.67.0.2", 13001, 443, b"split-reconnected")
        clients["in"].sendall(frame(via_labels(False), request))
        opcode, labels, forwarded = decode(clients["out"].recv(70000))
        assert opcode == 1 and forwarded[28:] == b"split-reconnected"
        assert labels[3] & 0xff == 2
    finally:
        for process in reversed(processes):
            stop(process)
        stop(holder)
        for server in servers.values():
            server.close()
        for log in logs:
            log.close()


def invalid_namespace(binary, root, uid, gid):
    server = IpcServer(root / "invalid.sock", uid, gid)
    log_path = root / "invalid.log"
    try:
        result = subprocess.run([
            binary, "bad0", "--switch-socket", str(root / "invalid.sock"),
            "--switch-port-id", "bad", "--switch-ipc", "v1",
            "--tun-netns", "/definitely/missing/tuntom-netns",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=5)
        assert result.returncode == 1 and "Cannot open network namespace" in result.stderr
        assert not Path("/sys/class/net/bad0").exists()
    finally:
        server.close()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <exit-adapter> <divert-adapter>")
    uid, gid = prerequisites()
    with tempfile.TemporaryDirectory(prefix="tuntom-adapter-netns.") as directory:
        root = Path(directory)
        os.chown(root, uid, gid)
        os.chmod(root, 0o770)
        exit_binary = str(Path(sys.argv[1]).resolve())
        divert_binary = str(Path(sys.argv[2]).resolve())
        exit_case(exit_binary, root, uid, gid, True)
        exit_case(exit_binary, root, uid, gid, False)
        divert_case(divert_binary, root, uid, gid)
        split_multiqueue_case(divert_binary, root, uid, gid)
        invalid_namespace(exit_binary, root, uid, gid)
    print("PASS: adapters drop privileges and preserve direct/provider TUN traffic plus multiqueue reconnect")


if __name__ == "__main__":
    main()
