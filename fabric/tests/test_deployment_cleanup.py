from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from deployment_cleanup import erase_secret, undeploy, matches_process


class CleanupTests(unittest.TestCase):
    def test_only_marked_deployment_is_removed_and_key_is_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); root = base / 'owned'; root.mkdir()
            (root / '.fabric-deployment-id').write_text('abc')
            (root / 'secrets').mkdir(); key = root / 'secrets/master.env'; key.write_text('secret-value')
            unrelated = base / 'other'; unrelated.mkdir(); (unrelated / 'key').write_text('retain')
            original = Path.iterdir
            with key.open('rb') as handle, patch.object(Path, 'iterdir', lambda p: iter([]) if p == Path('/proc') else original(p)):
                undeploy(root, 'abc', 'client', ['1'])
                handle.seek(0); self.assertEqual(handle.read(), b'')
            self.assertFalse(root.exists()); self.assertEqual((unrelated / 'key').read_text(), 'retain')
            undeploy(root, 'abc', 'client', ['1'])  # Idempotent after complete removal.

    def test_wrong_marker_and_symlink_are_never_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); root = base / 'owned'; root.mkdir()
            (root / '.fabric-deployment-id').write_text('other')
            with self.assertRaisesRegex(RuntimeError, 'identity'): undeploy(root, 'abc', 'server', ['1'])
            link = base / 'link'; link.symlink_to(root)
            with self.assertRaises(RuntimeError): undeploy(link, 'abc', 'server', ['1'])
            self.assertTrue(root.exists())

    def test_secret_symlink_is_not_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            original = Path(directory) / 'key'; original.write_text('keep')
            link = Path(directory) / 'link'; link.symlink_to(original)
            with self.assertRaises(OSError): erase_secret(link)
            self.assertEqual(original.read_text(), 'keep')

    def test_process_match_requires_exact_binary_role_member_and_socket(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = Path(directory); root = Path('/opt/tuntom/deployments/example')
            (proc / 'exe').symlink_to(root / 'bin/tuntom')
            args = ['tuntom','client','1','ut1c','192.0.2.1','--control-socket',str(root/'run/1c.control')]
            (proc / 'cmdline').write_bytes(('\0'.join(args)+'\0').encode())
            self.assertTrue(matches_process(proc,root,'client',['1']))
            self.assertFalse(matches_process(proc,root,'server',['1']))
            self.assertFalse(matches_process(proc,root,'client',['2']))
            args[-1]='/other/control';(proc/'cmdline').write_bytes('\0'.join(args).encode())
            self.assertFalse(matches_process(proc,root,'client',['1']))
