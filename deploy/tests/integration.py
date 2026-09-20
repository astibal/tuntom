#!/usr/bin/env python3
"""Run real Ansible against a fake systemd inside a disposable user namespace.

Usage: python3 deploy/tests/integration.py /path/to/ansible-venv/bin/python
Requires bubblewrap with unprivileged user namespaces. No host services run.
"""
import hashlib
import json
import os
from pathlib import Path
import pty
import shutil
import subprocess
import sys
import tempfile

DEPLOY=Path(__file__).resolve().parents[1]
FAKE_SYSTEMCTL='''#!/usr/bin/python3
import json,sys,os
from pathlib import Path
statefile=Path('/mnt/systemd.json')
state=json.loads(statefile.read_text()) if statefile.exists() else {}
args=[x for x in sys.argv[1:] if not x.startswith('-')]
action=args[0]; units=args[1:]
def path(u): return Path('/etc/systemd/system')/u
def value(u): return state.setdefault(u,{'ActiveState':'inactive','UnitFileState':'disabled' if u.endswith('.target') else 'static'})
def members(u):
    if not path(u).exists(): return []
    return next((l.split('=',1)[1].split() for l in path(u).read_text().splitlines() if l.startswith('Wants=')),[])
if action=='show':
    u=units[0]; v=value(u)
    print('LoadState='+('loaded' if path(u).is_file() else 'not-found'))
    print('FragmentPath='+(str(path(u)) if path(u).is_file() else ''))
    print('SubState='+('running' if v['ActiveState']=='active' else 'dead'))
    for k,w in v.items(): print(k+'='+w)
elif action in ('is-enabled','is-active'):
    result=value(units[0])['UnitFileState' if action=='is-enabled' else 'ActiveState']
    print(result); sys.exit(0 if result in ('enabled','active','static') else 1)
elif action in ('enable','disable'):
    for u in units:
        value(u)['UnitFileState']='enabled' if action=='enable' else 'disabled'
        link=Path('/etc/systemd/system/multi-user.target.wants')/u
        link.parent.mkdir(exist_ok=True)
        if action=='enable' and not link.is_symlink(): link.symlink_to(path(u))
        elif action=='disable': link.unlink(missing_ok=True)
elif action in ('start','stop','restart'):
    for u in units:
        value(u)['ActiveState']='inactive' if action=='stop' else 'active'
        for m in members(u): value(m)['ActiveState']=value(u)['ActiveState']
        if action!='stop' and path(u).exists():
            for line in path(u).read_text().splitlines():
                if line.startswith('Requires='):
                    for m in line.split('=',1)[1].split(): value(m)['ActiveState']='active'
elif action in ('daemon-reload','reset-failed'): pass
else: raise SystemExit('unsupported fake systemctl action: '+action)
statefile.write_text(json.dumps(state))
if action not in ('show','is-active','is-enabled'):
    with open('/mnt/events','a') as log: log.write(' '.join(args)+'\\n')
'''
FAKE_BUILD='''#!/usr/bin/python3
import os,sys
from pathlib import Path
output=Path(sys.argv[sys.argv.index('-o')+1])
if Path('/mnt/fail-build').exists(): raise SystemExit(33)
revision=Path('/mnt/revision').read_text()
output.write_text('#!/bin/sh\\n# fixture '+output.name+' '+revision+'\\nexit 0\\n')
output.chmod(0o755)
'''


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def inside(python):
    uidmap=Path('/proc/self/uid_map').read_text().split()
    assert os.geteuid()==0 and uidmap[0]=='0' and uidmap[1]!='0' and uidmap[2]=='1', 'must run in the disposable user namespace'
    assert Path('/mnt/isolated').exists(), 'missing isolation marker'
    inventory=Path('/mnt/inventory')
    inventory.mkdir()
    (inventory/'hosts.yml').write_text('all:\n  hosts:\n    local:\n      ansible_connection: local\n      ansible_python_interpreter: /usr/bin/python3\n')
    root=Path('/var/lib/tuntom-deploy/instances/tunnel-psx2')
    instance=inventory/'instances/tunnels/psx2'
    instance.mkdir(parents=True)
    (instance/'secrets').mkdir(mode=0o700)
    secret='0123456789abcdef0123456789abcdef'
    (instance/'secrets/master.key').write_text(secret+'\n')
    (instance/'secrets/master.key').chmod(0o600)
    definition='''tuntom_instance:
  schema: 1
  tunnel_id: 42
  count: 2
  client:
    host: local
    peer_address: 192.0.2.2
    switch_socket: /run/shared.sock
    port_id: psx2-client
    label: 42
  server:
    host: local
    switch_socket: /run/shared.sock
    port_id: psx2-server
    label: 42
'''
    (instance/'instance.yml').write_text(definition)
    (instance/'manual.txt').write_text('central original\n')
    Path('/mnt/tmp').mkdir(exist_ok=True)
    env=dict(os.environ,PATH='/mnt/bin:'+os.environ['PATH'],TMPDIR='/mnt/tmp',ANSIBLE_LOCAL_TEMP='/mnt/ansible',ANSIBLE_REMOTE_TEMP='/mnt/remote',ANSIBLE_NOCOLOR='1',TUNTOM_DEPLOY_PYTHON=python)
    def invoke(op,extra=(),ok=True,wipe=False):
        command=[python,str(DEPLOY/'helpers/controller.py'),'--inventory',str(inventory),'--'+op,*extra]
        if '--instance' not in extra: command+=['--tunnel','psx2']
        master=slave=None
        if wipe:
            master,slave=pty.openpty()
            os.write(master,b'ano\n')
        try:
            result=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,stdin=slave,timeout=180)
        finally:
            if master is not None: os.close(master); os.close(slave)
        assert secret not in result.stdout, 'credential leaked to controller output'
        if (result.returncode==0)!=ok:
            print(result.stdout)
            raise AssertionError(f'{op}: unexpected return code {result.returncode}')
        return result.stdout
    def events():
        file=Path('/mnt/events')
        return file.read_text() if file.exists() else ''
    def state(): return json.loads(Path('/mnt/systemd.json').read_text())
    print('install and inspection',flush=True)
    invoke('install')
    assert root.is_dir()
    assert (root/'secrets/master.key').read_text().strip()==secret
    assert (root/'manifest.tsv').is_file()
    invoke('info')
    central_before=digest(instance/'instance.yml')
    invoke('stop',['--host','local','--instance','tunnel:42_1c'])
    assert state()['tuntom-tunnel-psx2-42_1c.service']['ActiveState']=='inactive'
    assert state()['tuntom-tunnel-psx2.target']['UnitFileState']=='enabled'
    invoke('disable')
    print('binary-only update preserves manual config, stopped members, autostart and secrets',flush=True)
    (root/'assets/manual.txt').write_text('emergency remote edit\n')
    (instance/'manual.txt').write_text('new central edit\n')
    protected={str(p):digest(p) for p in [root/'assets/manual.txt',root/'config.json',root/'runtime.py',root/'secrets/master.key']}
    Path('/mnt/revision').write_text('2')
    invoke('update')
    assert protected=={p:digest(Path(p)) for p in protected}
    assert state()['tuntom-tunnel-psx2-42_1c.service']['ActiveState']=='inactive'
    assert state()['tuntom-tunnel-psx2.target']['UnitFileState']=='disabled'
    assert 'manual.txt' in invoke('info')
    before=events()
    invoke('update')
    assert events()==before, 'unchanged binaries must not restart anything'
    print('failed build leaves installed data and service state intact',flush=True)
    Path('/mnt/fail-build').touch()
    before=events(); oldbin=digest(root/'bin/main')
    invoke('update',ok=False)
    assert before==events() and oldbin==digest(root/'bin/main')
    Path('/mnt/fail-build').unlink()
    print('update-config replaces central files while preserving deliberately stopped members',flush=True)
    invoke('update-config')
    assert (root/'assets/manual.txt').read_text()=='new central edit\n'
    assert state()['tuntom-tunnel-psx2-42_1c.service']['ActiveState']=='inactive'
    assert state()['tuntom-tunnel-psx2.target']['UnitFileState']=='disabled'
    assert digest(instance/'instance.yml')==central_before
    print('unreachable remote blocks wipe; --local is the explicit exception',flush=True)
    # A prepared definition has one missing host and no local installation.
    disconnected=inventory/'instances/tunnels/offline'
    disconnected.mkdir()
    (disconnected/'instance.yml').write_text(definition.replace('tunnel_id: 42','tunnel_id: 43').replace('  server:\n    host: local','  server:\n    host: missing'))
    with open(inventory/'hosts.yml','a') as f:
        f.write('    missing:\n      ansible_host: 192.0.2.1\n      ansible_ssh_common_args: "-o ConnectTimeout=1 -o ConnectionAttempts=1"\n')
    # Temporarily select the offline definition using its alias.
    offline_args=[python,str(DEPLOY/'helpers/controller.py'),'--inventory',str(inventory),'--wipe','--tunnel','offline','--check']
    before=events()
    result=subprocess.run(offline_args,text=True,capture_output=True,env=env,timeout=30)
    assert result.returncode!=0 and 'no changes made' in result.stderr, result.stdout+result.stderr
    assert before==events() and disconnected.exists()
    result=subprocess.run(offline_args+['--local'],text=True,capture_output=True,env=env,timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    print('wipe removes selected manual files, credentials, drop-ins, old staging and central source',flush=True)
    other=Path('/var/lib/tuntom-deploy/instances/unrelated')
    other.mkdir(); (other/'keep').write_text('keep')
    (root/'manual-secret.txt').write_text('private manual file')
    drop=Path('/etc/systemd/system/tuntom-tunnel-psx2-42c.service.d')
    drop.mkdir(); (drop/'manual.conf').write_text('[Service]\nRestartSec=9\n')
    owner=json.loads((root/'installed.json').read_text())['owner']
    oldstage=Path('/var/lib/tuntom-deploy/staging/tunnel-psx2-00000000-0000-0000-0000-000000000001')
    oldstage.mkdir(); (oldstage/'owner.json').write_text(json.dumps({'owner':owner})); (oldstage/'secret').write_text(secret)
    invoke('wipe',wipe=True)
    assert not root.exists() and not instance.exists() and not oldstage.exists() and not drop.exists()
    assert not (inventory/'.state/tunnel-psx2').exists()
    assert (other/'keep').read_text()=='keep' and disconnected.exists()
    assert not list(Path('/etc/systemd/system').rglob('*psx2*'))
    assert not Path('/run/tuntom-deploy.lock').exists()
    print('integration: PASS',flush=True)


def main():
    if '--inside' in sys.argv: return inside(sys.argv[-1])
    python=str(Path(sys.argv[1]).absolute()) if len(sys.argv)>1 else sys.executable
    with tempfile.TemporaryDirectory(prefix='tt-integration-') as temporary:
        work=Path(temporary)
        (work/'isolated').touch()
        (work/'revision').write_text('1')
        (work/'bin').mkdir(); (work/'etc').mkdir()
        for name in ['passwd','group','nsswitch.conf','os-release']:
            shutil.copyfile('/etc/'+name,work/'etc'/name)
        (work/'etc/machine-id').write_text('feedfacefeedfacefeedfacefeedface\n')
        for path in ['systemd/system','shadow','gshadow']:
            p=work/'etc'/path
            if '.' not in path and '/' not in path: p.touch(mode=0o600)
            else: p.mkdir(parents=True)
        for name,content in {'systemctl':FAKE_SYSTEMCTL,'g++':FAKE_BUILD,'ip':'#!/bin/sh\nexit 1\n','iptables':'#!/bin/sh\nexit 1\n'}.items():
            path=work/'bin'/name; path.write_text(content); path.chmod(0o755)
        cmd=['bwrap','--unshare-all','--die-with-parent','--uid','0','--gid','0','--ro-bind','/','/',
             '--bind',str(work),'/mnt','--bind',str(work/'etc'),'/etc','--proc','/proc','--dev','/dev',
             '--tmpfs','/run','--dir','/run/systemd/system','--tmpfs','/var/lib','--tmpfs','/var/log',
             '--tmpfs','/root',sys.executable,str(Path(__file__).resolve()),'--inside',python]
        raise SystemExit(subprocess.run(cmd).returncode)


if __name__=='__main__': main()
