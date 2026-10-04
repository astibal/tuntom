"""Explicit DNS service probes. URL query parameters describe the test, not discovery."""
import ipaddress
import secrets
import struct
from urllib.parse import urlsplit, parse_qs

TYPES = {'A':1, 'NS':2, 'CNAME':5, 'SOA':6, 'PTR':12, 'MX':15, 'TXT':16, 'AAAA':28, 'SRV':33}
RCODES = {0:'NOERROR',1:'FORMERR',2:'SERVFAIL',3:'NXDOMAIN',4:'NOTIMP',5:'REFUSED'}


def wire_name(name):
    if not isinstance(name,str) or not name:raise ValueError('DNS query name is required')
    name=name.rstrip('.')
    if not name:return b'\0'
    labels=[part.encode('idna') for part in name.split('.')]
    if any(not 1<=len(part)<=63 for part in labels):raise ValueError('invalid DNS label length')
    result=b''.join(bytes([len(part)])+part for part in labels)+b'\0'
    if len(result)>255:raise ValueError('DNS name is too long')
    return result


def target_config(url):
    if not isinstance(url,str) or len(url)>2048 or any(ord(c)<=32 or ord(c)==127 for c in url):
        raise ValueError('invalid Peek URL')
    p=urlsplit(url)
    if p.scheme not in {'http','https','tcp','tls','dns','dot','doh'} or not p.hostname or p.username is not None or p.password is not None or p.fragment:
        raise ValueError('use HTTP(S), TCP, TLS, DNS, DoT or DoH URL without credentials or fragment')
    port=p.port if p.port is not None else {'http':80,'https':443,'tls':443,'tcp':0,'dns':53,'dot':853,'doh':443}[p.scheme]
    if not 1<=port<=65535:raise ValueError('invalid server port')
    result={'protocol':p.scheme,'host':p.hostname,'port':port,'path':p.path or ('/dns-query' if p.scheme=='doh' else '/')}
    if p.scheme in ('http','https'):return result
    if p.scheme in ('tcp','tls'):
        params=parse_qs(p.query,keep_blank_values=True,strict_parsing=True)
        if p.path not in ('','/') or set(params)-({'sni'} if p.scheme=='tls' else set()) or any(len(v)!=1 for v in params.values()):
            raise ValueError('TCP/TLS targets accept only a host, port and optional TLS sni')
        sni=params.get('sni',[p.hostname])[0]
        if not sni or len(sni)>253 or any(c.isspace() or c in '/@?#' for c in sni):raise ValueError('invalid TLS SNI')
        result['sni']=sni
        return result
    params=parse_qs(p.query,keep_blank_values=True,strict_parsing=True)
    if set(params)-{'name','type','ad','expect'} or any(len(v)!=1 for v in params.values()):raise ValueError('invalid DNS probe parameters')
    name=params.get('name',[''])[0];wire_name(name)
    kind=params.get('type',['A'])[0].upper()
    if kind not in TYPES:raise ValueError('unsupported DNS record type')
    ad=params.get('ad',['0'])[0]
    if ad not in ('0','1'):raise ValueError('ad must be 0 or 1')
    expected=params.get('expect',[''])[0]
    if len(expected)>1024:raise ValueError('expected answer is too long')
    if expected and kind in ('A','AAAA'):
        address=ipaddress.ip_address(expected)
        if address.version != (4 if kind=='A' else 6):raise ValueError('expected address does not match query type')
        expected=str(address)
    if p.scheme in ('dns','dot') and p.path not in ('','/'):raise ValueError('DNS/DoT has no HTTP path')
    result.update(name=name.rstrip('.').lower() or '.',type=kind,require_ad=ad=='1',expected=expected)
    return result


def make_query(config):
    ident=secrets.randbits(16)
    # RD + AD signal; EDNS DO requests DNSSEC material without disabling validation.
    flags=0x0120
    packet=struct.pack('!6H',ident,flags,1,0,0,1)+wire_name(config['name'])+struct.pack('!HH',TYPES[config['type']],1)
    packet+=b'\0'+struct.pack('!HHIH',41,1232,0x8000,0)
    return ident,packet


def read_name(data,offset):
    labels=[];end=None;visited=set();size=1
    while True:
        if offset>=len(data) or offset in visited:raise ValueError('invalid DNS name pointer')
        visited.add(offset);length=data[offset];offset+=1
        if length&0xc0==0xc0:
            if offset>=len(data):raise ValueError('truncated DNS pointer')
            pointer=((length&0x3f)<<8)|data[offset]
            if pointer>=offset-1:raise ValueError('DNS pointer must refer backwards')
            if end is None:end=offset+1
            offset=pointer;continue
        if length&0xc0:raise ValueError('invalid DNS label')
        if not length:return '.'.join(labels).lower() or '.',end or offset
        if offset+length>len(data):raise ValueError('truncated DNS name')
        size+=length+1
        if size>255:raise ValueError('DNS name too long')
        labels.append(data[offset:offset+length].decode('ascii'));offset+=length


def parse_response(data,ident,config):
    if len(data)<12 or len(data)>65535:raise ValueError('invalid DNS message length')
    rid,flags,qd,an,ns,ar=struct.unpack_from('!6H',data)
    if rid!=ident or not flags&0x8000 or flags&0x7800 or qd!=1:raise ValueError('DNS response does not match query')
    name,pos=read_name(data,12)
    if pos+4>len(data):raise ValueError('truncated DNS question')
    qt,qc=struct.unpack_from('!HH',data,pos);pos+=4
    if name!=config['name'].encode('idna').decode().lower() or qt!=TYPES[config['type']] or qc!=1:
        raise ValueError('DNS question mismatch')
    if flags&0x0200:return {'truncated':True}
    answers=[];extended=0
    for index in range(an+ns+ar):
        owner,pos=read_name(data,pos)
        if pos+10>len(data):raise ValueError('truncated DNS record')
        kind,cls,ttl,length=struct.unpack_from('!HHIH',data,pos);pos+=10;end=pos+length
        if end>len(data):raise ValueError('truncated DNS record data')
        value=None
        if kind==41:extended=ttl>>24
        if index<an and cls==1:
            if kind in (1,28):
                if length!=(4 if kind==1 else 16):raise ValueError('invalid address length')
                value=str(ipaddress.ip_address(data[pos:end]))
            elif kind in (2,5,12):
                value,used=read_name(data,pos)
                if used!=end:raise ValueError('invalid name record length')
            elif kind in (15,33):
                prefix=2 if kind==15 else 6
                if length<prefix+1:raise ValueError('invalid structured record')
                host,used=read_name(data,pos+prefix)
                if used!=end:raise ValueError('invalid structured record length')
                value=' '.join(map(str,struct.unpack_from('!H' if kind==15 else '!HHH',data,pos)))+' '+host
            elif kind==16:
                parts=[];cursor=pos
                while cursor<end:
                    n=data[cursor];cursor+=1
                    if cursor+n>end:raise ValueError('invalid TXT record')
                    parts.append(data[cursor:cursor+n].decode('utf-8',errors='replace'));cursor+=n
                value=''.join(parts)
            elif kind==6:
                primary,cursor=read_name(data,pos);mailbox,cursor=read_name(data,cursor)
                if cursor+20!=end:raise ValueError('invalid SOA record')
                value=primary+' '+mailbox+' '+' '.join(map(str,struct.unpack_from('!5I',data,cursor)))
            if value is not None:answers.append({'name':owner,'type':next(k for k,v in TYPES.items() if v==kind),'ttl':ttl,'value':value})
        pos=end
    if pos!=len(data):raise ValueError('trailing DNS data')
    # Only requested owner or its answer CNAME chain can satisfy the test.
    names={config['name'].encode('idna').decode().lower()}
    for _ in range(len(answers)):
        names.update(r['value'] for r in answers if r['type']=='CNAME' and r['name'] in names)
    values=[r['value'] for r in answers if r['type']==config['type'] and r['name'] in names]
    expected=config['expected']
    normalize=lambda value:value.rstrip('.').lower() if config['type'] in ('NS','CNAME','PTR') else value
    matches=not expected or any(normalize(v)==normalize(expected) for v in values)
    rcode=(flags&15)|(extended<<4);ad=bool(flags&0x20)
    return {'truncated':False,'rcode':RCODES.get(rcode,str(rcode)),'rcode_number':rcode,'ad':ad,
            'answers':answers,'values':values,'expected':expected or None,'matches_expected':matches,
            'require_ad':config['require_ad'],'name':config['name'],'type':config['type'],
            'ok':rcode==0 and bool(values) and matches and (ad or not config['require_ad'])}


def recv_exact(sock,count,deadline):
    import time
    data=bytearray()
    while len(data)<count:
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('DNS probe deadline exceeded')
        sock.settimeout(remaining);part=sock.recv(count-len(data))
        if not part:raise ValueError('truncated DNS stream')
        data.extend(part)
    return bytes(data)


def stream_exchange(sock,packet,deadline):
    sock.sendall(struct.pack('!H',len(packet))+packet)
    length=struct.unpack('!H',recv_exact(sock,2,deadline))[0]
    return recv_exact(sock,length,deadline)
