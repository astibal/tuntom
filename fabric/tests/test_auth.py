import hashlib
import hmac
import json
from pathlib import Path
import tempfile
import time
import unittest

from auth import AuthManager, UserStore, b64, password_record, unb64


def login(manager, username, password, *, bootstrap=False):
    nonce = b64(b"c" * 24)
    challenge = manager.challenge(username, nonce, bootstrap)
    identity = "bootstrap" if bootstrap else username
    message = manager._message(identity, nonce, challenge["nonce"], challenge["salt"], challenge["iterations"])
    salted = hashlib.pbkdf2_hmac("sha256", password.encode(), unb64(challenge["salt"]), challenge["iterations"])
    client_key = hmac.digest(salted, b"Client Key", "sha256")
    signature = hmac.digest(hashlib.sha256(client_key).digest(), message, "sha256")
    proof = b64(bytes(a ^ b for a, b in zip(client_key, signature)))
    result = manager.login(challenge["challenge_id"], proof)
    key = hmac.digest(hmac.digest(salted, b"Server Key", "sha256"), b"Session Key\0" + message, "sha256")
    return result, key


class AuthTests(unittest.TestCase):
    def test_edit_preserves_password_and_requires_it_for_creation(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "users.json"
            store = UserStore(path)
            record = password_record("long-reader-password")
            store.save_user("reader", "admin-ro", True, record, "bootstrap")
            store.save_user("reader", "admin-ro", False, None, "bootstrap")
            restored = UserStore(path)
            self.assertEqual(restored.users["reader"]["password"], record)
            self.assertFalse(restored.users["reader"]["enabled"])
            with self.assertRaisesRegex(ValueError, "password is required"):
                store.save_user("new", "admin-ro", True, None, "bootstrap")
            store.save_user("root", "admin", True, record, "bootstrap")
            with self.assertRaisesRegex(ValueError, "last enabled admin"):
                store.save_user("root", "admin-ro", True, None, "bootstrap")

    def test_bootstrap_and_user_login_signed_request_and_replay(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "users.json"
            manager = AuthManager("bootstrap-token", path)
            bootstrap, _ = login(manager, "bootstrap", "bootstrap-token", bootstrap=True)
            manager.users.save_user("reader", "admin-ro", True, password_record("long-reader-password"), "bootstrap")
            result, key = login(manager, "reader", "long-reader-password")
            self.assertEqual(result["role"], "admin-ro")
            stamp, nonce, body = str(int(time.time())), b64(b"n" * 18), b""
            canonical = f"GET\n/api/v1/snapshot\n{stamp}\n{nonce}\n{hashlib.sha256(body).hexdigest()}".encode()
            signature = b64(hmac.digest(key, canonical, "sha256"))
            self.assertEqual(manager.verify(result["session_id"], stamp, nonce, signature,
                                            "GET", "/api/v1/snapshot", body)["username"], "reader")
            with self.assertRaises(PermissionError):
                manager.verify(result["session_id"], stamp, nonce, signature, "GET", "/api/v1/snapshot", body)
            manager.logout(result["session_id"])
            with self.assertRaises(PermissionError):
                manager.verify(result["session_id"], stamp, b64(b"x" * 18), signature,
                               "GET", "/api/v1/snapshot", body)
            result, _ = login(manager, "reader", "long-reader-password")
            manager.revoke_user("reader")
            self.assertNotIn(result["session_id"], manager.sessions)
            self.assertIn(bootstrap["session_id"], manager.sessions)

    def test_file_is_private_and_last_admin_is_protected(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "users.json"
            store = UserStore(path)
            store.save_user("root", "admin", True, password_record("a-secure-password"), "bootstrap")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertNotIn("a-secure-password", path.read_text())
            with self.assertRaises(ValueError):
                store.delete("root")
            data = json.loads(path.read_text())
            self.assertEqual(data["generation"], 1)

    def test_wrong_password_and_disabled_user_are_indistinguishable(self):
        with tempfile.TemporaryDirectory() as root:
            manager = AuthManager("bootstrap-token", Path(root) / "users.json")
            manager.users.save_user("off", "admin-ro", False, password_record("long-reader-password"), "bootstrap")
            for username, password in (("off", "long-reader-password"), ("missing", "something-long")):
                with self.subTest(username=username), self.assertRaisesRegex(PermissionError, "invalid username or password"):
                    login(manager, username, password)


if __name__ == "__main__":
    unittest.main()
