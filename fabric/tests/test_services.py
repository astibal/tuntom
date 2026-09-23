from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from services import Services, normalize_label


class ServicesTests(unittest.TestCase):
    def test_crud_normalization_and_optimistic_generation(self):
        store = Services(None)
        try:
            created = store.save({"name": "GitHub", "kind": "external", "description": "Development",
                "labels": ["0x2a", 43], "peek_targets": [{"url": "https://github.com", "interval": 60}]})
            self.assertEqual(created["labels"], ["42", "43"])
            self.assertEqual(created["generation"], 1)
            updated = store.save({"id": created["id"], "generation": created["generation"],
                "name": created["name"], "kind": created["kind"], "description": "Updated",
                "labels": created["labels"], "peek_targets": created["peek_targets"]})
            self.assertEqual(updated["generation"], 2)
            stale = {key: created[key] for key in ("id", "generation", "name", "kind", "description", "labels", "peek_targets")}
            with self.assertRaisesRegex(RuntimeError, "changed"):
                store.save(stale)
            store.delete(updated["id"], 2)
            self.assertEqual(store.list()["services"], [])
        finally:
            store.close()

    def test_label_has_exactly_one_owner(self):
        store = Services(None)
        try:
            store.save({"name": "One", "labels": ["42"]})
            with self.assertRaisesRegex(ValueError, "already belongs"):
                store.save({"name": "Two", "labels": ["0x2a"]})
            self.assertEqual(len(store.list()["services"]), 1)
        finally:
            store.close()

    def test_validation(self):
        self.assertEqual(normalize_label("0xffffffffffffffff"), "18446744073709551615")
        for value in (-1, "01", "0x10000000000000000", True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                normalize_label(value)
        store = Services(None)
        try:
            with self.assertRaisesRegex(ValueError, "HTTPS"):
                store.save({"name": "Bad", "peek_targets": [{"url": "http://example.com"}]})
        finally:
            store.close()


if __name__ == "__main__":
    unittest.main()
