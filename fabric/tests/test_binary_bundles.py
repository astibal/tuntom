import base64
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from binary_bundles import BinaryBundles
from test_tunnel_deployments import Endpoints, draft
from tunnel_deployments import TunnelDeployments


def elf():
    value = bytearray(64); value[:6] = b'\x7fELF\x02\x01'; value[16] = 3; value[18] = 62
    return bytes(value)


def upload():
    return dict(name='tunnel-test', revision='abc123', os='ubuntu', version='26.04', architecture='x86_64',
        files={name: base64.b64encode(elf()).decode() for name in ('tuntom', 'tuntomctl')})


class Hosts(Endpoints):
    def get(self, ident):
        row = super().get(ident)
        row['snapshot'] = {'system': {'os': 'ubuntu', 'version': '26.04', 'architecture': 'x86_64'}}
        return row


class BinaryBundleTests(unittest.TestCase):
    def test_upload_persistence_and_archive_integrity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'store.db'
            store = BinaryBundles(path)
            row = store.create(upload()); store.close()
            store = BinaryBundles(path)
            try:
                self.assertEqual(store.list()['binary_bundles'], [row])
                _, data = store.archive(row['id'])
                with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                    files = {member.name: archive.extractfile(member).read() for member in archive.getmembers()}
                    self.assertEqual(archive.getmember('tuntom').mode, 0o755)
                self.assertEqual(files['tuntom'], elf())
                for line in files['manifest.sha256'].decode().splitlines():
                    digest, name = line.split('  ', 1)
                    self.assertEqual(hashlib.sha256(files[name]).hexdigest(), digest)
                with self.assertRaises(ValueError): store.compatible(row['id'], {'os':'ubuntu','version':'24.04','architecture':'x86_64'})
            finally: store.close()

    def test_rejects_wrong_architecture_and_incomplete_files(self):
        store = BinaryBundles(None)
        try:
            data = upload(); del data['files']['tuntomctl']
            with self.assertRaises(ValueError): store.create(data)
            data = upload(); data['files']['tuntom'] = base64.b64encode(b'#!/bin/sh\n').decode()
            with self.assertRaisesRegex(ValueError, 'ELF'): store.create(data)
            data = upload(); data['architecture'] = 'aarch64'
            with self.assertRaisesRegex(ValueError, 'platform'): store.create(data)
            self.assertEqual(store.list()['binary_bundles'], [])
        finally: store.close()

    def test_bundle_deploy_transfers_both_binaries_and_hashes(self):
        endpoints = Hosts(); store = TunnelDeployments(None, endpoints)
        try:
            row = store.binary_bundles.create(upload())
            data = draft(); data['software'] = {'source':'bundle', 'bundle_id':row['id']}
            deployment = store.create(data)
            script = store.script(deployment['id'], 'side_a/scripts/20-install-software.sh')
            self.assertNotIn('{shell(', script)
            self.assertIn(row['files']['tuntomctl']['sha256'], script)
            self.assertEqual(subprocess.run(['bash','-n'], input=script, text=True, capture_output=True).returncode, 0)
            store.deploy(deployment['id'])
            transfers = [call[2] for call in endpoints.runs if call[2]]
            self.assertEqual(len(transfers), 2)
            for transfer in transfers:
                with tarfile.open(fileobj=io.BytesIO(transfer)) as archive:
                    self.assertEqual(archive.extractfile('incoming/tuntomctl').read(), elf())
            _, archive_data = store.archive(deployment['id'], 'deploy')
            with tarfile.open(fileobj=io.BytesIO(archive_data)) as archive:
                self.assertEqual(archive.extractfile('side_a/incoming/tuntom').read(), elf())
        finally: store.close()

    def test_build_pins_commit_and_uses_portable_flags(self):
        class Builder(Hosts):
            def run(self, ident, command, input_data=None, timeout=600):
                self.script = input_data.decode()
                result = upload(); result.pop('name'); result['revision'] = 'a' * 40
                return json.dumps(result)
        endpoints = Builder(); store = BinaryBundles(None)
        try:
            row = store.build({'name':'compiled', 'revision':'dev', 'endpoint_id':'a'*32}, endpoints, 'https://example.test/repo.git')
            self.assertEqual(row['revision'], 'a'*40)
            self.assertIn('-march=x86-64 -mtune=generic', endpoints.script)
            self.assertNotIn('-march=native', endpoints.script)
            self.assertIn('trap ', endpoints.script)
            self.assertEqual(subprocess.run(['bash','-n'], input=endpoints.script, text=True, capture_output=True).returncode, 0)
            with self.assertRaises(ValueError): store.build({'name':'bad','revision':'$(id)','endpoint_id':'a'*32}, endpoints, 'repo')
        finally: store.close()

class BundleAPITests(unittest.TestCase):
    def test_admin_upload_download_and_delete_guards(self):
        from test_tunnel_deployments import DeploymentAPITests
        from errors import APIError
        factory = DeploymentAPITests().handler
        store = TunnelDeployments(None, Hosts())
        try:
            handler = factory('/api/v1/binary-bundles','POST',store,role='admin-ro'); handler.body=upload
            with self.assertRaises(APIError) as denied: handler.route()
            self.assertEqual(denied.exception.status,403)
            handler = factory('/api/v1/binary-bundles','POST',store); handler.body=upload
            row = handler.route()[1]
            response = factory('/api/v1/binary-bundles/'+row['id']+'/archive','GET',store,role='admin-ro').route()
            self.assertEqual(response[2],'application/gzip')
            handler = factory('/api/v1/binary-bundles/build','POST',store,writes=False)
            with self.assertRaises(APIError) as denied: handler.route()
            self.assertEqual(denied.exception.status,403)
            deployment = store.create(draft()); path='/api/v1/tunnel-deployments/'+deployment['id']+'/delete'
            with self.assertRaises(APIError) as denied: factory(path,'POST',store,role='admin-ro').route()
            self.assertEqual(denied.exception.status,403)
            with self.assertRaises(APIError) as conflict: factory(path,'POST',store).route()
            self.assertEqual(conflict.exception.status,409)
            store._set_status(deployment['id'],'running'); store.undeploy(deployment['id'])
            self.assertTrue(factory(path,'POST',store).route()[1]['deleted'])
        finally: store.close()
