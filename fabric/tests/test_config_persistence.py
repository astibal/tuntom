"""Durability selection must reach the daemon and must never silently downgrade."""
import unittest
import test_classifier
RULES = test_classifier.RULES
from server import digest
from errors import APIError
from control import ControlError

class PersistenceTests(unittest.TestCase):
    make = test_classifier.ClassifierTests.make

    def test_default_saved_and_explicit_temporary(self):
        fabric, calls = self.make()
        for persist in (None, True, False):
            body = {'rules': RULES, 'expected_sha256': digest(RULES), 'expected_generation': '2'}

            if persist is not None: body['persist'] = persist
            result = fabric.classifier('id', 'load', body)
            self.assertEqual(calls[-1][0], 'classifier-load' if persist is False else 'classifier-load-save')
            self.assertEqual(result['persistence'], 'runtime only' if persist is False else 'saved on target')

    def test_failed_save_is_not_retried_as_runtime(self):
        fabric, calls = self.make()
        original = fabric.control_query
        def query(endpoint, operation, body=''):

            if operation.endswith('-save'):
                calls.append((operation, body))
                raise ControlError('state directory is read-only')

            return original(endpoint, operation, body)

        fabric.control_query = query
        with self.assertRaises(APIError):
            fabric.classifier('id', 'load', {'rules': RULES, 'expected_sha256': digest(RULES), 'expected_generation': '2'})

        self.assertEqual([op for op, _ in calls if op.startswith('classifier-load')], ['classifier-load-save'])

    def test_persist_type_is_strict(self):
        fabric, calls = self.make()
        for value in ('false', 0, None, []):
            with self.assertRaises(APIError):
                fabric.classifier('id', 'load', {'persist': value})

        self.assertEqual(calls, [])
