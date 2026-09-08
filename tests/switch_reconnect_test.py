#!/usr/bin/env python3
"""PF-01: full accept queues must not stop control, UDP or adapter TUN work."""
import contextlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from switch_tunnel_test import frame, terminate, wait_for


def snapshot(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
        sock.settimeout(1)
        sock.connect(path)
        sock.sendall(b"show stats")
        return dict(line.split("=", 1) for line in sock.recv(65536).decode().splitlines())


def until(check, timeout=4):
    deadline = time.monotonic() + timeout
    while not check():
        assert time.monotonic() < deadline, "condition did not become true"
        time.sleep(0.02)


def listen(stack, path):
    sock = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
    sock.bind(path)
    sock.listen(1)
    sock.settimeout(4)
    return sock


def fill_queue(stack, path):
    # Linux listen(1) permits two queued AF_UNIX connections.
    for _ in range(2):
        sock = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
        sock.settimeout(1)
        sock.connect(path)


def accept_port(stack, listener, port):
    peer, _ = listener.accept()
    stack.enter_context(peer)
    peer.settimeout(2)
    encoded = port.encode()
    assert peer.recv(128) == b"TTP\x01" + bytes([len(encoded), 0, 0, 0]) + encoded
    return peer


def drain_fillers(listener):
    for _ in range(2):
        peer, _ = listener.accept()
        peer.close()


def start(stack, args, **kwargs):
    process = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, **kwargs)
    stack.callback(terminate, process)
    return process


def check_backoff(before, after, elapsed):
    attempts = int(after["switch_reconnect_attempts"]) - int(before["switch_reconnect_attempts"])
    assert 1 <= attempts <= int(elapsed) + 1, ("reconnect spin or no retries", attempts, elapsed)
    assert after["switch_connected"] == "0"


def tunnel_case(binary):
    with tempfile.TemporaryDirectory(prefix="tuntom-pf01.") as directory, contextlib.ExitStack() as stack:
        client_path, server_path = directory + "/client.sock", directory + "/server.sock"
        client_ctl, server_ctl = directory + "/client.ctl", directory + "/server.ctl"
        listener = listen(stack, client_path)
        server_listener = listen(stack, server_path)
        fill_queue(stack, client_path)
        env = dict(os.environ, TUNTOM_SECRET="00112233445566778899aabbccddeeff")
        common = ["--quiet", "--no-pmtud"]
        server = start(stack, [binary, "server", "240", "-", *common,
                              "--switch-socket", server_path, "--switch-port-id", "server",
                              "--switch-label", "2", "--control-socket", server_ctl], env=env)
        client = start(stack, [binary, "client", "240", "-", "localhost", *common,
                              "--switch-socket", client_path, "--switch-port-id", "client",
                              "--switch-label", "1", "--control-socket", client_ctl], env=env)
        server_peer = accept_port(stack, server_listener, "server")
        wait_for([client_ctl, server_ctl], [server, client])
        # Both the initial handshake and control must work despite the full queue.
        until(lambda: snapshot(client_ctl)["session_confirmed"] == "1")
        payload = b"packet flow survives unavailable switch"
        for cycle in range(2):
            before = snapshot(client_ctl)
            began = time.monotonic()
            while time.monotonic() - began < 2.2:
                server_peer.sendall(frame(99, payload))
                assert snapshot(client_ctl)["switch_connected"] == "0"
                time.sleep(0.05)
            after = snapshot(client_ctl)
            check_backoff(before, after, time.monotonic() - began)
            assert int(after["udp_rx_packets"]) > int(before["udp_rx_packets"])
            assert int(after["switch_drops"]) > int(before["switch_drops"])
            assert after["session_confirmed"] == "1"
            assert client.poll() is None and server.poll() is None
            drain_fillers(listener)
            client_peer = accept_port(stack, listener, "client")
            until(lambda: snapshot(client_ctl)["switch_connected"] == "1")
            client_peer.sendall(frame(99, payload))
            assert server_peer.recv(65536) == frame(2, payload)
            server_peer.sendall(frame(99, payload))
            # An already in-flight DATA record can arrive just after reconnect.
            assert client_peer.recv(65536) == frame(1, payload)
            if cycle == 0:
                # Repeat the failure after an established connection is lost.
                fill_queue(stack, client_path)
                client_peer.close()
                until(lambda: snapshot(client_ctl)["switch_connected"] == "0")
        print("PASS: tuntom full accept queue at startup/reconnect, control, UDP and recovery")


def adapter_case(binary):
    with tempfile.TemporaryDirectory(prefix="adapter-pf01.") as directory, contextlib.ExitStack() as stack:
        path, ctl = directory + "/switch.sock", directory + "/adapter.ctl"
        listener = listen(stack, path)
        tun, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.enter_context(tun)
        stack.enter_context(endpoint)
        tun.settimeout(2)
        env = dict(os.environ, TUNTOM_TEST_TUN_FD=str(endpoint.fileno()))
        adapter = start(stack, [binary, "test-tun", "--switch-socket", path,
                               "--switch-port-id", "adapter", "--control-socket", ctl,
                               "--l3-capacity", "16", "--l4-capacity", "16"],
                        env=env, pass_fds=(endpoint.fileno(),))
        peer = accept_port(stack, listener, "adapter")
        wait_for([ctl], [adapter])
        # Learn a reverse route before disconnect, then exercise it after recovery.
        packet = bytes.fromhex("4500001400000000400100000a0000010a000002")
        reply = packet[:12] + packet[16:20] + packet[12:16]
        peer.sendall(bytes([1, 2]) + frame(7, packet)[2:])
        assert tun.recv(65536) == packet
        fill_queue(stack, path)
        peer.close()
        until(lambda: snapshot(ctl)["switch_connected"] == "0")
        before = snapshot(ctl)
        began = time.monotonic()
        while time.monotonic() - began < 2.2:
            tun.sendall(reply)
            assert snapshot(ctl)["switch_connected"] == "0"
            time.sleep(0.05)
        after = snapshot(ctl)
        check_backoff(before, after, time.monotonic() - began)
        assert int(after["tun_rx_packets"]) > int(before["tun_rx_packets"])
        assert int(after["switch_disconnected_drops"]) > 0
        assert adapter.poll() is None
        drain_fillers(listener)
        peer = accept_port(stack, listener, "adapter")
        tun.sendall(reply)
        assert peer.recv(65536) == frame(7, reply)
        print("PASS: adapter full accept queue, control, TUN service and route recovery (simulated TUN)")


def listener_case(binary):
    with tempfile.TemporaryDirectory(prefix="switch-listener-pf01.") as directory, contextlib.ExitStack() as stack:
        path, ctl = directory + "/switch.sock", directory + "/switch.ctl"
        switch = start(stack, [binary, "--socket", path, "--control-socket", ctl])
        wait_for([path, ctl], [switch])
        socket_flags = []
        for fd in Path(f"/proc/{switch.pid}/fd").iterdir():
            if os.readlink(fd).startswith("socket:"):
                fields = dict(line.split(":", 1) for line in
                              Path(f"/proc/{switch.pid}/fdinfo/{fd.name}").read_text().splitlines())
                socket_flags.append(int(fields["flags"], 8))
        assert len(socket_flags) == 2 and all(flags & os.O_NONBLOCK for flags in socket_flags)
        assert snapshot(ctl)["component"] == "switch"
        print("PASS: switch listeners are nonblocking")


if __name__ == "__main__":
    tunnel_case(sys.argv[1])
    adapter_case(sys.argv[2])
    listener_case(sys.argv[3])
