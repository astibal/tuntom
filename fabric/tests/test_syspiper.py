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
        addresses=[{'ifname':'ut42','addr_info':[{'local':'100.100.1.1','peer':'100.100.1.2/32'}]},
                   {'ifname':'eth0','addr_info':[{'local':'192.168.99.1','peer':'192.168.99.2/24'}]}]
        found=candidates([client],addresses,['192.168.55.143'],'here')
        self.assertEqual(list(found),['127.0.0.1','192.168.55.143','100.100.1.1','100.100.1.2'])
        self.assertEqual(found['192.168.55.143']['sources'],['manual','tunnel_peer'])
        self.assertEqual(found['192.168.55.143']['processes'],[client.id])
        self.assertEqual(list(candidates([replace(client,net_namespace='other')],addresses,[],'here')),['127.0.0.1'])
        self.assertEqual(list(candidates([replace(client,peer='example.com',interface='')],[],[],'here')),['127.0.0.1'])
        for value in ('0.0.0.0','::','224.0.0.1','fe80::1','192.168.0.0/24','host','http://1.2.3.4','1.2.3.4:8181'):
            self.assertIsNone(ip_literal(value))

    def test_disabled_and_no_secret_in_snapshot(self):
        f=Fabric(syspiper={'key':'test-secret','nodes':['192.168.55.143']})
        with patch('syspiper.interface_addresses',return_value=([],None)):
            f.syspiper.discover()
        snapshot=f.snapshot()
        self.assertNotIn('test-secret',json.dumps(snapshot))
        self.assertEqual(len(snapshot['syspiper']['nodes']),2)
        self.assertFalse(Fabric().snapshot()['syspiper']['enabled'])
        f.close()


class PollTests(unittest.TestCase):
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
            with patch('syspiper.interface_addresses',return_value=([],None)):
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
        self.assertEqual(data['details'][0][1],'18446744073709551615')
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
