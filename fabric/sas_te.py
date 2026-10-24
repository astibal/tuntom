"""Review-only SAS Tunel Endpoint rendering and read-only SSH lookup for UI previews."""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import uuid


def token(value, field, pattern):
    if not isinstance(value, str) or not re.fullmatch(pattern, value):
        raise ValueError('invalid '+field)
    return value


def installation_location(location):
    if type(location.get('microservice_contract_version')) is not int or location['microservice_contract_version'] != 3:
        raise ValueError('SAS contract V3 required: lookup must advertise microservice_contract_version=3 and microservices_dir; V2 fallback is disabled')
    paths=[]
    for key in ('work_dir','microservices_dir'):
        value=location.get(key)
        if not isinstance(value,str) or not value.startswith('/') or value=='/' or len(value)>4096 or any(ord(c)<32 for c in value) or any(part in ('.','..') for part in value.split('/')):
            raise ValueError('invalid authoritative '+key)
        paths.append(Path(value))
    work,install=paths
    if install==work or work in install.parents or install in work.parents:
        raise ValueError('microservices_dir must be separate from appliance work_dir')
    return str(install)


def network_options(value):
    if value is None:return {'addresses':[],'routes':[]}
    if not isinstance(value,dict) or set(value)!={'addresses','routes'}:raise ValueError('network expects addresses and routes')
    if not isinstance(value['addresses'],list) or not isinstance(value['routes'],list) or len(value['addresses'])>16 or len(value['routes'])>32:raise ValueError('too many network entries')
    addresses=[];routes=[]
    for address in value['addresses']:
        if not isinstance(address,str) or '/' not in address:raise ValueError('interface addresses require a CIDR prefix')
        addresses.append(str(ipaddress.ip_interface(address)))
    if len(set(addresses))!=len(addresses):raise ValueError('duplicate interface address')
    for route in value['routes']:
        if not isinstance(route,dict) or set(route)-{'destination','via','metric'} or 'destination' not in route:raise ValueError('invalid route fields')
        destination=ipaddress.ip_network(route['destination'],strict=True)
        result={'destination':str(destination),'metric':route.get('metric',100)}
        if type(result['metric']) is not int or not 0<=result['metric']<=4294967295:raise ValueError('invalid route metric')
        if route.get('via'):
            gateway=ipaddress.ip_address(route['via'])
            if gateway.version!=destination.version:raise ValueError('route destination and gateway family differ')
            result['via']=str(gateway)
        routes.append(result)
    if len({(r['destination'],r['metric']) for r in routes})!=len(routes):raise ValueError('duplicate route')
    return {'addresses':addresses,'routes':routes}


def render(template, location):
    if set(template)-{'network'} != {'type','instance_id','deployment_id','prefix','tunnel_id','interface','peer','switch','build','runtime'}:
        raise ValueError('unexpected SAS TE template fields')
    if template['type'] != 'sas-tunel-endpoint': raise ValueError('expected SAS Tunel Endpoint')
    network=network_options(template.get('network'))
    instance=str(uuid.UUID(template['instance_id']))
    if instance != template['instance_id']: raise ValueError('use canonical full instance UUID')
    deployment=token(template['deployment_id'],'deployment_id',r'[0-9a-f]{32}')
    prefix=token(template['prefix'],'prefix',r'[1-9][0-9]{0,8}')
    ident=template['tunnel_id']
    if not isinstance(ident,int) or isinstance(ident,bool) or not 1 <= ident <= 255: raise ValueError('tunnel_id must be 1..255')
    interface=token(template['interface'],'interface',r'[A-Za-z0-9][A-Za-z0-9_.-]{0,14}')
    peer=str(ipaddress.IPv4Address(template['peer']))
    if location.get('id') != instance: raise ValueError('location instance identity mismatch')
    if location.get('namespace_exists') is not True or location.get('work_dir_exists') is not True:
        raise ValueError('instance namespace/workspace unavailable')
    origin=token(location.get('origin'),'origin',r'[A-Za-z0-9][A-Za-z0-9.-]{0,252}')
    namespace=token(location.get('namespace'),'namespace',r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}')
    installation=installation_location(location)
    work=location.get('work_dir')
    if not isinstance(work,str) or not work.startswith('/') or '..' in work.split('/') or any(ord(c)<32 for c in work):
        raise ValueError('invalid authoritative work_dir')
    switch=template['switch']
    if set(switch) != {'socket','port_id','label'}: raise ValueError('invalid switch fields')
    if not isinstance(switch['socket'],str) or not switch['socket'].startswith('/') or len(switch['socket'])>=108 or any(ord(c)<32 for c in switch['socket']):
        raise ValueError('invalid switch socket')
    port=token(switch['port_id'],'switch port',r'[A-Za-z0-9][A-Za-z0-9_.-]{0,47}')
    label=switch['label']
    if not isinstance(label,int) or isinstance(label,bool) or not 0<=label<2**64: raise ValueError('invalid switch label')
    build=template['build']
    if set(build) != {'commit','version'}: raise ValueError('invalid build fields')
    commit=token(build.get('commit','unknown'),'build commit',r'unknown|[0-9a-f]{40}|[0-9a-f]{64}')
    version=token(build['version'],'version',r'[A-Za-z0-9][A-Za-z0-9._+-]{0,95}')
    runtime=template['runtime']
    if set(runtime) not in ({'user','group'}, {'user','group','create_user_if_needed'}): raise ValueError('invalid runtime fields')
    if type(runtime.get('create_user_if_needed',False)) is not bool: raise ValueError('invalid account creation option')
    user=token(runtime['user'],'runtime user',r'[a-z_][a-z0-9_-]{0,31}')
    group=token(runtime['group'],'runtime group',r'[a-z_][a-z0-9_-]{0,31}')
    secret='./tuntom/secrets/'+deployment+'.key'
    args=['./tuntom/tuntom','client',str(ident),interface,peer,
          '--tun-netns','@INSTANCE_NETNS@','--tun-up','--user',user,'--group',group,
          '--control-socket','@RUNTIME_CONTROL@','--no-stats']
    # SAS owns locking, pending-start tracking, process containment and restarts.
    # A PID is published only after local input validation, immediately before exec.
    # Substitute only explicit placeholders, never variable names such as TUNTOM_SECRET.
    script='''#!/bin/sh
set -eu
umask 077
# SAS V3 supplies read-only installation cwd and private writable per-run state.
# The installer MUST NOT execute this script. SAS owns locks and process lifecycle.
: "${SAS_INSTANCE_NETNS:?SAS instance namespace handle missing}"
: "${SAS_MICROSERVICE_DIR:?SAS V3 installation path missing}"
: "${SAS_MICROSERVICE_RUNTIME_DIR:?SAS V3 runtime path missing}"
: "${SAS_MICROSERVICE_PID_FILE:?SAS V3 PID path missing}"
: "${SAS_MICROSERVICE_RUN_ID:?SAS V3 run identity missing}"
test "${SAS_MICROSERVICE_PREFIX:-}" = @PREFIX@ || { echo 'SAS prefix mismatch' >&2; exit 1; }
case "$SAS_MICROSERVICE_DIR:$SAS_MICROSERVICE_RUNTIME_DIR" in /*:/*) ;; *) echo 'SAS paths must be absolute' >&2; exit 1;; esac
install_dir=$(cd -P "$SAS_MICROSERVICE_DIR" && pwd -P)
runtime_dir=$(cd -P "$SAS_MICROSERVICE_RUNTIME_DIR" && pwd -P)
test "$(pwd -P)" = "$install_dir" || { echo 'SAS installation cwd mismatch' >&2; exit 1; }
case "$runtime_dir/" in "$install_dir/"*) echo 'Runtime must be outside installation' >&2; exit 1;; esac
case "$install_dir/" in "$runtime_dir/"*) echo 'Installation must be outside runtime' >&2; exit 1;; esac
test "$SAS_MICROSERVICE_PID_FILE" = "$SAS_MICROSERVICE_RUNTIME_DIR/@PREFIX@.pid" || { echo 'SAS PID path mismatch' >&2; exit 1; }
test -x ./tuntom/tuntom || { echo 'Missing executable tuntom' >&2; exit 1; }
TUNTOM_SECRET=
IFS= read -r TUNTOM_SECRET < @SECRET@ || test -n "$TUNTOM_SECRET"
case "$TUNTOM_SECRET" in ''|*[!0-9a-fA-F]*) echo 'Invalid PSK file' >&2; exit 1;; esac
test "${#TUNTOM_SECRET}" -eq 32 || { echo 'PSK must be 32 hex characters' >&2; exit 1; }
export TUNTOM_SECRET
pid_tmp="$SAS_MICROSERVICE_RUNTIME_DIR/@PREFIX@.pid.tmp.$$"
(set -C; printf '%s\\n' "$$" > "$pid_tmp")
mv -f -- "$pid_tmp" "$SAS_MICROSERVICE_PID_FILE"
exec @COMMAND@
'''.replace('@SECRET@',shlex.quote(secret)).replace('@PREFIX@',prefix).replace('@COMMAND@',shlex.join(args).replace("'@RUNTIME_CONTROL@'", '"$SAS_MICROSERVICE_RUNTIME_DIR/'+prefix+'.control"').replace('@RUNTIME_CONTROL@', '"$SAS_MICROSERVICE_RUNTIME_DIR/'+prefix+'.control"').replace("'@INSTANCE_NETNS@'", '"$SAS_INSTANCE_NETNS"').replace('@INSTANCE_NETNS@', '"$SAS_INSTANCE_NETNS"'))
    listener=['tuntom','server',str(ident),'-','--switch-socket',switch['socket'],
              '--switch-port-id',port,'--switch-label',str(label),'--user',user,'--group',group]
    plan={'type':'SAS Tunel Endpoint','contract_version':3,'live_enabled':False,
          'instance_id':instance,'deployment_id':deployment,'prefix_proposal':prefix,
          'location_snapshot':dict(location),'target_origin':origin,'host_microservices_dir':installation,'appliance_work_dir':work,
          'execution':{'process_network':'SAS-approved transport namespace outside instance',
                       'tun_network':namespace,'cwd':'SAS_MICROSERVICE_DIR (read-only installation)', 'runtime':'SAS_MICROSERVICE_RUNTIME_DIR (private per run)', 'pid_file':'SAS_MICROSERVICE_PID_FILE','pid_namespace':'host-visible'},
          'network':network, 'network_owner':'Fabric persistent TUN only when addresses/routes requested; removed after verified SAS stop',
          'required_tools':['/bin/sh','mv'], 'required_accounts':runtime,
          'account_provisioning':{'create_if_needed':runtime.get('create_user_if_needed',False),
              'execution':'deployment_only_after_verified_SAS_runtime_profile',
              'policy':'create missing system account/group only; preserve existing UID/GID; fail if unsupported; never modify host accounts as a substitute for rootfs accounts'},
          'required_payload':{'tuntom':{'commit':commit,'version':version,'abi':'must match versioned SAS runtime profile'}},
          'prepare_directories':['tuntom/secrets'],
          'secret_destination':'tuntom/secrets/'+deployment+'.key',
          'switch':{'argv':listener,'udp_port':40000+ident,'secret':'same PSK, provision separately','start_owner':'separate managed switch deployment'},
          'gates':['SAS V3 status/stop interoperability', 'versioned plain/rootfs execution profile and V3 mount/permission isolation',
                   'SAS agreement for external transport netns and visible target netns handle',
                   'fresh destination lookup and active-instance verification under locks',
                   'prefix allocation under db.lock; preserved stable service lock',
                   'verified empty SAS slot before installation',
                   'switch tunnel ID / port collision checks',
                   'installer provisions protected key; SAS provisions private runtime and read-only installation; appliance cannot access either tree'],
          'update_undeploy':['lock db.lock then all affected prefix locks in numeric order',
                             'withdraw affected registrations atomically',
                             'query and stop exact owner=tuntom / run_id via SAS',
                             'require verified empty current runs; errors preserve files and withdrawn state',
                             'only then replace/remove owned installation files; SAS exclusively owns runtime/PID cleanup',
                             'update restores registration; undeploy preserves other owners and lock files',
                             'Fabric never starts services or signals PIDs']}
    files={ 'plan.json':json.dumps(plan,indent=2)+'\n',
            'payload/microservices/'+prefix+'-start.sh':script,
            'registration.row':f'{prefix} tuntom {ident} deployment={deployment} role=tunnel-client\n',
            'payload/microservices/tuntom/version-commit.txt':commit+'\n','payload/microservices/tuntom/version.txt':version+'\n'}
    files['manifest.sha256']=''.join(hashlib.sha256(v.encode()).hexdigest()+'  '+k+'\n' for k,v in sorted(files.items()))
    return files


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--template',type=Path,required=True)
    parser.add_argument('--location',type=Path,required=True,help='authoritative sasctl instance location UUID JSON snapshot')
    parser.add_argument('--output',type=Path,required=True,help='new local review directory, never an instance workspace')
    args=parser.parse_args()
    files=render(json.loads(args.template.read_text()),json.loads(args.location.read_text()))
    args.output.mkdir(mode=0o700,parents=False,exist_ok=False)
    for name,content in files.items():
        path=args.output/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content);path.chmod(0o600)
    print('Review-only SAS TE template generated. No registration, process or remote host changed.')




def ssh_origin_observation(endpoints, entry, origin):
    """Read the connection as seen by the origin, never infer a transport route."""
    try:
        raw = endpoints.run(entry, "hostname; hostname -f; printf '%s\\n' \"$SSH_CONNECTION\"", timeout=5)
        lines = raw.strip().splitlines()
        if len(lines) != 3 or origin.rstrip('.').lower() not in {x.rstrip('.').lower() for x in lines[:2]}:
            return {'status': 'unavailable', 'reason': 'entry_is_not_verified_origin'}
        source, source_port, destination, destination_port = lines[2].split()
        source = str(ipaddress.ip_address(source))
        destination = str(ipaddress.ip_address(destination))
        if not all(p.isdecimal() and 1 <= int(p) <= 65535 for p in (source_port, destination_port)):
            raise ValueError('invalid SSH ports')
        return {'status': 'observed', 'origin': origin, 'source_ip': source,
                'destination_ip': destination, 'destination_port': int(destination_port),
                'scope': 'direct_entry_origin_ssh', 'transport_verified': False}
    except (ValueError, RuntimeError, OSError):
        return {'status': 'unavailable', 'reason': 'ssh_connection_unavailable'}


def sanity_observation(endpoints, entry, location, runtime):
    user=token(runtime.get('user','tuntom'),'runtime user',r'[a-z_][a-z0-9_-]{0,31}')
    group=token(runtime.get('group','tuntom'),'runtime group',r'[a-z_][a-z0-9_-]{0,31}')
    namespace=token(location.get('namespace'),'namespace',r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}')
    work=location.get('work_dir')
    if not isinstance(work,str) or not work.startswith('/') or '..' in work.split('/') or len(work)>4096 or any(ord(c)<32 for c in work):
        raise ValueError('invalid authoritative work_dir')
    config={'origin':location['origin'],'microservices_dir':installation_location(location),'work_dir':work,'namespace':namespace,'user':user,'group':group}
    try:
        raw=endpoints.run(entry,'python3 - '+shlex.quote(json.dumps(config)),
                          input_data=Path(__file__).with_name('sas_sanity.py').read_bytes(),timeout=10)
        if len(raw.encode())>32768: raise ValueError('sanity response too large')
        result=json.loads(raw)
        if not isinstance(result,dict) or result.get('status') not in {'checked','unavailable'}:
            raise ValueError('invalid sanity response')
        return result
    except (ValueError, OSError, RuntimeError):
        return {'status':'unavailable','reason':'probe_failed','scope':'origin_host','runtime_verified':False}


def installed_binary():
    # Fixed deployment locations, never a path supplied by an API caller.
    for path in (Path('/var/lib/tuntom-fabric/bin/tuntom'), Path('/opt/tuntom/bin/tuntom'), Path(__file__).resolve().parents[1]/'build-lab/tuntom'):
        if not os.access(path, os.R_OK): continue
        if not path.is_file(): continue
        if path.stat().st_size > 32*1024*1024: raise ValueError('installed binary exceeds 32 MiB')
        content=path.read_bytes()
        if len(content)<64 or content[:6] != b'\x7fELF\x02\x01':
            raise ValueError('installed tuntom is not a supported ELF64 binary')
        machine=int.from_bytes(content[18:20],'little')
        architecture={62:'x86_64',183:'aarch64'}.get(machine)
        if not architecture: raise ValueError('unsupported installed binary architecture')
        metadata={'path':str(path),'sha256':hashlib.sha256(content).hexdigest(),'architecture':architecture}
        try:
            output=subprocess.run([str(path),'--version'],capture_output=True,text=True,timeout=5,check=True).stdout.strip()
        except (subprocess.SubprocessError, UnicodeError) as error:
            raise ValueError('Installed tuntom --version failed') from error
        match=re.fullmatch(r'tuntom ([0-9]+\.[0-9]+\.[0-9]+)',output)
        if not match: raise ValueError('Installed tuntom returned an unsupported --version response')
        if hashlib.sha256(path.read_bytes()).hexdigest()!=metadata['sha256']:
            raise ValueError('Installed binary changed while reading its version; retry')
        metadata['version']=match[1]
        candidate=path.parent/'version-commit.txt'
        if candidate.is_file() and candidate.stat().st_size<=256:
            commit=candidate.read_text().strip()
            if re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}',commit): metadata['commit']=commit
        return metadata,content
    raise ValueError('Installed tuntom binary not found in Fabric deployment locations')


def instance_reference(value):
    return token(value,'instance UUID or alias',r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}')


def resolved_instance(reference,location):
    if not isinstance(location,dict):raise ValueError('SAS lookup must return a JSON object')
    try:canonical=str(uuid.UUID(location.get('id','')))
    except (ValueError,TypeError,AttributeError):raise ValueError('SAS lookup did not return a valid instance UUID') from None
    if canonical!=location.get('id'):raise ValueError('SAS lookup returned a non-canonical UUID')
    try:requested=str(uuid.UUID(reference))
    except ValueError:requested=None
    if requested is not None and requested!=canonical:raise ValueError('location instance identity mismatch')
    return canonical


def resolve_request(data, endpoints):
    if not isinstance(data, dict) or set(data) not in ({'entry_endpoint_id', 'instance_id'}, {'entry_endpoint_id', 'instance_id', 'runtime'}):
        raise ValueError('expected entry_endpoint_id and instance_id')
    entry=token(data['entry_endpoint_id'],'entry endpoint',r'[0-9a-f]{32}')
    runtime=data.get('runtime',{'user':'tuntom','group':'tuntom'})
    if not isinstance(runtime,dict) or set(runtime) != {'user','group'}: raise ValueError('invalid runtime fields')
    for key in ('user','group'): token(runtime[key],'runtime '+key,r'[a-z_][a-z0-9_-]{0,31}')
    instance=instance_reference(data['instance_id'])
    raw=endpoints.run(entry,'sas_which_instance '+shlex.quote(instance),timeout=30)
    if len(raw.encode()) > 65536: raise ValueError('SAS lookup response too large')
    location=json.loads(raw)
    resolved_instance(instance,location)
    origin=token(location.get('origin'),'origin',r'[A-Za-z0-9][A-Za-z0-9.-]{0,252}')
    fields={'id','origin','namespace','namespace_path','work_dir','state','namespace_exists','work_dir_exists','unit','slice','microservices_dir','microservice_contract_version','microservices_dir_exists','microservice_capabilities','microservice_mode','microservice_execution_enabled','microservice_rootfs','microservice_scan_interval_seconds','transport_namespace','transport_namespace_path','transport_namespace_exists'}
    location={k:v for k,v in location.items() if k in fields}
    installation_location(location)
    observation=ssh_origin_observation(endpoints,entry,origin)
    try: binary,_=installed_binary()
    except (OSError,ValueError) as error: binary={'error':str(error)}
    return {'location':location,'ssh_observation':observation,'installed_binary':binary,
            'sanity':sanity_observation(endpoints,entry,location,runtime)}


def preview_request(data, endpoints):
    """Authenticated UI preview; the only remote action is authoritative lookup."""
    import base64
    import io
    import secrets
    import tarfile
    if not isinstance(data,dict) or set(data) not in ({'entry_endpoint_id','template'}, {'entry_endpoint_id','template','package'}):
        raise ValueError('expected entry_endpoint_id and template')
    entry=token(data['entry_endpoint_id'],'entry endpoint',r'[0-9a-f]{32}')
    if not isinstance(data['template'],dict): raise ValueError('invalid template')
    package=data.get('package',False)
    if type(package) is not bool: raise ValueError('invalid package option')
    template=dict(data['template'])
    if isinstance(template.get('switch'),dict):
        template['switch']=dict(template['switch'])
        label=template['switch'].get('label')
        if isinstance(label,str) and re.fullmatch(r'[0-9]{1,20}',label): template['switch']['label']=int(label)
    template.setdefault('deployment_id',secrets.token_hex(16))
    metadata=None
    binary_content=None
    if package:
        metadata,binary_content=installed_binary()
        template['build']={'version':metadata['version'],'commit':metadata.get('commit','unknown')}
    # Validate all other fields before SSH; SAS resolves aliases to pinned UUIDs.
    instance=instance_reference(template.get('instance_id'))
    validation_id='11111111-1111-4111-8111-111111111111'
    render({**template,'instance_id':validation_id,'peer':template.get('peer') or '192.0.2.1'},dict(id=validation_id,origin='validation',namespace='validation',
                         work_dir='/validation/work',microservices_dir='/validation/microservices',microservice_contract_version=3,namespace_exists=True,work_dir_exists=True))
    result=endpoints.run(entry,'sas_which_instance '+shlex.quote(instance),timeout=30)
    if len(result.encode())>65536: raise ValueError('SAS lookup response too large')
    location=json.loads(result)
    template['instance_id']=resolved_instance(instance,location)
    fields={'id','origin','namespace','namespace_path','work_dir','state','namespace_exists','work_dir_exists','unit','slice','microservices_dir','microservice_contract_version','microservices_dir_exists','microservice_capabilities','microservice_mode','microservice_execution_enabled','microservice_rootfs','microservice_scan_interval_seconds','transport_namespace','transport_namespace_path','transport_namespace_exists'}
    location={k:v for k,v in location.items() if k in fields}
    installation_location(location)
    observation=ssh_origin_observation(endpoints,entry,location.get('origin',''))
    if not template.get('peer'):
        if observation['status'] != 'observed':
            raise ValueError('Cannot detect transport IP on the origin; enter Switch transport IP manually')
        template['peer']=observation['source_ip']
        observation['used_as_transport_ip']=True
    files=render(template,location)
    plan=json.loads(files['plan.json'])
    plan['ssh_observation']=observation
    if package:
        sanity=sanity_observation(endpoints,entry,location,template['runtime'])
        checks={row['name']:row for row in sanity.get('checks',[])}
        if sanity.get('status') != 'checked' or checks.get('architecture',{}).get('detail') != metadata['architecture']:
            raise ValueError('Resolve must verify origin host architecture matches the installed binary')
        for field in ('installation','installation_paths','namespace','sh','mv','tun_device'):
            if checks.get(field,{}).get('status') != 'ok': raise ValueError('Host prerequisite missing: '+field)
        for field in (() if location.get('microservice_mode')=='managed' else ('user','group')):
            if checks.get(field,{}).get('status') != 'ok':
                if not template['runtime'].get('create_user_if_needed') or not sanity.get('host_account_creation_possible'):
                    raise ValueError('Missing runtime account/group; enable account creation if supported')
        for key in ('commit','version'):
            if metadata.get(key) and metadata[key] != template['build'][key]:
                raise ValueError('Build '+key+' differs from installed binary metadata')
        plan['live_enabled']=location.get('microservice_mode')=='managed' and location.get('microservice_execution_enabled') is True and all(location.get('microservice_capabilities',{}).get(key) is True for key in ('installation','supervision','status','stop'))
        plan['binary']=metadata
        plan['sanity']=sanity
        plan['execution']['filesystem']='SAS V3 managed rootfs' if location.get('microservice_mode')=='managed' else 'SAS V3 plain mode with private mount sandbox'
        plan['account_provisioning']['execution']='future deployment on origin host; no changes during package creation'
        plan['gates'].append('verify shared-library compatibility on origin before deployment')
        files['PACKAGE-README.txt']='SAS V3 plain-mode deployment package. No automatic deployment.\nBinary included at payload/microservices/tuntom/tuntom. Version is read from the installed binary --version; commit is optional sidecar metadata.\nDo not run the service manually: SAS owns startup and stopping.\nNo PSK is included. Before deployment provision a protected shared key, verify binary library compatibility, resolve switch collisions, create missing accounts only if allowed, and prepare the server side listed in plan.json.\nRegistration is a proposal, not an instruction to activate the service now.\n'

    files['plan.json']=json.dumps(plan,indent=2)+'\n'
    files['template.json']=json.dumps(template,indent=2)+'\n'
    files['manifest.sha256']=''.join(hashlib.sha256(v.encode()).hexdigest()+'  '+k+'\n' for k,v in sorted(files.items()) if k!='manifest.sha256')
    if binary_content is not None:
        files['manifest.sha256']+=hashlib.sha256(binary_content).hexdigest()+'  payload/microservices/tuntom/tuntom\n'
    output=io.BytesIO()
    with tarfile.open(fileobj=output,mode='w:gz') as archive:
        for name,content in sorted(files.items()):
            raw=content.encode();info=tarfile.TarInfo(name);info.size=len(raw);info.mode=0o600
            archive.addfile(info,io.BytesIO(raw))
        if binary_content is not None:
            info=tarfile.TarInfo('payload/microservices/tuntom/tuntom');info.size=len(binary_content);info.mode=0o755
            archive.addfile(info,io.BytesIO(binary_content))
    return {'plan':json.loads(files['plan.json']),'files':files,
            'archive_base64':base64.b64encode(output.getvalue()).decode(),
            'filename':('deploy_package_sas-te-' if package else 'sas-te-')+template['deployment_id']+'.tar.gz'}


if __name__=='__main__':main()
