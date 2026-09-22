"""Classifier framing, guarded edits, and collector permissions."""
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from server import Fabric, digest
from errors import APIError
from control import ControlError, query
from collector import RemoteFabric
import test_fabric
import test_network
from test_network import HEADER, row, REMOTE

RULES='format 1\nclassify to [17]\n'

class ClassifierTests(unittest.TestCase):
    def make(self, write=True, source='local'):
        fabric=Fabric(allow_write=write,discover_fn=lambda:([],{}))
        self.addCleanup(fabric.close)
        fabric.endpoint=lambda key:SimpleNamespace(kind='adapter',source=source,id=key)
        calls=[]
        def query(endpoint,operation,body=''):
            calls.append((operation,body))
            return {'stats':'format=txt\nformat_version=1\nclassifier_generation=2\nclassifier_enabled=1\n',
                    'classifier-show':RULES,'classifier-check':'valid=1\n'}.get(operation,'classifier_generation=3\n')
        fabric.control_query=query
        return fabric,calls

    def test_show_check_and_all_mutations(self):
        for source in ('local','discovered'):
            f,calls=self.make(source=source)
            shown=f.classifier('id','show')
            self.assertEqual(shown['generation'],'2')
            self.assertEqual(shown['rules'],RULES)
            checked=f.classifier('id','check',{'rules':RULES+'# changed\n'})
            self.assertIn('+# changed',checked['diff'])
            self.assertNotIn('classifier-load',[c[0] for c in calls])
            for op in ('load','load-flush','disable'):
                payload={'expected_sha256':digest(RULES),'expected_generation':'2'}
                if op!='disable':payload['rules']=RULES
                f.classifier('id',op,payload)
                self.assertEqual(calls[-1],('classifier-'+op,'' if op=='disable' else RULES))

    def test_stale_generation_or_hash_never_mutates(self):
        f,calls=self.make()
        for op in ('load','load-flush','disable'):
            for revision,generation in ((digest(RULES),'1'),('old','2'),(None,None)):
                with self.assertRaises(APIError) as caught:
                    f.classifier('id',op,{'rules':'' if op=='disable' else RULES,'expected_sha256':revision,'expected_generation':generation})
                self.assertEqual(caught.exception.status,409)
        self.assertFalse(any(op in {'classifier-load','classifier-load-flush','classifier-disable'} for op,_ in calls))

    def test_read_only_enforced_in_both_web_proxy_and_collector(self):
        f,calls=self.make(write=False)
        proxy=RemoteFabric('/unused',allow_write=False)
        for target in (f,proxy):
            for op in ('load','load-flush','disable'):
                with self.assertRaises(APIError) as caught:target.classifier('id',op,{})
                self.assertEqual(caught.exception.status,403)
        self.assertEqual(calls,[])
        f.classifier('id','check',{'rules':RULES})

    def test_invalid_inputs_and_unsupported_kind(self):
        f,calls=self.make()
        for op,body in [('bogus',{}),('check',{}),('disable',{'rules':RULES}),('load',{'rules':RULES,'path':'/etc/file'})]:
            with self.assertRaises(APIError):f.classifier('id',op,body)
        self.assertEqual(calls,[])
        f.endpoint=lambda key:SimpleNamespace(kind='divert')
        with self.assertRaises(APIError):f.classifier('id','show')

    def test_ambiguous_write_is_not_retried(self):
        f,calls=self.make()
        original=f.control_query
        def broken(endpoint,operation,body=''):
            if operation=='classifier-load-flush':
                calls.append((operation,body));raise TimeoutError('lost response')
            return original(endpoint,operation,body)
        f.control_query=broken
        with self.assertRaises(APIError) as caught:
            f.classifier('id','load-flush',{'rules':RULES,'expected_sha256':digest(RULES),'expected_generation':'2'})
        self.assertIn('outcome may be unknown',str(caught.exception))
        self.assertEqual(sum(op=='classifier-load-flush' for op,_ in calls),1)

    def test_network_classifier_body_and_per_instance_reservation(self):
        fixture=test_network.NetworkTests()
        calls=[]
        n=fixture.make(lambda *args:calls.append(args) or 'valid=1\n')
        n.ingest('local',HEADER+row())
        self.assertEqual(n.read('control:'+REMOTE,'classifier-check',RULES),'valid=1\n')
        self.assertEqual(calls[-1][-1],RULES)
        n.active.add(REMOTE)
        with self.assertRaises(APIError) as caught:n.read('control:'+REMOTE,'classifier-disable')
        self.assertEqual(caught.exception.status,429)

    def test_control_rejects_body_on_show_or_disable(self):
        for op in ('classifier-show','classifier-disable'):
            with self.assertRaises(ValueError):query('/nonexistent',op,RULES)

    def test_classifier_socket_framing(self):
        fixture=test_fabric.ControlTests()
        for op in ('show','check','load','load-flush','disable'):
            self.assertEqual(fixture.exchange('classifier-'+op,[b'OK 8',b'valid=1\n'],'' if op in ('show','disable') else RULES),'valid=1\n')

    def test_http_collector_classifier_pipeline(self):
        import tempfile,threading,os,http.client,json
        from collector import CollectorServer
        from server import Server
        f,calls=self.make()
        with tempfile.TemporaryDirectory() as root:
            collector=CollectorServer(root+'/collector',f,os.getuid())
            thread=threading.Thread(target=collector.serve_forever,daemon=True);thread.start()
            proxy=RemoteFabric(root+'/collector',allow_write=True)
            web=Server(('127.0.0.1',0),proxy,'classifier-test-token')
            webthread=threading.Thread(target=web.serve_forever,daemon=True);webthread.start()
            try:
                def request(method,suffix,body=None):
                    connection=http.client.HTTPConnection('127.0.0.1',web.server_port,timeout=3)
                    headers={'Authorization':'Bearer classifier-test-token','Content-Type':'application/json'}
                    connection.request(method,'/api/v1/endpoints/id/classifier'+suffix,None if body is None else json.dumps(body),headers)
                    response=connection.getresponse();result=(response.status,json.loads(response.read()));connection.close();return result
                status,shown=request('GET','');self.assertEqual(status,200)
                payload={'rules':RULES,'expected_generation':shown['generation'],'expected_sha256':shown['sha256']}
                self.assertEqual(request('POST','/check',payload)[0],200)
                self.assertEqual(request('POST','/load-flush',payload)[0],200)
                proxy.allow_write=False
                self.assertEqual(request('POST','/disable',{'expected_generation':'2','expected_sha256':digest(RULES)})[0],403)
                self.assertEqual(request('GET','/load')[0],405)
            finally:
                web.shutdown();web.server_close();webthread.join()
                collector.shutdown();collector.server_close();thread.join()
