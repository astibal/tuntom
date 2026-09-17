#!/usr/bin/env python3
"""Real V5 tunnel -> V2 MP -> adapter loop; only the kernel TUN is substituted."""
import contextlib
import os
from pathlib import Path
import socket
import signal
import struct
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from switch_mp_integration_test import Switch, until
from switch_v2_integration_test import Peer, frame
from switch_tunnel_test import terminate
from switch_reconnect_test import snapshot
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fabric.control import query


def packet(seq, length=1500):
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, length, seq, 0, 64, 1, 0,
                         b"\x0a\x00\x00\x01", b"\x0a\x00\x00\x02")
    return header + struct.pack("!I", seq) + bytes([seq % 251]) * (length - 24)


def run(tuntom, binary, adapter, faults, decline):
    options = ["--workers", "4", "--ipc-slots", "32", "--exit-port", "adapter",
               "--route", "app:10=client:99", "--route", "server:2=adapter:83",
               "--route", "adapter:83=server:99", "--route", "client:1=app:13"]
    with Switch(binary, *options) as switch, contextlib.ExitStack() as stack:
        directory = switch.root
        children = []
        env = dict(os.environ, TUNTOM_SECRET="00112233445566778899aabbccddeeff")
        controls = {}
        for role, label in (("server", "2"), ("client", "1")):
            ctl = directory / (role + ".ctl"); controls[role] = ctl
            args = [tuntom, role, "235", "-"] + (["127.0.0.1"] if role == "client" else [])
            args += ["--quiet", "--no-stats", "--no-pmtud", "--no-ttl-compensate", "--mtu", "9000",
                     "--transport-mtu", "1500", "--switch-socket", str(switch.data), "--switch-port-id", role,
                     "--switch-label", label, "--control-socket", str(ctl)]
            log = stack.enter_context(open(directory / (role + ".log"), "w+"))
            child = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=log, env=env)
            children.append((child, log)); stack.callback(terminate, child)
        tun, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.enter_context(tun); stack.enter_context(endpoint); tun.settimeout(8)
        controls["adapter"] = directory / "adapter.ctl"
        adapter_env = dict(os.environ, TUNTOM_TEST_TUN_FD=str(endpoint.fileno()))
        if decline:
            marker = directory / "client-fault"; marker.write_text("mmap")
            adapter_env.update(LD_PRELOAD=str(Path(faults).resolve()), TUNTOM_V2_FAULT_MARKER=str(marker))
        log = stack.enter_context(open(directory / "adapter.log", "w+"))
        child = subprocess.Popen([adapter, "test-v2", "--mtu", "9000", "--switch-socket", str(switch.data),
                                  "--switch-port-id", "adapter", "--control-socket", str(controls["adapter"]),
                                  "--l3-capacity", "64", "--l4-capacity", "64"],
                                 env=adapter_env, pass_fds=(endpoint.fileno(),), stdout=subprocess.DEVNULL, stderr=log)
        children.append((child, log)); stack.callback(terminate, child)
        adapter_process = child

        def ready():
            for child, log in children:
                if child.poll() is not None:
                    log.seek(0); raise AssertionError(log.read())
            if not all(path.exists() for path in controls.values()):
                return False
            stats = {role: snapshot(str(path)) for role, path in controls.items()}
            return all(value["switch_connected"] == "1" for value in stats.values()) and stats["client"]["session_confirmed"] == "1"
        until(ready, timeout=10)
        app = Peer(switch, "app")
        try:
            # Repeated bursts warm adaptive polling and exercise short and jumbo
            # V5 reassembly, grouped RX state, adapter reverse cache and TX flush.
            for iteration in range(24):
                packets = [packet(iteration * 8 + j + 1, 9000 if j % 3 else 64) for j in range(8)]
                app.send([frame([10], data) for data in packets])
                for expected in packets:
                    assert tun.recv(10000) == expected, "V5 -> mmap -> adapter payload"
                replies = [data[:12] + data[16:20] + data[12:16] + data[20:] for data in packets]
                for data in replies:
                    tun.sendall(data)
                for expected in replies:
                    assert app.receive() == frame([13], expected), "adapter -> mmap -> V5 payload"
            # Queue genuinely ready input before resuming our own adapter child.
            # This exercises its real adaptive batch/flush path without a fill timer.
            bulk = []
            adapter_process.send_signal(signal.SIGSTOP)
            try:
                tun.setblocking(False)
                for i in range(128):
                    data = packet(1000 + i, 64)
                    reply = data[:12] + data[16:20] + data[12:16] + data[20:]
                    try:
                        tun.send(reply)
                    except BlockingIOError:
                        break
                    bulk.append(reply)
            finally:
                tun.settimeout(8)
                adapter_process.send_signal(signal.SIGCONT)
            assert len(bulk) >= 32
            for expected in bulk:
                assert app.receive() == frame([13], expected)
            for role, path in controls.items():
                dump = query(str(path), "flows")
                assert "view=flows\n" in dump
                if role == "adapter":
                    assert "table=l3 ip_version=4 src=10.0.0.2 dst=10.0.0.1" in dump
                    assert "labels=[0x0000000000000053]" in dump
                    assert "flow_count=1\n" in dump
                else:
                    assert "tracking=none\n" in dump and "flow_count=0\n" in dump
            transport_stats = {}
            for role, path in controls.items():
                stats = snapshot(str(path))
                transport_stats[role] = {k: int(v) for k, v in stats.items()
                                         if k in ("switch_ipc_tx_records", "switch_ipc_tx_mapped_frames", "switch_ipc_tx_largest_batch")}
                assert stats["switch_ipc_version"] == "2", (role, stats)
                assert stats["switch_ipc_mmap"] == ("0" if decline and role == "adapter" else "1"), (role, stats)
                expected_tx = 192 + (len(bulk) if role in ("adapter", "client") else 0)
                expected_rx = 192 + (len(bulk) if role == "server" else 0)
                assert int(stats["switch_tx_packets"]) == expected_tx and int(stats["switch_rx_packets"]) == expected_rx, (role, stats)
                if not (decline and role == "adapter"):
                    assert int(stats["switch_ipc_tx_mapped_frames"]) > 0
                    assert int(stats["switch_ipc_rx_mapped_frames"]) > 0
            if not decline:
                assert transport_stats["adapter"]["switch_ipc_tx_largest_batch"] > 1, transport_stats
            print("path transport statistics:", transport_stats)
            stats = switch.stats()
            assert stats["frames_rx"] == stats["frames_tx"] == 768 + 2 * len(bulk), stats
            assert not any(stats[k] for k in ("queue_full_drops", "malformed_frames", "send_errors", "pool_stalls")), stats
        finally:
            app.close()
    print(f"PASS actual tuntom/V5 + MP + adapter; mmap_declined_by_adapter={decline}")


def main():
    for decline in (False, True):
        run(*sys.argv[1:5], decline)


if __name__ == "__main__":
    main()
