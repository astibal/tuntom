from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tunnel_deployments import TunnelDeployments, validate, check_switch_ports
from deployment_cleanup import matches_process
from test_tunnel_deployments import Endpoints, draft, LocalFabric, SWITCH


def via_draft(mode='split'):
    value = draft()
    value['side_a'], value['side_b'] = value['side_b'], value['side_a']
    value['count'] = 8
    value['via'] = {'mode': mode, 'service': 'smithproxy', 'instance': 'smithproxy#0',
        'in_port': 'proxy-in', 'out_port': 'proxy-out', 'in_tun': 'di0', 'out_tun': 'do0',
        'namespace': '', 'trust_key': '/opt/tuntom/etc/authority.pub', 'trust_public': 'x25519 ' + '12' * 32 + ' 1 0\n'}
    return value


class ViaDeploymentTests(unittest.TestCase):
    def test_split_runbook_owns_eight_workers_and_two_relay_groups(self):
        store = TunnelDeployments(None, Endpoints())
        self.addCleanup(store.close)
        row = store.create(via_draft())
        bundle = store.get(row['id'], True)['bundle']
        self.assertEqual(len([k for k in bundle if '/runtime/run-via-' in k]), 8)
        for index in range(8):
            member = '231' + (f'_{index}' if index else '')
            relay = bundle[f'side_a/runtime/run-{member}c.sh']
            self.assertIn(f'client {member} - ', relay)
            self.assertIn('--relay-listen', relay)
            self.assertNotIn('--switch-label', relay)
            hub = bundle[f'side_b/runtime/run-{member}s.sh']
            self.assertIn('--relay-connect', hub)
            self.assertIn(f'--relay-port-id edge-{"in" if index < 4 else "out"}_{index % 4}', hub)
            worker = bundle[f'side_a/runtime/run-via-{index}.sh']
            self.assertIn('runuser -u tuntom -g tuntom -- python3 -', worker)
            self.assertIn('adapter-access.py', worker)
            self.assertIn('--side ' + ('in' if index < 4 else 'out'), worker)
            self.assertIn('--shared-flows', worker)
            self.assertIn('--allow-control-trusted', worker)
        for name, content in bundle.items():
            if name.endswith('.sh'):
                result = subprocess.run(['bash', '-n'], input=content, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, name + result.stderr)
        self.assertIn('client-relay edge-in_*', bundle['via-service.rules.txt'])
        self.assertIn('tuntom-divert-adapter', bundle['side_a/scripts/95-undeploy.sh'])
        self.assertIn('src/divert/main.cpp', bundle['side_a/scripts/20-install-software.sh'])

    def test_paired_worker_has_all_paths_and_no_split_state(self):
        store = TunnelDeployments(None, Endpoints()); self.addCleanup(store.close)
        row = store.create(via_draft('paired'))
        bundle = store.get(row['id'], True)['bundle']
        worker = bundle['side_a/runtime/run-via-0.sh']
        self.assertEqual(worker.count('--relay-path '), 8)
        self.assertNotIn('--shared-flows', worker)
        self.assertNotIn('side_a/runtime/run-via-1.sh', bundle)

    def test_invalid_shape_and_limits(self):
        for key, value in [('count', 3), ('count', 18)]:
            data = via_draft(); data[key] = value
            with self.assertRaises(ValueError): validate(data)
        data = via_draft(); data['software'] = {'source': 'binary', 'sha256': '0' * 64}
        with self.assertRaisesRegex(ValueError, 'Git build'): validate(data)
        data = via_draft(); data['via']['out_tun'] = 'di0'
        with self.assertRaisesRegex(ValueError, 'distinct'): validate(data)
        data = via_draft(); data['via']['instance'] = '$(id)'
        with self.assertRaises(ValueError): validate(data)
        data = via_draft(); data['via']['in_port'] = 'a' * 32; data['via']['instance'] = 'b' * 32
        with self.assertRaisesRegex(ValueError, '63-byte'): validate(data)

    def test_collision_uses_split_physical_names(self):
        config, _ = validate(via_draft())
        with self.assertRaisesRegex(ValueError, 'edge-out_3'):
            check_switch_ports(config['side_b'], 8, ['edge-out_3'], config['via'])

    def test_local_collector_receives_validated_via_intent(self):
        fabric = LocalFabric(); store = TunnelDeployments(None, Endpoints(), fabric=fabric)
        self.addCleanup(store.close)
        data = via_draft(); data['side_b'].pop('endpoint_id')
        data['side_b']['switch_id'] = SWITCH; data['side_b']['attachment'].pop('switch_socket')
        row = store.create(data)
        store.deploy(row['id'])
        prepare = next(call for call in fabric.calls if call['action'] == 'prepare')
        self.assertEqual(validate(prepare['intent'])[0]['via']['mode'], 'split')

    def test_cleanup_matches_only_owned_adapter_control_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = Path(directory); root = proc / 'deployment'
            (proc / 'exe').symlink_to(root / 'bin/tuntom-divert-adapter')
            (proc / 'cmdline').write_bytes(('adapter\0di0\0do0\0--control-socket\0' + str(root / 'run/via-2.control') + '\0').encode())
            self.assertTrue(matches_process(proc, root, 'client', ['231']))
            (proc / 'cmdline').write_bytes(b'adapter\0di0\0do0\0--control-socket\0/other/worker.control\0')
            self.assertFalse(matches_process(proc, root, 'client', ['231']))

class ViaTrustProvisioningTests(unittest.TestCase):
    def test_only_public_grants_are_accepted(self):
        from via_deployments import public_key_records
        public = 'x25519 ' + '34' * 32 + ' 0xff 3\n'
        self.assertEqual(public_key_records(public), 'x25519 ' + '34' * 32 + ' 255 3\n')
        for value in [public.replace('x25519', 'x25519-secret'), 'unrelated file contents', '', public.replace('0xff', '-1')]:
            with self.assertRaises(ValueError): public_key_records(value)

    def test_source_is_read_on_collector_and_packaged_on_both_sides(self):
        public = 'x25519 ' + '34' * 32 + ' 255 3\n'
        class KeyFabric(LocalFabric):
            def switch_deployment(self, body):
                result = super().switch_deployment(body)
                if body.get('trust_key'): result['trust_public'] = public
                return result
        fabric = KeyFabric(); store = TunnelDeployments(None, Endpoints(), fabric=fabric)
        self.addCleanup(store.close)
        data = via_draft(); data['via'].pop('trust_public')
        data['side_b'].pop('endpoint_id'); data['side_b']['switch_id'] = SWITCH
        data['side_b']['attachment'].pop('switch_socket')
        row = store.create(data)
        bundle = store.get(row['id'], True)['bundle']
        for side in ('side_a', 'side_b'):
            self.assertEqual(bundle[side + '/config/control-trust.pub'], public)
            self.assertIn('/config/control-trust.pub', bundle[side + '/scripts/00-preflight.sh'])
        self.assertIn('--control-trust-key /opt/tuntom/deployments/core-to-vpn/config/control-trust.pub', bundle['side_a/runtime/run-231c.sh'])
        # Model an old failed draft with no bundled public material, then retry.
        import json
        old = row['config']; old['via'].pop('trust_public')
        with store.db:
            store.db.execute('UPDATE tunnel_deployment SET config=?,status=? WHERE id=?', (json.dumps(old),'failed',row['id']))
        store.deploy(row['id'])
        self.assertEqual(store.get(row['id'])['config']['via']['trust_public'], public)

    def test_local_retry_can_materialize_public_key_in_legacy_intent(self):
        import json
        from unittest.mock import patch
        import switch_deployments as local
        data = via_draft(); data['side_b'].pop('endpoint_id'); data['side_b']['switch_id'] = SWITCH
        data['side_b']['attachment'].pop('switch_socket')
        resolved = {'name':'core','switch_socket':'/run/core.sock','binaries':{'tuntom':'/opt/bin/tuntom','tuntomctl':'/opt/bin/tuntomctl'}}
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory) / 'deployments'
            body = {'action':'prepare','intent':data,'deployment_id':'d'*32,'secret':'ab'*16}
            with patch.object(local,'BASE',base), patch.object(local,'trusted',side_effect=Path), patch.object(local,'inspect_switch',return_value=resolved), patch.object(local.os,'geteuid',return_value=0), patch.object(local,'run_script'):
                local.execute(None,body)
                root = base / data['name']
                old = json.loads((root/'.fabric-intent').read_text()); old['via'].pop('trust_public')
                (root/'.fabric-intent').write_text(json.dumps(old,sort_keys=True))
                (root/'config/control-trust.pub').unlink()
                local.execute(None,body)
                self.assertTrue((root/'config/control-trust.pub').is_file())
