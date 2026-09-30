from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import switch_deployments as local
from test_tunnel_deployments import switch_draft, SWITCH


class SwitchDeploymentTests(unittest.TestCase):
    def test_rejects_remote_wrong_kind_and_namespaces(self):
        endpoint = SimpleNamespace(source='discovered', kind='switch', switch_socket='/run/core.sock')
        fabric = SimpleNamespace(endpoint=lambda _: endpoint, snapshot=lambda: {"endpoints": []})
        with self.assertRaisesRegex(ValueError, 'local observed'): local.inspect_switch(fabric, SWITCH)
        endpoint.source = 'local'; endpoint.kind = 'tunnel'
        with self.assertRaises(ValueError): local.inspect_switch(fabric, SWITCH)
        endpoint.kind = 'switch'; endpoint.mount_namespace = 'different'
        with self.assertRaisesRegex(ValueError, 'namespaces'): local.inspect_switch(fabric, SWITCH)

    def test_no_command_or_bundle_rpc(self):
        for body in ({'action': 'shell', 'command': 'id'}, {'action': 'inspect', 'switch_id': SWITCH, 'path': '/tmp'},
                     {'action': 'prepare', 'bundle': {}, 'command': 'id'}):
            with self.assertRaises(ValueError): local.execute(None, body)

    def test_generated_local_lifecycle_and_identity_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory) / 'deployments'
            resolved = {'name': 'core', 'switch_socket': '/run/core.sock',
                        'binaries': {'tuntom': '/opt/bin/tuntom', 'tuntomctl': '/opt/bin/tuntomctl'}}
            body = {'action': 'prepare', 'intent': switch_draft(), 'deployment_id': 'd' * 32, 'secret': 'ab' * 16}
            with patch.object(local, 'BASE', base), patch.object(local, 'trusted', side_effect=Path), \
                 patch.object(local, 'inspect_switch', return_value=resolved), patch.object(local.os, 'geteuid', return_value=0), \
                 patch.object(local, 'run_script') as run:
                local.execute(None, body)
                root = base / body['intent']['name']
                self.assertEqual((root / 'secrets/master.env').stat().st_mode & 0o777, 0o600)
                self.assertNotIn('abababab', (root / '.fabric-intent').read_text())
                self.assertIn('--switch-socket /run/core.sock', (root / 'runtime/run-231s.sh').read_text())
                self.assertEqual([call.args[1] for call in run.call_args_list],
                                 ['00-preflight.sh', '10-install-dependencies.sh', '20-install-software.sh', '30-install-config.sh'])
                body.pop('secret'); body['action'] = 'start'; local.execute(None, body)
                self.assertEqual(run.call_args.args[1], '50-verify.sh')
                body['action'] = 'rollback'; local.execute(None, body)
                self.assertEqual(run.call_args.args[1], '99-rollback.sh')
                body['deployment_id'] = 'e' * 32
                with self.assertRaisesRegex(ValueError, 'identity'): local.execute(None, body)

    def test_privileged_paths_reject_symlinks_and_writable_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'binary'; path.write_text('')
            link = Path(directory) / 'link'; link.symlink_to(path)
            with self.assertRaises(ValueError): local.trusted(link)
            path.chmod(0o777)
            with self.assertRaises(ValueError): local.trusted(path)

    def test_mutations_require_admin_and_write_permission(self):
        from server import Fabric
        from errors import APIError
        from journal import actor
        for role, enabled in (("admin-ro", True), ("internal", True), ("admin", False)):
            token = actor.set({"username": "tester", "role": role})
            try:
                with patch.object(local, "execute") as execute:
                    with self.assertRaises(APIError) as caught:
                        Fabric.switch_deployment(SimpleNamespace(allow_write=enabled), {"action": "start"})
                    self.assertEqual(caught.exception.status, 403)
                    execute.assert_not_called()
            finally: actor.reset(token)

    def test_protected_binaries_precede_untrusted_lab_paths(self):
        endpoint = SimpleNamespace(source="local", kind="switch", switch_socket="/run/core.sock",
            executable="/opt/lab/build/tuntom-switch", mount_namespace=os.readlink("/proc/self/ns/mnt"),
            net_namespace=os.readlink("/proc/self/ns/net"), name="core")
        fabric = SimpleNamespace(endpoint=lambda _: endpoint, snapshot=lambda: {"endpoints": []})
        with tempfile.TemporaryDirectory() as directory:
            binaries = Path(directory)
            for name in ("tuntom", "tuntomctl"):
                path = binaries / name; path.write_text(""); path.chmod(0o755)
            with patch.object(local, "BINARIES", binaries), patch.object(local, "trusted", side_effect=Path) as trust, \
                 patch.object(local.os, "stat", return_value=SimpleNamespace(st_mode=0o140600)), \
                 patch.object(Path, "is_file", return_value=True):
                result = local.inspect_switch(fabric, "switch-id")
            self.assertEqual(result["binaries"]["tuntom"], str(binaries / "tuntom"))
            self.assertEqual([call.args[0] for call in trust.call_args_list], [binaries / "tuntom", binaries / "tuntomctl"])

    def test_transport_ip_uses_kernel_route_without_sending_probe(self):
        from unittest.mock import MagicMock
        connection = MagicMock(); connection.getsockname.return_value = ("192.0.2.1", 50000)
        with patch.object(local, "inspect_switch", return_value={}), patch.object(local.socket, "socket") as factory:
            factory.return_value.__enter__.return_value = connection
            result = local.execute(None, {"action": "inspect", "switch_id": SWITCH, "route_to": "192.0.2.5"})
            self.assertEqual(result["transport_ip"], "192.0.2.1")
            connection.connect.assert_called_once_with(("192.0.2.5", 9))
            connection.send.assert_not_called(); connection.sendto.assert_not_called()
