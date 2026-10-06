from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tunnel_deployments import TunnelDeployments, render_bundle, validate
import deployment_cleanup as cleanup
from test_tunnel_deployments import Endpoints, draft, LocalFabric, switch_draft
from test_via_deployments import via_draft


def systemd_draft(via=False, boot=False):
    value = via_draft() if via else draft()
    for key in ('side_a','side_b'):
        value[key].update(runtime='systemd', autostart=boot)
    return value


class FakeSystemd:
    def __init__(self, directory):
        self.directory = directory; self.calls = []; self.states = {}; self.fail_stop = False
    def __call__(self, *args, check=True):
        self.calls.append(args)
        action = args[0]
        if action == 'show':
            name = args[1]; dest = self.directory / name
            loaded = dest.exists() or name in self.states
            load = 'loaded' if loaded else 'not-found'
            state = self.states.get(name, 'inactive')
            if '--value' in args:
                return subprocess.CompletedProcess([],0 if loaded else 1,load+'\n','')
            values = {'FragmentPath':str(dest) if loaded else '', 'DropInPaths':'', 'LoadState':load,
                      'ActiveState':state,'MainPID':'123' if state=='active' and name.endswith('.service') else '0','ControlPID':'0'}
            return subprocess.CompletedProcess([],0 if loaded else 1,'\n'.join(k+'='+v for k,v in values.items()),'')
        if action == 'start':
            for file in self.directory.iterdir():
                if file.is_file(): self.states[file.name] = 'active'
        if action == 'stop' and not self.fail_stop: self.states[args[1]] = 'inactive'
        if action == 'enable':
            wants = self.directory/'multi-user.target.wants'; wants.mkdir(exist_ok=True)
            (wants/args[1]).symlink_to(self.directory/args[1])
        if action == 'disable':
            (self.directory/'multi-user.target.wants'/args[1]).unlink(missing_ok=True)
        if action == 'daemon-reload':
            self.states = {k:v for k,v in self.states.items() if (self.directory/k).exists()}
        return subprocess.CompletedProcess([],0,'','')


class SystemdDeploymentTests(unittest.TestCase):
    def bundle(self, via=False, boot=False, root=None):
        value = systemd_draft(via,boot)
        config, _ = validate(value); config['deployment_id'] = 'd'*32
        return config, render_bundle(config, {config[k]['endpoint_id']:'host' for k in ('side_a','side_b')}, 'https://example.test/repo.git', root=root)

    def test_data_units_and_via_dependencies_verify_offline(self):
        for via in (False,True):
            with self.subTest(via=via), tempfile.TemporaryDirectory() as directory:
                config,bundle = self.bundle(via=via,boot=True)
                units=[]
                for name,content in bundle.items():
                    if name.startswith('side_a/systemd/') and not name.endswith('.json'):
                        path=Path(directory)/Path(name).name;path.write_text(content);units.append(str(path))
                    if name.endswith('.sh'):
                        result=subprocess.run(['bash','-n'],input=content,text=True,capture_output=True)
                        self.assertEqual(result.returncode,0,name+result.stderr)
                result=subprocess.run(['systemd-analyze','verify',*units],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                manifest=json.loads(bundle['side_a/systemd/manifest.json'])
                self.assertEqual(sum(n.endswith('.service') for n in manifest['units']), 16 if via else 4)
                for name in manifest['units']:
                    if name.endswith('.service'):
                        unit=bundle['side_a/systemd/'+name]
                        self.assertIn('KillMode=control-group',unit)
                        self.assertIn('PartOf='+manifest['target'],unit)
                self.assertNotIn('screen ',bundle['side_a/scripts/40-start.sh'])
                self.assertNotIn('systemd enable',bundle['side_a/scripts/30-install-config.sh'])
                self.assertIn('systemd enable',bundle['side_a/scripts/60-enable.sh'])
                if via:
                    self.assertIn('Relay socket 0 not ready',bundle['side_a/runtime/run-via-0.sh'])
                    unit=bundle['side_a/systemd/'+cleanup.systemd_prefix('d'*32)+'_via_0.service']
                    self.assertIn('_231c.service',unit)

    def test_runtime_validation_preserves_screen_default(self):
        self.assertEqual(validate(draft())[0]['side_a']['runtime'],'screen')
        for value in ('other',None):
            data=draft();data['side_a']['runtime']=value
            with self.assertRaises(ValueError):validate(data)
        data=draft();data['side_a']['autostart']=True
        with self.assertRaises(ValueError):validate(data)
        data=systemd_draft();data['side_a']['autostart']='yes'
        with self.assertRaises(ValueError):validate(data)

    def test_autostart_is_enabled_only_after_both_sides_verify(self):
        endpoints=Endpoints();store=TunnelDeployments(None,endpoints);self.addCleanup(store.close)
        row=store.create(systemd_draft(boot=True));store.deploy(row['id'])
        calls=[v[1] for v in endpoints.runs]
        enables=[i for i,c in enumerate(calls) if c.endswith('/60-enable.sh')]
        verifies=[i for i,c in enumerate(calls) if c.endswith('/50-verify.sh')]
        self.assertEqual(len(enables),2)
        self.assertGreater(min(enables),max(verifies))

    def test_enable_failure_rolls_back_both_sides(self):
        class Failing(Endpoints):
            def run(self,*args,**kwargs):
                result=super().run(*args,**kwargs)
                if args[1].endswith('/60-enable.sh'):raise OSError('enable failed')
                return result
        endpoints=Failing();store=TunnelDeployments(None,endpoints);self.addCleanup(store.close)
        row=store.create(systemd_draft(boot=True))
        with self.assertRaisesRegex(RuntimeError,'enable failed'):store.deploy(row['id'])
        self.assertEqual(sum(c[1].endswith('/99-rollback.sh') for c in endpoints.runs),2)
        self.assertEqual(store.get(row['id'])['status'],'failed')

    def installed_fixture(self, directory):
        root=Path(directory)/'deployment';root.mkdir()
        config,bundle=self.bundle(root=str(root))
        for name,content in bundle.items():
            if name.startswith('side_a/'):
                path=root/name[len('side_a/'):];path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)
        (root/'.fabric-deployment-id').write_text(config['deployment_id'])
        (root/'secrets').mkdir();(root/'secrets/master.env').write_text('secret-test')
        units=Path(directory)/'units';units.mkdir()
        manager=FakeSystemd(units)
        return root,config,units,manager

    def test_undeploy_stops_disables_removes_units_and_erases_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root,config,units,manager=self.installed_fixture(directory)
            unrelated=units/'other.service';unrelated.write_text('keep')
            original=Path.iterdir
            with patch.object(cleanup,'SYSTEMD_UNITS',units),patch.object(cleanup,'systemctl',manager),patch.object(Path,'iterdir',lambda p:iter([]) if p==Path('/proc') else original(p)):
                cleanup.systemd_action(root,config['deployment_id'],'install')
                cleanup.systemd_action(root,config['deployment_id'],'enable')
                cleanup.systemd_action(root,config['deployment_id'],'start')
                with (root/'secrets/master.env').open('rb') as key:
                    cleanup.undeploy(root,config['deployment_id'],'server',['231','231_1','231_2','231_3'])
                    key.seek(0);self.assertEqual(key.read(),b'')
                self.assertFalse(root.exists());self.assertEqual(unrelated.read_text(),'keep')
                self.assertEqual(list(units.glob('tuntomfabric_*')),[])
                self.assertEqual(list((units/'multi-user.target.wants').iterdir()),[])
                cleanup.undeploy(root,config['deployment_id'],'server',['231'])
                actions=[c[0] for c in manager.calls]
                self.assertLess(actions.index('disable'),actions.index('stop'))

    def test_foreign_unit_and_failed_stop_retain_keys_for_retry(self):
        for failure in ('foreign','stop'):
            with self.subTest(failure=failure),tempfile.TemporaryDirectory() as directory:
                root,config,units,manager=self.installed_fixture(directory)
                with patch.object(cleanup,'SYSTEMD_UNITS',units),patch.object(cleanup,'systemctl',manager):
                    cleanup.systemd_action(root,config['deployment_id'],'install')
                    cleanup.systemd_action(root,config['deployment_id'],'start')
                    if failure=='foreign':next(units.glob('*.service')).write_text('foreign content')
                    else:manager.fail_stop=True
                    with self.assertRaises(RuntimeError):cleanup.undeploy(root,config['deployment_id'],'server',['231'])
                    self.assertEqual((root/'secrets/master.env').read_text(),'secret-test')
                    self.assertTrue(list(units.glob('*.service')))

    def test_partial_install_can_be_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            root,config,units,manager=self.installed_fixture(directory)
            manifest=json.loads((root/'systemd/manifest.json').read_text())
            name=next(n for n in manifest['units'] if n.endswith('.service'))
            (units/name).write_text((root/'systemd'/name).read_text())
            original=Path.iterdir
            with patch.object(cleanup,'SYSTEMD_UNITS',units),patch.object(cleanup,'systemctl',manager),patch.object(Path,'iterdir',lambda p:iter([]) if p==Path('/proc') else original(p)):
                cleanup.undeploy(root,config['deployment_id'],'server',['231'])
            self.assertFalse(root.exists());self.assertEqual(list(units.iterdir()),[])

    def test_runtime_readout_is_bounded_and_secret_redacted(self):
        class Logs(Endpoints):
            def run(self,*args,**kwargs):
                return 'Id=' + cleanup.systemd_prefix('d'*32) + '_231.service\nTUNTOM_SECRET=' + 'ab'*16 + '\nActiveState=active\n'
        store=TunnelDeployments(None,Logs());self.addCleanup(store.close)
        row=store.create(systemd_draft());store._set_status(row['id'],'running')
        result=store.runtime_status(row['id'])
        self.assertEqual(len(result['sides']),2)
        for side in result['sides'].values():
            self.assertIn('ActiveState=active',side['output'])
            self.assertIn(cleanup.systemd_prefix('d'*32),side['output'])
            self.assertNotIn('ab'*16,side['output'])

    def test_missing_root_does_not_claim_removed_while_units_remain(self):
        with tempfile.TemporaryDirectory() as directory:
            units=Path(directory)/'units';units.mkdir()
            (units/(cleanup.systemd_prefix('d'*32)+'.service')).write_text('keep')
            with patch.object(cleanup,'SYSTEMD_UNITS',units):
                with self.assertRaisesRegex(RuntimeError,'units remain'):
                    cleanup.undeploy(Path(directory)/'gone','d'*32,'server',['1'])
            self.assertEqual(len(list(units.iterdir())),1)

    def test_read_only_local_status_does_not_require_writes(self):
        from types import SimpleNamespace
        from server import Fabric
        from journal import actor
        import switch_deployments as local
        token=actor.set({'username':'reader','role':'admin-ro'})
        self.addCleanup(actor.reset,token)
        with patch.object(local,'execute',return_value={'output':'ActiveState=active'}) as run:
            result=Fabric.switch_deployment(SimpleNamespace(allow_write=False),{'action':'status','deployment_id':'d'*32})
        self.assertIn('active',result['output']);run.assert_called_once()
