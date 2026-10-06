from pathlib import Path
import os
import socket
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adapter_access import check


class AdapterAccessTests(unittest.TestCase):
    def test_checks_socket_permissions_without_connecting(self):
        cwd = os.getcwd()
        self.addCleanup(os.chdir, cwd)
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'relay.sock')
            with socket.socket(socket.AF_UNIX) as listener:
                listener.bind(path)
                listener.listen()
                listener.setblocking(False)
                check(directory, [path])
                with self.assertRaises(BlockingIOError):
                    listener.accept()
                with patch('adapter_access.os.access', side_effect=lambda p, mode: p == '.'):
                    with self.assertRaisesRegex(PermissionError, 'Relay socket'):
                        check(directory, [path])
                with patch('adapter_access.os.access', return_value=False):
                    with self.assertRaisesRegex(PermissionError, 'Runtime directory'):
                        check(directory, [path])
            os.chdir(cwd)

    def test_missing_socket_and_regular_file_are_rejected(self):
        cwd = os.getcwd()
        self.addCleanup(os.chdir, cwd)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'relay.sock'
            with self.assertRaises(FileNotFoundError):
                check(directory, [str(path)])
            path.touch()
            with self.assertRaisesRegex(OSError, 'not a socket'):
                check(directory, [str(path)])
            os.chdir(cwd)
