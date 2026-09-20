#!/usr/bin/env python3
"""Read-only host inspection and deployment ownership checks for Ansible.

Receives a public compiled specification as base64 JSON. Never receives the
secret value. File transfer, builds and service changes are Ansible tasks.
"""
from __future__ import annotations
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import tempfile

BASE=Path('/var/lib/tuntom-deploy/instances')
UNITS=Path('/etc/systemd/system')
RUN=Path('/run/tuntom-deploy')
LOCK=Path('/run/tuntom-deploy.lock')
STAGING=BASE.parent/'staging'


def run(argv):
    return subprocess.run(argv,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)


def safe_root(spec):
    key=spec['key']
    if not re.fullmatch(r'(tunnel|switch|adapter)-[A-Za-z0-9][A-Za-z0-9_-]{0,31}',key):
        raise RuntimeError('invalid deployment key')
    root=BASE/key
    if str(root)!=spec['root'] or str(RUN/key)!=spec['run']:
        raise RuntimeError('invalid deployment paths')
    for path in [BASE.parent,BASE,root,UNITS,RUN,RUN/key,STAGING]:
        if path.is_symlink(): raise RuntimeError(f'refusing symlink: {path}')
    return root


def digest(path):
    result=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''): result.update(chunk)
    return result.hexdigest()


def tree(root, secrets=False):
    result={}
    if not root.exists(): return result
    for path in sorted(root.rglob('*')):
        rel=str(path.relative_to(root))
        if 'secrets' in path.relative_to(root).parts:
            if secrets and path.is_file():
                result[rel]={'secret':True,'mode':oct(stat.S_IMODE(path.lstat().st_mode))}
            continue
        if path.is_symlink(): result[rel]='symlink'
        elif path.is_file(): result[rel]=digest(path)
        elif not path.is_dir(): result[rel]='special'
    return result


def unit_names(spec):
    prefix='tuntom-'+spec['key']
    return [e['unit'] for e in spec['endpoints']]+[spec['target']]+[prefix+s+'.service' for s in ('-prepare','-ready','-failed')]


def state(unit):
    result=run(['systemctl','show',unit,'--property=LoadState,ActiveState,SubState,UnitFileState,FragmentPath'])
    values=dict(line.split('=',1) for line in result.stdout.splitlines() if '=' in line)
    if result.returncode and values.get('LoadState')!='not-found':
        raise RuntimeError('cannot query systemd: '+result.stderr.strip())
    return values


def verify_units(spec, owner, *, installed=False):
    expected=f'# tuntom-deploy owner={owner}'
    for unit in unit_names(spec):
        if not re.fullmatch(r'tuntom-'+re.escape(spec['key'])+r'(?:-[A-Za-z0-9_-]+)?\.(?:service|target)',unit):
            raise RuntimeError('unit outside instance ownership')
        path=UNITS/unit
        current=state(unit)
        if path.is_symlink(): raise RuntimeError(f'symlink unit: {unit}')
        if path.exists():
            if not path.is_file() or not path.read_text().startswith(expected+'\n'):
                raise RuntimeError(f'foreign unit: {unit}')
        elif current.get('LoadState') not in ('not-found','masked') or current.get('FragmentPath'):
            raise RuntimeError(f'unit exists outside managed directory: {unit}')
        if (UNITS/(unit+'.d')).exists():
            # A drop-in on our instance unit is included in wipe and reported.
            if (UNITS/(unit+'.d')).is_symlink():
                raise RuntimeError(f'symlink drop-in directory: {unit}')
            if not installed:
                raise RuntimeError(f'unclaimed unit drop-ins: {unit}')


def stages(spec):
    found=[]
    for path in STAGING.glob(spec['key']+'-*'):
        if not re.fullmatch(re.escape(spec['key'])+r'-[0-9a-f-]{36}',path.name): continue
        receipt=path/'owner.json'
        if path.is_symlink() or receipt.is_symlink(): raise RuntimeError('symlink staging')
        if receipt.is_file() and json.loads(receipt.read_text()).get('owner')==spec['owner']:
            found.append(str(path))
    return sorted(found)


def legacy(spec):
    if spec['kind']!='tunnel':
        kind=spec['kind']
        path=Path('/run/tuntom-mk')/(kind+'-'+spec['alias'])/'endpoints'
        if not path.is_file(): return None
        if path.is_symlink() or path.stat().st_uid!=0 or path.stat().st_mode & 0o022:
            raise RuntimeError('untrusted legacy state')
        values=path.read_bytes().split(b'\0')
        if len(values)!=8 or values[-1]: raise RuntimeError('invalid legacy endpoint state')
        if kind=='switch' and values[0].decode()!=spec['endpoints'][0]['env']['TUNTOM_SWITCH_SOCKET']:
            raise RuntimeError('legacy switch socket differs; stop old setup explicitly first')
        return {'kind':kind,'path':str(path),'sha256':digest(path)}
    roles=sorted({e['role'] for e in spec['endpoints']})
    records=[]
    for role in roles:
        endpoints=[e for e in spec['endpoints'] if e['role']==role]
        ident=endpoints[0]['env']['TUNTOM_GROUP_ID']
        path=Path('/var/lib/tuntom-mk')/role/ident/'active'/'manifest.tsv'
        if not path.is_file(): continue
        if path.is_symlink() or path.stat().st_uid!=0 or path.stat().st_mode & 0o022:
            raise RuntimeError('untrusted legacy manifest')
        rows=list(csv.DictReader(path.open(),delimiter='\t'))
        if len(rows)!=len(endpoints): raise RuntimeError('legacy member count differs; pass the tested --count')
        for row,e in zip(rows,endpoints):
            env=e['env']
            expected={'instance':env['TUNTOM_INSTANCE'],role+'_if':f'ut{e["id"]}',
                      role+'_socket':env['TUNTOM_SWITCH_SOCKET'],role+'_port':env['TUNTOM_SWITCH_PORT_ID'],
                      role+'_has_tun':str(int(bool(e['interface'])))}
            if any(row.get(k)!=v for k,v in expected.items()):
                raise RuntimeError('legacy resources differ; stop old setup explicitly first')
        records.append({'kind':'tunnel','path':str(path),'sha256':digest(path)})
    return records or None


def retire_legacy(job):
    """Move stopped trial artifacts into this installation's wipe boundary."""
    spec=job['spec']
    root=safe_root(spec)
    if not job['before']['legacy']: return
    if legacy(spec): raise RuntimeError('mk_* state is still active; refusing to move its files')
    if json.loads((root/'installed.json').read_text())['owner']!=spec['owner']:
        raise RuntimeError('cannot retire trial files into a foreign installation')
    destination=root/'retired-mk'
    destination.mkdir(mode=0o700,exist_ok=True)
    locks=[]
    try:
        paths=[]
        if spec['kind']=='tunnel':
            for role in sorted({e['role'] for e in spec['endpoints']}):
                members=[e for e in spec['endpoints'] if e['role']==role]
                ident=members[0]['env']['TUNTOM_GROUP_ID']
                lock=(Path('/run/tuntom')/('mk_'+ident+'.lock')) if role=='client' else Path('/run/tuntom-mk')/('server_'+ident+'.lock')
                if lock.is_symlink(): raise RuntimeError('symlink mk lock')
                lock.parent.mkdir(mode=0o700,exist_ok=True)
                fd=os.open(lock,os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
                locks.append(fd); fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
                paths.append(Path('/var/lib/tuntom-mk')/role/ident)
                for e in members:
                    paths += [Path('/tmp')/('tuntom_'+e['id']+suffix) for suffix in ('','.log')]
        else:
            for key in [spec['key']]+(['switch-mp-'+spec['alias']] if spec['kind']=='switch' else []):
                directory=Path('/run/tuntom-mk')/key
                if directory.is_symlink(): raise RuntimeError('symlink mk directory')
                if directory.exists():
                    fd=os.open(directory/'lock',os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
                    locks.append(fd); fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
                    paths += [p for p in directory.iterdir() if p.name!='lock']
                paths.append(Path('/var/lib/tuntom-mk')/key)
        # Check again under the mk locks, then preserve artifacts inside our
        # owned directory. Shared lock inodes and companion tools stay in place.
        if legacy(spec): raise RuntimeError('mk_* setup changed before retirement')
        receipt=root/'retirement.json'
        receipt.write_text(json.dumps(job))
        receipt.chmod(0o600)
        for path in paths:
            if not path.exists() and not path.is_symlink(): continue
            if any(p.is_symlink() for p in [path,*path.parents]): raise RuntimeError('symlink trial artifact: '+str(path))
            target=destination/str(path).lstrip('/')
            # An interrupted cross-filesystem move can leave both copies. The
            # original is authoritative until its move finishes successfully.
            if target.is_symlink(): raise RuntimeError('symlink retired artifact')
            if target.is_dir(): shutil.rmtree(target)
            elif target.exists(): target.unlink()
            target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
            shutil.move(str(path),str(target))
        receipt.unlink()
    finally:
        for fd in locks: os.close(fd)


def inspect(spec):
    root=safe_root(spec)
    installed=root/'installed.json'
    old=None
    if installed.exists():
        if installed.is_symlink() or installed.stat().st_uid!=0:
            raise RuntimeError('untrusted installed metadata')
        old=json.loads(installed.read_text())
        if old['owner']!=spec['owner']:
            raise RuntimeError(f'instance belongs to another inventory: {spec["key"]}')
        safe_root(old['spec'])
    elif root.exists():
        raise RuntimeError(f'unclaimed installation directory: {root}')
    effective=old['spec'] if old else spec
    verify_units(effective,spec['owner'],installed=bool(old))
    if old: verify_units(spec,spec['owner'],installed=True)
    services={unit:state(unit) for unit in unit_names(effective)}
    assets=tree(root/'assets')
    previous=old.get('assets',{}) if old else {}
    changes=sorted(k for k in assets.keys()|previous.keys() if assets.get(k)!=previous.get(k))
    result={'key':spec['key'],'host':spec['host'],'installed':bool(old),'complete':bool(old and old.get('complete')),
            'services':services,'root':str(root),'run':effective['run'],
            'manual_changes':changes,'files':tree(root,secrets=True),
            'spec':effective,'owner':spec['owner'],
            'machine_id':Path('/etc/machine-id').read_text().strip(),
            'desired_changed':bool(old and old['source_digest']!=spec['source_digest']),
            'legacy':None if old else legacy(spec)}
    result['staging']=stages(spec)
    result['unit_files']={unit:{'sha256':digest(UNITS/unit) if (UNITS/unit).is_file() else None,
                               'dropins':tree(UNITS/(unit+'.d'))} for unit in services}
    if old:
        for unit,value in result['unit_files'].items():
            if value!=old.get('unit_files',{}).get(unit): result['manual_changes'].append('unit:'+unit)
        for binary,checksum in tree(root/'bin').items():
            if checksum!=old.get('binaries',{}).get(binary): result['manual_changes'].append('bin/'+binary)
    if old and (root/'config.json').is_file() and digest(root/'config.json')!=old.get('config_digest'):
        result['manual_changes'].append('config.json')
    return result


def resources(spec):
    result={('tun',e['interface']) for e in spec['endpoints'] if e['interface']}
    result|={('udp',e['env']['TUNTOM_UDP_PORT']) for e in spec['endpoints'] if e['role']=='server'}
    result|={('switch',e['env']['TUNTOM_SWITCH_SOCKET']) for e in spec['endpoints'] if spec['kind']=='switch'}
    result|={('port',e['env']['TUNTOM_SWITCH_SOCKET']+'\0'+e['env'].get('TUNTOM_SWITCH_PORT_ID','')) for e in spec['endpoints'] if spec['kind']!='switch' and e['env'].get('TUNTOM_SWITCH_SOCKET')}
    result|={('table',e['env']['TUNTOM_TABLE']) for e in spec['endpoints'] if spec['kind']=='tunnel' and e['interface']}
    result|={('chain',e['env']['TUNTOM_CHAIN']) for e in spec['endpoints'] if spec['kind']=='tunnel' and e['interface']}
    return result


def network_marks(spec):
    return [(int(e['env']['TUNTOM_MARK']),int(e['env']['TUNTOM_MARK_MASK'])) for e in spec['endpoints'] if spec['kind']=='tunnel' and e['interface']]


def marks_overlap(a,b):
    return ((a[0]^b[0]) & a[1] & b[1])==0


def check_socket(path, allowed=False):
    path=Path(path)
    if any(p.is_symlink() for p in [path,*path.parents]): raise RuntimeError('symlink socket path: '+str(path))
    if not path.exists(): return
    if not stat.S_ISSOCK(path.stat().st_mode): raise RuntimeError('non-socket endpoint: '+str(path))
    if allowed: return
    with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as probe:
        probe.settimeout(0.2)
        try: probe.connect(str(path))
        except (ConnectionRefusedError,FileNotFoundError): return
    raise RuntimeError('live socket already exists: '+str(path))


def assert_free(spec, preflight=False):
    root=safe_root(spec)
    # Registry ownership also covers currently stopped deployments.
    expected=resources(spec)
    allowed=set()
    if preflight:
        current=inspect(spec)
        if current['installed']: allowed=resources(current['spec'])
        elif current['legacy']: allowed=expected
    for path in BASE.glob('*/installed.json'):
        if path.parent==root: continue
        if path.is_symlink() or path.parent.is_symlink(): raise RuntimeError('symlink installation registry')
        other=json.loads(path.read_text())['spec']
        if resources(other) & expected: raise RuntimeError('resources belong to another deployment: '+other['key'])
        if any(marks_overlap(a,b) for a in network_marks(spec) for b in network_marks(other)):
            raise RuntimeError('routing mark overlaps another deployment: '+other['key'])
    for e in spec['endpoints']:
        if e['interface'] and ('tun',e['interface']) not in allowed and run(['ip','link','show','dev',e['interface']]).returncode==0:
            raise RuntimeError('interface already exists: '+e['interface'])
        check_socket(e['control'], allowed=preflight and (root/'installed.json').exists())
        if spec['kind']=='switch':
            path=e['env']['TUNTOM_SWITCH_SOCKET']
            check_socket(path,allowed=('switch',path) in allowed)
        if spec['kind']=='tunnel' and e['interface']:
            env=e['env']
            if ('chain',env['TUNTOM_CHAIN']) not in allowed:
                for table,field in [('nat','NAT'),('nat','SNAT'),('mangle','MANGLE'),('mangle','FORWARD')]:
                    chain=env['TUNTOM_'+field+'_CHAIN']
                    if run(['iptables','-t',table,'-S',chain]).returncode==0:
                        raise RuntimeError('firewall chain already exists: '+chain)
            if ('table',env['TUNTOM_TABLE']) not in allowed:
                for family in ('-4','-6'):
                    query=run(['ip','-j','-N',family,'rule','show'])
                    if query.returncode: raise RuntimeError('cannot inspect routing rules')
                    for rule in json.loads(query.stdout):
                        if str(rule.get('table'))==env['TUNTOM_TABLE']:
                            raise RuntimeError('routing table is already referenced: '+env['TUNTOM_TABLE'])
                        if 'fwmark' in rule:
                            mark=int(str(rule['fwmark']),0)
                            mask=int(str(rule.get('fwmask',0xffffffff)),0)
                            if marks_overlap((mark,mask),(int(env['TUNTOM_MARK']),int(env['TUNTOM_MARK_MASK']))):
                                raise RuntimeError('routing mark is already used')
                    routes=run(['ip','-j',family,'route','show','table',env['TUNTOM_TABLE']])
                    if routes.returncode==0 and json.loads(routes.stdout):
                        raise RuntimeError('routing table already contains routes: '+env['TUNTOM_TABLE'])
                    if routes.returncode not in (0,2): raise RuntimeError('cannot inspect routing table')
        if e['role']=='server' and ('udp',e['env']['TUNTOM_UDP_PORT']) not in allowed:
            port=int(e['env']['TUNTOM_UDP_PORT'])
            with socket.socket(socket.AF_INET6,socket.SOCK_DGRAM) as probe:
                probe.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_V6ONLY,0)
                probe.bind(('::',port))


def write_metadata(spec, binary_only=False, claim=False):
    root=safe_root(spec)
    if binary_only:
        data=json.loads((root/'installed.json').read_text())
        data['binaries']=tree(root/'bin')
    elif claim:
        data={'owner':spec['owner'],'spec':spec,'source_digest':'','complete':False}
        if not root.exists():
            BASE.mkdir(mode=0o700,parents=True,exist_ok=True)
            staging=Path(tempfile.mkdtemp(prefix='.claim-',dir=BASE))
            try:
                path=staging/'installed.json'
                path.write_text(json.dumps(data,indent=2))
                path.chmod(0o600)
                os.rename(staging,root)
            finally:
                if staging.exists(): shutil.rmtree(staging)
            return
        if not (root/'installed.json').is_file() or json.loads((root/'installed.json').read_text())['owner']!=spec['owner']:
            raise RuntimeError('cannot claim an existing unowned directory')
    else:
        data={'owner':spec['owner'],'spec':spec,'source_digest':spec['source_digest'],
              'config_digest':digest(root/'config.json'),'assets':tree(root/'assets'),
              'binaries':tree(root/'bin'),'complete':True}
        data['unit_files']={unit:{'sha256':digest(UNITS/unit),'dropins':tree(UNITS/(unit+'.d'))} for unit in unit_names(spec)}
    temporary=root/'installed.json.new'
    with open(temporary,'w',opener=lambda p,f:os.open(p,f,0o600)) as stream:
        json.dump(data,stream,indent=2)
    os.replace(temporary,root/'installed.json')


def perform(action,spec):
    if action=='inspect':
        result=inspect(spec)
    elif action=='recheck':
        job=spec
        result=inspect(job['spec'])
        before=job['before']
        for key in ('owner','machine_id','installed','legacy','unit_files'):
            if result[key]!=before[key]: raise RuntimeError('installation changed since inspection: '+key)
        def stable(files):
            return {k:v for k,v in files.items() if not k.startswith(('logs/','active/'))}
        if stable(result['files'])!=stable(before['files']): raise RuntimeError('installation files changed since inspection')
        for unit,value in before['services'].items():
            for key in ('ActiveState','UnitFileState'):
                if result['services'].get(unit,{}).get(key)!=value.get(key):
                    raise RuntimeError('service changed since inspection: '+unit)
    elif action in ('assert-free','preflight'):
        assert_free(spec,preflight=action=='preflight'); result={'ok':True}
    elif action=='stage':
        safe_root(spec)
        directory=STAGING/(spec['key']+'-'+spec['operation_id'])
        if directory.exists(): raise RuntimeError('staging already exists')
        directory.mkdir(mode=0o700,parents=True)
        (directory/'owner.json').write_text(json.dumps({'owner':spec['owner']}))
        (directory/'owner.json').chmod(0o600)
        result={'ok':True}
    elif action=='wipe-staging':
        safe_root(spec)
        for path in stages(spec): shutil.rmtree(path)
        result={'ok':True}
    elif action=='retire-legacy':
        retire_legacy(spec); result={'ok':True}
    elif action=='finish-retirement':
        root=safe_root(spec)
        receipt=root/'retirement.json'
        if receipt.is_symlink(): raise RuntimeError('symlink retirement receipt')
        if receipt.exists(): retire_legacy(json.loads(receipt.read_text()))
        result={'ok':True}
    elif action in ('metadata','metadata-binary','claim'):
        write_metadata(spec,binary_only=action=='metadata-binary',claim=action=='claim'); result={'ok':True}
    elif action in ('lock','unlock'):
        token=spec['operation_id']
        if LOCK.is_symlink(): raise RuntimeError('symlink deployment lock')
        if action=='lock':
            try:
                fd=os.open(LOCK,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
                with os.fdopen(fd,'w') as stream: stream.write(token)
            except FileExistsError:
                if LOCK.read_text()!=token:
                    raise RuntimeError('another deployment holds '+str(LOCK)+'; inspect before recovering its lock')
        elif LOCK.exists() and LOCK.read_text()==token:
            LOCK.unlink()
        result={'ok':True}
    else:
        raise RuntimeError('unknown node action')
    return result
