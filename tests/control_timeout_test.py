#!/usr/bin/env python3
"""Control CLI responses, peer failures, and bounded waits without a daemon."""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run(ctl):
    with tempfile.TemporaryDirectory(prefix="tuntom-ctl.") as directory:
        path = str(Path(directory) / "control")
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
            server.bind(path)
            server.listen(0)
            server.settimeout(3)

            def start():
                return subprocess.Popen([ctl, path, "show", "stats"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def finish(process, success, timeout=False):
                try:
                    out, err = process.communicate(timeout=8)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
                assert (process.returncode == 0) == success, (out, err)
                if timeout:
                    assert b"timed out" in err, err
                return out

            for payload in (b"packets=42\n", b"x" * 65536, b"x" * 65537, None):
                process = start()
                with server.accept()[0] as peer:
                    peer.settimeout(3)
                    assert peer.recv(64) == b"show stats"
                    if payload is not None:
                        peer.sendall(payload)
                out = finish(process, payload is not None and len(payload) <= 65536)
                if payload is not None and len(payload) <= 65536:
                    assert out == payload

            began = time.monotonic()
            process = start()
            with server.accept()[0] as peer:
                peer.settimeout(3)
                assert peer.recv(64) == b"show stats"
                finish(process, False, timeout=True)
            assert 4.5 <= time.monotonic() - began < 8

            # A backlog of zero admits one connection on Linux. Hold it queued.
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as queued:
                queued.connect(path)
                began = time.monotonic()
                finish(start(), False, timeout=True)
                assert 4.5 <= time.monotonic() - began < 8
                with server.accept()[0]:
                    pass

            # Recover after backlog pressure, then keep the response pending.
            # Time spent connecting must count toward the same five seconds.
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as queued:
                queued.connect(path)
                began = time.monotonic()
                process = start()
                time.sleep(2)
                with server.accept()[0]:
                    pass
                with server.accept()[0] as peer:
                    peer.settimeout(3)
                    assert peer.recv(64) == b"show stats"
                    finish(process, False, timeout=True)
                assert 4.5 <= time.monotonic() - began < 6.5

        result = subprocess.run([ctl, path, "show", "stats"], capture_output=True, timeout=2)
        assert result.returncode != 0
    print("control timeout tests passed")


if __name__ == "__main__":
    run(sys.argv[1])
