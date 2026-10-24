"""Trusted host-side SAS installer. No service execution or PID manipulation."""
import base64
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


def command(argv):
    env=dict(os.environ)
    env.setdefault('SAS_RUNNER_TOKEN_FILE','/etc/smithproxy-appliance/runner-client.env')
    result=subprocess.run(argv,capture_output=True,text=True,timeout=40,env=env)
    if result.returncode:
        # CLI errors can contain response details; never echo credentials/payloads.
        raise RuntimeError('SAS command failed: '+argv[-1]+' (exit '+str(result.returncode)+')')
    return json.loads(result.stdout)


def trusted(path):
    for item in (path,*path.parents):
        info=item.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_uid!=0 or info.st_mode & 0o022:
            raise RuntimeError('Untrusted installation path: '+str(item))


def atomic(path,content,gid,mode=0o640):
    fd,tmp=tempfile.mkstemp(prefix='.fabric-',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as stream:
            os.fchown(stream.fileno(),0,gid);os.fchmod(stream.fileno(),mode)
            stream.write(content);stream.flush();os.fsync(stream.fileno())
        os.replace(tmp,path)
        fd=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)


def rows(path):
    if not path.exists(): return '',{}
    trusted(path)
    text=path.read_text();result={}
    for line in text.splitlines():
        fields=line.split('#',1)[0].split()
        if not fields:continue
        if len(fields)<2 or not re.fullmatch('[1-9][0-9]{0,8}',fields[0]) or fields[0] in result:
            raise RuntimeError('Malformed or duplicate SAS registration')
        result[fields[0]]=fields[1]
    return text,result


def validate_state(state,config):
    if state.get('contract_version')!=3 or state.get('instance_id')!=config['instance_id'] or state.get('prefix')!=config['prefix'] or state.get('instance_state')!='present':
        raise RuntimeError('SAS status identity/state mismatch')
    if not state.get('verified_at'): raise RuntimeError('SAS status is not verified')
    return state


def transport_context(location):
    """SAS V3 TE execution uses host netns; profile transport-veth is unrelated."""
    return {'_transport_namespace':'host','_transport_separate':True}


def check_transport(config,values):
    if not config.get('peer'):return
    import ipaddress
    namespace=config.get('_transport_namespace',config['namespace'])
    separate=config.get('_transport_separate',False)
    # A dataplane route cannot loop an outer socket in another network namespace.
    if not separate:
        for route in values.get('routes',[]):
            if ipaddress.ip_address(config['peer']) in ipaddress.ip_network(route['destination']):
                raise RuntimeError('SAS runs transport in instance namespace '+namespace+
                    '; requested route covers the transport IP '+config['peer']+
                    '. Configure a separate transport namespace in SAS before deploying these routes')
    command_prefix=['nsenter','--net=/proc/1/ns/net','ip'] if namespace=='host' else ['ip','-n',namespace]
    result=subprocess.run(command_prefix+['-4','-j','route','get',config['peer']],capture_output=True,text=True,timeout=10)
    paths=json.loads(result.stdout) if result.returncode==0 else []
    if not paths or any(row.get('type') in ('unreachable','blackhole','prohibit') for row in paths):
        raise RuntimeError('No route to transport IP '+config['peer']+' in SAS execution namespace '+namespace+
            (' (separate transport)' if separate else ' (instance; SAS declares no separate transport namespace)'))
    if not separate and any(row.get('dev')==config['interface'] for row in paths):
        raise RuntimeError('Transport route uses its own TUN in SAS execution namespace '+namespace)


def network(config,remove=False):
    values=config.get('network',{})
    if not values.get('addresses') and not values.get('routes'):return
    interface=config['interface'];namespace=config['namespace'];alias='fabric:sas:'+config['deployment_id']
    def ip(*args,allow_missing=False):
        result=subprocess.run(['ip','-n',namespace,*args],capture_output=True,text=True,timeout=10)
        if result.returncode and not allow_missing:raise RuntimeError('Network configuration failed: '+result.stderr.strip()[:500])
        return result
    listed=ip('-j','link','show',allow_missing=True)
    if listed.returncode:raise RuntimeError('Cannot inspect instance namespace interfaces')
    link=next((row for row in json.loads(listed.stdout) if row.get('ifname')==interface),None)
    if link and link.get('ifalias')!=alias:raise RuntimeError('TUN interface exists and is not owned by this deployment')
    if remove:
        if link:ip('link','delete','dev',interface)
        return
    check_transport(config,values)
    # Reject existing routes via other devices before modifying the interface.
    for route in values.get('routes',[]):
        family='-6' if ':' in route['destination'] else '-4'
        existing=json.loads(ip(family,'-j','route','show','exact',route['destination']).stdout)
        if any(row.get('dev')!=interface for row in existing):raise RuntimeError('Route destination already exists through another interface')
    if not link:
        ip('tuntap','add','dev',interface,'mode','tun')
        ip('link','set','dev',interface,'alias',alias)
    ip('link','set','dev',interface,'up')
    for address in values.get('addresses',[]):ip('address','replace',address,'dev',interface)
    for route in values.get('routes',[]):
        args=['-6' if ':' in route['destination'] else '-4','route','replace',route['destination']]
        if route.get('via'):args+=['via',route['via']]
        ip(*args,'dev',interface,'metric',str(route['metric']))



def apply_network(config, payload):
    """Reconcile only recorded addresses/routes; journal survives lost SSH replies."""
    desired=config['desired_network']
    ledger=payload/'.network.json'
    previous=json.loads(ledger.read_text()) if ledger.exists() else config.get('network',{'addresses':[],'routes':[]})
    interface=config['interface']
    def ip(*args, missing=False):
        result=subprocess.run(['ip','-n',config['namespace'],*args],capture_output=True,text=True,timeout=10)
        if result.returncode and not (missing and any(v in result.stderr for v in ('No such process','Cannot assign requested address'))):
            raise RuntimeError('Network update failed: '+result.stderr.strip()[:500])
        return result.stdout
    links=json.loads(ip('-j','link','show'))
    link=next((v for v in links if v.get('ifname')==interface),None)
    if not link or link.get('ifalias')!='fabric:sas:'+config['deployment_id']:
        raise RuntimeError('Apply requires the persistent TUN owned by this deployment; an existing unmarked interface cannot be adopted live')
    check_transport(config,desired)
    for route in desired['routes']:
        family='-6' if ':' in route['destination'] else '-4'
        current=json.loads(ip(family,'-j','route','show','exact',route['destination']))
        for row in current:
            if row.get('dev')!=interface:raise RuntimeError('Route conflicts with another interface')
            expected={'destination':route['destination'],'metric':row.get('metric',0)}
            if row.get('gateway'):expected['via']=row['gateway']
            if expected not in previous['routes'] and expected != route:
                raise RuntimeError('Route conflicts with an untracked route')
    # Persist the union before any mutation, allowing retry/cleanup after partial failure.
    tracked={key:list(previous[key])+[v for v in desired[key] if v not in previous[key]] for key in ('addresses','routes')}
    atomic(ledger,json.dumps(tracked).encode(),65532)
    def route_args(route,operation):
        args=['-6' if ':' in route['destination'] else '-4','route',operation,route['destination']]
        if route.get('via'):args+=['via',route['via']]
        return args+['dev',interface,'metric',str(route['metric'])]
    for route in previous['routes']:
        if route not in desired['routes']:ip(*route_args(route,'del'),missing=True)
    for address in previous['addresses']:
        if address not in desired['addresses']:ip('address','del',address,'dev',interface,missing=True)
    for address in desired['addresses']:ip('address','replace',address,'dev',interface)
    for route in desired['routes']:ip(*route_args(route,'replace'))
    atomic(ledger,json.dumps(desired).encode(),65532)
    return {'network':desired}


def execute(config):
    if os.geteuid()!=0: raise RuntimeError('SAS installation requires root SSH access')
    instance=config['instance_id'];prefix=config['prefix'];ident=config['deployment_id'];action=config['action']
    if not re.fullmatch('[0-9a-f-]{36}',instance) or not re.fullmatch('[1-9][0-9]{0,8}',prefix) or not re.fullmatch('[0-9a-f]{32}',ident):raise ValueError('invalid identity')
    location=command(['sas_which_instance',instance])
    if location.get('id')!=instance or location.get('microservice_contract_version')!=3 or location.get('microservices_dir_exists') is not True:
        raise RuntimeError('SAS V3 installation unavailable')
    if location.get('origin') not in {socket.gethostname(),socket.getfqdn()} or location['origin']!=config['origin']:
        raise RuntimeError('SSH target is not the authoritative origin')
    if location.get('microservices_dir')!=config['installation'] or location.get('namespace')!=config['namespace']:
        raise RuntimeError('Instance location changed; resolve again')
    if location.get('microservice_mode')!='managed' or location.get('microservice_execution_enabled') is not True or not all(location.get('microservice_capabilities',{}).get(k) is True for k in ('installation','supervision','status','stop')):
        raise RuntimeError('SAS managed supervision/status/stop capabilities required')
    profile=location.get('microservice_rootfs') or {}
    if profile!=config['profile'] or profile.get('schema')!=1 or profile.get('runtime_uid')!=65532 or profile.get('runtime_gid')!=65532:
        raise RuntimeError('SAS execution profile changed or unsupported')
    installation=Path(location['microservices_dir']);trusted(installation)
    directory_identity=(installation.stat().st_dev,installation.stat().st_ino)
    work=Path(location['work_dir']).resolve()
    if installation.resolve()==work or work in installation.resolve().parents or installation.resolve() in work.parents:
        raise RuntimeError('Installation overlaps appliance workspace')
    if not location.get('namespace_exists'): raise RuntimeError('Instance namespace unavailable')
    ctl=['/opt/smithproxy-appliance/sasctl','instance','microservice',instance,prefix]
    def status(): return validate_state(command(ctl+['status']),config)
    if action=='status':return status()
    locks=[]
    try:
        for name in ('db.lock',prefix+'.lock'):
            fd=os.open(installation/name,os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
            locks.append(fd)
            info=os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid!=0 or info.st_mode&0o022:raise RuntimeError('Untrusted lock')
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        # Revalidate location while holding the shared SAS filesystem locks.
        fresh=command(['sas_which_instance',instance])
        for key in ('id','origin','microservices_dir','namespace','microservice_rootfs','microservice_execution_enabled'):
            if fresh.get(key)!=location.get(key):raise RuntimeError('Instance changed under lock')
        trusted(installation)
        if (installation.stat().st_dev,installation.stat().st_ino)!=directory_identity:raise RuntimeError('SAS installation directory replaced during lock acquisition')
        if not fresh.get('namespace_exists') or not fresh.get('microservices_dir_exists') or not all(fresh.get('microservice_capabilities',{}).get(k) is True for k in ('installation','supervision','status','stop')):raise RuntimeError('SAS capabilities or directories disappeared')
        if action in ('activate','apply'):
            config={**config,**transport_context(fresh)}
        registry=installation/'db.info';text,registered=rows(registry)
        if any(key!=prefix and owner=='tuntom' for key,owner in registered.items()):raise RuntimeError('Shared Tuntom registration requires coordinated update; no files changed')
        if prefix in registered and registered[prefix]!='tuntom':raise RuntimeError('Prefix belongs to another owner')
        payload=installation/'tuntom'
        marker=payload/'.fabric-deployment'
        owned=False
        if payload.exists():
            trusted(payload)
            if marker.is_file() and not marker.is_symlink():
                trusted(marker);owned=marker.read_text().strip()==ident
            if not owned:raise RuntimeError('Existing Tuntom payload belongs to another deployment; choose an unused instance')
        script=installation/(prefix+'-start.sh')
        if script.exists() and not owned:raise RuntimeError('Existing SAS script is not owned by this deployment')
        if action=='apply':
            state=status()
            if not owned or registered.get(prefix)!='tuntom' or state.get('owner')!='tuntom' or not state.get('registered'):
                raise RuntimeError('Apply requires an owned registered SAS deployment')
            ledger=payload/'.network.json'
            if ledger.exists():trusted(ledger)
            return apply_network(config,payload)
        if action in ('withdraw','remove'):
            if not owned:
                state=status()
                if not payload.exists() and not script.exists() and prefix not in registered and state.get('process_group_empty') is True and state.get('state') in ('never_started','stopped') and state.get('owner') in (None,'tuntom'):
                    return {'state':'removed' if action=='remove' else 'stopped','registered':False}
                raise RuntimeError('Deployment ownership marker missing')
            kept=''.join(line for line in text.splitlines(keepends=True) if not (line.split('#',1)[0].split() or [None])[0]==prefix)
            atomic(registry,kept.encode(),65532)
            state=status()
            if state.get('owner') not in (None,'tuntom'):raise RuntimeError('SAS run owner mismatch')
            if state.get('run_id'):
                if state.get('owner')!='tuntom':raise RuntimeError('Cannot stop an unowned run')
                result=command(ctl+['stop','--owner','tuntom','--run-id',state['run_id']])
                if result.get('contract_version')!=3 or result.get('instance_id')!=instance or result.get('prefix')!=prefix or result.get('owner')!='tuntom' or result.get('run_id')!=state['run_id'] or result.get('result') not in ('stopped','already_stopped') or result.get('process_group_empty') is not True:
                    raise RuntimeError('SAS stop did not confirm exact-run emptiness')
            state=status()
            if state.get('process_group_empty') is not True or state.get('state') not in ('stopped','never_started'):
                raise RuntimeError('SAS group is not verified empty; files preserved')
            if action=='remove':
                cleanup=dict(config)
                if (payload/'.network.json').exists():cleanup['network']={'addresses':['owned'],'routes':[]}
                network(cleanup,remove=True)
                # Only our private installation payload; never runtime or stable locks.
                for item in payload.rglob('*'):trusted(item)
                if script.exists():trusted(script);script.unlink()
                shutil.rmtree(payload)
            return {'state':'removed' if action=='remove' else 'stopped','registered':False}
        state=status()
        if state.get('process_group_empty') is not True or state.get('state') not in ('never_started','stopped') or state.get('owner') not in (None,'tuntom'):
            raise RuntimeError('Existing SAS run must be undeployed before installation')
        if prefix in registered:raise RuntimeError('Prefix already registered; inspect status before retrying')
        if action=='prepare':
            # Compare required ELF symbol versions against the exact SAS rootfs manifest.
            digest=profile.get('sha256','')
            if not re.fullmatch('[0-9a-f]{64}',digest):raise RuntimeError('Missing rootfs digest')
            manifest_path=installation.parent.parent/'microservice-rootfs'/digest/'rootfs.json'
            trusted(manifest_path)
            manifest=json.loads(manifest_path.read_text())
            if manifest.get('sha256')!=digest:raise RuntimeError('Rootfs manifest identity mismatch')
            installed_libraries={Path(name).name for name in manifest.get('files',{})}
            if not set(config.get('needed_libraries',[])).issubset(installed_libraries):raise RuntimeError('Required shared libraries absent from SAS rootfs')
            available=set(v for values in manifest.get('abi_versions',{}).values() for v in values)
            if not set(config['abi_versions']).issubset(available):raise RuntimeError('Binary requires ABI versions absent from SAS rootfs')
            raw=base64.b64decode(config['binary'],validate=True)
            if hashlib.sha256(raw).hexdigest()!=config['sha256']:raise RuntimeError('Binary digest mismatch')
            os.chown(installation,0,65532);os.chmod(installation,0o750)
            if not payload.exists():payload.mkdir(mode=0o750)
            os.chown(payload,0,65532);os.chmod(payload,0o750)
            atomic(marker,(ident+'\n').encode(),65532)
            secrets=payload/'secrets'
            secrets.mkdir(exist_ok=True,mode=0o750);trusted(secrets);os.chown(secrets,0,65532)
            for name,value in [('tuntom',raw),('version.txt',(config['version']+'\n').encode()),('version-commit.txt',(config.get('commit','unknown')+'\n').encode())]:
                target=payload/name
                if target.exists() or target.is_symlink():trusted(target)
                atomic(target,value,65532,0o750 if name=='tuntom' else 0o640)
            atomic(secrets/(ident+'.key'),(config['secret']+'\n').encode(),65532)
            atomic(script,config['script'].encode(),65532,0o750)
            atomic(payload/'.prepared',(config['sha256']+'\n').encode(),65532)
            return {'state':'prepared','registered':False}
        if action=='activate':
            if not owned or not script.is_file():raise RuntimeError('Prepare deployment first')
            trusted(script);trusted(payload/'.prepared')
            if (payload/'.prepared').read_text().strip()!=config['sha256']:raise RuntimeError('Prepared payload changed')
            if hashlib.sha256((payload/'tuntom').read_bytes()).hexdigest()!=config['sha256']:raise RuntimeError('Installed binary changed')
            network(config)
            if text and not text.endswith('\n'):text+='\n'
            atomic(registry,(text+config['registration']).encode(),65532)
            return {'state':'registered','registered':True,'scan_interval':location.get('microservice_scan_interval_seconds')}
        raise ValueError('unsupported installer action')
    finally:
        for fd in reversed(locks):os.close(fd)
