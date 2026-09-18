"""Discovery fixtures, control framing, API boundaries and real switch lifecycle."""
from __future__ import annotations

from dataclasses import replace
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from control import CHUNK, ControlError, query
from discovery import discover, kind_of, options
from server import Fabric, Server, parse_stats, topology
from collector import CollectorServer, RemoteFabric

REPO = Path(__file__).resolve().parents[2]
TOKEN = "test-token-with-at-least-24-characters"


def until(predicate, timeout=7):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.04)
    raise AssertionError("condition did not become true")


class DiscoveryTests(unittest.TestCase):
    def test_known_names_and_no_grep_false_positives(self):
        cases = [
            ("/tmp/tuntom_42_1c", ["/tmp/tuntom_42_1c", "client", "42_1", "ut42_1c", "remote"], "tuntom_42_1c", "tunnel"),
            ("/var/lib/tuntom-deploy/instances/switch-test/bin/main", ["main", "--socket", "/tmp/data"], "tuntom-switch", "switch"),
            ("/var/lib/tuntom-deploy/instances/tunnel-test/bin/main", ["main", "server", "42", "ut42s"], "main", "tunnel"),
            ("/bin/python3", ["python3", "tuntom_monitor.py"], "python3", None),
            ("/bin/bash", ["bash", "-c", "grep tuntom_"], "bash", None),
            ("/tmp/tuntomctl", ["tuntomctl", "/tmp/data", "show", "stats"], "tuntomctl", None),
            ("/tmp/main", ["main", "a", "b", "--switch-socket", "/tmp/sw"], "tuntom-divert", "divert"),
        ]
        for executable, argv, comm, expected in cases:
            self.assertEqual(kind_of(executable, argv, comm), expected)

    def test_public_options_only(self):
        parsed = options(["tuntom-divert", "tun-in", "tun-out", "--cookie", "SECRET", "--secret=HIDDEN",
                          "--control-socket=/tmp/ctl", "--mtu", "9000", "--debug", "--password", "PRIVATE"])
        self.assertEqual(parsed, {"control-socket": "/tmp/ctl", "mtu": "9000", "debug": True})

    def test_proc_scan_relative_paths_and_pid_identity(self):
        with tempfile.TemporaryDirectory() as root:
            proc = Path(root)
            (proc / "sys/kernel/random").mkdir(parents=True)
            (proc / "sys/kernel/random/boot_id").write_text("fixture-boot")
            (proc / "uptime").write_text("10000.0 0.0")
            directory = proc / "123"
            directory.mkdir()
            (directory / "comm").write_text("tuntom-switch\n")
            (directory / "cmdline").write_bytes(b"/tmp/tuntom-switch\0--socket\0data.sock\0--control-socket\0control.sock\0")
            fields = ["S"] + ["0"] * 49
            fields[17], fields[19], fields[21] = "3", "100", "20"
            (directory / "stat").write_text("123 (name with ) spaces) " + " ".join(fields))
            (directory / "cwd").symlink_to("/tmp/test-cwd")
            (directory / "cgroup").write_text("0::/system.slice/tuntom-switch-test.service\n")
            endpoints, info = discover(proc)
            self.assertEqual(len(endpoints), 1)
            self.assertEqual(endpoints[0].control, "/tmp/test-cwd/control.sock")
            self.assertEqual(endpoints[0].id, "fixture-boot:123:100")
            self.assertEqual(endpoints[0].unit, "tuntom-switch-test.service")
            self.assertEqual(endpoints[0].threads, 3)
            (directory / "comm").write_text("tuntom\n")
            (directory / "cmdline").write_bytes(b"/tmp/tuntom\0server\0" b"232\0-\0--relay-connect\0data.sock\0--relay-port-id\0relay-core\0--control-socket\0control.sock\0")
            relay = discover(proc)[0][0]
            self.assertEqual(relay.switch_socket, "/tmp/test-cwd/data.sock")
            self.assertEqual(relay.port_id, "relay-core")
            fields[19] = "200"
            (directory / "stat").write_text("123 (restarted) " + " ".join(fields))
            self.assertNotEqual(discover(proc)[0][0].id, endpoints[0].id)

    def test_uint64_stats_and_malformed_input(self):
        stats = parse_stats("format=txt\nformat_version=1\nsession_tx_counter=18446744073709551615\n")
        self.assertEqual(stats["session_tx_counter"], "18446744073709551615")
        for invalid in ["format=txt\n", "format=txt\nformat_version=1\na=1\na=2\n", "garbage"]:
            with self.assertRaises(OSError):
                parse_stats(invalid)

    def test_named_port_stats_keys(self):
        key = "port_proxy-in.path@node_relay_ack_retry_packets"
        stats = parse_stats(f"format=txt\nformat_version=1\n{key}=18446744073709551615\nport_0_name=proxy-in.path@node\nport_0_ipc_rx_packets=42\n")
        self.assertEqual(stats[key], "18446744073709551615")
        self.assertEqual(stats["port_0_ipc_rx_packets"], "42")
        with self.assertRaisesRegex(OSError, "duplicate key at line 4"):
            parse_stats(f"format=txt\nformat_version=1\n{key}=1\n{key}=2\n")
        for key in ("bad key", "bad\tkey", "bad\x00key"):
            with self.assertRaisesRegex(OSError, "malformed key/value at line 3"):
                parse_stats(f"format=txt\nformat_version=1\n{key}=1\n")

    def test_topology_unknown_and_conflicting_namespaces(self):
        switch = {"id":"sw", "kind":"switch", "switch_socket":"/tmp/data", "mount_namespace":"mnt-a"}
        tunnel = {"id":"tn", "kind":"tunnel", "switch_socket":"/tmp/data", "mount_namespace":"", "port_id":"port"}
        self.assertFalse(topology([switch, tunnel])[0]["namespace_verified"])
        self.assertTrue(topology([switch, {**tunnel, "mount_namespace":"mnt-a"}])[0]["namespace_verified"])
        self.assertEqual(topology([switch, {**tunnel, "mount_namespace":"mnt-b"}]), [])
        self.assertEqual(topology([switch, {**switch, "id":"sw2"}, tunnel]), [])


class ControlTests(unittest.TestCase):
    def exchange(self, operation, records, body="", expected_pid=None):
        with tempfile.TemporaryDirectory(prefix="tt-control-") as root:
            path = str(Path(root) / "control")
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            listener.bind(path)
            listener.listen()
            listener.settimeout(2)
            errors = []

            def serve():
                try:
                    with listener.accept()[0] as client:
                        client.settimeout(2)
                        request = client.recv(CHUNK)
                        if not request:
                            return
                        expected = f"show {operation}" if operation in ("stats", "flows") else f"rules {operation} {len(body.encode())}"
                        self.assertEqual(request.decode(), expected)
                        remaining = len(body.encode())
                        while remaining:
                            data = client.recv(CHUNK)
                            if not data:
                                break
                            remaining -= len(data)
                        for record in records:
                            client.send(record)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except Exception as error:
                    errors.append(error)

            worker = threading.Thread(target=serve)
            worker.start()
            try:
                return query(path, operation, body, timeout=2, expected_pid=expected_pid)
            finally:
                worker.join(timeout=3)
                listener.close()
                if errors:
                    raise errors[0]

    def test_framing_chunks_errors_and_peer_identity(self):
        result = "a" * (CHUNK + 20)
        self.assertEqual(self.exchange("show", [f"OK {len(result)}\n".encode(), result[:CHUNK].encode(), result[CHUNK:].encode()]), result)
        with self.assertRaises(ControlError):
            self.exchange("check", [b"ERROR 3\n", b"bad"], "format 2\n")
        with self.assertRaises(OSError):
            self.exchange("show", [b"OK 2\n", b"too large"])
        with self.assertRaises(OSError):
            self.exchange("show", [b"OK 1048577\n"])
        with self.assertRaises(OSError):
            self.exchange("show", [b"OK 0\n"], expected_pid=os.getpid() + 100000)

    def test_flow_dump_large_response_and_rejections(self):
        result = "a" * (1024 * 1024 + 20)
        records = [f"OK {len(result)}\n".encode()]
        records += [result[i:i + CHUNK].encode() for i in range(0, len(result), CHUNK)]
        self.assertEqual(self.exchange("flows", records), result)
        with self.assertRaises(ControlError):
            self.exchange("flows", [b"ERROR 3\n", b"bad"])
        for records in ([b"OK 268435457\n"], [b"OK 5\n", b"abc"], [b"OK 2\n", b"abc"]):
            with self.assertRaises(OSError):
                self.exchange("flows", records)
        with self.assertRaises(ValueError):
            query("/unused", "flows", "unexpected")


class APITests(unittest.TestCase):
    def setUp(self):
        self.fabric = Fabric(discover_fn=lambda: ([], {"source": "procfs", "host": "fixture"}))
        self.server = Server(("127.0.0.1", 0), self.fabric, TOKEN)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()
        self.fabric.close()

    def request(self, method, path, data=None, *, headers=None, auth=True, address="127.0.0.1", port=None):
        connection = http.client.HTTPConnection(address, port or self.server.server_port, timeout=10)
        outgoing = {"Authorization": "Bearer " + TOKEN} if auth else {}
        if data is not None:
            outgoing["Content-Type"] = "application/json"
        outgoing.update(headers or {})
        connection.request(method, path, json.dumps(data) if data is not None else None, outgoing)
        response = connection.getresponse()
        raw = response.read()
        status, response_headers = response.status, dict(response.getheaders())
        connection.close()
        return status, json.loads(raw) if response_headers["Content-Type"].startswith("application/json") else raw

    def test_auth_origin_host_assets_and_unknown_routes(self):
        self.assertEqual(self.request("GET", "/api/v1/snapshot", auth=False)[0], 401)
        self.assertEqual(self.request("GET", "/api/v1/snapshot")[0], 200)
        self.assertEqual(self.request("GET", "/api/v1/snapshot", headers={"Host":"evil.example"})[0], 403)
        self.assertEqual(self.request("POST", "/api/v1/refresh", headers={"Origin":"https://evil.example"})[0], 403)
        self.assertEqual(self.request("GET", "/api/v1/missing")[0], 404)
        self.assertEqual(self.request("GET", "/../server.py")[0], 404)
        for path in ("/", "/app.js", "/style.css", "/favicon.svg"):
            self.assertEqual(self.request("GET", path, auth=False)[0], 200)

    def test_wildcard_bind_accepts_destination_ip_and_keeps_request_guards(self):
        server = Server(("0.0.0.0", 0), self.fabric, TOKEN)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            # A second loopback address exercises wildcard binding without
            # depending on the machine's LAN configuration.
            destination = {"address":"127.0.0.2", "port":server.server_port}
            origin = f"http://127.0.0.2:{server.server_port}"
            self.assertEqual(self.request("GET", "/", auth=False, **destination)[0], 200)
            self.assertEqual(self.request("GET", "/api/v1/snapshot", auth=False, **destination)[0], 401)
            self.assertEqual(self.request("GET", "/api/v1/snapshot", headers={"Origin":origin}, **destination)[0], 200)
            for headers in ({"Host":"evil.example"}, {"Host":f"127.0.0.3:{server.server_port}"},
                            {"Origin":"http://evil.example"}, {"Sec-Fetch-Site":"cross-site"}):
                self.assertEqual(self.request("GET", "/api/v1/snapshot", headers=headers, **destination)[0], 403)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    def test_real_switch_lifecycle_and_rules(self):
        binaries = [REPO / "cmake-build-debug" / name for name in ("tuntom-switch", "tomtom-switch-mp")]
        if not all(p.is_file() for p in binaries):
            self.skipTest("build tuntom-switch and tomtom-switch-mp in cmake-build-debug for integration tests")
        self.fabric.discover = discover
        self.fabric.interval = .1
        self.fabric.start()
        for binary in binaries:
            with self.subTest(binary=binary.name), tempfile.TemporaryDirectory(prefix="tt-fabric-") as root:
                self.fabric.allow_write = False
                root = Path(root)
                rules = root / "rules"
                rules.write_text("format 2\nserial 1\nswitch a, [1] to b, [2] allow\n")
                args = [str(binary), "--socket", str(root / "data"), "--control-socket", str(root / "control"), "--rules-file", str(rules)]
                if "mp" in binary.name:
                    args += ["--workers", "2", "--pool-size", "16", "--queue-size", "16"]
                with (root / "log").open("w+") as log:
                    process = subprocess.Popen(args, stdout=log, stderr=log)
                    peers = []
                    collector = CollectorServer(str(root / "reader"), self.fabric, os.getuid())
                    collector_thread = threading.Thread(target=collector.serve_forever, daemon=True)
                    collector_thread.start()
                    remote = RemoteFabric(str(root / "reader"), allow_write=True)
                    # Exercise the real HTTP → Unix collector → daemon path.
                    self.server.fabric = remote
                    try:
                        def found():
                            if process.poll() is not None:
                                log.seek(0)
                                raise AssertionError(log.read())
                            return next((e for e in self.fabric.snapshot()["endpoints"] if e["pid"] == process.pid and e["status"] == "reachable"), None)
                        endpoint = until(found)
                        self.assertEqual(endpoint["metrics"]["component"], "switch")
                        for name in (b"a", b"b"):
                            peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                            peer.settimeout(3)
                            peer.connect(str(root / "data"))
                            peer.sendall(b"TTP\x01" + bytes([len(name), 0, 0, 0]) + name)
                            peers.append(peer)
                        until(lambda: found()["metrics"].get("connections_current") == "2")
                        peers[0].sendall(struct.pack("!BBBBIQ", 1, 1, 0, 1, 20, 1) + b"test")
                        self.assertEqual(peers[1].recv(1024)[-4:], b"test")
                        if "mp" in binary.name:
                            until(lambda: len(found()["switch_detail"]["ports"]) == 2)
                            live = found()
                            self.assertEqual({p["name"] for p in live["switch_detail"]["ports"]}, {"a", "b"})
                            self.assertEqual(len(live["switch_detail"]["workers"]), 2)
                        report_path = "/api/v1/endpoints/" + endpoint["id"] + "/diagnostics"
                        code, report = self.request("GET", report_path)
                        self.assertEqual(code, 200, report)
                        self.assertEqual(report["endpoint"]["pid"], process.pid)
                        self.assertEqual(report["logs"]["status"], "ok")
                        self.assertIn("active_rules", report)
                        path = "/api/v1/endpoints/" + endpoint["id"] + "/rules"
                        code, active = self.request("GET", path)
                        self.assertEqual(code, 200, active)
                        candidate = "format 2\nserial 2\nswitch a, [1] to b, [3] allow\n"
                        payload = {"rules": candidate, "expected_sha256":active["sha256"]}
                        code, checked = self.request("POST", path + "/check", payload)
                        self.assertEqual(code, 200, checked)
                        self.assertIn("+serial 2", checked["diff"])
                        self.assertEqual(self.request("POST", path + "/load", payload)[0], 403)
                        self.fabric.allow_write = True
                        self.assertEqual(self.request("POST", path + "/load", {**payload, "expected_sha256":"stale"})[0], 409)
                        code, result = self.request("POST", path + "/load", payload)
                        self.assertEqual(code, 200, result)
                        self.assertIn("applied serial=2", result["result"])
                        self.assertEqual(self.request("POST", path + "/load", payload)[0], 409)
                        self.assertEqual(self.request("POST", path + "/check", {"rules":"format 2\nserial 3\nnonsense\n"})[0], 422)
                        self.assertIn("serial 2", self.request("GET", path)[1]["rules"])
                        # An already-open socket cannot silently target a different PID.
                        live = self.fabric.endpoint(endpoint["id"])
                        with self.assertRaises(OSError):
                            self.fabric.control_query(replace(live, start_ticks=live.start_ticks + 1), "stats")
                    finally:
                        self.server.fabric = self.fabric
                        collector.shutdown()
                        collector.server_close()
                        collector_thread.join()
                        for peer in peers:
                            peer.close()
                        process.terminate()
                        process.wait(timeout=5)
                    until(lambda: not any(e["pid"] == process.pid for e in self.fabric.snapshot()["endpoints"]))
                    self.assertEqual(self.request("GET", path)[0], 404)


if __name__ == "__main__":
    unittest.main(verbosity=2)
