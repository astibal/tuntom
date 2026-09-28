import ipaddress
import struct
import unittest
from unittest.mock import patch, MagicMock
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import peek
from peek_dns import target_config,make_query,parse_response,wire_name,stream_exchange


def reply(packet, address='2001:db8::1', flags=0x81a0):
    offset=12
    while packet[offset]:offset+=packet[offset]+1
    question=packet[12:offset+5]
    raw=ipaddress.ip_address(address).packed
    return packet[:2]+struct.pack('!5H',flags,1,1,0,0)+question+b'\xc0\x0c'+struct.pack('!HHIH',28 if len(raw)==16 else 1,1,60,len(raw))+raw

class DNSProbeTests(unittest.TestCase):
    def config(self,scheme='dns',extra=''):
        return target_config(f'{scheme}://resolver.example:853/dns-query?name=some.domain.tld&type=AAAA{extra}' if scheme=='doh' else f'{scheme}://resolver.example/?name=some.domain.tld&type=AAAA{extra}')

    def test_parameters_and_address_type(self):
        for scheme,port in [('dns',53),('dot',853),('doh',853)]:
            self.assertEqual(self.config(scheme)['port'],port)
        for url in ['dns://example/?name=x&type=AAAA&expect=1.2.3.4','dns://example/?name=x&ad=yes','dns://example/?name=x&name=y','dns://example/path?name=x','dns://example/?name=x&type=AXFR','dns://example:0/?name=x']:
            with self.subTest(url=url),self.assertRaises(ValueError):target_config(url)

    def test_dns_assertions_and_negative_answer(self):
        config=self.config(extra='&ad=1&expect=2001:db8::1');ident,packet=make_query(config)
        answer=parse_response(reply(packet),ident,config)
        self.assertTrue(answer['ok']);self.assertEqual(answer['values'],['2001:db8::1']);self.assertTrue(answer['ad'])
        self.assertFalse(parse_response(reply(packet,flags=0x8180),ident,config)['ok'])
        self.assertFalse(parse_response(reply(packet,address='2001:db8::2'),ident,config)['ok'])
        self.assertFalse(parse_response(reply(packet,flags=0x8183),ident,config)['ok'])
        data=bytearray(reply(packet));data[0]^=1
        with self.assertRaises(ValueError):parse_response(data,ident,config)
        for count in (0,10,len(reply(packet))-1):
            with self.assertRaises(ValueError):parse_response(reply(packet)[:count],ident,config)

    def test_owner_and_question_matching(self):
        config=self.config();ident,packet=make_query(config);data=reply(packet)
        question_end=len(data)-28
        # Replace owner pointer with an unrelated owner; matching address alone is insufficient.
        unrelated=data[:question_end]+wire_name('unrelated.example')+data[question_end+2:]
        self.assertFalse(parse_response(unrelated,ident,config)['ok'])
        wrong=bytearray(data);wrong[13]=ord('X')
        with self.assertRaises(ValueError):parse_response(wrong,ident,config)

    def test_udp_and_tcp_fallback(self):
        config=self.config();ident,packet=make_query(config)
        udp=MagicMock();udp.recv.return_value=reply(packet,flags=0x8380)
        tcp=MagicMock();tcp.recv.side_effect=[struct.pack('!H',len(reply(packet))),reply(packet)]
        with patch('peek.public_addresses',return_value=[(2,('8.8.8.8',53),'8.8.8.8')]),patch('peek.make_query',return_value=(ident,packet)),patch('peek.socket.socket',return_value=udp),patch('peek.connect',return_value=(tcp,'8.8.8.8',.001)):
            result=peek.probe({'id':'dns','url':'dns://resolver.example/?name=some.domain.tld&type=AAAA'})
        self.assertTrue(result['ok'],result);self.assertEqual(result['transport'],'tcp');udp.close.assert_called();tcp.close.assert_called()

    def test_dot_and_doh_exchange(self):
        for protocol in ('dot','doh'):
            config=self.config(protocol);ident,packet=make_query(config);sock=MagicMock();sock.recv.side_effect=[struct.pack('!H',len(reply(packet))),reply(packet)]
            response=MagicMock();response.status=200;response.getheader.return_value='application/dns-message';response.read.return_value=reply(packet)
            context=MagicMock();context.wrap_socket.return_value=sock
            with patch('peek.public_addresses',return_value=[(2,('8.8.8.8',config['port']),'8.8.8.8')]),patch('peek.make_query',return_value=(ident,packet)),patch('peek.connect',return_value=(MagicMock(),'8.8.8.8',.001)),patch('peek.ssl.create_default_context',return_value=context),patch('peek.decode_certificate',return_value={}),patch('peek.certificate_result',return_value={}),patch('peek.http.client.HTTPResponse',return_value=response):
                result=peek.probe_dns(config,1)
            self.assertTrue(result['ok']);context.wrap_socket.assert_called_once();self.assertEqual(context.wrap_socket.call_args.kwargs['server_hostname'],'resolver.example')
            if protocol=='doh':self.assertTrue(sock.sendall.call_args.args[0].startswith(b'POST /dns-query HTTP/1.1'))

    def test_private_resolver_rejected_for_all_transports(self):
        for protocol in ('dns','dot','doh'):
            with patch('peek.socket.getaddrinfo',return_value=[(2,1,6,'',('127.0.0.1',53))]):
                result=peek.probe({'id':'test','url':f'{protocol}://localhost/?name=example.com'})
            self.assertEqual(result['error']['kind'],'address_not_public')

if __name__=='__main__':unittest.main()
