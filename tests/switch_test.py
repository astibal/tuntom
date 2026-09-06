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
    with tempfile.TemporaryDirectory(prefix="tuntom-switch-test.") as directory:
        path_a = os.path.join(directory, "a.sock")
        path_b = os.path.join(directory, "b.sock")
        process = subprocess.Popen([
            binary,
            "--port", f"a={path_a}",
            "--port", f"b={path_b}",
            "--route", "a:17=b:83",
            "--default-back=on",
        ])
        try:
            wait_for(path_a)
            wait_for(path_b)
            a = connect(path_a)
            b = connect(path_b)
            time.sleep(0.05)

            payload = b"\x45\x00\x00\x14"
            a.sendall(frame(1, [17, 200], payload))
            expected = frame(1, [83, 200], payload)
            if b.recv(65535) != expected:
                raise RuntimeError("label swap or stack preservation failed")

            unmatched = frame(1, [99], payload)
            a.sendall(unmatched)
            expected_exit = frame(2, [99], payload)
            if a.recv(65535) != expected_exit:
                raise RuntimeError("default-back did not return EXIT")

            a.close()
            b.close()
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    print("PASS: switch routing, label swap, stack preservation and default-back")


if __name__ == "__main__":
    main()
