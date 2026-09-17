"""Interpretations, bounded logs and the separate collector trust boundary."""
from dataclasses import replace
import json
import http.client
import os
from pathlib import Path
import socket
import select
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit, parse_qs

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from collector import CollectorServer, RemoteFabric, receive, send
from discovery import Endpoint
from logs import MAX_LOG, read_logs, redact, tail_fd
from server import APIError, Fabric
from telemetry import changes, health, switch_detail


def endpoint(kind="switch"):
    return Endpoint("boot:123:100", 123, 100, "fixture", kind, "switch-test", "/tmp/tuntom-switch",
                    os.getuid(), "S", 5, 4096, 2, control="/not-a-control-socket")


class TelemetryTests(unittest.TestCase):
    def test_exact_deltas_reset_and_port_replacement(self):
        previous = {"queue_full_drops": "18446744073709550000", "worker_0_cpu_ns": "1000000000",
                    "buffers_in_use": "10", "port_0_name": "a", "port_0_generation": "1", "port_0_ipc_tx_packets": "2"}
        current = {**previous, "queue_full_drops": "18446744073709550035", "worker_0_cpu_ns": "2000000000",
                   "buffers_in_use": "20", "port_0_name": "b", "port_0_generation": "2", "port_0_ipc_tx_packets": "5"}
        delta = changes(current, previous, 5)
        self.assertEqual(delta["counters"]["queue_full_drops"], "35")
        self.assertNotIn("buffers_in_use", delta["counters"])
        self.assertIn("port_0_ipc_tx_packets", delta["resets"])
        self.assertEqual(switch_detail(current, delta)["workers"][0]["cpu_percent"], 20)
        self.assertEqual(changes({"queue_full_drops":"0"}, previous, 5)["resets"], ["queue_full_drops"])
        self.assertFalse(changes(current, None, None)["ready"])
        self.assertEqual(changes({"queue_full_drops":"9" * 5000}, previous, 5)["counters"], {})

    def test_health_idle_faults_policy_and_missing_data(self):
        e = endpoint("tunnel")
        m = {"session_ready":"1", "udp_rx_bps_5s":"0", "udp_tx_bps_5s":"0", "drops_replay":"4", "policy_drops":"10"}
        sample = {"status":"reachable", "metrics":m, "changes":changes(m, m, 5)}
        result = health(e, sample)
        self.assertEqual(result["level"], "ok")
        self.assertEqual(result["checks"][2]["code"], "traffic_idle")
        sample["changes"] = changes({**m, "policy_drops":"15"}, m, 5)
        self.assertEqual(health(e, sample)["level"], "ok")
        sample["changes"] = changes({**m, "drops_replay":"5"}, m, 5)
        self.assertEqual(health(e, sample)["level"], "warn")
        sample["metrics"] = {**m, "session_ready":"0"}
        self.assertEqual(health(e, sample)["checks"][1]["code"], "session_down")
        self.assertEqual(health(e, {"status":"unavailable"})["level"], "unknown")
        sample["metrics"] = {**m, "udp_rx_bps_5s":"invalid"}
        self.assertEqual(health(e, sample)["checks"][2]["state"], "unknown")

    def test_failed_probe_discards_baseline(self):
        f = Fabric()
        e = endpoint()
        with patch.object(f, "control_query", return_value="format=txt\nformat_version=1\nqueue_full_drops=7\n"):
            self.assertFalse(f._sample(e)["changes"]["ready"])
            self.assertTrue(f._sample(e)["changes"]["ready"])
        with patch.object(f, "control_query", side_effect=OSError("unavailable")):
            self.assertEqual(f._sample(e)["metrics"], {})
        with patch.object(f, "control_query", return_value="format=txt\nformat_version=1\nqueue_full_drops=10\n"):
            self.assertFalse(f._sample(e)["changes"]["ready"])


class LogTests(unittest.TestCase):
    def test_bounded_tail_redaction_and_no_pipe_reads(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "log"
            path.write_text("old\n" * MAX_LOG + "\n".join(f"line {i}" for i in range(70)) + "\ncookie=SECRET token xyz\n")
            result = tail_fd(path)
            self.assertTrue(result["truncated"])
            self.assertLessEqual(len(result["text"].splitlines()), 60)
            self.assertNotIn("SECRET", result["text"])
            self.assertNotIn("xyz", result["text"])
            self.assertIn("line 69", result["text"])
        read, write = os.pipe()
        try:
            os.write(write, b"do not consume")
            self.assertIsNone(tail_fd(f"/proc/self/fd/{read}"))
            self.assertEqual(os.read(read, 14), b"do not consume")
        finally:
            os.close(read)
            os.close(write)
        self.assertNotIn("PRIVATE DATA", redact("-----BEGIN PRIVATE KEY-----\nPRIVATE DATA\n-----END PRIVATE KEY-----"))
        self.assertNotIn("PRIVATE", redact('{"token":"PRIVATE","password": "PRIVATE"}'))

    def test_logs_reject_reused_pid(self):
        e = replace(endpoint(), pid=os.getpid(), start_ticks=-1)
        with self.assertRaisesRegex(OSError, "identity changed"):
            read_logs(e)


class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory(prefix="tt-reader-")
        self.path = str(Path(self.root.name) / "reader")
        self.fabric = Fabric(discover_fn=lambda: ([], {"source":"procfs"}))
        self.server = CollectorServer(self.path, self.fabric, os.getuid())
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.remote = RemoteFabric(self.path)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.root.cleanup()

    def test_peer_uid_operation_allowlist_and_write_gates(self):
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)
        self.assertEqual(self.remote.snapshot()["collector"]["mode"], "separate")
        self.remote.set()
        self.assertTrue(self.fabric.wake.is_set())
        for operation in ("exec", "read", "stats"):
            with self.assertRaises(APIError) as caught:
                self.remote.call(operation, "/etc/passwd")
            self.assertEqual(caught.exception.status, 400)
        with self.assertRaises(APIError) as caught:
            self.remote.logs("/etc/passwd")
        self.assertEqual(caught.exception.status, 404)
        self.fabric.allow_write = True
        self.assertFalse(self.remote.snapshot()["allow_write"])
        with self.assertRaises(APIError) as caught:
            self.remote.rules("anything", "load", {"rules":"format 2"})
        self.assertEqual(caught.exception.status, 403)
        self.server.allowed_uid = os.getuid() + 1
        with self.assertRaises(APIError) as caught:
            self.remote.snapshot()
        self.assertEqual(caught.exception.status, 403)

    def test_frame_limit_bad_requests_and_socket_collision(self):
        for value in ({"operation":"snapshot", "path":"/etc/passwd"}, []):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.connect(self.path)
                send(sock, value)
                self.assertEqual(receive(sock)["status"], 400)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(self.path)
            sock.sendall(struct.pack("!I", 9 * 1024 * 1024))
            self.assertEqual(receive(sock)["status"], 502)
        with self.assertRaises(OSError):
            CollectorServer(self.path, self.fabric, os.getuid())
        self.assertEqual(self.remote.snapshot()["endpoints"], [])

    @unittest.skipIf(os.geteuid() == 0, "HTTP CLI intentionally refuses root")
    def test_cli_remote_errors_keep_http_status(self):
        # A CLI entry point must share APIError with the imported IPC module.
        script = Path(__file__).resolve().parents[1] / "server.py"
        golden = "shared-lab-golden-token-for-testing"
        env_token = "environment-token-with-lower-priority"
        with subprocess.Popen([sys.executable, "-B", str(script), "--collector", self.path, "--port", "0",
                               "--golden-token", golden], env={**os.environ, "TUNTOM_FABRIC_TOKEN":env_token},
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                self.assertTrue(select.select([process.stdout], [], [], 5)[0])
                line = process.stdout.readline().decode()
                self.assertTrue(line.startswith("Tuntom Fabric: http"), line)
                url = urlsplit(line.strip().split(" ", 2)[2])
                token = parse_qs(url.fragment)["token"][0]
                self.assertEqual(token, golden)
                connection = http.client.HTTPConnection("127.0.0.1", url.port, timeout=4)
                connection.request("GET", "/api/v1/endpoints/unknown/logs", headers={"Authorization":"Bearer " + token})
                response = connection.getresponse()
                self.assertEqual(response.status, 404, response.read())
                connection.close()
                connection = http.client.HTTPConnection("127.0.0.1", url.port, timeout=4)
                connection.request("GET", "/api/v1/snapshot", headers={"Authorization":"Bearer " + env_token})
                response = connection.getresponse()
                self.assertEqual(response.status, 401, response.read())
                connection.close()
            finally:
                process.terminate()
                process.wait(timeout=5)


class TokenCLITests(unittest.TestCase):
    def test_invalid_explicit_token_never_falls_back_or_echoes_value(self):
        script = Path(__file__).resolve().parents[1] / "server.py"
        for token in ("", "short", "invalid token with spaces and secret"):
            result = subprocess.run([sys.executable, "-B", str(script), "--golden-token", token],
                                    env={**os.environ, "TUNTOM_FABRIC_TOKEN":"valid-environment-token-for-testing"},
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("at least 24 URL-safe characters", result.stderr)
            if token:
                self.assertNotIn(token, result.stderr)


if __name__ == "__main__":
    unittest.main()
