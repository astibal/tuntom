#!/usr/bin/env python3
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def frame(opcode, labels, payload):
    size = 8 + 8 * len(labels) + len(payload)
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), size) + \
        b"".join(struct.pack("!Q", label) for label in labels) + payload


def register(peer, port_id):
    encoded = port_id.encode("ascii")
    peer.sendall(b"TTP\x01" + bytes([len(encoded), 0, 0, 0]) + encoded)


def wait_for(path):
    for _ in range(100):
        if os.path.exists(path):
            return
        time.sleep(0.01)
    raise RuntimeError("switch socket was not created")


def connect(path):
    peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    peer.connect(path)
    peer.settimeout(2)
    return peer


def stop(process):
    process.send_signal(signal.SIGTERM)
    process.wait(timeout=2)
    if process.returncode != 0:
        raise RuntimeError("switch exited unsuccessfully")


def main():
    binary = sys.argv[1]
    ctl_binary = sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="tuntom-switch-test.") as directory:
        path = os.path.join(directory, "switch.sock")
        control_path = os.path.join(directory, "switch.control")
        process = subprocess.Popen([
            binary,
            "--socket", path,
            "--control-socket", control_path,
            "--route", "a:17=b:83",
            "--route", "a:18=exit:84",
            "--exit-port", "exit",
            "--default-back=on",
        ])
        try:
            wait_for(path)
            a = connect(path)
            b = connect(path)
            register(a, "a")
            register(b, "b")
            exit_peer = connect(path)
            register(exit_peer, "exit")
            time.sleep(0.05)

            payload = b"\x45\x00\x00\x14"
            a.sendall(frame(1, [17, 200], payload))
            expected = frame(1, [83, 200], payload)
            if b.recv(65535) != expected:
                raise RuntimeError("label swap or stack preservation failed")

            a.sendall(frame(1, [18, 200], payload))
            expected_exit_route = frame(2, [84, 200], payload)
            if exit_peer.recv(65535) != expected_exit_route:
                raise RuntimeError("route to exit port did not produce EXIT")

            replacement_b = connect(path)
            register(replacement_b, "b")
            time.sleep(0.05)
            a.sendall(frame(1, [17, 200], payload))
            if replacement_b.recv(65535) != expected:
                raise RuntimeError("reconnected port did not replace its old connection")

            unmatched = frame(1, [99], payload)
            a.sendall(unmatched)
            expected_exit = frame(2, [99], payload)
            if a.recv(65535) != expected_exit:
                raise RuntimeError("default-back did not return EXIT")

            stats = subprocess.check_output([
                ctl_binary, control_path, "show", "stats"
            ], text=True)
            required = {
                "component=switch",
                "connections_current=3",
                "route_hits=3",
                "route_misses=1",
                "exit_deliveries=1",
                "default_back=1",
                "send_errors=0",
            }
            missing = required.difference(stats.splitlines())
            if missing:
                raise RuntimeError(f"invalid switch control stats: {sorted(missing)}")

            a.close()
            b.close()
            replacement_b.close()
            exit_peer.close()
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    print("PASS: registration, reconnect, label swap, exit routes and default-back")


if __name__ == "__main__":
    main()
