import base64
import hashlib
import json
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import sas_install
from sas_deployments import SASDeployments


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.instance='11111111-1111-4111-8111-111111111111'
        self.install=self.root/'microservices'/self.instance;self.install.mkdir(parents=True)
        self.work=self.root/'work';self.work.mkdir()
        self.profile={'schema':1,'runtime_uid':65532,'runtime_gid':65532,'sha256':'a'*64}
        self.location={'id':self.instance,'microservice_contract_version':3,'microservices_dir_exists':True,'origin':socket.gethostname(),
                       'microservices_dir':str(self.install),'namespace':'instance','namespace_exists':True,'work_dir':str(self.work),
                       'microservice_mode':'managed','microservice_execution_enabled':True,'microservice_capabilities':dict.fromkeys(('installation','supervision','status','stop'),True),
                       'microservice_rootfs':self.profile}
        self.config={'deployment_id':'b'*32,'instance_id':self.instance,'prefix':'10','origin':socket.gethostname(),'installation':str(self.install),
                     'namespace':'instance','profile':self.profile,'abi_versions':['GLIBC_2.17'],'binary':base64.b64encode(b'binary').decode(),
                     'sha256':hashlib.sha256(b'binary').hexdigest(),'version':'5.1.123','secret':'ab'*16,'script':'#!/bin/sh\nexit 0\n',
                     'registration':'10 tuntom 123 deployment='+('b'*32)+'\n'}
        manifest=self.root/'microservice-rootfs'/('a'*64);manifest.mkdir(parents=True)
        (manifest/'rootfs.json').write_text(json.dumps({'sha256':'a'*64,'abi_versions':{'libc':['GLIBC_2.17']}}))
        self.run_id=None;self.empty=True;self.fail_stop=False
        for name,value in [('trusted',lambda path:None),('command',self.command)]:
            patcher=patch.object(sas_install,name,value);patcher.start();self.addCleanup(patcher.stop)
        for name in ('chown','fchown'):
            patcher=patch.object(sas_install.os,name,lambda *args:None);patcher.start();self.addCleanup(patcher.stop)
        patcher=patch.object(sas_install.os,'geteuid',return_value=0);patcher.start();self.addCleanup(patcher.stop)
        actual_fstat=sas_install.os.fstat
        def root_fstat(fd):
            values=list(actual_fstat(fd));values[4]=0
            return sas_install.os.stat_result(values)
        patcher=patch.object(sas_install.os,'fstat',side_effect=root_fstat);patcher.start();self.addCleanup(patcher.stop)

    def command(self,args):
        if args[0]=='sas_which_instance':return dict(self.location)
        if 'stop' in args:
            if self.fail_stop:raise RuntimeError('stop timeout')
            self.empty=True
            return {'contract_version':3,'instance_id':self.instance,'prefix':'10','owner':'tuntom','run_id':self.run_id,'result':'stopped','process_group_empty':True}
        text,rows=sas_install.rows(self.install/'db.info')
        return {'contract_version':3,'instance_id':self.instance,'prefix':'10','instance_state':'present','verified_at':'2026-10-07T00:00:00Z',
                'owner':'tuntom' if self.run_id else rows.get('10'),'run_id':self.run_id,'registered':'10' in rows,
                'process_group_empty':self.empty,'state':('stopped' if self.empty else 'running') if self.run_id else 'never_started'}

    def action(self,name):return sas_install.execute({**self.config,'action':name})

    def test_prepare_register_and_verified_remove_preserve_other_owner_and_locks(self):
        original='# keep comment\n11 other-owner opaque\n'
        (self.install/'db.info').write_text(original)
        self.assertEqual(self.action('prepare')['state'],'prepared')
        self.assertEqual((self.install/'db.info').read_text(),original)
        lock_inode=(self.install/'10.lock').stat().st_ino
        self.assertEqual(self.action('activate')['state'],'registered')
        self.assertIn('10 tuntom',(self.install/'db.info').read_text())
        self.run_id='run-one';self.empty=False
        self.assertEqual(self.action('withdraw')['state'],'stopped')
        self.assertTrue((self.install/'tuntom/tuntom').exists())
        self.assertEqual(self.action('remove')['state'],'removed')
        self.assertFalse((self.install/'tuntom').exists())
        self.assertEqual((self.install/'db.info').read_text(),original)
        self.assertEqual((self.install/'10.lock').stat().st_ino,lock_inode)
        self.assertEqual(self.action('remove')['state'],'removed')

    def test_failed_stop_keeps_files_and_registration_withdrawn(self):
        self.action('prepare');self.action('activate')
        self.run_id='run-one';self.empty=False;self.fail_stop=True
        with self.assertRaisesRegex(RuntimeError,'timeout'):self.action('remove')
        self.assertTrue((self.install/'tuntom/tuntom').exists())
        self.assertNotIn('10 tuntom',(self.install/'db.info').read_text())

    def test_no_false_capability_or_foreign_prefix_bypass(self):
        self.location['microservice_capabilities']['stop']=False
        with self.assertRaisesRegex(RuntimeError,'capabilities'):self.action('prepare')
        self.assertFalse((self.install/'tuntom').exists())
        self.location['microservice_capabilities']['stop']=True
        (self.install/'db.info').write_text('10 other-owner\n')
        with self.assertRaisesRegex(RuntimeError,'another owner'):self.action('prepare')

    def test_missing_abi_blocks_payload_writes(self):
        self.config['abi_versions']=['GLIBC_99.0']
        with self.assertRaisesRegex(RuntimeError,'ABI'):self.action('prepare')
        self.assertFalse((self.install/'tuntom').exists())


class OrchestratorTests(unittest.TestCase):
    def test_lost_registration_response_preserves_listener(self):
        store=SASDeployments(None,Mock(),Mock(),None);self.addCleanup(store.close)
        store.update=Mock();store.local=Mock();store.remote=Mock(side_effect=[{'state':'prepared'},RuntimeError('lost response')])
        store.deploy({'deployment_id':'b'*32},'ab'*16)
        self.assertEqual([call.args[1] for call in store.local.call_args_list],['prepare','start'])
        self.assertEqual(store.update.call_args.args[1],'attention')

    def test_failed_sas_stop_never_removes_listener(self):
        store=SASDeployments(None,Mock(),Mock(),None);self.addCleanup(store.close)
        store.update=Mock();store.local=Mock();store.remote=Mock(side_effect=RuntimeError('unknown SAS run'))
        store.remove({'deployment_id':'b'*32})
        store.local.assert_not_called()
        self.assertEqual(store.update.call_args.args[1],'attention')


class NetworkTests(unittest.TestCase):
    def config(self):
        from sas_te import network_options
        return {'interface':'ut0','namespace':'instance','deployment_id':'b'*32,
                'network':network_options({'addresses':['10.80.0.2/30'],'routes':[{'destination':'10.20.0.0/16','via':'10.80.0.1'}]})}

    def test_default_makes_no_network_changes(self):
        with patch.object(sas_install.subprocess,'run') as run:
            sas_install.network({'network':{'addresses':[],'routes':[]}})
            run.assert_not_called()

    def test_foreign_interface_is_never_modified(self):
        result=Mock(returncode=0,stdout=json.dumps([{'ifname':'ut0','ifalias':'someone-else'}]))
        with patch.object(sas_install.subprocess,'run',return_value=result) as run:
            with self.assertRaisesRegex(RuntimeError,'not owned'):sas_install.network(self.config())
            self.assertEqual(run.call_count,1)

    def test_conflicting_route_blocks_before_tun_creation(self):
        with patch.object(sas_install.subprocess,'run',side_effect=[Mock(returncode=0,stdout='[]'),Mock(returncode=0,stdout='[{"dev":"eth0"}]')]) as run:
            with self.assertRaisesRegex(RuntimeError,'another interface'):sas_install.network(self.config())
            self.assertEqual(run.call_count,2)

    def test_owned_network_is_applied_in_instance_and_removed(self):
        calls=[]
        def ip(args,**kwargs):
            calls.append(args)
            return Mock(returncode=0,stdout='[]',stderr='')
        with patch.object(sas_install.subprocess,'run',side_effect=ip):sas_install.network(self.config())
        self.assertTrue(all(args[:3]==['ip','-n','instance'] for args in calls))
        self.assertIn(['ip','-n','instance','tuntap','add','dev','ut0','mode','tun'],calls)
        self.assertIn(['ip','-n','instance','address','replace','10.80.0.2/30','dev','ut0'],calls)
        with patch.object(sas_install.subprocess,'run',side_effect=[Mock(returncode=0,stdout=json.dumps([{'ifname':'ut0','ifalias':'fabric:sas:'+'b'*32}])),Mock(returncode=0)]) as run:
            sas_install.network(self.config(),remove=True)
            self.assertEqual(run.call_args.args[0],['ip','-n','instance','link','delete','dev','ut0'])

    def test_network_rejects_non_cidr_mismatched_gateway_and_shell_text(self):
        from sas_te import network_options
        for value in ({'addresses':['10.1.2.3'],'routes':[]},
                      {'addresses':[],'routes':[{'destination':'10.0.0.0/8','via':'::1'}]},
                      {'addresses':[],'routes':[{'destination':'0.0.0.0/0; touch /tmp/no'}]}):
            with self.assertRaises(ValueError):network_options(value)


class APITests(unittest.TestCase):
    def test_deploy_requires_admin_writes_and_audits(self):
        from test_tunnel_deployments import DeploymentAPITests
        from errors import APIError
        for role,writes in [('admin-ro',True),('admin',False)]:
            handler=DeploymentAPITests().handler('/api/v1/sas-te/deploy','POST',None,role=role,writes=writes)
            handler.server.sas_deployments=Mock()
            with self.assertRaises(APIError):handler.route()
            handler.server.sas_deployments.create.assert_not_called()
        handler=DeploymentAPITests().handler('/api/v1/sas-te/deploy','POST',None)
        handler.server.sas_deployments=Mock();handler.server.sas_deployments.create.return_value={'id':'b'*32}
        handler.body=lambda:{'request':'test'}
        self.assertEqual(handler.route()[0],200)
        self.assertEqual(handler.server.fabric.audit_event.call_args.args[0]['action'],'sas_te.deploy')

class ApplyNetworkTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.payload=Path(self.temp.name)
        self.old={'addresses':['10.9.0.1/24'],'routes':[{'destination':'10.20.0.0/16','metric':100}]}
        self.new={'addresses':['10.9.0.2/24'],'routes':[{'destination':'10.30.0.0/16','metric':200}]}
        self.config={'deployment_id':'b'*32,'interface':'tun0','namespace':'instance','peer':'192.0.2.1','network':self.old,'desired_network':self.new}
        self.calls=[];self.fail=False
        def run(args,**kwargs):
            self.calls.append(args)
            if self.fail and 'replace' in args:raise RuntimeError('SSH interrupted')
            if 'get' in args:return Mock(returncode=0,stdout='[{"dev":"uplink"}]',stderr='')
            if 'link' in args:return Mock(returncode=0,stdout=json.dumps([{'ifname':'tun0','ifalias':'fabric:sas:'+('b'*32)}]),stderr='')
            return Mock(returncode=0,stdout='[]',stderr='')
        p=patch.object(sas_install.subprocess,'run',side_effect=run);p.start();self.addCleanup(p.stop)
        p=patch.object(sas_install,'atomic',side_effect=lambda path,data,gid:path.write_bytes(data));p.start();self.addCleanup(p.stop)

    def test_reconcile_removes_old_and_repeated_apply_does_not_delete(self):
        sas_install.apply_network(self.config,self.payload)
        self.assertTrue(any('del' in c and '10.20.0.0/16' in c for c in self.calls))
        self.assertTrue(any('del' in c and '10.9.0.1/24' in c for c in self.calls))
        self.assertEqual(json.loads((self.payload/'.network.json').read_text()),self.new)
        self.calls.clear();sas_install.apply_network(self.config,self.payload)
        self.assertFalse(any('del' in c for c in self.calls))
        self.assertTrue(all(c[:3]==['ip','-n','instance'] for c in self.calls))

    def test_partial_update_journals_union_and_retry_can_remove_it(self):
        self.fail=True
        with self.assertRaises(RuntimeError):sas_install.apply_network(self.config,self.payload)
        ledger=json.loads((self.payload/'.network.json').read_text())
        self.assertIn(self.new['routes'][0],ledger['routes'])
        self.fail=False;self.calls.clear()
        sas_install.apply_network({**self.config,'desired_network':{'addresses':[],'routes':[]}},self.payload)
        self.assertTrue(any('del' in c and '10.30.0.0/16' in c for c in self.calls))

    def test_transport_route_rejected_before_mutation(self):
        self.config['desired_network']={'addresses':[],'routes':[{'destination':'0.0.0.0/0','metric':100}]}
        with self.assertRaisesRegex(RuntimeError,'transport IP'):sas_install.apply_network(self.config,self.payload)
        self.assertFalse((self.payload/'.network.json').exists())
        self.assertFalse(any('replace' in c or 'del' in c for c in self.calls))

class ApplyOrchestratorTests(unittest.TestCase):
    def test_revision_busy_gate_and_successful_persistence(self):
        store=SASDeployments(None,Mock(),Mock(allow_write=True),None);self.addCleanup(store.close)
        ident='c'*32
        config={'deployment_id':ident,'instance_id':'instance','prefix':'10','origin':'host','name':'test','network':{'addresses':[],'routes':[]}}
        store.db.execute('INSERT INTO sas_deployment VALUES(?,?,?,?,?,?)',(ident,json.dumps(config),'running','[]',None,'now'));store.db.commit()
        store.launch=Mock();store.remote=Mock(return_value={})
        body={'network':{'addresses':['10.8.0.1/24'],'routes':[]},'expected_revision':'stale'}
        with self.assertRaisesRegex(RuntimeError,'changed'):store.apply(ident,body)
        body['expected_revision']=store.get(ident)['network_revision']
        store.apply(ident,body)
        with self.assertRaisesRegex(RuntimeError,'active'):store.apply(ident,body)
        store.apply_worker(config,body['network'])
        self.assertEqual(store.get(ident,True)['network'],body['network'])
        self.assertNotEqual(store.get(ident)['network_revision'],body['expected_revision'])

    def test_apply_api_requires_admin_and_writes(self):
        from test_tunnel_deployments import DeploymentAPITests
        from errors import APIError
        for role,writes in [('admin-ro',True),('admin',False)]:
            handler=DeploymentAPITests().handler('/api/v1/sas-deployments/'+('b'*32)+'/apply','POST',None,role=role,writes=writes)
            handler.server.sas_deployments=Mock()
            with self.assertRaises(APIError):handler.route()
            handler.server.sas_deployments.apply.assert_not_called()

class TransportNamespaceTests(unittest.TestCase):
    def test_separate_transport_accepts_dataplane_route_and_same_device_name(self):
        config={'namespace':'instance','_transport_namespace':'uplink','_transport_separate':True,'peer':'192.168.88.10','interface':'ut0'}
        with patch.object(sas_install.subprocess,'run',return_value=Mock(returncode=0,stdout='[{"dev":"ut0"}]')) as run:
            sas_install.check_transport(config,{'routes':[{'destination':'192.168.0.0/16'}]})
        self.assertEqual(run.call_args.args[0][:3],['ip','-n','uplink'])

    def test_missing_route_names_execution_namespace_without_claiming_loop(self):
        config={'namespace':'instance','peer':'192.168.88.10','interface':'ut0'}
        with patch.object(sas_install.subprocess,'run',return_value=Mock(returncode=2,stdout='')):
            with self.assertRaisesRegex(RuntimeError,'No route.*instance.*no separate'):
                sas_install.check_transport(config,{'routes':[]})

    def test_host_execution_ignores_profile_transport_namespace(self):
        context=sas_install.transport_context({'namespace':'instance','transport_namespace':'profile-veth'})
        self.assertEqual(context,{'_transport_namespace':'host','_transport_separate':True})
        config={**context,'namespace':'instance','peer':'192.168.88.10','interface':'ut0'}
        with patch.object(sas_install.subprocess,'run',return_value=Mock(returncode=0,stdout='[{"dev":"enp2s0"}]')) as run:
            sas_install.check_transport(config,{'routes':[{'destination':'192.168.0.0/16'}]})
        self.assertEqual(run.call_args.args[0],['nsenter','--net=/proc/1/ns/net','ip','-4','-j','route','get','192.168.88.10'])


class NetworkAuditTests(unittest.TestCase):
    def test_real_audit_accepts_valid_diff_and_rejects_unvalidated_fields(self):
        from server import Fabric
        from errors import APIError
        fabric=Mock();fabric.journal=Mock()
        event={'action':'sas_te.apply','target':'c'*32,'outcome':'succeeded','details':{'before':{'addresses':[],'routes':[]},'after':{'addresses':['10.0.0.1/24'],'routes':[]}}}
        Fabric.audit_event(fabric,event)
        self.assertEqual(fabric.journal.append.call_args.args[3],event['details'])
        with self.assertRaises(APIError):Fabric.audit_event(fabric,{**event,'action':'sas_te.deploy'})
        with self.assertRaises(APIError):Fabric.audit_event(fabric,{**event,'details':{'after':{'secret':'not network metadata'}}})
