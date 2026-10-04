import io
import sys
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch, Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import peek
from peek_dns import target_config
from peek_payload import match_payload, validate_pattern, MAX_PAYLOAD
from services import Services

class Socket:
    def __init__(self, body=b'ready', status=200):
        self.data=io.BytesIO(f'HTTP/1.1 {status} OK\r\nContent-Length: {len(body)}\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n'.encode()+body)
        self.sent=b'';self.closed=False
    def sendall(self,data):self.sent+=data
    def recv_into(self,buffer):return self.data.readinto(buffer)
    def settimeout(self,value):pass
    def close(self):self.closed=True

class TransportTests(unittest.TestCase):
    def test_target_validation(self):
        self.assertEqual(target_config('tcp://example.org:1234')['port'],1234)
        self.assertEqual(target_config('tls://1.1.1.1:443?sni=example.org')['sni'],'example.org')
        self.assertEqual(target_config('http://example.org')['port'],80)
        for url in ['tcp://example.org','tcp://example.org:20/path','tls://example.org?expect=x','tcp://user@example.org:80']:
            with self.assertRaises(ValueError):target_config(url)

    def test_tcp_connect_sends_no_application_payload(self):
        sock=Socket()
        with patch('peek.public_addresses',return_value=[]),patch('peek.connect',return_value=(sock,'1.1.1.1',.01)):
            result=peek.probe({'id':'a','url':'tcp://example.org:443'})
        self.assertTrue(result['ok']);self.assertEqual(result['connect_ms'],10)
        self.assertEqual(sock.sent,b'');self.assertTrue(sock.closed)

    def test_payload_get_and_semantic_failure(self):
        for body,pattern,status,ok,kind in [(b'ready','ready',200,True,None),(b'not ready','^ready$',200,False,'payload_mismatch'),(b'ready','ready',500,False,'http_status')]:
            sock=Socket(body,status)
            with patch('peek.public_addresses',return_value=[]),patch('peek.connect',return_value=(sock,'1.1.1.1',.01)):
                result=peek.probe({'id':'a','url':'http://example.org/status','payload_regex':pattern})
            self.assertEqual(result['ok'],ok,result);self.assertTrue(result['available'])
            self.assertTrue(sock.sent.startswith(b'GET /status '));self.assertTrue(sock.closed)
            self.assertEqual(result.get('error',{}).get('kind'),kind)
            self.assertNotIn('body',result['payload'])

    def test_payload_bounds_and_regex_timeout(self):
        status,result=peek.read_http_payload(Socket(b'x'*(MAX_PAYLOAD+1)),'example.org','/','x')
        self.assertEqual(result['status'],'too_large');self.assertFalse(result['matched'])
        with patch('peek_payload.subprocess.run',side_effect=subprocess.TimeoutExpired('worker',.5)):
            self.assertEqual(match_payload('ready','ready')['status'],'timeout')
        with self.assertRaises(ValueError):validate_pattern('[')
        self.assertTrue(match_payload('(?i)READY','ready')['matched'])

    def test_regex_survives_service_and_scheduled_probe_storage(self):
        services=Services(None);self.addCleanup(services.close)
        target={'url':'https://example.org','interval':60,'payload_regex':'(?m)^ready$'}
        row=services.save({'name':'Example','peek_targets':[target]})
        self.assertEqual(row['peek_targets'],[target])
        store=peek.PeekHistory(':memory:');self.addCleanup(store.close)
        store.renew([{'id':'a',**target}]);store.db.execute('UPDATE target SET next_probe=0')
        self.assertEqual(store.due()[0]['payload_regex'],target['payload_regex'])
        with self.assertRaises(ValueError):services.save({'name':'bad','peek_targets':[{'url':'tcp://example.org:443','payload_regex':'x'}]})

    def test_tls_profile_without_http(self):
        sock=Mock();sock.getpeercert.return_value=b'cert';sock.version.return_value='TLSv1.3';sock.cipher.return_value=('CIPHER','TLSv1.3',256)
        sock.get_unverified_chain.return_value=[b'cert'];sock.selected_alpn_protocol.return_value=None
        cert={'time_valid':True,'days_remaining':10}
        with patch('peek.public_addresses',return_value=[]),patch('peek.tls_connection',return_value=(sock,'1.1.1.1',.01,.02,True,None)),patch('peek.decode_certificate',return_value={}),patch('peek.certificate_result',return_value=cert):
            result=peek.probe({'id':'a','url':'tls://1.1.1.1?sni=example.org'})
        self.assertTrue(result['ok']);self.assertEqual(result['tls']['sni'],'example.org')
        self.assertTrue(result['tls']['chain_available']);self.assertTrue(result['tls']['expires_soon'])
        sock.sendall.assert_not_called();sock.close.assert_called_once()
