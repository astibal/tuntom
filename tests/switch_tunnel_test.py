#!/usr/bin/env python3
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def frame(label, payload):
    size = 16 + len(payload)
    return struct.pack("!BBBBIQ", 1, 1, 0, 1, size, label) + payload


def wait_for(paths, processes):
    for _ in range(200):
        if all(os.path.exists(path) for path in paths):
            return
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError("test process exited during startup")
        time.sleep(0.01)
    raise RuntimeError("switch sockets were not created")


def terminate(process):
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def start_switch(binary, path):
    return subprocess.Popen([
        binary,
        "--socket", path,
        "--route", "app:10=client:99",
        "--route", "server:2=app:11",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def exchange(app, payload, expected, live_processes):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        for process in live_processes:
            if process.poll() is not None:
                error = process.stderr.read().decode(errors="replace")
                raise RuntimeError(f"test process exited: {error}")
        app.sendall(frame(10, payload))
        try:
            received = app.recv(65535)
            if received != expected:
                raise RuntimeError("unexpected packet across switched tuntom link")
            return
        except TimeoutError:
            pass
    raise RuntimeError("no packet crossed switched tuntom link")


def check_startup_permission_failure(tuntom, switch):
    with tempfile.TemporaryDirectory(prefix="tuntom-switch-permission-test.") as directory:
        switch_path = os.path.join(directory, "switch.sock")
        switch_process = start_switch(switch, switch_path)
        try:
            wait_for([switch_path], [switch_process])
            os.chmod(switch_path, 0)
            environment = os.environ.copy()
            environment["TUNTOM_SECRET"] = "00112233445566778899aabbccddeeff"
            process = subprocess.Popen([
                tuntom, "server", "238", "-", "--quiet",
                "--switch-socket", switch_path, "--switch-port-id", "denied",
                "--switch-label", "1",
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                _, error = process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                terminate(process)
                raise RuntimeError("tuntom accepted an inaccessible switch socket")
            if process.returncode == 0 or b"Cannot access switch socket" not in error:
                raise RuntimeError(
                    "tuntom did not report an inaccessible switch socket: " +
                    error.decode(errors="replace"))
        finally:
            os.chmod(switch_path, 0o660)
            terminate(switch_process)


def main():
    tuntom = sys.argv[1]
    switch = sys.argv[2]
    ctl = sys.argv[3]
    processes = []
    with tempfile.TemporaryDirectory(prefix="tuntom-switch-link-test.") as directory:
        switch_path = os.path.join(directory, "switch.sock")
        server_control = os.path.join(directory, "server.control")
        client_control = os.path.join(directory, "client.control")
        switch_process = start_switch(switch, switch_path)
        processes.append(switch_process)

        try:
            wait_for([switch_path], processes)
            environment = os.environ.copy()
            environment["TUNTOM_SECRET"] = "00112233445566778899aabbccddeeff"
            server = subprocess.Popen([
                tuntom, "server", "237", "-", "--quiet",
                "--mtu", "9000", "--transport-mtu", "1500", "--no-pmtud",
                "--switch-socket", switch_path, "--switch-port-id", "server",
                "--switch-label", "2",
                "--control-socket", server_control,
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            processes.append(server)
            client = subprocess.Popen([
                tuntom, "client", "237", "-", "localhost", "--quiet",
                "--mtu", "9000", "--transport-mtu", "1500", "--no-pmtud",
                "--switch-socket", switch_path, "--switch-port-id", "client",
                "--switch-label", "1",
                "--control-socket", client_control,
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            processes.append(client)

            app = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            app.connect(switch_path)
            app.sendall(b"TTP\x01\x03\x00\x00\x00app")
            app.settimeout(0.2)
            payload = b"\x45\x00\x00\x14tuntom-switch-link"
            expected = frame(11, payload)

            exchange(app, payload, expected, [switch_process, server, client])

            fragmented_payload = bytes(range(256)) * 35 + bytes(range(40))
            exchange(app, fragmented_payload, frame(11, fragmented_payload),
                     [switch_process, server, client])

            wait_for([server_control, client_control], [server, client])
            client_snapshot = subprocess.check_output(
                [ctl, client_control, "show", "stats"], text=True)
            if ("mode=client" not in client_snapshot or
                    "switch_connected=1" not in client_snapshot or
                    "suite=2" not in client_snapshot or
                    "encryption=ascon-aead128" not in client_snapshot or
                    "pfs=1" not in client_snapshot):
                raise RuntimeError("invalid tuntom control stats response")
            server_snapshot = subprocess.check_output(
                [ctl, server_control, "show", "stats"], text=True)
            fields = dict(line.split("=", 1) for line in server_snapshot.splitlines())
            if (int(fields.get("reassembly_completed_packets", "0")) < 1 or
                    fields.get("reassembly_active_entries") != "0" or
                    fields.get("reassembly_active_bytes") != "0"):
                raise RuntimeError("fragmented traffic or live reassembly metrics failed")

            app.close()
            terminate(switch_process)
            time.sleep(1.2)
            if server.poll() is not None or client.poll() is not None:
                raise RuntimeError("tuntom exited when the switch disappeared")

            switch_process = start_switch(switch, switch_path)
            processes.append(switch_process)
            wait_for([switch_path], [switch_process])

            app = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            app.connect(switch_path)
            app.sendall(b"TTP\x01\x03\x00\x00\x00app")
            app.settimeout(0.2)
            exchange(app, payload, expected, [switch_process, server, client])
            app.close()
        finally:
            for process in reversed(processes):
                terminate(process)

    check_startup_permission_failure(tuntom, switch)
    print("PASS: switch traffic, tuntomctl stats, reconnect and permission validation")


if __name__ == "__main__":
    main()
