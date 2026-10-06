import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from deployment_access import grant_traversal

class AccessTests(TestCase):
    def test_mask_expansion_does_not_grant_execute_to_other_users(self):
        acl = 'user::rwx\nuser:123:rwx\nuser:456:rw-\ngroup::rwx\nmask::rw-\nother::---\n'
        with patch('deployment_access.subprocess.run', return_value=SimpleNamespace(stdout=acl)) as run:
            grant_traversal('/example',456)
        modification=run.call_args.args[0][3]
        self.assertIn('user:123:rw-',modification)
        self.assertIn('group::rw-',modification)
        self.assertTrue(modification.endswith('user:456:rwx,mask::rwx'))

    def test_private_root_gets_only_traverse_for_runtime_user(self):
        acl='user::rwx\ngroup::---\nother::---\n'
        with patch('deployment_access.subprocess.run', return_value=SimpleNamespace(stdout=acl)) as run, patch('deployment_access.pwd.getpwuid',return_value=SimpleNamespace(pw_name='tuntom',pw_gid=456)), patch('deployment_access.os.getgrouplist',return_value=[456]), patch('deployment_access.os.stat',return_value=SimpleNamespace(st_gid=0)):
            grant_traversal('/example',456)
        self.assertTrue(run.call_args.args[0][3].endswith('user:456:--x,mask::--x'))

    def test_prepare_checks_actual_traversal_and_skips_existing_access(self):
        from deployment_access import prepare
        with patch('deployment_access.pwd.getpwnam',return_value=SimpleNamespace(pw_uid=456)), patch('deployment_access.subprocess.run',return_value=SimpleNamespace(returncode=0)) as run, patch('deployment_access.grant_traversal') as grant:
            prepare('/opt/tuntom')
        grant.assert_not_called()
        self.assertEqual(run.call_count,3)
        for call in run.call_args_list:
            self.assertIn('import os,sys; os.chdir(sys.argv[1])',call.args[0])
