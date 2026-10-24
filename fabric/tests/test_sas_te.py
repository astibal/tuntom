import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from sas_te import render


class SASTemplateTests(unittest.TestCase):
    def setUp(self):
        base=Path(__file__).resolve().parents[1]/'templates'
        self.template=json.loads((base/'sas-te.example.json').read_text())
        self.location=json.loads((base/'sas-location.example.json').read_text())

    def test_review_plan_and_relative_script(self):
        files=render(self.template,self.location)
        plan=json.loads(files['plan.json'])
        self.assertFalse(plan['live_enabled'])
        self.assertEqual(plan['contract_version'],3)
        script=files['payload/microservices/10-start.sh']
        self.assertIn('--tun-netns "$SAS_INSTANCE_NETNS"',script)
        self.assertNotIn(self.location['work_dir'],script)
        self.assertNotIn('/work/',script)
        self.assertNotIn('screen',script)
        self.assertNotIn('db.info',files)
        self.assertEqual(subprocess.run(['sh','-n'],input=script,text=True,capture_output=True).returncode,0)

    def test_exec_keeps_published_pid_and_invalid_secret_never_publishes(self):
        files=render(self.template,self.location)
        for valid in (True,False):
            with self.subTest(valid=valid),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);services=root/'microservices';services.mkdir()
                payload=services/'tuntom';(payload/'secrets').mkdir(parents=True)
                key=payload/'secrets'/('a'*32+'.key');key.write_text('ab'*16+'\n' if valid else 'not a key\n')
                executable=payload/'tuntom'
                executable.write_text('#!'+sys.executable+'\nimport os,json,sys\nprint(json.dumps({"pid":os.getpid(),"args":sys.argv[1:],"key_present":"TUNTOM_SECRET" in os.environ}))\n')
                executable.chmod(0o755)
                (services/'10-start.sh').write_text(files['payload/microservices/10-start.sh'])
                import os
                runtime=root/'runtime';runtime.mkdir()
                env={**os.environ,'SAS_INSTANCE_NETNS':'/run/sas/instance.netns','SAS_MICROSERVICE_PREFIX':'10','SAS_MICROSERVICE_RUN_ID':'test-run','SAS_MICROSERVICE_DIR':str(services),'SAS_MICROSERVICE_RUNTIME_DIR':str(runtime),'SAS_MICROSERVICE_PID_FILE':str(runtime/'10.pid')}
                process=subprocess.Popen(['sh','./10-start.sh'],cwd=services,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                out,err=process.communicate(timeout=5)
                if valid:
                    self.assertEqual(process.returncode,0,err)
                    result=json.loads(out)
                    self.assertEqual(result['pid'],process.pid)
                    self.assertEqual(int((runtime/'10.pid').read_text()),process.pid)
                    self.assertFalse((services/'10.pid').exists())
                    self.assertIn(str(runtime/'10.control'),result['args'])
                    self.assertTrue(result['key_present'])
                    self.assertEqual(result['args'][0],'client')
                    self.assertIn('/run/sas/instance.netns',result['args'])
                else:
                    self.assertNotEqual(process.returncode,0)
                    self.assertFalse((runtime/'10.pid').exists())

    def test_v3_rejects_legacy_and_overlapping_installation_paths(self):
        from sas_te import installation_location, resolve_request
        from unittest.mock import Mock
        for change in ({'microservice_contract_version':2}, {'microservice_contract_version':None},
                       {'microservices_dir':None}, {'microservices_dir':self.location['work_dir']+'/microservices'},
                       {'microservices_dir':'/srv/sas/example'}, {'microservices_dir':'/srv/../work'}):
            location={**self.location,**change}
            with self.subTest(change=change), self.assertRaises(ValueError): installation_location(location)
            endpoints=Mock();endpoints.run.return_value=json.dumps(location)
            with self.assertRaises(ValueError):
                resolve_request({'entry_endpoint_id':'b'*32,'instance_id':self.location['id']},endpoints)
            self.assertEqual(endpoints.run.call_count,1)

    def test_v3_start_rejects_missing_or_installation_runtime(self):
        import os
        script=render(self.template,self.location)['payload/microservices/10-start.sh']
        with tempfile.TemporaryDirectory() as directory:
            install=Path(directory)/'microservices';install.mkdir()
            runtime=Path(directory)/'runtime';runtime.mkdir()
            env={key:value for key,value in os.environ.items() if not key.startswith('SAS_MICROSERVICE_')}
            variants=[{}, {'SAS_MICROSERVICE_PREFIX':'10','SAS_MICROSERVICE_RUN_ID':'run',
                       'SAS_MICROSERVICE_DIR':str(install),'SAS_MICROSERVICE_RUNTIME_DIR':str(install),
                       'SAS_MICROSERVICE_PID_FILE':str(install/'10.pid')},
                      {'SAS_MICROSERVICE_PREFIX':'10','SAS_MICROSERVICE_RUN_ID':'run',
                       'SAS_MICROSERVICE_DIR':str(install),'SAS_MICROSERVICE_RUNTIME_DIR':str(runtime),
                       'SAS_MICROSERVICE_PID_FILE':str(install/'10.pid')}]
            for variant in variants:
                result=subprocess.run(['sh'],input=script,text=True,capture_output=True,cwd=install,env={**env,**variant})
                self.assertNotEqual(result.returncode,0)
                self.assertFalse(list(install.iterdir()))
                self.assertFalse(list(runtime.iterdir()))

    def test_lookup_identity_and_namespace_are_validated(self):
        self.location['id']='22222222-2222-4222-8222-222222222222'
        with self.assertRaisesRegex(ValueError,'identity mismatch'):render(self.template,self.location)
        self.location['id']=self.template['instance_id']
        self.location['namespace']='ns;touch /tmp/bad'
        with self.assertRaisesRegex(ValueError,'namespace'):render(self.template,self.location)
        self.location['namespace']='instance'
        self.location['namespace_exists']=False
        with self.assertRaisesRegex(ValueError,'unavailable'):render(self.template,self.location)

    def test_prefix_and_build_identity_are_explicit(self):
        self.template['prefix']='010'
        with self.assertRaisesRegex(ValueError,'prefix'):render(self.template,self.location)
        self.template['prefix']='10';self.template['build']['commit']='main'
        with self.assertRaisesRegex(ValueError,'commit'):render(self.template,self.location)

    def test_alias_preview_pins_authoritative_uuid(self):
        from sas_te import preview_request, resolved_instance, instance_reference
        from unittest.mock import Mock
        endpoints=Mock();endpoints.run.return_value=json.dumps(self.location)
        template={**self.template,'instance_id':'tatka-smoula'}
        result=preview_request({'entry_endpoint_id':'b'*32,'template':template},endpoints)
        endpoints.run.assert_any_call('b'*32,'sas_which_instance tatka-smoula',timeout=30)
        self.assertEqual(json.loads(result['files']['template.json'])['instance_id'],self.location['id'])
        with self.assertRaises(ValueError):resolved_instance('22222222-2222-4222-8222-222222222222',self.location)
        with self.assertRaises(ValueError):resolved_instance('alias',{'id':'broken'})
        for invalid in ('--help','bad;cmd','two words',''):
            with self.assertRaises(ValueError):instance_reference(invalid)

    def test_ui_preview_uses_lookup_and_archive_matches_review(self):
        import base64,io,tarfile
        from sas_te import preview_request
        from unittest.mock import Mock
        endpoints=Mock();endpoints.run.return_value=json.dumps(self.location)
        template=dict(self.template);template.pop('deployment_id')
        template['switch']={**template['switch'],'label':str(2**64-1)}
        result=preview_request({'entry_endpoint_id':'b'*32,'template':template},endpoints)
        endpoints.run.assert_any_call('b'*32,'sas_which_instance '+template['instance_id'],timeout=30)
        self.assertFalse(result['plan']['live_enabled'])
        with tarfile.open(fileobj=io.BytesIO(base64.b64decode(result['archive_base64']))) as archive:
            self.assertEqual(archive.extractfile('plan.json').read().decode(),result['files']['plan.json'])
            self.assertEqual(archive.getmember('payload/microservices/10-start.sh').mode,0o600)
        self.assertIn(str(2**64-1),result['plan']['switch']['argv'])

    def test_ssh_origin_observation(self):
        from sas_te import ssh_origin_observation
        from unittest.mock import Mock
        endpoints=Mock()
        endpoints.run.return_value='runner\nrunner.example\n192.0.2.1 45000 192.0.2.2 22\n'
        result=ssh_origin_observation(endpoints,'b'*32,'runner.example')
        self.assertEqual(result['source_ip'],'192.0.2.1')
        self.assertFalse(result['transport_verified'])
        self.assertEqual(ssh_origin_observation(endpoints,'b'*32,'other')['status'],'unavailable')
        endpoints.run.return_value='runner\nrunner.example\nmalformed\n'
        self.assertEqual(ssh_origin_observation(endpoints,'b'*32,'runner')['status'],'unavailable')

    def test_blank_peer_uses_origin_ssh_source(self):
        from sas_te import preview_request
        from unittest.mock import Mock
        endpoints=Mock()
        origin=self.location['origin']
        endpoints.run.side_effect=[json.dumps(self.location),f'{origin}\n{origin}\n192.0.2.8 45000 192.0.2.2 22\n']
        self.template['peer']=''
        result=preview_request({'entry_endpoint_id':'b'*32,'template':self.template},endpoints)
        self.assertEqual(json.loads(result['files']['template.json'])['peer'],'192.0.2.8')
        self.assertTrue(result['plan']['ssh_observation']['used_as_transport_ip'])

    def test_package_includes_executable_and_manifest(self):
        import base64, io, tarfile, hashlib
        from sas_te import preview_request
        from unittest.mock import Mock, patch
        endpoints=Mock()
        endpoints.run.side_effect=[json.dumps(self.location),'other\nother\n192.0.2.1 44000 192.0.2.2 22\n']
        binary=b'test package binary'
        metadata={'version':'5.1.123','path':'/opt/tuntom/bin/tuntom','architecture':'x86_64','sha256':hashlib.sha256(binary).hexdigest()}
        checks=[{'name':name,'status':'ok','detail':''} for name in ('installation','installation_paths','namespace','sh','mv','tun_device','user','group')]
        checks.append({'name':'architecture','status':'info','detail':'x86_64'})
        self.template.pop('build')
        with patch('sas_te.installed_binary',return_value=(metadata,binary)), patch('sas_te.sanity_observation',return_value={'status':'checked','checks':checks}):
            result=preview_request({'entry_endpoint_id':'b'*32,'template':self.template,'package':True},endpoints)
        with tarfile.open(fileobj=io.BytesIO(base64.b64decode(result['archive_base64']))) as archive:
            self.assertEqual(archive.extractfile('payload/microservices/tuntom/version.txt').read(),b'5.1.123\n')
            self.assertEqual(archive.extractfile('payload/microservices/tuntom/version-commit.txt').read(),b'unknown\n')
            self.assertEqual(archive.extractfile('payload/microservices/tuntom/tuntom').read(),binary)
            self.assertEqual(archive.getmember('payload/microservices/tuntom/tuntom').mode,0o755)
            self.assertIn(metadata['sha256']+'  payload/microservices/tuntom/tuntom',archive.extractfile('manifest.sha256').read().decode())
        self.assertFalse(result['plan']['live_enabled'])

    def test_invalid_ui_input_does_not_invoke_ssh(self):
        from sas_te import preview_request
        from unittest.mock import Mock
        endpoints=Mock();self.template['instance_id']='$(id)'
        with self.assertRaises(ValueError):preview_request({'entry_endpoint_id':'b'*32,'template':self.template},endpoints)
        endpoints.run.assert_not_called()

    def test_api_preview_enforces_admin_and_real_audit_validation(self):
        from test_tunnel_deployments import DeploymentAPITests
        from server import Fabric
        from errors import APIError
        from unittest.mock import Mock
        from types import SimpleNamespace
        handler=DeploymentAPITests().handler('/api/v1/sas-te/preview','POST',None,writes=False)
        endpoints=Mock();endpoints.run.return_value=json.dumps(self.location)
        handler.server.controlled_endpoints=endpoints
        handler.body=lambda:{'entry_endpoint_id':'b'*32,'template':self.template}
        audit=SimpleNamespace(journal=SimpleNamespace(append=Mock(return_value='audit-id')))
        handler.server.fabric.audit_event=lambda body:Fabric.audit_event(audit,body)
        result=handler.route();self.assertEqual(result[0],200)
        audit.journal.append.assert_called_once()
        handler=DeploymentAPITests().handler('/api/v1/sas-te/preview','POST',None,role='admin-ro')
        with self.assertRaises(APIError):handler.route()
