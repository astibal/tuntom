import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from flows import parse_flows, MAX_FLOW_REPLY
from control import query, ResponseTooLarge, ControlError
from server import Fabric, APIError
from test_observer import endpoint
import test_fabric
import test_observer


def dump(rows=(), tracking='retained', pid=123):
    return f'format=txt\nformat_version=1\nview=flows\npid={pid}\ntracking={tracking}\n'+''.join('flow '+row+'\n' for row in rows)+f'flow_count={len(rows)}\n'

EXIT='table=l4 ip_version=4 src=10.0.0.2 dst=10.0.0.1 protocol=6 src_port=443 dst_port=12345 idle_ms=120 labels=[0x0000000000000011,0xffffffffffffffff]'
ROUTE='table=routes ip_version=6 src=::1 dst=::2 protocol=17 src_port=65535 dst_port=53 idle_ms=0 path=3 client_labels=[0x0000000000000011] server_labels=[] client_saved_labels=[0xffffffffffffffff] client_cookie=6382179 client_chain=7 client_step=2 client_origin=18446744073709551615 client_action=1 client_reverse=0 server_divert_body=[0x0000000000000099]'
ADMISSION='table=existing_tcp ip_version=4 src=10.0.0.1 dst=10.0.0.2 protocol=6 src_port=1 dst_port=2 labels=unknown'


class ParserTests(unittest.TestCase):
    def test_precise_stacks_context_and_learning_semantics(self):
        data=parse_flows(dump([EXIT,ROUTE,ADMISSION]),123)
        self.assertEqual(data['flow_count'],3)
        self.assertEqual(data['admission_count'],1)
        self.assertEqual(data['rows'][0]['labels'],['0x0000000000000011','0xffffffffffffffff'])
        self.assertEqual(data['rows'][1]['client_origin'],'18446744073709551615')
        self.assertEqual(data['rows'][1]['server_labels'],[])
        self.assertIsNone(data['rows'][2]['labels'])
        self.assertEqual(data['label_count'],2) # DIVERT body metadata is not a stack of labels.
        self.assertEqual({x['value']:x['rows'] for x in data['labels']}, {'0x0000000000000011':2,'0xffffffffffffffff':2})
        self.assertEqual(parse_flows(dump(tracking='none'),123)['tracking'],'none')

    def test_truncation_is_explicit_with_full_counts(self):
        with patch('flows.MAX_ROWS',1):
            result=parse_flows(dump([EXIT,ROUTE,ADMISSION]),123)
        self.assertTrue(result['truncated'])
        self.assertEqual(result['returned_count'],1)
        self.assertEqual(result['flow_count'],3)
        self.assertEqual(result['table_counts'],{'l4':1,'routes':1,'existing_tcp':1})

    def test_malformed_and_wrong_identity_fail_closed(self):
        for text in (dump([EXIT],pid=124),dump([EXIT],tracking='none'),dump([EXIT]).replace('flow_count=1','flow_count=0'),
                     dump([EXIT.replace('src_port=443','src_port=65536')]),dump([EXIT.replace('ip_version=4','ip_version=6')]),
                     dump([EXIT.replace('0xffffffffffffffff','0x10000000000000000')]),dump([EXIT+' table=l3']),
                     dump([EXIT])+'flow '+EXIT,dump([EXIT]).replace('format_version=1','format_version=2')):
            with self.assertRaises(ValueError):parse_flows(text,123)

    def test_wire_limit_rejects_before_reading_large_body(self):
        with tempfile.TemporaryDirectory() as root:
            path=root+'/control';server=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);server.bind(path);server.listen()
            seen=[]
            def serve():
                conn,_=server.accept()
                with conn:
                    seen.append(conn.recv(256))
                    conn.send(f'OK {MAX_FLOW_REPLY+1}\n'.encode())
            thread=threading.Thread(target=serve);thread.start()
            try:
                with self.assertRaises(ResponseTooLarge):query(path,'flows',max_response_bytes=MAX_FLOW_REPLY)
            finally:thread.join();server.close()
            self.assertEqual(seen,[b'show flows'])


class FlowAPITests(unittest.TestCase):
    setUp=test_fabric.APITests.setUp
    tearDown=test_fabric.APITests.tearDown
    request=test_fabric.APITests.request
    def test_read_only_authorization_and_errors(self):
        self.fabric.discover=lambda:([endpoint()],{'source':'fixture'})
        with patch.object(self.fabric,'control_query',return_value=dump([EXIT])) as control:
            path='/api/v1/endpoints/boot%3A123%3A100/flows'
            self.assertEqual(self.request('GET',path,auth=False)[0],401)
            status,data=self.request('GET',path)
            self.assertEqual(status,200)
            self.assertEqual(data['rows'][0]['labels'][-1],'0xffffffffffffffff')
            self.assertFalse(self.fabric.allow_write)
            self.assertEqual(control.call_args.args[1],'flows')
        with patch.object(self.fabric,'control_query',side_effect=ResponseTooLarge('large')):
            self.assertEqual(self.request('GET',path)[0],413)
        self.fabric.flow_lock.acquire()
        try:self.assertEqual(self.request('GET',path)[0],429)
        finally:self.fabric.flow_lock.release()
        self.fabric.discover=lambda:([],{})
        self.assertEqual(self.request('GET',path)[0],404)


class FlowCollectorTests(unittest.TestCase):
    setUp=test_observer.CollectorTests.setUp
    tearDown=test_observer.CollectorTests.tearDown
    def test_remote_read_without_write_permission_and_no_background_dump(self):
        self.fabric.discover=lambda:([endpoint()],{'source':'fixture'})
        with patch.object(self.fabric,'control_query',return_value=dump([ROUTE,ADMISSION])) as control:
            self.remote.snapshot()
            control.assert_not_called()
            self.assertEqual(self.remote.flows(endpoint().id)['flow_count'],2)
        with patch.object(self.fabric,'control_query',side_effect=ControlError('unsupported')):
            with self.assertRaises(APIError) as error:self.remote.flows(endpoint().id)
            self.assertEqual(error.exception.status,422)
