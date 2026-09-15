#!/usr/bin/env python3
"""Fast tests: ownership, selection, credentials, teardown and unit ordering."""
import argparse
import copy
import csv
import fcntl
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

DEPLOY=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(DEPLOY/'helpers'),str(DEPLOY/'ansible'/'module_utils')]
import controller
import bootstrap
import model
import runtime
import tuntom_node as node
from units import render


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='tt-unit-')
        self.addCleanup(self.temp.cleanup)
        self.work=Path(self.temp.name)
        self.instance=self.work/'instances'/'tunnels'/'psx2'
        self.instance.mkdir(parents=True)
        self.data={'schema':1,'tunnel_id':42,'count':2,
                   'client':{'host':'local','peer_address':'192.0.2.2'},'server':{'host':'remote'}}
        (self.instance/'instance.yml').write_text('tuntom_instance: {}\n')

    def item(self,data=None):
        return model.Instance('tunnel','psx2',self.instance,data or self.data)

    def spec(self):
        return dict(self.item().compile('local'),owner='test-owner',operation_id='test-operation')


class ModelTests(Fixture):
    def test_group_matches_mk_contract(self):
        self.data['count']=64
        item=self.item()
        last=item.compile('remote')['endpoints'][-1]
        self.assertEqual(last['id'],'42_63s')
        self.assertEqual(last['env']['TUNTOM_LOCAL_IP'],'10.254.42.254')
        self.assertEqual(last['env']['TUNTOM_LOCAL_IPV6'],'fd42::10:254:42:fe')
        self.assertEqual(last['env']['TUNTOM_UDP_PORT'],str(40000+42+63*256))
        rows=list(csv.DictReader(io.StringIO(item.manifest()),delimiter='\t'))
        self.assertEqual(len(rows),64)
        self.assertEqual(rows[-1]['server_if'],'ut42_63s')
        self.assertEqual(rows[-1]['server_ipv4'],last['env']['TUNTOM_LOCAL_IP'])

    def test_switch_only_and_no_address(self):
        self.data['no_address']=True
        self.data['client'].update(switch_socket='/run/fabric.sock',port_id='psx2',label=42)
        e=self.item().compile('local')['endpoints'][1]
        self.assertEqual(e['interface'],'')
        self.assertEqual(e['env']['TUNTOM_LOCAL_IP'],'')
        self.assertEqual(e['env']['TUNTOM_SWITCH_PORT_ID'],'psx2_1')

    def test_reject_dangerous_paths_and_ambiguous_yaml(self):
        with self.assertRaises(model.Invalid): model.inside(self.instance,'../other/key')
        (self.instance/'link').symlink_to('/etc/passwd')
        with self.assertRaises(model.Invalid): model.inside(self.instance,'link')
        (self.work/'bad.yml').write_text('a: 1\na: 2\n')
        with self.assertRaises(model.Invalid): model.read_yaml(self.work/'bad.yml')
        for path in ('relative.sock','/run/../other.sock'):
            with self.assertRaises(model.Invalid): model.socket_path(path)

    def test_secrets_never_enter_public_spec_or_assets(self):
        secret='a'*32
        (self.instance/'secrets').mkdir()
        (self.instance/'secrets/master.key').write_text(secret)
        before=self.item().compile('local')
        (self.instance/'secrets/master.key').write_text('b'*32)
        self.assertEqual(before['source_digest'],self.item().compile('local')['source_digest'])
        self.assertNotIn(secret,json.dumps(before))
        controller.prepare_assets(self.item(),self.work/'assets')
        self.assertFalse((self.work/'assets/secrets').exists())

    def test_invalid_ranges_and_fields(self):
        for change in ({'count':65},{'count':True},{'unknown':1},{'mark':1},{'secret_file':'elsewhere/key'}):
            with self.subTest(change=change), self.assertRaises(model.Invalid):
                self.item(dict(self.data,**change))
        with self.assertRaisesRegex(model.Invalid,'reserved'):
            self.item(dict(self.data,count=1,table=254))

    def test_systemd_units_verify_and_keep_secrets_out(self):
        spec=self.spec()
        units=render(spec)
        for name,content in units.items(): (self.work/name).write_text(content)
        env=dict(os.environ,SYSTEMD_LOG_LEVEL='warning')
        result=subprocess.run(['systemd-analyze','verify',*[str(self.work/name) for name in units]],text=True,capture_output=True,env=env)
        self.assertEqual(result.returncode,0,result.stderr)
        unit=units[spec['endpoints'][0]['unit']]
        self.assertIn('LoadCredential=master:',unit)
        self.assertNotIn('TUNTOM_SECRET=',unit)
        self.assertIn('Restart=on-failure',unit)
        self.assertNotIn('Wants=',units['tuntom-tunnel-psx2-ready.service'])


class ControllerTests(Fixture):
    def test_inventory_is_locked_before_reading_hosts(self):
        def hosts(directory):
            fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY)
            try:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            finally: os.close(fd)
            return {'local':{},'remote':{}}
        with patch.object(controller,'load_instances',return_value=[self.item()]),patch.object(controller,'inventory_hosts',side_effect=hosts),patch.object(controller,'Session') as session:
            controller.main(['--info','--inventory',str(self.work)])
            session.return_value.inspect.assert_called_once()

    def session(self,operation='update'):
        args=controller.parser().parse_args(['--'+operation,'--tunnel','psx2','--inventory',str(self.work)])
        s=controller.Session(args,[self.item()],{'local':{'ansible_connection':'local'},'remote':{}},self.work/'scratch')
        s.work.mkdir(exist_ok=True)
        for host,jobs in s.payload.items():
            for job in jobs:
                spec=copy.deepcopy(job['spec'])
                report={'installed':True,'complete':True,'spec':spec,'legacy':None,'machine_id':host,
                        'services':{u:{'ActiveState':'active','UnitFileState':'disabled'} for u in node.unit_names(spec)}}
                report['services'][spec['endpoints'][1]['unit']]['ActiveState']='inactive'
                s.reports[host,job['key']]=report
                job['before']=report
        return s

    def test_binary_update_uses_deployed_arguments(self):
        s=self.session()
        for jobs in s.payload.values(): jobs[0]['spec']['endpoints'][0]['args'].append('--changed-central')
        s.plan()
        with patch.object(controller,'source_archive'),patch.object(controller,'prepare_assets',side_effect=AssertionError('must not read config during binary update')):
            s.prepare()
        for jobs in s.payload.values():
            job=jobs[0]
            self.assertNotIn('--changed-central',job['spec']['endpoints'][0]['args'])
            self.assertEqual(job['active_members'],[job['spec']['endpoints'][0]['unit']])

    def test_unreachable_wipe_refuses_before_remember_or_mutation(self):
        s=self.session('wipe')
        s.reports['remote','tunnel-psx2']={'reachable':False,'error':'unreachable'}
        with self.assertRaisesRegex(model.Invalid,'no changes made'): s.plan()
        self.assertFalse((self.work/'.state').exists())

    def test_partial_host_remember_preserves_other_machine_id(self):
        s=self.session('stop')
        s.identities['tunnel-psx2']['machine_ids']={'remote':'previous'}
        del s.payload['remote']
        s.remember()
        self.assertEqual(s.identities['tunnel-psx2']['machine_ids']['remote'],'previous')

    def test_wipe_central_deletes_only_selected_tree_and_secrets(self):
        s=self.session('wipe')
        (self.work/'hosts.yml').write_text('all:\n  hosts:\n    local: {}\n    remote: {}\n')
        (self.instance/'instance.yml').write_text('tuntom_instance:\n  schema: 1\n  tunnel_id: 42\n  client: {host: local, peer_address: remote}\n  server: {host: remote}\n')
        other=self.instance.parent/'other'
        other.mkdir()
        (other/'instance.yml').write_text((self.instance/'instance.yml').read_text().replace('42','43'))
        (self.instance/'secrets').mkdir()
        (self.instance/'secrets/key').write_text('secret')
        (self.instance/'manual.txt').write_text('manual')
        s.remember()
        s.wipe_central()
        self.assertFalse(self.instance.exists())
        self.assertFalse((self.work/'.state/tunnel-psx2').exists())
        self.assertTrue(other.exists())
        self.assertTrue((self.work/'hosts.yml').exists())

    def test_importer_preserves_flags_and_never_overwrites_definition(self):
        inventory=self.work/'imported'
        with patch.dict(os.environ,{'TUNTOM_SECRET':'a'*32,'TUNTOM_MTU':'9000'},clear=True):
            bootstrap.main(['42','root@example.invalid','--inventory',str(inventory),'--name','psx2','--count','3','--prepare-only','--no-address','--crypto-auth-only','--all-tools','--client-switch','/run/fabric.sock','psx2','42'])
            item=model.load_instances(inventory)[0]
            self.assertEqual(item.definition['count'],3)
            self.assertEqual(item.mtu,9000)
            self.assertTrue(item.definition['all_tools'])
            self.assertTrue(item.definition['no_address'])
            key=item.directory/'secrets/master.key'
            self.assertEqual(key.stat().st_mode & 0o777,0o600)
            with self.assertRaisesRegex(model.Invalid,'already exists'):
                bootstrap.main(['42','root@example.invalid','--inventory',str(inventory),'--name','psx2','--prepare-only'])


class NodeTests(Fixture):
    def setUp(self):
        super().setUp()
        for variable,path in [('BASE',self.work/'installed'),('RUN',self.work/'run'),('UNITS',self.work/'units'),('STAGING',self.work/'staging'),('LOCK',self.work/'lock')]:
            self.addCleanup(patch.stopall)
            patch.object(node,variable,path).start()
            if variable!='LOCK': path.mkdir()
        self.specification=self.spec()
        self.specification.update(root=str(node.BASE/'tunnel-psx2'),run=str(node.RUN/'tunnel-psx2'))

    def test_foreign_instance_root_and_unit_never_overwritten(self):
        root=Path(self.specification['root'])
        root.mkdir()
        with self.assertRaisesRegex(RuntimeError,'unclaimed'): node.inspect(self.specification)
        root.rmdir()
        unit=self.specification['endpoints'][0]['unit']
        (node.UNITS/unit).write_text('[Unit]\nDescription=someone else\n')
        with patch.object(node,'state',return_value={'LoadState':'not-found'}):
            with self.assertRaisesRegex(RuntimeError,'foreign unit'): node.inspect(self.specification)

    def test_stopped_other_deployment_still_reserves_resources(self):
        path=node.BASE/'tunnel-other'
        path.mkdir()
        other=dict(self.specification,key='tunnel-other')
        (path/'installed.json').write_text(json.dumps({'spec':other}))
        with self.assertRaisesRegex(RuntimeError,'another deployment'): node.assert_free(self.specification)

    def test_existing_unmanaged_routes_or_marks_block_install(self):
        for conflict in ('route','mark','chain'):
            with self.subTest(conflict=conflict):
                def run(argv):
                    code,stdout=1,''
                    if argv[0]=='iptables' and conflict=='chain' and argv[-1]=='TUNTOM_42_F':
                        self.assertEqual(argv[2],'mangle')
                        code=0
                    if 'rule' in argv:
                        code=0
                        stdout=json.dumps([{'fwmark':'0x2a0000','fwmask':'0xffff0000','table':'1234'}] if conflict=='mark' else [])
                    if 'route' in argv:
                        code,stdout=(0,'[{"dst":"default"}]') if conflict=='route' else (2,'')
                    return subprocess.CompletedProcess(argv,code,stdout,'')
                with patch.object(node,'run',side_effect=run),patch.object(node,'check_socket'):
                    with self.assertRaisesRegex(RuntimeError,'already'): node.assert_free(self.specification)

    def test_wipe_staging_matches_owner_and_exact_key(self):
        spec=self.specification
        spec['operation_id']='00000000-0000-0000-0000-000000000001'
        node.perform('stage',spec)
        other=dict(spec,key='tunnel-psx2-other',root=str(node.BASE/'tunnel-psx2-other'),run=str(node.RUN/'tunnel-psx2-other'))
        node.perform('stage',other)
        self.assertEqual(len(node.stages(spec)),1)
        node.perform('wipe-staging',spec)
        self.assertEqual(node.stages(spec),[])
        self.assertEqual(len(node.stages(other)),1)

    def test_not_found_systemctl_status_is_not_connection_error(self):
        result=subprocess.CompletedProcess([],1,'LoadState=not-found\nActiveState=inactive\n','')
        with patch.object(node,'run',return_value=result):
            self.assertEqual(node.state('missing.service')['LoadState'],'not-found')

    def test_claim_is_visible_with_ownership_even_before_install_finishes(self):
        spec=self.specification
        node.write_metadata(spec,claim=True)
        data=json.loads((Path(spec['root'])/'installed.json').read_text())
        self.assertEqual(data['owner'],spec['owner'])
        self.assertFalse(data['complete'])
        with self.assertRaisesRegex(RuntimeError,'unowned'):
            node.write_metadata(dict(spec,owner='other-owner'),claim=True)


class RuntimeTests(Fixture):
    def runtime(self):
        spec=self.spec()
        spec.update(root=str(self.work),run=str(self.work/'run'))
        for e in spec['endpoints']:
            e['control']=str(self.work/(e['id']+'.control'))
            e['stats']=str(self.work/(e['id']+'.stats'))
        (self.work/'config.json').write_text(json.dumps(spec))
        return runtime.Runtime(self.work)

    def test_failed_cleanup_keeps_saved_hooks_for_retry(self):
        rt=self.runtime()
        snapshot=rt.active/'42c'
        snapshot.mkdir(parents=True)
        (snapshot/'endpoint.json').write_text(json.dumps(rt.config['endpoints'][0]))
        with patch.object(rt,'stop'),patch.object(rt,'net',side_effect=RuntimeError('network failure')),patch.object(rt,'hook'),patch.object(runtime,'idle_socket'):
            with self.assertRaisesRegex(RuntimeError,'network failure'): rt.cleanup('42c')
        self.assertTrue(snapshot.exists())
        with patch.object(rt,'stop'),patch.object(rt,'net'),patch.object(rt,'hook'),patch.object(runtime,'idle_socket'):
            rt.cleanup('42c')
        self.assertFalse(snapshot.exists())

    def test_legacy_rules_are_data_not_shell(self):
        rules=self.work/'rules'
        rules.write_text('route in:42=out:7\n# comment\ndefault-back off\n')
        self.assertEqual(runtime.switch_rules(['--rules-file',str(rules)],'single'),['--route','in:42=out:7','--default-back=off'])
        rules.write_text('exec dangerous\n')
        with self.assertRaises(RuntimeError): runtime.switch_rules(['--rules-file',str(rules)],'mp')

    def test_tunnel_readiness_uses_actual_control_protocol(self):
        rt=self.runtime()
        result=subprocess.CompletedProcess([],0,'format=txt\ntunnel_id=42\nmode=client\n','')
        with patch.object(runtime,'command',return_value=result): rt.ready(rt.config['endpoints'][0])


if __name__=='__main__': unittest.main()
