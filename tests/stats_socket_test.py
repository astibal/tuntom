#!/usr/bin/env python3
"""File-independent stats, continued sampling and file-only signal controls."""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

# Keep the test runner from leaving helper bytecode in the source tree.
sys.dont_write_bytecode = True
from switch_tunnel_test import exchange, frame, start_switch, terminate, wait_for


def snapshot(ctl, path):
    output = subprocess.check_output([ctl, path, "show", "stats"], text=True, timeout=2)
    fields = dict(line.split("=", 1) for line in output.splitlines())
    assert "error" not in fields, output
    return fields


def wait_until(check, description, timeout=7):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError(description)


def file_state(path):
    if not os.path.exists(path):
        return None
    stat = os.stat(path)
    with open(path, "rb") as stream:
        return stat.st_ino, stat.st_mtime_ns, stream.read()


def run_case(tuntom, switch, ctl, unavailable_path):
    processes = []
    with tempfile.TemporaryDirectory(prefix="tuntom-stats-socket.") as directory:
        switch_path = os.path.join(directory, "switch.sock")
        client_control = os.path.join(directory, "client.control")
        server_control = os.path.join(directory, "server.control")
        client_stats = os.path.join(directory, "client.stats")
        # Cover both keeping an existing file untouched and never creating one.
        if not unavailable_path:
            with open(client_stats, "wb") as stream:
                stream.write(b"existing stats must not be overwritten\n")
        original_file = file_state(client_stats)
        server_file_args = (["--stats-file", os.path.join(directory, "missing", "server.stats")]
                            if unavailable_path else [])
        environment = os.environ.copy()
        environment["TUNTOM_SECRET"] = "00112233445566778899aabbccddeeff"
        app = None
        try:
            processes.append(start_switch(switch, switch_path))
            wait_for([switch_path], processes)
            common = ["--quiet", "--no-stats", "--pfs", "--mtu", "9000",
                      "--transport-mtu", "1500", "--no-pmtud",
                      "--switch-socket", switch_path]
            server = subprocess.Popen([
                tuntom, "server", "239", "-", *common,
                "--switch-port-id", "server", "--switch-label", "2",
                "--control-socket", server_control, *server_file_args,
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            processes.append(server)
            client = subprocess.Popen([
                tuntom, "client", "239", "-", "localhost", *common,
                "--switch-port-id", "client", "--switch-label", "1",
                "--control-socket", client_control, "--stats-file", client_stats,
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            processes.append(client)
            wait_for([server_control, client_control], processes)
            assert snapshot(ctl, server_control)["stats_enabled"] == "0"
            assert snapshot(ctl, client_control)["stats_enabled"] == "0"
            assert file_state(client_stats) == original_file

            app = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            app.connect(switch_path)
            app.sendall(b"TTP\x01\x03\x00\x00\x00app")
            app.settimeout(0.2)
            payload = bytes(range(256)) * 35 + bytes(range(40))
            expected = frame(11, payload)
            # Enough datagrams to cross the 1/1024 sampling interval even if
            # the first selected datagram was handshake/control traffic.
            for _ in range(600):
                exchange(app, payload, expected, processes)
            fields = snapshot(ctl, server_control)
            assert int(fields["rx_processing_samples"]) > 0, fields
            assert int(fields["reassembly_span_samples"]) > 0, fields
            received = int(fields["udp_rx_packets"])
            exchange(app, payload, expected, processes)
            assert int(snapshot(ctl, server_control)["udp_rx_packets"]) > received
            assert file_state(client_stats) == original_file
            assert not os.path.exists(os.path.join(directory, "missing"))
            assert not any(".tmp." in name for name in os.listdir(directory))

            if not unavailable_path:
                # Rates represent completed five-second buckets, not the
                # incomplete current bucket. Socket reads must keep them live.
                wait_until(lambda: float(snapshot(ctl, server_control)["udp_rx_bps_5s"]) > 0,
                           "throughput stopped with --no-stats")
                before = snapshot(ctl, client_control)
                assert file_state(client_stats) == original_file
                client.send_signal(signal.SIGUSR1)
                wait_until(lambda: file_state(client_stats) != original_file,
                           "SIGUSR1 did not enable file export")
                enabled = snapshot(ctl, client_control)
                assert enabled["stats_enabled"] == "1"
                assert int(enabled["throughput_window_buckets"]) >= int(before["throughput_window_buckets"])
                assert float(enabled["udp_tx_bps_1m"]) > 0, "toggle erased throughput history"
                client.send_signal(signal.SIGUSR1)
                wait_until(lambda: snapshot(ctl, client_control)["stats_enabled"] == "0",
                           "SIGUSR1 did not disable file export")
                frozen_file = file_state(client_stats)
                for _ in range(20):
                    exchange(app, payload, expected, processes)
                assert int(snapshot(ctl, client_control)["udp_tx_packets"]) > int(enabled["udp_tx_packets"])
                time.sleep(1.1)
                assert file_state(client_stats) == frozen_file, "disabled export touched the file"
                client.send_signal(signal.SIGUSR2)
                wait_until(lambda: file_state(client_stats) != frozen_file,
                           "SIGUSR2 did not write explicit snapshot")
                assert b"stats_enabled=0\n" in file_state(client_stats)[2]
                assert snapshot(ctl, client_control)["stats_enabled"] == "0"
        finally:
            if app is not None:
                app.close()
            for process in reversed(processes):
                terminate(process)


if __name__ == "__main__":
    for unavailable_path in (False, True):
        run_case(*sys.argv[1:4], unavailable_path)
    print("PASS: stats without files, no socket disk I/O, continued sampling/rates and file-only toggles")
