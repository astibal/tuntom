#!/usr/bin/env python3
"""PF-02: inject resource failures into real loops, retain control and recover."""
import contextlib
import os
from pathlib import Path
import socket
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from switch_reconnect_test import accept_port, listen, snapshot, start, until
from switch_tunnel_test import frame, wait_for


def fault_environment(library, marker, fault):
    return dict(os.environ, LD_PRELOAD=library, TUNTOM_TEST_FAULT=fault,
                TUNTOM_TEST_FAULT_MARKER=str(marker),
                TUNTOM_SECRET="00112233445566778899aabbccddeeff")


def cpu_ticks(process):
    fields = Path(f"/proc/{process.pid}/stat").read_text().split()
    return int(fields[13]) + int(fields[14])


def check_failure(process, ctl, marker, fault, send, receive):
    counter = "runtime_poll_errors" if fault.startswith("poll") else "runtime_allocation_errors"
    before = snapshot(ctl)
    marker.touch()
    if fault.startswith("alloc"):
        # Forwarding formerly allocated a 1200-byte contiguous frame per packet.
        # Keep that allocation failure armed: the new path must deliver DATA
        # without hitting it or entering resource-recovery backoff at all.
        for _ in range(20):
            send()
            receive()
        assert process.poll() is None
        assert snapshot(ctl)[counter] == before[counter]
        marker.unlink()
        send()
        receive()
        return
    began = time.monotonic()
    ticks = cpu_ticks(process)
    while time.monotonic() - began < 1.3:
        try:
            send()
        except TimeoutError:
            pass # A persistently failing data poll is allowed to backpressure.
        fields = snapshot(ctl)
        assert process.poll() is None
        time.sleep(0.05)
    elapsed = time.monotonic() - began
    errors = int(fields[counter]) - int(before[counter])
    assert 1 <= errors <= int(elapsed * 10) + 3, (fault, fields)
    if fault.endswith("once"):
        assert errors == 1, (fault, fields)
    else:
        assert errors > 1, (fault, fields)
    # A repeatedly failing poll must not use an entire CPU core.
    cpu = (cpu_ticks(process) - ticks) / os.sysconf("SC_CLK_TCK") / elapsed
    assert cpu < 0.5, ("resource failure spin", fault, cpu)
    # Recovery must receive a fresh packet, not an old reply queued before the
    # first failing poll (which may have been armed while a real poll slept).
    while True:
        try:
            receive()
        except TimeoutError:
            break
    marker.unlink()
    deadline = time.monotonic() + 3
    while True:
        try:
            send()
            receive()
            break
        except TimeoutError:
            assert time.monotonic() < deadline, "DATA did not recover"
    assert process.poll() is None
    assert int(snapshot(ctl)[counter]) >= int(fields[counter])


def switch_case(binary, library, fault):
    with tempfile.TemporaryDirectory(prefix="switch-pf02.") as directory, contextlib.ExitStack() as stack:
        path, ctl = directory + "/switch.sock", directory + "/control"
        marker = Path(directory) / "fault"
        process = start(stack, [binary, "--socket", path, "--control-socket", ctl,
                                "--default-back=on"], env=fault_environment(library, marker, fault))
        wait_for([path, ctl], [process])
        peer = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
        peer.settimeout(0.1)
        peer.connect(path)
        peer.sendall(b"TTP\x01\x04\x00\x00\x00test")
        until(lambda: snapshot(ctl)["registrations_ok"] == "1")
        wire = frame(1, b"x" * 1184)
        expected = bytes([1, 2]) + wire[2:]

        def receive():
            assert peer.recv(65536) == expected

        check_failure(process, ctl, marker, fault, lambda: peer.sendall(wire), receive)


def adapter_case(binary, library, fault):
    with tempfile.TemporaryDirectory(prefix="adapter-pf02.") as directory, contextlib.ExitStack() as stack:
        path, ctl = directory + "/switch.sock", directory + "/control"
        marker = Path(directory) / "fault"
        listener = listen(stack, path)
        tun, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.enter_context(tun)
        stack.enter_context(endpoint)
        tun.settimeout(0.1)
        env = fault_environment(library, marker, fault)
        env["TUNTOM_TEST_TUN_FD"] = str(endpoint.fileno())
        process = start(stack, [binary, "test", "--switch-socket", path, "--switch-port-id", "test",
                                "--control-socket", ctl, "--l3-capacity", "16", "--l4-capacity", "16"],
                        env=env, pass_fds=(endpoint.fileno(),))
        peer = accept_port(stack, listener, "test")
        peer.settimeout(0.1)
        wait_for([ctl], [process])
        packet = bytes.fromhex("450004a000000000400100000a0000010a000002") + b"x" * 1164
        peer.sendall(bytes([1, 2]) + frame(7, packet)[2:])
        assert tun.recv(65536) == packet
        reply = packet[:12] + packet[16:20] + packet[12:16] + packet[20:]

        def receive():
            assert peer.recv(65536) == frame(7, reply)

        check_failure(process, ctl, marker, fault, lambda: tun.sendall(reply), receive)


def tunnel_case(binary, library, fault, role="server"):
    with tempfile.TemporaryDirectory(prefix="tunnel-pf02.") as directory, contextlib.ExitStack() as stack:
        marker = Path(directory) / "fault"
        if fault == "random_always":
            marker.touch()
        peers, controls, processes = {}, {}, {}
        for mode in ("server", "client"):
            path, ctl = directory + f"/{mode}.sock", directory + f"/{mode}.ctl"
            listener = listen(stack, path)
            env = fault_environment(library, marker, fault)
            if mode != role:
                env.pop("LD_PRELOAD")
            args = [binary, mode, "241", "-"] + (["localhost"] if mode == "client" else [])
            args += ["--quiet", "--no-stats", "--no-pmtud", "--switch-socket", path,
                     "--switch-port-id", mode, "--switch-label", "7", "--control-socket", ctl]
            processes[mode] = start(stack, args, env=env)
            peers[mode] = accept_port(stack, listener, mode)
            peers[mode].settimeout(0.1)
            controls[mode] = ctl
        wait_for(list(controls.values()), list(processes.values()))
        if fault == "random_always":
            until(lambda: int(snapshot(controls[role])["handshake_random_errors"]) > 0, timeout=7)
            assert snapshot(controls[role])["session_confirmed"] == "0"
            calls = int(snapshot(controls[role])["handshake_random_errors"])
            time.sleep(1.2)
            fields = snapshot(controls[role])
            assert calls <= int(fields["handshake_random_errors"]) <= calls + 2
            marker.unlink()
        until(lambda: snapshot(controls["client"])["session_confirmed"] == "1", timeout=8)
        wire = frame(7, b"x" * 1184)

        def receive():
            assert peers["server"].recv(65536) == wire

        if fault == "random_always":
            peers["client"].sendall(wire)
            receive()
        else:
            check_failure(processes[role], controls[role], marker, fault,
                          lambda: peers["client"].sendall(wire), receive)


if __name__ == "__main__":
    tunnel, switch, adapter, library = sys.argv[1:]
    for fault in ("poll_once", "poll_always", "alloc_once", "alloc_always"):
        for name, function, binary in (("switch", switch_case, switch),
                                       ("adapter", adapter_case, adapter),
                                       ("tuntom", tunnel_case, tunnel)):
            function(binary, library, fault)
            print(f"PASS: {name} {fault}, live control and DATA; removed frame allocations stay absent", flush=True)
    for role in ("client", "server"):
        tunnel_case(tunnel, library, "random_always", role)
        print(f"PASS: {role} RNG failure at startup, control and handshake recovery", flush=True)
