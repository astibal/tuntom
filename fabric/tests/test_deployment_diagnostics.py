import subprocess
import unittest
from unittest.mock import patch
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from controlled_endpoints import SSHDiscovery
from deployment_diagnostics import safe_output
from tunnel_deployments import TunnelDeployments
from test_tunnel_deployments import Endpoints, draft


class DeploymentDiagnosticsTests(unittest.TestCase):
    def test_stdout_failure_and_exit_code_are_reported(self):
        result = subprocess.CompletedProcess([], 22, b'CONTROL public key is missing\n', b'')
        with patch('controlled_endpoints.subprocess.run', return_value=result):
            with self.assertRaisesRegex(OSError, r'exit 22.*\nCONTROL public key is missing'):
                SSHDiscovery().command('192.0.2.1', 22, 'test', 'preflight')

    def test_sensitive_output_is_redacted_and_bounded(self):
        secret = 'ab' * 16
        raw = 'x' * 5000 + '\nTUNTOM_SECRET=' + secret + '\nBearer private-value\n' + secret
        result = safe_output(raw)
        self.assertLessEqual(len(result), 4096)
        self.assertNotIn(secret, result)
        self.assertNotIn('private-value', result)
        self.assertNotIn('private contents', safe_output('-----BEGIN OPENSSH PRIVATE KEY-----\nprivate contents\n-----END OPENSSH PRIVATE KEY-----'))

    def test_successful_command_output_is_not_truncated_for_callers(self):
        raw = b'x' * 5000
        with patch('controlled_endpoints.subprocess.run', return_value=subprocess.CompletedProcess([], 0, raw, b'')):
            self.assertEqual(SSHDiscovery().command('192.0.2.1', 22, 'test', 'read'), raw.decode())

    def test_failed_step_is_retained_with_previous_successes(self):
        class FailingEndpoints(Endpoints):
            def run(self, ident, command, input_data=None, timeout=600):
                if command.endswith('/00-preflight.sh'):
                    raise OSError('exit 22: CONTROL public key is missing')
                return 'UPLOAD_OK\n'
        store = TunnelDeployments(None, FailingEndpoints()); self.addCleanup(store.close)
        row = store.create(draft())
        with self.assertRaisesRegex(RuntimeError, 'side_a / 00-preflight.sh'):
            store.deploy(row['id'])
        failed = store.get(row['id'])
        state = failed['side_states']['side_a']
        self.assertEqual(state['step'], '00-preflight.sh')
        self.assertEqual(state['state'], 'failed')
        self.assertIn('public key', state['error'])
        self.assertEqual([(v['step'],v['state']) for v in state['history']],
                         [('upload','working'),('upload','succeeded'),('00-preflight.sh','working'),('00-preflight.sh','failed')])
