import sys
from pathlib import Path
from dataclasses import replace
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from discovery import Endpoint
from network import Network, parse_discovery
from server import parse_stats, Fabric
from errors import APIError

HEADER = 'path\tstate\tinstance\tcomponent\tcapabilities\n'
SELF, LOCAL, REMOTE = 'a'*32, 'b'*32, 'c'*32

def row(instance=REMOTE, path='port:edge/peer', component='tunnel', state='FOUND'):
    return f'{path}\t{state}\t{instance}\t{component}\tcontrol,discover\n'

def origin():
    return Endpoint('local', 123, 1, 'host', 'switch', 'sw', '/switch', 1000, 'S', 1, 10, 1,
                    control='/control', switch_socket='/sw', mount_namespace='mnt')

def samples():
    return {'local': {'status':'reachable','metrics':dict.fromkeys(
        ('control_enabled','control_discover_enabled','control_can_initiate'), '1')}}

def wait(predicate):
    end = time.monotonic()+3
    while time.monotonic()<end:
        if predicate(): return
        time.sleep(.01)
    raise AssertionError('timed out')


class NetworkTests(unittest.TestCase):
    def make(self, probe=lambda *args:'format=txt\nformat_version=1\ncomponent=tunnel\n'):
        network = Network(probe, parse_stats)
        network.update([origin()], samples())
        return network

    def test_paths_limits_and_escaping(self):
        parsed = parse_discovery(HEADER+row(path='port:a%2Fb%25/peer'))
        self.assertEqual(parsed[0][3], [{'port':'a/b%'},{'peer':True}])
        for text in ['bad', HEADER+row(path='port:bad%zz'), HEADER+row(path='peer/'*17+'peer'),
                     HEADER+row(instance='bad'), HEADER+row(path='port:a%09b')]:
            with self.assertRaises(ValueError): parse_discovery(text)

    def test_local_dedup_alternatives_and_instances(self):
        n=self.make()
        local=replace(origin(), id='local-tunnel', kind='tunnel', port_id='edge')
        n.update([origin(), local], samples())
        n.ingest('local', HEADER+row(SELF,'self','switch')+row(LOCAL,'port:edge')+
                 row()+row(path='port:other/peer',state='ALT_PATH'))
        items,meta=n.snapshot()
        self.assertEqual(len(items),1)
        self.assertEqual(items[0]['id'],'control:'+REMOTE)
        self.assertEqual(items[0]['source'],'discovered')
        self.assertIsNone(items[0]['pid'])
        self.assertEqual(len(items[0]['control_routes']),2)
        n.ingest('local', HEADER+row(instance='d'*32))
        self.assertEqual(len(n.nodes[REMOTE]['routes']),1)
        self.assertIn('d'*32,n.nodes)

    def test_partial_discovery_stale_and_bound(self):
        n=self.make();n.ingest('local',HEADER+row())
        n.ingest('local',HEADER)
        self.assertEqual(len(n.nodes),1)
        n.nodes[REMOTE]['seen']-=241
        self.assertTrue(n.snapshot()[0][0]['discovery_stale'])
        n.limit=1;n.ingest('local',HEADER+row(instance='d'*32,path='port:other/peer'))
        self.assertTrue(n.snapshot()[1]['truncated'])
        self.assertEqual(len(n.nodes),1)

    def test_poll_failure_backoff_alternate_and_success_history(self):
        class History:
            records=[]
            def record(self,e,s):self.records.append((e.id,s))
        failing=True
        def probe(*args):
            if failing:raise TimeoutError('offline')
            return 'format=txt\nformat_version=1\ncomponent=tunnel\nudp_rx_bytes=18446744073709551615\n'
        n=self.make(probe);n.history=History()
        n.ingest('local',HEADER+row()+row(path='port:alt/peer'))
        token,route=next(iter(n.nodes[REMOTE]['routes'].items()))
        n._reserve(REMOTE,'local');n._poll_node(REMOTE,token,route)
        self.assertEqual(n.nodes[REMOTE]['failures'],1)
        self.assertGreater(n.nodes[REMOTE]['next'],time.monotonic()+9)
        self.assertNotEqual(next(iter(n.nodes[REMOTE]['routes'])),token)
        failing=False
        token,route=next(iter(n.nodes[REMOTE]['routes'].items()))
        n._reserve(REMOTE,'local');n._poll_node(REMOTE,token,route)
        self.assertEqual(n.nodes[REMOTE]['failures'],0)
        self.assertEqual(n.snapshot()[0][0]['metrics']['udp_rx_bytes'],'18446744073709551615')
        self.assertEqual(len(n.history.records),2)
        self.assertNotIn('process',[c['key'] for c in n.snapshot()[0][0]['health']['checks']])

    def test_slow_node_no_queue_and_discovery_cadence(self):
        gate=threading.Event();calls=[]
        def probe(origin,op,route):
            calls.append((op,route))
            if op=='discover':return HEADER+row()+row('d'*32,'port:fast/peer')
            if route[0]=={'port':'edge'}:gate.wait(3)
            return 'format=txt\nformat_version=1\ncomponent=tunnel\n'
        n=self.make(probe);n.start()
        try:
            wait(lambda:n.nodes.get('d'*32,{}).get('sample',{}).get('status')=='reachable')
            self.assertIn(REMOTE,n.active)
            with self.assertRaises(APIError) as error:n.read('control:'+REMOTE,'stats')
            self.assertEqual(error.exception.status,429)
            self.assertEqual(sum(op=='discover' for op,_ in calls),1)
            n.refresh();wait(lambda:sum(op=='discover' for op,_ in calls)==2)
            self.assertEqual(sum(op=='stats' and r[0]=={'port':'edge'} for op,r in calls),1)
        finally:gate.set();n.close()

    def test_origin_loss_and_read_only_actions(self):
        n=self.make();n.ingest('local',HEADER+row())
        with self.assertRaises(APIError) as error:n.read('control:'+REMOTE,'load')
        self.assertEqual(error.exception.status,403)
        n.update([], {})
        with self.assertRaises(APIError) as error:n.read('control:'+REMOTE,'stats')
        self.assertEqual(error.exception.status,503)

    def test_path_rebound_discards_inflight_reply(self):
        n=self.make()
        def probe(*args):
            n.ingest('local',HEADER+row('d'*32))
            return 'format=txt\nformat_version=1\n'
        n.probe=probe;n.ingest('local',HEADER+row())
        token,route=next(iter(n.nodes[REMOTE]['routes'].items()))
        n._reserve(REMOTE,'local');n._poll_node(REMOTE,token,route)
        self.assertEqual(n.nodes[REMOTE]['sample']['status'],'pending')
        self.assertNotIn(REMOTE,n.active)

    def test_remote_flow_pid_is_syntax_checked_without_local_pid_assumption(self):
        from flows import parse_flows
        text='format=txt\nformat_version=1\nview=flows\npid=123\ntracking=none\nflow_count=0\n'
        self.assertEqual(parse_flows(text,None)['tracking'],'none')
        with self.assertRaises(ValueError):parse_flows(text.replace('pid=123','pid=garbage'),None)

    def test_network_metrics_budget_fails_visibly(self):
        n=self.make(lambda *args:'format=txt\nformat_version=1\nbig='+('x'*70000)+'\n')
        n.ingest('local',HEADER+row())
        token,route=next(iter(n.nodes[REMOTE]['routes'].items()))
        n._reserve(REMOTE,'local');n._poll_node(REMOTE,token,route)
        self.assertEqual(n.snapshot()[0][0]['status'],'unavailable')
        self.assertIn('limit',n.snapshot()[0][0]['error'])

    def test_fabric_routes_remote_actions_without_proc_or_logs(self):
        f=Fabric(discover_fn=lambda:([origin()],{}))
        f.network.update([origin()], samples());f.network.ingest('local',HEADER+row())
        key='control:'+REMOTE
        self.assertEqual(f.endpoint(key).source,'discovered')
        with self.assertRaises(APIError):f.logs(key)
        with self.assertRaises(APIError):f.submit_request(key,{'operation':'discover'})
        self.assertEqual(len(f.snapshot()['endpoints']),1)
        f.close()

if __name__=='__main__':unittest.main()
