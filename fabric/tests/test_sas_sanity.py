import json
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sas_sanity import inspect
from sas_te import resolve_request, render


class SanityTests(unittest.TestCase):
    def test_read_only_probe_reports_missing_account_and_existing_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'tuntom').mkdir()
            binary=work/'tuntom/tuntom'
            binary.write_text('#!/bin/sh\nexit 99\n')
            binary.chmod(0o755)
            before=sorted(str(p) for p in work.rglob('*'))
            with patch('sas_sanity.pwd.getpwnam',side_effect=KeyError), patch('sas_sanity.grp.getgrnam',side_effect=KeyError):
                result=inspect({'origin':socket.gethostname(),'work_dir':str(work/'work'),'microservices_dir':directory,'namespace':'test-missing','user':'tuntom','group':'tuntom'})
            checks={row['name']:row for row in result['checks']}
            self.assertEqual(checks['user']['status'],'missing')
            self.assertEqual(checks['installation_paths']['status'],'warning')
            self.assertEqual(checks['installation_binary']['status'],'missing')
            self.assertFalse(result['runtime_verified'])
            self.assertEqual(before,sorted(str(p) for p in work.rglob('*')))

    def test_wrong_origin_is_not_inspected(self):
        self.assertEqual(inspect({'origin':'not-this-origin.invalid'})['reason'],'origin_mismatch')

    def test_resolve_includes_probe_and_validates_account_before_ssh(self):
        location=json.loads((Path(__file__).resolve().parents[1]/'templates/sas-location.example.json').read_text())
        endpoints=Mock()
        endpoints.run.side_effect=[json.dumps(location),'different\ndifferent\n192.0.2.1 45000 192.0.2.2 22\n',json.dumps({'status':'checked','checks':[],'runtime_verified':False})]
        data={'entry_endpoint_id':'b'*32,'instance_id':location['id'],'runtime':{'user':'tuntom','group':'tuntom'}}
        result=resolve_request(data,endpoints)
        self.assertEqual(result['sanity']['status'],'checked')
        call=endpoints.run.call_args
        self.assertTrue(call.args[1].startswith('python3 - '))
        self.assertIn(b'def inspect',call.kwargs['input_data'])
        endpoints.reset_mock()
        data['runtime']['user']='root; touch /tmp/bad'
        with self.assertRaises(ValueError): resolve_request(data,endpoints)
        endpoints.run.assert_not_called()

    def test_installed_binary_reads_version_without_commit_sidecar(self):
        from sas_te import installed_binary
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'tuntom'
            content=bytearray(64);content[:6]=b'\x7fELF\x02\x01';content[18:20]=(62).to_bytes(2,'little')
            binary.write_bytes(content)
            def candidate(value):
                return binary if str(value)=='/var/lib/tuntom-fabric/bin/tuntom' else Path(value)
            with patch('sas_te.Path',side_effect=candidate), patch('sas_te.subprocess.run',return_value=subprocess.CompletedProcess([],0,'tuntom 5.1.123\n','')) as run:
                metadata,_=installed_binary()
                self.assertEqual(metadata['version'],'5.1.123')
                self.assertNotIn('commit',metadata)
                self.assertEqual(run.call_args.args[0],[str(binary),'--version'])
            with patch('sas_te.Path',side_effect=candidate), patch('sas_te.subprocess.run',return_value=subprocess.CompletedProcess([],0,'usage: tuntom\n','')):
                with self.assertRaisesRegex(ValueError,'unsupported --version'): installed_binary()

    def test_account_creation_option_is_a_plan_not_a_resolve_mutation(self):
        base=Path(__file__).resolve().parents[1]/'templates'
        template=json.loads((base/'sas-te.example.json').read_text())
        location=json.loads((base/'sas-location.example.json').read_text())
        template['runtime']['create_user_if_needed']=True
        plan=json.loads(render(template,location)['plan.json'])
        self.assertTrue(plan['account_provisioning']['create_if_needed'])
        self.assertFalse(plan['live_enabled'])
        template['runtime']['create_user_if_needed']='yes'
        with self.assertRaises(ValueError): render(template,location)
