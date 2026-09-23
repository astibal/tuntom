import http.client
import json
from pathlib import Path
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import peek


class PeekTests(unittest.TestCase):
    def test_rejects_non_https_and_private_destinations(self):
        result = peek.probe({"id": "plain", "url": "http://example.com"})
        self.assertEqual(result["error"]["kind"], "invalid_target")
        with mock.patch("socket.getaddrinfo", return_value=[
                (peek.socket.AF_INET, peek.socket.SOCK_STREAM, 6, "", ("127.0.0.1", 443))]):
            result = peek.probe({"id": "local", "url": "https://localhost"})
        self.assertEqual(result["error"]["kind"], "address_not_public")

    def test_api_auth_shape_order_and_health(self):
        token = "a-secure-test-token-123456789"
        server = peek.PeekServer(("127.0.0.1", 0), token, 2, 1)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            connection.request("GET", "/healthz")
            self.assertEqual(connection.getresponse().status, 200)
            connection.close()

            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            connection.request("POST", "/v1/probe", b'{"targets":[]}', {"Content-Type": "application/json"})
            self.assertEqual(connection.getresponse().status, 401)
            connection.close()

            def fake(target, timeout):
                return {"id": target["id"], "ok": True}
            body = json.dumps({"targets": [{"id": "one"}, {"id": "two"}]})
            with mock.patch("peek.probe", side_effect=fake):
                connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
                connection.request("POST", "/v1/probe", body, {
                    "Authorization": "Bearer " + token, "Content-Type": "application/json"})
                response = connection.getresponse()
                payload = json.loads(response.read())
                self.assertEqual(response.status, 200)
                self.assertEqual([item["id"] for item in payload["results"]], ["one", "two"])
                connection.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    unittest.main()
