import base64
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from headless_bundles import archive
from binary_bundles import BinaryBundles
from test_binary_bundles import upload, elf


def request(kind='endpoint', format='source'):
    return dict(name='appliance',kind=kind,format=format,peer='192.0.2.10',tunnel_id=231,
                count=8 if kind=='divert' else 1,port_id='appliance',switch_socket='/run/core.sock')


def unpack(data):
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        return {member.name:tar.extractfile(member).read() for member in tar.getmembers()}


class HeadlessTests(unittest.TestCase):
    def test_source_kits_are_offline_relocatable_and_include_both_sides(self):
        for kind in ('endpoint','divert'):
            _, content=archive(request(kind),None)
            files=unpack(content)
            self.assertIn(b'-std=c++17',files['Makefile'])
            self.assertIn('src/main.cpp',files)
            self.assertIn('switch/start.sh',files)
            self.assertNotIn('endpoint/secrets/master.env',files)
            for name,data in files.items():
                if name.endswith('.sh'):
                    check=subprocess.run(['bash','-n'],input=data,capture_output=True)
                    self.assertEqual(check.returncode,0,name+str(check.stderr))
                    self.assertNotIn(b'/opt/tuntom/deployments/appliance',data)
                    self.assertNotIn(b'apt-get',data)
            start=files['endpoint/start.sh'].decode()
            code=start.split("<<'KEY'\n",1)[1].split('\nKEY',1)[0]
            compile(code,'secret-loader','exec')
            self.assertIn("key.lower()+'\\n'",code)
            for line in files['manifest.sha256'].decode().splitlines():
                digest,name=line.split('  ',1)
                self.assertEqual(hashlib.sha256(files[name]).hexdigest(),digest)
            if kind=='divert':
                self.assertIn(b'client-relay appliance-in_*',files['switch/via-service.rules.txt'])
                self.assertIn(b'src/divert/main.cpp',files['Makefile'])

    def test_binary_divert_requires_and_packages_adapter(self):
        store=BinaryBundles(None);self.addCleanup(store.close)
        metadata=store.create(upload())
        data=request('divert','binary');data['bundle_id']=metadata['id']
        with self.assertRaisesRegex(ValueError,'lacks the divert'):archive(data,store)
        value=upload();value['files']['tuntom-divert-adapter']=base64.b64encode(elf()).decode()
        metadata=store.create(value);data['bundle_id']=metadata['id']
        files=unpack(archive(data,store)[1])
        self.assertEqual(files['endpoint/bin/tuntom-divert-adapter'],elf())
        self.assertNotIn('Makefile',files)
        self.assertEqual(json.loads(files['bundle.json'])['platform'],'Ubuntu 26.04 x86-64')

    def test_rejects_invalid_id_or_odd_split(self):
        data=request();data['tunnel_id']=None
        with self.assertRaises(ValueError):archive(data,None)
        data=request('divert');data['count']=3
        with self.assertRaises(ValueError):archive(data,None)

    def test_download_requires_admin_but_never_deploys(self):
        from test_tunnel_deployments import DeploymentAPITests, TunnelDeployments, Endpoints
        from errors import APIError
        endpoints=Endpoints();store=TunnelDeployments(None,endpoints);self.addCleanup(store.close)
        factory=DeploymentAPITests()
        handler=factory.handler('/api/v1/headless-bundles','POST',store,writes=False)
        handler.body=lambda:request()
        from server import Fabric
        from types import SimpleNamespace
        from unittest.mock import Mock
        audit=SimpleNamespace(journal=SimpleNamespace(append=Mock(return_value='audit-id')))
        handler.server.fabric.audit_event=lambda body:Fabric.audit_event(audit,body)
        self.assertEqual(handler.route()[0],200)
        audit.journal.append.assert_called_once()
        self.assertEqual(endpoints.runs,[])
        self.assertEqual(store.list()['tunnel_deployments'],[])
        handler=factory.handler('/api/v1/headless-bundles','POST',store,role='admin-ro')
        handler.body=lambda:request()
        with self.assertRaises(APIError):handler.route()

    def test_copied_key_is_removed_but_external_key_survives_undeploy(self):
        import tempfile
        from unittest.mock import patch
        import deployment_cleanup
        files=unpack(archive(request(),None)[1])
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory);root=base/'endpoint';root.mkdir()
            external=base/'external.key';external.write_text('ab'*16);external.chmod(0o600)
            code=files['endpoint/start.sh'].decode().split("<<'KEY'\n",1)[1].split('\nKEY',1)[0]
            subprocess.run([sys.executable,'-c',code,str(external),str(root)],check=True)
            self.assertEqual((root/'secrets/master.env').read_text(),'TUNTOM_SECRET='+'ab'*16+'\n')
            (root/'.fabric-deployment-id').write_bytes(files['endpoint/.fabric-deployment-id'])
            original=Path.iterdir
            with patch.object(Path,'iterdir',lambda p:iter([]) if p==Path('/proc') else original(p)):
                deployment_cleanup.undeploy(root,files['endpoint/.fabric-deployment-id'].decode().strip(),'client',['231'])
            self.assertFalse(root.exists())
            self.assertEqual(external.read_text(),'ab'*16)
