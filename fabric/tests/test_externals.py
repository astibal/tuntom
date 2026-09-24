from pathlib import Path
import json
import hashlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import externals


class Reply:
    def __init__(self, value):
        self.value = json.dumps(value).encode()
    def __enter__(self):
        return self
    def __exit__(self, *_):
        pass
    def read(self, _limit):
        return self.value


class ExternalsTests(unittest.TestCase):
    def test_empty_targets_do_not_require_peek(self):
        self.assertEqual(externals.observe([{"id":"s", "name":"S", "kind":"external", "peek_targets":[]}], None, None)["observations"], [])

    def test_observations_are_enriched_with_service_context(self):
        services = [{"id":"abc", "name":"GitHub", "kind":"external", "labels":["42"],
                     "peek_targets":[{"url":"https://github.com", "interval":120}]}]
        target_id="abc:"+hashlib.sha256(b"https://github.com").hexdigest()[:16]
        result = {"observed_at":"2026-01-01T00:00:00+00:00", "results":[{
            "id":target_id, "url":"https://github.com", "ok":True, "available":True,
        }]}
        with mock.patch("externals.urlopen", return_value=Reply(result)) as call:
            value = externals.observe(services, "http://peek:8780", "secret")
        self.assertEqual(value["sampled_at"], result["observed_at"])
        self.assertEqual(value["observations"][0]["service_name"], "GitHub")
        self.assertEqual(value["observations"][0]["service_labels"], ["42"])
        self.assertEqual(value["observations"][0]["interval"], 120)
        request = call.call_args.args[0]
        self.assertEqual(request.get_header("Authorization"), "Bearer secret")

    def test_config_is_required_when_targets_exist(self):
        services = [{"id":"abc", "name":"GitHub", "kind":"external",
                     "peek_targets":[{"url":"https://github.com", "interval":60}]}]
        with self.assertRaisesRegex(externals.PeekError, "nakonfigurovaný"):
            externals.observe(services, None, None)


if __name__ == "__main__":
    unittest.main()
