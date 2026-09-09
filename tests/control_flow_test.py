#!/usr/bin/env python3
"""Idle control clients alongside real switch, adapter and tunnel packet flow."""
import contextlib
from pathlib import Path
import socket
import sys

sys.dont_write_bytecode = True
from runtime_recovery_test import adapter_case, switch_case, tunnel_case
from switch_reconnect_test import snapshot, until


def check_flow(process, ctl, marker, fault, send, receive):
    def fd_count():
        return len(list(Path(f"/proc/{process.pid}/fd").iterdir()))

    try:
        baseline = fd_count()
    except PermissionError:
        # Tuntom disables dumpability during hardening, restricting /proc/PID/fd.
        baseline = None
        print("SKIP: FD count unavailable for hardened daemon; flow and expiry still checked", flush=True)
    for _ in range(30):
        with contextlib.ExitStack() as stack:
            # Leave a slot for a normal statistics query.
            for _ in range(3):
                idle = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
                idle.settimeout(1)
                idle.connect(ctl)
            for _ in range(5):
                send()
                receive()
            assert snapshot(ctl)["format"] == "txt"
            assert process.poll() is None
            if baseline is not None:
                assert fd_count() <= baseline + 4
    if baseline is not None:
        until(lambda: fd_count() <= baseline)
    # A client admitted without a request must be closed by a timer alone.
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as idle:
        idle.settimeout(1)
        idle.connect(ctl)
        assert idle.recv(64) == b""
    send()
    receive()
    assert process.poll() is None


if __name__ == "__main__":
    tunnel, switch, adapter, library = sys.argv[1:]
    for name, fixture, binary in (("switch", switch_case, switch),
                                   ("adapter", adapter_case, adapter),
                                   ("tuntom", tunnel_case, tunnel)):
        fixture(binary, library, "none", check=check_flow)
        print(f"PASS: {name} idle control clients, packet flow, stats and timer expiry", flush=True)
