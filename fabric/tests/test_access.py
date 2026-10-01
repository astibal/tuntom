import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from discovery import kind_of, options
from telemetry import health, access_detail

class AccessTests(unittest.TestCase):
    def endpoint(self, role='client'):
        return SimpleNamespace(kind='tunnel', state='S', switch_socket='', source='local', access_role=role, access_parent_id='')
    def sample(self, **metrics):
        return {'status':'reachable', 'metrics':{'session_ready':'1', 'udp_rx_bps_5s':'0', 'udp_tx_bps_5s':'0', **metrics}, 'changes':{'ready':True,'counters':{}}}
    def test_worker_detection_and_public_options(self):
        self.assertEqual(kind_of('/proc/self/exe',['/proc/self/exe','access-worker'],'exe'),'tunnel')
        self.assertEqual(kind_of('/opt/tuntom','/opt/tuntom access-worker'.split(),'tuntom'),'tunnel')
        self.assertEqual(options(['tuntom','server','33','--auth-command','/bin/helper','--auth-username','alice','--password','secret']),{'auth-command':'/bin/helper'})
    def test_legacy_does_not_invent_auth(self):
        self.assertIsNone(access_detail(self.endpoint(''), self.sample()))
        self.assertIsNone(access_detail(self.endpoint(''), self.sample(access_role='none',access_telemetry_version='1')))
        known=access_detail(self.endpoint('worker'), {'status':'unavailable','metrics':{'access_data_allowed':'1'}})
        self.assertIsNone(known['data_allowed'])
    def test_handshake_is_not_permission_to_send_data(self):
        result=health(self.endpoint(),self.sample(access_telemetry_version='1',access_role='client',access_auth_state='waiting_result',access_data_allowed='0'))
        self.assertEqual(result['level'],'unknown')
        self.assertEqual(result['checks'][-1]['code'],'access_pending')
        result=health(self.endpoint(),self.sample(access_auth_state='rejected',access_data_allowed='0'))
        self.assertEqual(result['level'],'warn')
    def test_listener_denial_is_not_service_fault_and_capacity_is(self):
        result=health(self.endpoint('listener'),self.sample(session_ready='0',access_telemetry_version='1',access_auth_state='rejected',access_data_allowed='0',access_children='1',access_children_limit='256'))
        self.assertEqual(result['level'],'ok')
        result=health(self.endpoint('listener'),self.sample(access_telemetry_version='1',access_children='256',access_children_limit='256'))
        self.assertEqual(result['checks'][-1]['code'],'access_capacity')
    def test_config_failure_visible(self):
        result=health(self.endpoint(),self.sample(access_config_state='failed',access_data_allowed='1'))
        self.assertEqual(result['checks'][-1]['code'],'access_config_failed')
        self.assertEqual(result['level'],'warn')

    def test_proc_parent_identity_and_pid_reuse(self):
        import tempfile
        from discovery import discover
        with tempfile.TemporaryDirectory() as root:
            proc=Path(root); (proc/'sys/kernel/random').mkdir(parents=True)
            (proc/'sys/kernel/random/boot_id').write_text('boot');(proc/'uptime').write_text('1000 0')
            def process(pid,parent,start,args):
                d=proc/str(pid);d.mkdir(exist_ok=True);(d/'comm').write_text('tuntom')
                (d/'cmdline').write_bytes(b'\0'.join(arg.encode() for arg in args)+b'\0')
                fields=['S']+['0']*49;fields[1]=str(parent);fields[19]=str(start)
                (d/'stat').write_text(f'{pid} (tuntom) '+' '.join(fields))
            process(10,1,100,['/tmp/tuntom','server','33','-','--auth-command','/tmp/helper'])
            process(20,10,200,['/proc/self/exe','access-worker'])
            rows,_=discover(proc);parent,worker=rows
            self.assertEqual(worker.access_parent_id,parent.id)
            self.assertEqual(worker.role,'access-worker')
            self.assertEqual(worker.name,'access-worker · 20')
            process(10,1,300,['/tmp/tuntom','server','33','-','--auth-command','/tmp/helper'])
            self.assertEqual(discover(proc)[0][1].access_parent_id,'')
