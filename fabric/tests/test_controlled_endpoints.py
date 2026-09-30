from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from controlled_endpoints import ControlledEndpoints, validate_target


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

    def test_target_validation_is_ip_only(self):
        self.assertEqual(validate_target({"address": "2001:db8::1", "port": 22}), ("2001:db8::1", 22))
        for value in ({"address": "example.com"}, {"address": "127.0.0.1", "port": 0}):
            with self.subTest(value=value), self.assertRaises(ValueError): validate_target(value)


if __name__ == "__main__":
    unittest.main()
