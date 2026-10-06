from pathlib import Path
import sys
import subprocess
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tunnel_deployments import TunnelDeployments, validate


A, B = "a" * 32, "b" * 32


class Endpoints:
    def __init__(self): self.runs = []

    def get(self, ident):
        if ident not in {A, B}: raise KeyError("unknown controlled endpoint")
        return {"id": ident, "address": "192.0.2.10" if ident == A else "198.51.100.20", "status": "supported"}

    def run(self, ident, command, input_data=None, timeout=600):
        self.runs.append((ident, command, input_data, timeout)); return "OK\n"


def draft(secret=None):
    return {"name": "core-to-vpn", "tunnel_id": 231, "count": 4,
        "side_a": {"endpoint_id": A, "role": "listener", "runtime": "screen",
            "attachment": {"type": "switch", "switch_socket": "/run/tuntom/fabric.sock", "port_id": "edge", "label": 17}},
        "side_b": {"endpoint_id": B, "role": "initiator", "peer_address": "192.0.2.10", "runtime": "screen",
            "attachment": {"type": "tun"}},
        "software": {"source": "git_build", "revision": "main"},
        "secret": secret or {"mode": "generate"}}


class TunnelDeploymentTests(unittest.TestCase):
    def test_group_runbook_and_secret_separation(self):
        store = TunnelDeployments(None, Endpoints())
        try:
            created = store.create(draft({"mode": "provided", "value": "01" * 16}))
            self.assertEqual(created["status"], "draft")
            self.assertEqual(created["config"]["count"], 4)
            self.assertNotIn("value", created["config"]["secret"])
            run = store.script(created["id"], "side_b/runtime/run-231_3c.sh")
            self.assertIn("client 231_3 ut231_3c 192.0.2.10", run)
            self.assertNotIn("01010101", run)
            start = store.script(created["id"], "side_a/scripts/40-start.sh")
            self.assertIn("tuntom-core-to-vpn-231_3s", start)
            bundle = store.get(created["id"], include_bundle=True)["bundle"]
            for name, content in bundle.items():
                if name.endswith(".sh"):
                    checked = subprocess.run(["bash", "-n"], input=content, text=True, capture_output=True)
                    self.assertEqual(checked.returncode, 0, f"{name}: {checked.stderr}")
            with self.assertRaisesRegex(RuntimeError, "already belongs"):
                second = draft(); second["name"] = "collision"; store.create(second)
        finally: store.close()

    def test_validation_rejects_unsafe_or_ambiguous_input(self):
        value = draft(); value["side_b"]["role"] = "listener"; value["side_b"].pop("peer_address")
        with self.assertRaisesRegex(ValueError, "exactly one"): validate(value)
        value = draft(); value["side_b"]["endpoint_id"] = A
        with self.assertRaisesRegex(ValueError, "same Controlled Endpoint"): validate(value)
        value = draft(); value["software"]["revision"] = "--upload-pack=bad"
        with self.assertRaisesRegex(ValueError, "revision"): validate(value)

    def test_automatic_id(self):
        store = TunnelDeployments(None, Endpoints())
        try:
            value = draft(); value["tunnel_id"] = None
            self.assertEqual(store.create(value)["config"]["tunnel_id"], 1)
        finally: store.close()

    def test_deploy_installs_both_and_starts_listener_first(self):
        endpoints = Endpoints(); store = TunnelDeployments(None, endpoints)
        try:
            created = store.create(draft())
            result = store.deploy(created["id"])
            self.assertEqual(result["status"], "running")
            uploads = [call for call in endpoints.runs if call[2] is not None]
            self.assertEqual([call[0] for call in uploads], [A, B])
            starts = [call for call in endpoints.runs if "40-start.sh" in call[1]]
            self.assertEqual([call[0] for call in starts], [A, B])
            self.assertTrue(all(call[2].startswith(b"\x1f\x8b") for call in uploads))
        finally: store.close()


if __name__ == "__main__": unittest.main()


SWITCH = '11111111-1111-1111-1111-111111111111:42:1234'


class LocalFabric:
    def __init__(self): self.calls = []; self.stale = False; self.fail_start = False
    def snapshot(self):
        return {'endpoints': [{'kind': 'tunnel', 'name': '1s', 'source': 'local'}]}
    def switch_deployment(self, body):
        self.calls.append(body)
        if body['action'] == 'inspect':
            if self.stale: raise RuntimeError('switch no longer exists')
            return {'name': 'core-switch', 'switch_socket': '/run/core.sock',
                    'binaries': {'tuntom': '/opt/bin/tuntom', 'tuntomctl': '/opt/bin/tuntomctl'}}
        if body['action'] == 'start' and self.fail_start:
            from errors import APIError
            raise APIError(409, 'start failed')
        validate(body['intent'])
        return {'result': 'OK'}


def switch_draft():
    value = draft()
    value['side_a'].pop('endpoint_id')
    value['side_a']['switch_id'] = SWITCH
    value['side_a']['attachment'].pop('switch_socket')
    return value


class ExistingSwitchTests(unittest.TestCase):
    def setUp(self):
        self.endpoints, self.fabric = Endpoints(), LocalFabric()
        self.store = TunnelDeployments(None, self.endpoints, fabric=self.fabric)
        self.addCleanup(self.store.close)

    def test_one_ssh_host_and_switch_without_host_discovery(self):
        value = switch_draft(); value['tunnel_id'] = None
        created = self.store.create(value)
        self.assertEqual(created['config']['tunnel_id'], 2)
        local = self.store.script(created['id'], 'side_a/scripts/20-install-software.sh')
        self.assertIn('/opt/bin/tuntom', local)
        self.assertNotIn('git', local)
        self.assertNotIn('apt', self.store.script(created['id'], 'side_a/scripts/10-install-dependencies.sh'))
        result = self.store.deploy(created['id'])
        self.assertEqual(result['status'], 'running')
        self.assertEqual({call[0] for call in self.endpoints.runs}, {B})
        self.assertEqual([call['action'] for call in self.fabric.calls], ['inspect', 'inspect', 'prepare', 'start'])
        self.assertNotIn('secret', self.fabric.calls[-1])
        for name, content in self.store.get(created['id'], True)['bundle'].items():
            if name.endswith('.sh'):
                self.assertEqual(subprocess.run(['bash', '-n'], input=content, text=True, capture_output=True).returncode, 0, name)

    def test_stale_switch_prevents_ssh_mutations(self):
        created = self.store.create(switch_draft()); self.fabric.stale = True
        with self.assertRaisesRegex(RuntimeError, 'no longer exists'): self.store.deploy(created['id'])
        self.assertEqual(self.endpoints.runs, [])

    def test_collector_error_rolls_back_and_marks_failed(self):
        created = self.store.create(switch_draft()); self.fabric.fail_start = True
        from errors import APIError
        with self.assertRaises(APIError): self.store.deploy(created['id'])
        self.assertEqual(self.store.get(created['id'])['status'], 'failed')
        self.assertEqual(self.fabric.calls[-1]['action'], 'rollback')

    def test_switch_socket_cannot_be_supplied(self):
        value = switch_draft(); value['side_a']['attachment']['switch_socket'] = '/tmp/unobserved.sock'
        with self.assertRaisesRegex(ValueError, 'resolved by the collector'): self.store.create(value)

    def test_provided_key_is_not_forwarded_inside_intent(self):
        value = switch_draft(); value['secret'] = {'mode': 'provided', 'value': 'ab' * 16}
        created = self.store.create(value); self.store.deploy(created['id'])
        prepare = next(call for call in self.fabric.calls if call['action'] == 'prepare')
        self.assertEqual(prepare['intent']['secret'], {'mode': 'generate'})
        self.assertEqual(prepare['secret'], 'ab' * 16)


class AttachmentInputTests(unittest.TestCase):
    def test_occupied_group_port_and_label_precision(self):
        from tunnel_deployments import check_switch_ports
        side = draft()["side_a"]
        with self.assertRaisesRegex(ValueError, "edge_2"):
            check_switch_ports(side, 4, ["edge_2"])
        value = draft(); value["side_a"]["attachment"]["label"] = "18446744073709551615"
        config, _ = validate(value)
        self.assertEqual(config["side_a"]["attachment"]["label"], "18446744073709551615")
        value["side_a"]["attachment"]["label"] = ""
        with self.assertRaises(ValueError): validate(value)

    def test_explicit_running_id_is_rejected(self):
        store = TunnelDeployments(None, Endpoints(), fabric=LocalFabric())
        try:
            value = switch_draft(); value["tunnel_id"] = 1
            with self.assertRaisesRegex(RuntimeError, "already belongs"): store.create(value)
        finally: store.close()


class TeardownTests(unittest.TestCase):
    def test_archives_upgrade_existing_bundles_without_secrets(self):
        import io, tarfile, hashlib
        store = TunnelDeployments(None, Endpoints())
        try:
            created = store.create(draft({'mode':'provided','value':'ab'*16}))
            for kind in ('deploy','undeploy'):
                name, data = store.archive(created['id'],kind)
                self.assertEqual(name, kind+'_runbook_core-to-vpn.tar.gz')
                with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                    files={member.name:archive.extractfile(member).read() for member in archive.getmembers()}
                self.assertIn('side_a/scripts/95-undeploy.sh', files)
                self.assertNotIn(b'ab'*16, b''.join(files.values()))
                self.assertNotIn('secrets/master.env',files)
                for line in files['manifest.sha256'].decode().splitlines():
                    digest, filename=line.split('  ',1)
                    self.assertEqual(digest,hashlib.sha256(files[filename]).hexdigest())
            self.assertEqual(created['config']['id_allocation'],'manual')
        finally: store.close()

    def test_undeploy_both_sides_erases_collector_secret_and_is_idempotent(self):
        endpoints=Endpoints();fabric=LocalFabric();store=TunnelDeployments(None,endpoints,fabric=fabric)
        try:
            created=store.create(switch_draft());store.deploy(created['id']);endpoints.runs.clear()
            result=store.undeploy(created['id'])
            self.assertEqual(result['status'],'undeployed')
            self.assertNotIn(created['id'],store.memory_secrets)
            self.assertEqual(set(v['state'] for v in result['side_states'].values()),{'undeployed'})
            self.assertEqual(endpoints.runs[0][1],'bash -s')
            self.assertIn(b'.fabric-deployment-id',endpoints.runs[0][2])
            self.assertEqual(fabric.calls[-1]['action'],'undeploy')
            calls=len(endpoints.runs);store.undeploy(created['id']);self.assertEqual(len(endpoints.runs),calls)
        finally: store.close()

    def test_partial_failure_retries_only_remaining_side(self):
        from unittest.mock import patch
        endpoints=Endpoints();fabric=LocalFabric();store=TunnelDeployments(None,endpoints,fabric=fabric)
        try:
            created=store.create(switch_draft());store.deploy(created['id'])
            with patch.object(endpoints,'run',side_effect=OSError('host unreachable')):
                with self.assertRaisesRegex(RuntimeError,'unreachable'):store.undeploy(created['id'])
            value=store.get(created['id']);self.assertEqual(value['status'],'undeploy_failed')
            self.assertEqual(value['side_states']['side_a']['state'],'undeployed')
            self.assertIn(created['id'],store.memory_secrets)
            before=len(fabric.calls);store.undeploy(created['id']);self.assertEqual(len(fabric.calls),before)
            self.assertNotIn(created['id'],store.memory_secrets)
        finally:store.close()


class DeploymentAPITests(unittest.TestCase):
    def handler(self, path, method, store, role='admin', writes=True):
        from server import Handler
        from errors import APIError
        from types import SimpleNamespace
        from unittest.mock import Mock
        h = Handler.__new__(Handler); h.path=path; h.command=method; h.guard=lambda:None
        h.server=SimpleNamespace(tunnel_deployments=store,fabric=SimpleNamespace(snapshot=lambda:{'allow_write':writes},audit_event=Mock()))
        def authorize(admin=False):
            if admin and role!='admin': raise APIError(403,'admin required')
            return {'username':'test','role':role}
        h.authorize=authorize;h.respond=lambda *args:args
        return h

    def test_archive_is_gzip_and_undeploy_requires_admin_and_writes(self):
        from errors import APIError
        from unittest.mock import Mock
        store=TunnelDeployments(None,Endpoints())
        try:
            created=store.create(draft());base='/api/v1/tunnel-deployments/'+created['id']
            response=self.handler(base+'/archive/deploy','GET',store).route()
            self.assertEqual(response[0],200);self.assertEqual(response[2],'application/gzip')
            self.assertIn('deploy_runbook_core-to-vpn.tar.gz',response[3]['Content-Disposition'])
            self.assertTrue(response[1].startswith(b'\x1f\x8b'))
            for role,writes in [('admin-ro',True),('admin',False)]:
                with self.assertRaises(APIError) as error:self.handler(base+'/undeploy','POST',store,role,writes).route()
                self.assertEqual(error.exception.status,403)
            self.assertEqual(store.get(created['id'])['status'],'draft')
        finally:store.close()

class DeploymentDeletionTests(unittest.TestCase):
    def test_only_completed_undeploy_can_be_deleted(self):
        store = TunnelDeployments(None, Endpoints())
        try:
            row = store.create(draft())
            for status in ('draft', 'running', 'failed', 'undeploying', 'undeploy_failed'):
                store._set_status(row['id'], status)
                with self.assertRaisesRegex(RuntimeError, 'only undeployed'):
                    store.delete(row['id'])
                self.assertEqual(store.get(row['id'])['status'], status)
            store.undeploy(row['id'])
            self.assertTrue(store.delete(row['id'])['deleted'])
            self.assertEqual(store.list()['tunnel_deployments'], [])
            with self.assertRaises(KeyError): store.get(row['id'])
        finally:
            store.close()

class RuntimeAccessTests(unittest.TestCase):
    def test_check_reports_first_blocked_ancestor(self):
        from unittest.mock import patch
        from tunnel_deployments import RUNTIME_ACCESS_CHECK
        code = RUNTIME_ACCESS_CHECK.split("<<'PYACCESS'\n", 1)[1].rsplit('PYACCESS', 1)[0]
        with patch.object(sys, 'argv', ['-', '/opt/private/deployment']), patch('os.access', side_effect=lambda p, mode: str(p) != '/opt/private'):
            with self.assertRaisesRegex(SystemExit, 'cannot traverse /opt/private'):
                exec(code, {})
        with patch.object(sys, 'argv', ['-', '/opt/private/deployment']), patch('os.access', side_effect=lambda p, mode: str(p) != '/opt/private/deployment/run'):
            with self.assertRaisesRegex(SystemExit, 'cannot write to /opt/private/deployment/run'):
                exec(code, {})

    def test_retry_refreshes_stored_runbook_with_access_check(self):
        import json
        store = TunnelDeployments(None, Endpoints())
        self.addCleanup(store.close)
        row = store.create(draft())
        with store.db:
            store.db.execute("UPDATE tunnel_deployment SET status='failed',bundle='{}' WHERE id=?", (row['id'],))
        secret = store._secret(row['id'])
        store.deploy(row['id'])
        bundle = store.get(row['id'], include_bundle=True)['bundle']
        self.assertIn('Runtime access denied:', bundle['side_a/scripts/30-install-config.sh'])
        self.assertEqual(secret, store._secret(row['id']))
