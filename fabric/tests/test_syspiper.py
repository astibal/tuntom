import json
import os
from dataclasses import replace
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from syspiper import candidates, clean_snapshot, fetch, ReadError, Syspiper, ip_literal
from history import History
from test_observer import endpoint
from server import Fabric


class DiscoveryTests(unittest.TestCase):
    def test_only_known_ips_deduplicated_localhost_and_namespace(self):
        client=replace(endpoint('tunnel'),role='client',peer='192.168.55.143',interface='ut42',net_namespace='here')
        found=candidates([client],['192.168.55.143'],'here')
        self.assertEqual(list(found),['127.0.0.1','192.168.55.143'])
        self.assertEqual(found['192.168.55.143']['sources'],['manual'])
        self.assertEqual(list(candidates([client],[],'here')),['127.0.0.1'])
        for value in ('0.0.0.0','::','224.0.0.1','fe80::1','192.168.0.0/24','host','http://1.2.3.4','1.2.3.4:8181'):
            self.assertIsNone(ip_literal(value))

    def test_peer_access_replaces_outer_peer_and_requires_current_stats(self):
        peer=replace(endpoint('tunnel'),role='server',peer='192.0.2.99',interface='',net_namespace='here')
        metrics={'session_ready':'1','info_msg_peer_received':'1',
                 'peer_info_access':'10.1.1.1,10.1.1.1,127.0.0.2,::1,host,0.0.0.0,224.0.0.1,10.1.1.2',
                 'peer_info_custom':'192.0.2.88'}
        def discover(m=metrics,status='reachable'):
            return candidates([peer],[],'here',{peer.id:{'status':status,'metrics':m}})
        found=discover()
        self.assertEqual(list(found),['127.0.0.1','10.1.1.1','10.1.1.2'])
        self.assertEqual(found['10.1.1.1']['sources'],['peer_access'])
        self.assertEqual(found['10.1.1.1']['processes'],[peer.id])
        for m in ({}, {**metrics,'peer_info_access':''}, {**metrics,'session_ready':'0'},
                  {**metrics,'info_msg_peer_received':'0'}):
            self.assertEqual(list(discover(m)),['127.0.0.1'])
        self.assertEqual(list(discover(status='unavailable')),['127.0.0.1'])
        other=replace(peer,id='second-peer')
        samples={e.id:{'status':'reachable','metrics':metrics} for e in (peer,other)}
        found=candidates([peer,other],['10.1.1.1'],'here',samples)
        self.assertEqual(list(found),['127.0.0.1','10.1.1.1','10.1.1.2'])
        self.assertEqual(found['10.1.1.1']['sources'],['manual','peer_access'])
        self.assertEqual(found['10.1.1.1']['processes'],[peer.id,other.id])
        self.assertEqual(list(candidates([replace(peer,net_namespace='other')],[],'here',samples)),['127.0.0.1'])
        self.assertEqual(list(candidates([replace(peer,role='client')],[],'here')),['127.0.0.1'])

    def test_disabled_and_no_secret_in_snapshot(self):
        f=Fabric(syspiper={'key':'test-secret','nodes':['192.168.55.143']})
        with patch('syspiper.os.readlink',return_value='here'):
            f.syspiper.discover()
        snapshot=f.snapshot()
        self.assertNotIn('test-secret',json.dumps(snapshot))
        self.assertEqual(len(snapshot['syspiper']['nodes']),2)
        self.assertFalse(Fabric().snapshot()['syspiper']['enabled'])
        f.close()


class PollTests(unittest.TestCase):
    def test_apt_and_extended_metadata(self):
        def part(data): return {'status':'ok','data':data}
        raw={'apt':{'status':'ok','updates':part({'total':7,'security':2,'held':1,
             'security_held':0,'indexes':{'oldest_age_seconds':86400}})},
             'system':{'identity':part({'kernel':'test-kernel'}),'distro':part({'id':'debian'})},
             'interfaces':{'links':part({'eth0':{'is_up':True,'duplex':'full'}}),
                           'addresses':part({'eth0':[{'family':'ipv4','address':'192.0.2.1'}]})}}
        details=dict(clean_snapshot(raw)['details'])
        self.assertEqual(details['apt.updates.security'],'2')
        self.assertEqual(details['apt.updates.security_held'],'0')
        self.assertEqual(details['apt.indexes.oldest_age_seconds'],'86400')
        self.assertEqual(details['system.kernel'],'test-kernel')
        self.assertEqual(details['eth0.is_up'],'true')
        self.assertEqual(details['eth0.address.0.address'],'192.0.2.1')
        missing=clean_snapshot({'apt':{'updates':{'status':'unsupported','data':None,'reason':'python3_apt_missing'}}})
        self.assertNotIn('apt.updates.total',dict(missing['details']))
        self.assertEqual(dict(missing['details'])['apt.updates.reason'],'python3_apt_missing')
        calls=[]
        def read(ip,port,path,key):
            calls.append(path)
            return raw.get(path,{'status':'ok'})
        poller=Syspiper(key='test',fetch_fn=read)
        poller.sample('127.0.0.1')
        self.assertEqual(calls.count('apt'),1)

    def test_old_server_optional_unsupported_and_network_reset(self):
        counters={'sent':1000,'recv':2000}
        calls=[]
        def read(ip,port,path,key):
            calls.append(path)
            if path in ('system','interfaces','filesystems','pressure'):
                raise ReadError('unsupported')
            return {'status':'ok',**(counters if path=='net' else {'percent':0,'total':100,'used':0})}
        poll=Syspiper(key='secret',fetch_fn=read)
        poll.sample('127.0.0.1')
        first=poll.samples['127.0.0.1']
        self.assertEqual(first['values']['cpu'],0)
        self.assertIsNone(first['chart']['rx'])
        self.assertNotIn('issues',first['chart'])
        counters.update(sent=1500,recv=3000)
        poll.sample('127.0.0.1')
        self.assertGreater(poll.samples['127.0.0.1']['chart']['rx'],0)
        self.assertEqual(calls.count('system'),1)
        counters.update(sent=1,recv=1)
        poll.sample('127.0.0.1')
        self.assertIsNone(poll.samples['127.0.0.1']['chart']['rx'])

    def test_slow_system_probe_does_not_stop_process_discovery(self):
        entered, release, scanned_twice = threading.Event(), threading.Event(), threading.Event()
        scans=[]
        def discover():
            scans.append(1)
            if len(scans)>=2: scanned_twice.set()
            return [], {'source':'fixture'}
        def read(*args):
            entered.set()
            release.wait(5)
            raise ReadError('unreachable')
        f=Fabric(interval=1, discover_fn=discover, syspiper={'key':'secret','fetch_fn':read})
        try:
            with patch('syspiper.os.readlink',return_value='here'):
                f.start()
                self.assertTrue(entered.wait(2))
                self.assertTrue(scanned_twice.wait(2))
                self.assertTrue(f.snapshot()['syspiper']['enabled'])
        finally:
            release.set();f.close()

    def test_failure_is_bounded_and_persisted_as_gap(self):
        with tempfile.TemporaryDirectory() as root:
            store=History(Path(root)/'history.sqlite')
            calls=[]
            def read(*args):
                calls.append(args)
                raise ReadError('unauthorized')
            poll=Syspiper(key='secret',history=store,fetch_fn=read)
            poll.sample('127.0.0.1')
            self.assertEqual(len(calls),1)
            self.assertEqual(poll.samples['127.0.0.1']['status'],'unavailable')
            points=store.read(poll.identity('127.0.0.1'))['samples']
            self.assertIsNone(points[0]['cpu'])
            self.assertEqual(points[0]['issues'][0]['code'],'syspiper_unauthorized')
            with patch.object(store,'record_point',side_effect=sqlite3.OperationalError('disk full')):
                poll.sample('127.0.0.1')
            self.assertTrue(poll.snapshot()['history_error'])
            store.close()

    def test_normalization_keeps_counter_precision_and_rejects_unknown_fields(self):
        data=clean_snapshot({'cpu':{'percent':True,'api_key':'secret'},'ram':{'percent':float('nan')},
            'net':{'sent':18446744073709551615,'recv':0},
            'interfaces':{'counters':{'status':'ok','data':{'tun0':{'bytes_sent':18446744073709551615}}}}})
        self.assertIsNone(data['cpu']);self.assertIsNone(data['ram'])
        self.assertEqual(data['net_sent'],'18446744073709551615')
        self.assertEqual(dict(data['details'])['tun0.bytes_sent'],'18446744073709551615')
        self.assertNotIn('secret',json.dumps(data))


class TransportTests(unittest.TestCase):
    def test_auth_fixed_paths_redirect_and_response_limits(self):
        seen=[]
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_GET(self):
                seen.append((self.path,self.headers.get('X-API-Key')))
                self.send_response(302 if self.path=='/redirect' else 200)
                if self.path=='/redirect': self.send_header('Location','/secret-destination')
                self.end_headers()
                try:
                    if self.path=='/large':self.wfile.write(b'x'*(1024*1024+1))
                    elif self.path=='/invalid':self.wfile.write(b'[]')
                    else:self.wfile.write(b'{"status":"ok","percent":0}')
                except (BrokenPipeError,ConnectionResetError):pass
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            self.assertEqual(fetch('127.0.0.1',server.server_port,'cpu','secret')['percent'],0)
            for path,code in [('redirect','redirect_rejected'),('large','response_too_large'),('invalid','invalid_response')]:
                with self.assertRaises(ReadError) as error:fetch('127.0.0.1',server.server_port,path,'secret')
                self.assertEqual(error.exception.code,code)
            self.assertEqual(seen,[('/cpu','secret'),('/redirect','secret'),('/large','secret'),('/invalid','secret')])
        finally:
            server.shutdown();server.server_close();thread.join()
