#!/usr/bin/env python3
"""PF-06: UDP recovery, live TUN DOWN/UP and fatal device errors in real loops."""
import contextlib
import os
from pathlib import Path
import socket
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from runtime_recovery_test import cpu_ticks, fault_environment
from switch_reconnect_test import accept_port, listen, snapshot, start, until
from switch_tunnel_test import frame, wait_for


def rss_bytes(process):
    fields = Path(f"/proc/{process.pid}/stat").read_text().split()
    return int(fields[23]) * os.sysconf("SC_PAGE_SIZE")


def check_cpu(process, ticks, began, baseline_rss):
    elapsed = time.monotonic() - began
    cpu = (cpu_ticks(process) - ticks) / os.sysconf("SC_CLK_TCK") / elapsed
    assert cpu < 0.45, ("error spin", cpu)
    assert process.poll() is None
    assert rss_bytes(process) <= baseline_rss + 8 * 1024 * 1024, "memory growth during endpoint fault"


def udp_case(binary, library, role, fault):
    with tempfile.TemporaryDirectory(prefix="udp-pf06.") as directory, contextlib.ExitStack() as stack:
        marker = Path(directory) / "fault"
        peers, controls, processes = {}, {}, {}
        for mode in ("server", "client"):
            path, ctl = directory + f"/{mode}.sock", directory + f"/{mode}.ctl"
            listener = listen(stack, path)
            env = fault_environment(library, marker, fault)
            if mode != role:
                env.pop("LD_PRELOAD")
            args = [binary, mode, "241", "-"] + (["localhost"] if mode == "client" else [])
            args += ["--quiet", "--no-pmtud", "--switch-socket", path,
                     "--switch-port-id", mode, "--switch-label", "7", "--control-socket", ctl]
            processes[mode] = start(stack, args, env=env)
            peers[mode] = accept_port(stack, listener, mode)
            peers[mode].settimeout(0.1)
            controls[mode] = ctl
        until(lambda: snapshot(controls["client"])["session_confirmed"] == "1", timeout=8)
        wire = frame(7, b"x" * 1184)
        def traffic():
            for source, target in (("client", "server"), ("server", "client")):
                peers[source].sendall(wire)
                assert peers[target].recv(65536) == wire
        traffic()
        process, ctl = processes[role], controls[role]
        before = snapshot(ctl)
        marker.touch()
        began, ticks, baseline_rss = time.monotonic(), cpu_ticks(process), rss_bytes(process)
        while time.monotonic() - began < 1.4:
            # Poll/read faults must be detected without any incoming packets.
            if fault == "udp_send":
                peers[role].sendall(wire)
            fields = snapshot(ctl)
            time.sleep(0.05)
        check_cpu(process, ticks, began, baseline_rss)
        errors = int(fields["udp_endpoint_errors"]) - int(before["udp_endpoint_errors"])
        assert 1 <= errors <= 20, (fault, fields)
        attempts = int(fields["udp_reopen_attempts"]) - int(before["udp_reopen_attempts"])
        assert attempts <= 2, (fault, fields)
        if fault == "udp_refused":
            assert attempts == 0 and fields["udp_endpoint_available"] == "1"
        else:
            assert fields["udp_endpoint_available"] == "0", fields
        marker.unlink()
        # Drain packets sent during fault injection before asking for fresh traffic.
        for peer in peers.values():
            while True:
                try:
                    peer.recv(65536)
                except TimeoutError:
                    break
        until(lambda: snapshot(ctl)["udp_endpoint_available"] == "1", timeout=4)
        until(lambda: snapshot(controls["client"])["session_confirmed"] == "1", timeout=4)
        traffic()
        if fault != "udp_refused":
            assert int(snapshot(ctl)["udp_reopens"]) > int(before["udp_reopens"])
        print(f"PASS: {role} {fault}, bounded retries/CPU, live control and bidirectional UDP recovery", flush=True)


def tun_case(binary, library, component, fault):
    with tempfile.TemporaryDirectory(prefix="tun-pf06.") as directory, contextlib.ExitStack() as stack:
        path, ctl = directory + "/switch.sock", directory + "/control"
        marker = Path(directory) / "fault"
        listener = listen(stack, path)
        tun, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.enter_context(tun)
        stack.enter_context(endpoint)
        tun.settimeout(1)
        env = fault_environment(library, marker, fault)
        env["TUNTOM_TEST_TUN_FD"] = str(endpoint.fileno())
        args = [binary, "test"] if component == "adapter" else [binary, "server", "242", "test", "--switch-exit-node", "--quiet", "--no-pmtud", "--no-ttl-compensate", "--switch-label", "7"]
        args += ["--switch-socket", path, "--switch-port-id", "test", "--control-socket", ctl]
        process = start(stack, args, env=env, pass_fds=(endpoint.fileno(),))
        peer = accept_port(stack, listener, "test")
        wait_for([ctl], [process])
        packet = bytes.fromhex("4500001400000000400100000a0000010a000002")
        wire = bytes([1, 2]) + frame(7, packet)[2:]
        peer.sendall(wire)
        assert tun.recv(65536) == packet
        if fault == "tun_down":
            return_peer = peer
            if component == "tuntom":
                # TUN replies leave tuntom via UDP, not through its local switch.
                remote_path, remote_ctl = directory + "/remote.sock", directory + "/remote.ctl"
                remote_listener = listen(stack, remote_path)
                remote_env = dict(os.environ, TUNTOM_SECRET=env["TUNTOM_SECRET"])
                start(stack, [binary, "client", "242", "-", "localhost", "--quiet", "--no-pmtud",
                              "--switch-socket", remote_path, "--switch-port-id", "remote",
                              "--switch-label", "7", "--control-socket", remote_ctl], env=remote_env)
                return_peer = accept_port(stack, remote_listener, "remote")
                until(lambda: snapshot(remote_ctl)["session_confirmed"] == "1")
            reply = packet[:12] + packet[16:20] + packet[12:16]
            tun.sendall(reply)
            assert return_peer.recv(65536) == frame(7, reply)
            for cycle in range(2):
                before = snapshot(ctl)
                marker.touch()
                began, ticks, baseline_rss = time.monotonic(), cpu_ticks(process), rss_bytes(process)
                for count in range(1, 13):
                    peer.sendall(wire)
                    until(lambda: int(snapshot(ctl)["tun_write_errors"]) ==
                          int(before["tun_write_errors"]) + count)
                    fields = snapshot(ctl)
                    assert fields["tun_endpoint_available"] == "1", fields
                    assert fields["tun_endpoint_failures"] == "0", fields
                    assert fields["tun_tx_packets"] == before["tun_tx_packets"], fields
                    if component == "tuntom":
                        peer.sendall(frame(7, packet))
                        assert return_peer.recv(65536) == frame(7, packet)
                    time.sleep(0.05)
                check_cpu(process, ticks, began, baseline_rss)
                tun.settimeout(0.1)
                try:
                    tun.recv(65536)
                    raise AssertionError("DOWN packet was delivered")
                except TimeoutError:
                    pass
                marker.unlink()
                # A distinct fresh IP packet detects replay of any DOWN packets.
                fresh = packet[:4] + bytes([0, cycle + 1]) + packet[6:]
                peer.sendall(bytes([1, 2]) + frame(7, fresh)[2:])
                tun.settimeout(1)
                assert tun.recv(65536) == fresh
                tun.sendall(reply)
                assert return_peer.recv(65536) == frame(7, reply)
                tun.settimeout(0.1)
                try:
                    tun.recv(65536)
                    raise AssertionError("UP replayed a dropped packet")
                except TimeoutError:
                    pass
                fields = snapshot(ctl)
                assert fields["tun_endpoint_available"] == "1" and fields["tun_endpoint_failures"] == "0"
                assert int(fields["tun_tx_packets"]) == int(before["tun_tx_packets"]) + 1
            print(f"PASS: {component} repeated DOWN/UP, drops without replay, bounded CPU/RSS, live control and bidirectional recovery (simulated TUN)", flush=True)
            return
        marker.touch()
        if fault == "tun_write":
            peer.sendall(wire)
        # Poll/read/EOF faults must exit even without any incoming data.
        assert process.wait(timeout=3) == 1, (component, fault, process.returncode)
        assert not Path(ctl).exists(), "fatal exit must clean up the control socket"
        assert peer.recv(65536) == b"", "fatal exit must close the IPC connection"
        print(f"PASS: {component} {fault}, exit 1 for external recovery and clean endpoint shutdown", flush=True)


def control_case(binary, library, target):
    with tempfile.TemporaryDirectory(prefix="control-pf06.") as directory, contextlib.ExitStack() as stack:
        path, ctl = directory + "/switch.sock", directory + "/control"
        marker = Path(directory) / "fault"
        process = start(stack, [binary, "--socket", path, "--control-socket", ctl, "--default-back=on"],
                        env=fault_environment(library, marker, target + "_pollerr"))
        wait_for([path, ctl], [process])
        peer = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
        peer.settimeout(1)
        peer.connect(path)
        peer.sendall(b"TTP\x01\x04\x00\x00\x00test")
        until(lambda: snapshot(ctl)["registrations_ok"] == "1")
        marker.touch()
        began, ticks, baseline_rss = time.monotonic(), cpu_ticks(process), rss_bytes(process)
        wire = frame(7, b"packet")
        for _ in range(15):
            peer.sendall(wire)
            assert peer.recv(65536) == bytes([1, 2]) + wire[2:]
            time.sleep(0.05)
        check_cpu(process, ticks, began, baseline_rss)
        marker.unlink()
        assert int(snapshot(ctl)[("control" if target == "control" else "listener") + "_accept_errors"]) >= 1
        print(f"PASS: {target} listener error backoff, continued forwarding and recovery", flush=True)


if __name__ == "__main__":
    tunnel, switch, adapter, tun_fixture, library = sys.argv[1:]
    for role in ("client", "server"):
        for fault in ("udp_pollerr", "udp_hup", "udp_nval", "udp_read", "udp_send", "udp_socket", "udp_bind", "udp_refused"):
            udp_case(tunnel, library, role, fault)
    for component, binary in (("adapter", adapter), ("tuntom", tun_fixture)):
        for fault in ("tun_pollerr", "tun_hup", "tun_nval", "tun_read", "tun_eof", "tun_write", "tun_down"):
            tun_case(binary, library, component, fault)
    for target in ("control", "listener"):
        control_case(switch, library, target)
