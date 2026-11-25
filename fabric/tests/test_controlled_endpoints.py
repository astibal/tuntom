from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from controlled_endpoints import ControlledEndpoints, SSHDiscovery, validate_target


class FakeDiscovery:
    key = {"bits": 256, "fingerprint": "SHA256:test", "type": "ED25519",
           "known_host": "192.0.2.10 ssh-ed25519 AAAATEST"}

    def scan(self, address, port):
        return [self.key]

    def discover(self, address, port, known_hosts):
        assert known_hosts == self.key["known_host"] + "\n"
        return {"system": {"os": "ubuntu", "distribution": "Ubuntu", "version": "26.04",
                           "architecture": "x86_64"}, "supported": True, "support_reason": "supported platform"}


class ControlledEndpointsTests(unittest.TestCase):
    def test_discovery_platform_matrix(self):
        for version, architecture, supported in (
                ('24.04', 'x86_64', True), ('26.04', 'x86_64', True),
                ('22.04', 'x86_64', False), ('24.04', 'aarch64', False)):
            with self.subTest(version=version, architecture=architecture):
                output = f'ID=ubuntu\nNAME=Ubuntu\nVERSION_ID={version}\n__TUNTOM_ARCH__{architecture}\n'
                result = SimpleNamespace(returncode=0, stdout=output, stderr='')
                with patch('controlled_endpoints.subprocess.run', return_value=result):
                    snapshot = SSHDiscovery().discover('192.0.2.10', 22, 'test-key\n')
                self.assertEqual(snapshot['supported'], supported)
                self.assertEqual(snapshot['system']['version'], version)

    def test_host_key_confirmation_then_snapshot(self):
        store = ControlledEndpoints(None, FakeDiscovery())
        try:
            pending = store.probe({"address": "192.0.2.10", "port": 2222})
            self.assertEqual(pending["status"], "awaiting_host_key")
            self.assertNotIn("known_host", pending["host_keys"][0])
            with self.assertRaisesRegex(ValueError, "does not match"):
                store.confirm(pending["id"], "SHA256:wrong")
            ready = store.confirm(pending["id"], "SHA256:test")
            self.assertEqual(ready["status"], "supported")
            self.assertEqual(ready["snapshot"]["system"]["version"], "26.04")
            refreshed = store.refresh(pending["id"])
            self.assertEqual(refreshed["status"], "supported")
        finally:
            store.close()

    def test_metadata_survives_discovery_and_reopen(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'endpoints.db'
            store = ControlledEndpoints(path, FakeDiscovery())
            try:
                row = store.probe({"address": "192.0.2.10", "port": 2222})
                endpoint_id = row['id']
                store.update_metadata(endpoint_id, {'name': ' Proxy 1 ', 'description': 'Lab\nOwner: ops'})
                store.confirm(endpoint_id, 'SHA256:test')
                store.refresh(endpoint_id)
            finally:
                store.close()
            store = ControlledEndpoints(path, FakeDiscovery())
            try:
                row = store.list()['controlled_endpoints'][0]
                self.assertEqual(row['name'], 'Proxy 1')
                self.assertEqual(row['description'], 'Lab\nOwner: ops')
                self.assertEqual(row['address'], '192.0.2.10')
                self.assertEqual(row['status'], 'supported')
                for data in ({'name': 'x'}, {'name': 'x' * 121, 'description': ''},
                             {'name': 'bad\nname', 'description': ''},
                             {'name': '', 'description': 'x' * 2001}):
                    with self.subTest(data=data), self.assertRaises(ValueError):
                        store.update_metadata(endpoint_id, data)
                with self.assertRaises(KeyError):
                    store.update_metadata('0' * 32, {'name': '', 'description': ''})
                row = store.update_metadata(endpoint_id, {'name': '', 'description': ''})
                self.assertEqual(row['name'], '')
            finally:
                store.close()

    def test_target_validation_is_ip_only(self):
        self.assertEqual(validate_target({"address": "2001:db8::1", "port": 22}), ("2001:db8::1", 22))
        for value in ({"address": "example.com"}, {"address": "127.0.0.1", "port": 0}):
            with self.subTest(value=value), self.assertRaises(ValueError): validate_target(value)


if __name__ == "__main__":
    unittest.main()
