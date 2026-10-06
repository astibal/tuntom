"""Offline endpoint kits. Generate artifacts only; never start a deployment."""
import hashlib
import io
import json
from pathlib import Path
import re
import secrets
import subprocess
import tarfile

from tunnel_deployments import validate, render_bundle

REPOSITORY = Path(__file__).resolve().parent.parent


def source_files(repository=REPOSITORY):
    revision = subprocess.check_output(['git', '-C', str(repository), 'rev-parse', 'HEAD'], text=True).strip()
    content = subprocess.check_output(['git', '-C', str(repository), 'archive', revision, 'src', 'LICENSE.md'], timeout=30)
    files = {}
    with tarfile.open(fileobj=io.BytesIO(content)) as archive:
        for item in archive:
            if item.isfile():
                if item.size > 8 * 1024 * 1024: raise ValueError('source file exceeds size limit')
                files[item.name] = archive.extractfile(item).read()
    return revision, files


def configuration(data):
    allowed = {'name', 'kind', 'format', 'peer', 'tunnel_id', 'count', 'port_id', 'switch_socket', 'bundle_id', 'trust_public'}
    if not isinstance(data, dict) or set(data) - allowed: raise ValueError('invalid headless bundle fields')
    if data.get('kind') not in ('endpoint', 'divert'): raise ValueError('invalid headless endpoint type')
    if data.get('format') not in ('source', 'binary'): raise ValueError('invalid bundle format')
    name = data.get('name')
    count = data.get('count', 8 if data['kind'] == 'divert' else 1)
    value = dict(name=name, tunnel_id=data.get('tunnel_id'), count=count,
        software={'source':'git_build','revision':'main'}, secret={'mode':'generate'},
        side_a={'endpoint_id':'a'*32,'role':'initiator','peer_address':data.get('peer'),'runtime':'screen','attachment':{'type':'tun'}},
        side_b={'endpoint_id':'b'*32,'role':'listener','runtime':'screen','attachment':{
            'type':'switch','switch_socket':data.get('switch_socket'), 'port_id':data.get('port_id'), 'label':'0'}})
    if data['kind'] == 'divert':
        value['via'] = dict(mode='split', service=name, instance=name+'#0', in_port='proxy-in', out_port='proxy-out',
                            in_tun='di0', out_tun='do0', namespace='', trust_public=data.get('trust_public',''))
    config, _ = validate(value)
    if config['tunnel_id'] is None: raise ValueError('headless bundle requires an explicit unused tunnel ID')
    config['deployment_id'] = secrets.token_hex(16)
    return config


START = r'''#!/bin/bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
root=$(pwd -P)
[[ "$root" =~ ^/[a-zA-Z0-9_./-]+$ && ${#root} -lt 75 ]] || { echo "Use a short absolute directory without spaces (under 75 characters)." >&2; exit 1; }
[[ $EUID == 0 ]] || { echo "Start as root inside the intended network namespace." >&2; exit 1; }
[[ $# == 2 && $1 == --secret-file ]] || { echo "Usage: ./start.sh --secret-file /absolute/path/to/32-hex-key" >&2; exit 1; }
for command in python3 screen flock ip timeout runuser getfacl setfacl; do command -v "$command" >/dev/null || { echo "Missing dependency: $command" >&2; exit 1; }; done
for binary in tuntom tuntomctl; do test -x "$root/bin/$binary" || { echo "Missing binary: $binary. For a source kit, run make at the archive root first." >&2; exit 1; }; done
getent group tuntom >/dev/null || groupadd --system tuntom
id -u tuntom >/dev/null 2>&1 || useradd --system --gid tuntom --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin tuntom
# Never overwrite the key of an existing/running attempt. Undeploy before replacing it.
python3 - "$2" "$root" <<'KEY'
import os,pathlib,re,stat,sys
p=pathlib.Path(sys.argv[1]); root=pathlib.Path(sys.argv[2])
fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
with os.fdopen(fd) as f:
    st=os.fstat(f.fileno())
    if not stat.S_ISREG(st.st_mode) or st.st_mode & 0o077: sys.exit("Secret file must be regular and mode 0600.")
    key=f.read(128).strip()
if not re.fullmatch('[0-9a-fA-F]{32}',key): sys.exit("Secret file must contain exactly 32 hex characters.")
secret=root/'secrets'
secret.mkdir(mode=0o700,exist_ok=True)
if secret.is_symlink(): sys.exit("Refusing symlink secrets directory.")
target=secret/'master.env'
data='TUNTOM_SECRET='+key.lower()+'\n'
try: fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
except FileExistsError:
    if target.is_symlink() or target.read_text()!=data: sys.exit("Existing deployment key differs; undeploy first.")
else:
    with os.fdopen(fd,'w') as f: f.write(data)
KEY
bash scripts/00-preflight.sh
bash scripts/30-install-config.sh
trap 'bash scripts/99-rollback.sh >&2' ERR
bash scripts/40-start.sh
bash scripts/50-verify.sh
trap - ERR
echo "Endpoint ready. Namespace routing and switch service rules are managed separately."
'''


def archive(data, bundles):
    config = configuration(data)
    rendered = render_bundle(config, {'a'*32:'headless','b'*32:'switch'}, '', root='/opt/tuntom/deployments/'+config['name'])
    oldroot = '/opt/tuntom/deployments/'+config['name']
    if data['kind'] == 'endpoint' and data.get('trust_public'):
        from via_deployments import public_key_records
        grants = public_key_records(data['trust_public'])
        for side in ('side_a','side_b'):
            rendered[side+'/config/control-trust.pub'] = grants
            rendered[side+'/scripts/30-install-config.sh'] += 'chmod 0755 "$root/config"\nchmod 0644 "$root/config/control-trust.pub"\n'
            for path in list(rendered):
                if path.startswith(side+'/runtime/run-'):
                    rendered[path] = rendered[path].rstrip() + ' --allow-control-trusted --control-trust-key '+oldroot+'/config/control-trust.pub\n'

    files = {}
    for side, target in (('side_a','endpoint'),('side_b','switch')):
        for path, content in rendered.items():
            if not path.startswith(side+'/'): continue
            name = path[len(side)+1:]
            if name in ('scripts/10-install-dependencies.sh','scripts/20-install-software.sh','README.txt'): continue
            if name.endswith('.sh'):
                content = content.replace('root='+oldroot+'\n','')
                content = content.replace(oldroot, '${root}')
                content = content.replace('set -euo pipefail\n', 'set -euo pipefail\nroot=$(cd -- "$(dirname -- "$0")/.." && pwd -P)\n', 1)
            files[target+'/'+name] = content.encode()
        files[target+'/.fabric-deployment-id'] = (config['deployment_id']+'\n').encode()
        files[target+'/start.sh'] = START.encode()
        for action, script in (('status','80-status'),('stop','90-stop'),('undeploy','95-undeploy')):
            files[target+'/'+action+'.sh'] = ('#!/bin/bash\nset -euo pipefail\ncd -- "$(dirname -- "$0")"\nexec bash scripts/'+script+'.sh\n').encode()
    if 'via-service.rules.txt' in rendered:
        files['switch/via-service.rules.txt'] = rendered['via-service.rules.txt'].encode()
    names = ['tuntom','tuntomctl'] + (['tuntom-divert-adapter'] if data['kind']=='divert' else [])
    if data['format']=='source':
        revision, sources = source_files()
        files.update(sources)
        sources_by_name={'tuntom':'src/main.cpp','tuntomctl':'src/control/main.cpp','tuntom-divert-adapter':'src/divert/main.cpp'}
        make = 'CXX ?= g++\nCXXFLAGS ?= -O2\nCPPFLAGS ?=\nLDFLAGS ?=\nLDLIBS ?=\n.PHONY: all clean\nall: ' + ' '.join('endpoint/bin/'+n for n in names)+' '+' '.join('switch/bin/'+n for n in names[:2])+'\n'
        for name in names:
            make += '\nendpoint/bin/'+name+': '+sources_by_name[name]+' $(shell find src -type f)\n\tmkdir -p endpoint/bin\n\t$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -pthread $< $(LDFLAGS) $(LDLIBS) -o $@\n'
        for name in names[:2]:
            make += '\nswitch/bin/'+name+': endpoint/bin/'+name+'\n\tmkdir -p switch/bin\n\tcp $< $@\n'
        make += '\nclean:\n\trm -f '+ ' '.join('endpoint/bin/'+n for n in names)+' '+' '.join('switch/bin/'+n for n in names[:2])+'\n'
        files['Makefile']=make.encode()
        platform='native build'
        config['software']={'source':'git_build','revision':revision}
    else:
        metadata, binaries = bundles.get(data.get('bundle_id'), files=True)
        if (metadata['os'],metadata['version'],metadata['architecture']) != ('ubuntu','26.04','x86_64'):
            raise ValueError('binary kit requires Ubuntu 26.04 x86-64')
        if any(name not in binaries for name in names): raise ValueError('selected bundle lacks the divert adapter; build or upload a divert bundle')
        for name in names: files['endpoint/bin/'+name]=binaries[name]
        for name in names[:2]: files['switch/bin/'+name]=binaries[name]
        revision=metadata['revision']; platform='Ubuntu 26.04 x86-64'
        config['software']={'source':'bundle','bundle_id':metadata['id'],'revision':revision}
    metadata={'type':'Headless Divert Endpoint' if data['kind']=='divert' else 'Headless Endpoint',
              'revision':revision,'format':data['format'],'platform':platform,'config':config,
              'udp_ports':[40000+config['tunnel_id']+256*i for i in range(config['count'])]}
    files['bundle.json']=json.dumps(metadata,indent=2).encode()
    for side in ('endpoint','switch'):
        files[side+'/check.py']=Path(__file__).with_name('headless_check.py').read_bytes()
        files[side+'/check.json']=json.dumps(dict(side=side,revision=revision,format=data['format'],peer=data['peer'],
            switch_socket=data['switch_socket'],binaries=names if side=='endpoint' else names[:2]),indent=2).encode()
        files[side+'/check.sh']=b'#!/bin/bash\nset -euo pipefail\ncd -- "$(dirname -- "$0")"\nexec python3 check.py\n'

    files['README.txt']=f'''{metadata['type']} — {platform}
Revision: {revision}
{"Run make (C++17 compiler, make, Linux headers); no network access is used." if data['format']=='source' else "Binaries require Ubuntu 26.04 x86-64 and compatible system libraries."}
Copy endpoint/ into the intended host/network namespace. Copy switch/ onto the switch host.
For different CPU architectures build the source kit separately on each host.
Runtime dependencies: Python 3, screen, util-linux (flock/runuser), coreutils, iproute2, acl.
Start as root; /dev/net/tun and CAP_NET_ADMIN are required. The tuntom account is created if absent.
Use a trusted root-owned short path, with no symlink ancestors.
Supply the SAME private 0600 file containing a 32-hex PSK on both sides. No secret is included.
Before start: sudo ./check.sh on each side, inside the intended namespace. This is read-only.
First: switch/start.sh --secret-file /secure/key
Then: endpoint/start.sh --secret-file /secure/key
Peer IP: {data['peer']}; UDP listener ports: {metadata['udp_ports']}
Tunnel IDs and switch port IDs are not reserved by downloading. Check collisions before starting.
status.sh reads CONTROL readiness; stop.sh requests a graceful stop.
undeploy.sh verifies process identities, stops them and deletes its own side directory including copied keys.
The external secret file, shared accounts and directory traversal ACLs remain.
Run stop/undeploy in the SAME namespaces as start. Stop endpoint first, then switch.
No autostart or systemd changes. Namespace/IPVLAN/IP addresses/routes/proxy configuration remain external.
For VIA, merge switch/via-service.rules.txt into active rules and select the service in the desired chain.
Discovery requires a compatible trusted CONTROL configuration; supply public grants when creating the kit.
'''.encode()
    files['manifest.sha256']=''.join(hashlib.sha256(v).hexdigest()+'  '+k+'\n' for k,v in sorted(files.items())).encode()
    out=io.BytesIO()
    with tarfile.open(fileobj=out,mode='w:gz') as tar:
        for name,content in sorted(files.items()):
            info=tarfile.TarInfo(name);info.size=len(content);info.mode=0o755 if name.endswith('.sh') or '/bin/' in name else 0o644
            tar.addfile(info,io.BytesIO(content))
    return 'headless_'+data['kind']+'_'+data['format']+'_'+config['name']+'.tar.gz',out.getvalue()
