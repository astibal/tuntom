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


def main():
    tuntom = sys.argv[1]
    switch = sys.argv[2]
    processes = []
    with tempfile.TemporaryDirectory(prefix="tuntom-switch-link-test.") as directory:
        server_path = os.path.join(directory, "server.sock")
        client_path = os.path.join(directory, "client.sock")
        app_path = os.path.join(directory, "app.sock")
        switch_process = subprocess.Popen([
            switch,
            "--port", f"server={server_path}",
            "--port", f"client={client_path}",
            "--port", f"app={app_path}",
            "--route", "app:10=client:99",
            "--route", "server:2=app:11",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        processes.append(switch_process)

        try:
            wait_for([server_path, client_path, app_path], processes)
            environment = os.environ.copy()
            environment["TUNTOM_SECRET"] = "00112233445566778899aabbccddeeff"
            server = subprocess.Popen([
                tuntom, "server", "237", "-", "--quiet", "--no-stats",
                "--switch-socket", server_path, "--switch-label", "2",
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            processes.append(server)
            client = subprocess.Popen([
                tuntom, "client", "237", "-", "localhost", "--quiet", "--no-stats",
                "--switch-socket", client_path, "--switch-label", "1",
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            processes.append(client)

            app = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            app.connect(app_path)
            app.settimeout(0.2)
            payload = b"\x45\x00\x00\x14tuntom-switch-link"
            expected = frame(11, payload)

            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                for process in processes:
                    if process.poll() is not None:
                        error = process.stderr.read().decode(errors="replace")
                        raise RuntimeError(f"test process exited: {error}")
                app.sendall(frame(10, payload))
                try:
                    received = app.recv(65535)
                    if received != expected:
                        raise RuntimeError("unexpected packet across switched tuntom link")
                    break
                except TimeoutError:
                    pass
            else:
                raise RuntimeError("no packet crossed switched tuntom link")

            app.close()
        finally:
            for process in reversed(processes):
                terminate(process)

    print("PASS: tuntom switch mode carries an opaque payload without creating a TUN")


if __name__ == "__main__":
    main()
