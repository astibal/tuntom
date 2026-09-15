#!/usr/bin/env python3
"""Instance-local systemd lifecycle. Uses only the Python standard library.

This file is installed privately with an instance; it is never a supervisor.
The exec action replaces itself with the actual daemon tracked by systemd.
"""
from __future__ import annotations

import grp
import json
import os
from pathlib import Path
import pwd
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time


def command(argv, *, env=None, check=True, capture=False):
    return subprocess.run(argv, env=env, check=check, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None, timeout=30)


def atomic_json(path, value):
    fd, temp = tempfile.mkstemp(prefix=path.name+'.', dir=path.parent)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
    os.replace(temp, path)


def idle_socket(path):
    path = Path(path)
    if any(p.is_symlink() for p in path.parents):
        raise RuntimeError(f'symlink endpoint parent: {path}')
    if not path.exists() and not path.is_symlink():
        return
    if path.is_symlink() or not stat.S_ISSOCK(path.lstat().st_mode):
        raise RuntimeError(f'refusing non-socket endpoint: {path}')
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as probe:
        probe.settimeout(0.2)
        try:
            probe.connect(str(path))
        except (ConnectionRefusedError, FileNotFoundError):
            path.unlink(missing_ok=True)
            return
        except OSError as error:
            raise RuntimeError(f'cannot establish endpoint ownership: {path}') from error
    raise RuntimeError(f'endpoint still has a live listener: {path}')


def switch_rules(args, implementation):
    """Support the same legacy rule files as mk_switch, without shell evaluation."""
    args = list(args)
    if '--rules-file' not in args: return args
    index = args.index('--rules-file')
    path = args[index+1]
    lines = [line.split('#',1)[0].split() for line in Path(path).read_text().splitlines()]
    lines = [line for line in lines if line]
    if lines and lines[0][0]=='format': return args
    del args[index:index+2]
    for line in lines:
        if len(line)!=2: raise RuntimeError('invalid legacy switch rules line')
        key,value=line
        if key=='default-back' and value in ('on','off'): args.append('--default-back='+value)
        elif key in ('route','exit-port') or (key=='trunk-port' and implementation=='mp'):
            args.extend(['--'+key,value])
        else: raise RuntimeError('invalid legacy switch rule: '+key)
    return args


class Runtime:
    def __init__(self, root):
        self.root = Path(root)
        self.config = json.loads((self.root/'config.json').read_text())
        if self.config['root'] != str(self.root):
            raise RuntimeError('configuration root mismatch')
        self.run = Path(self.config['run'])
        self.active = self.root/'active'

    def endpoint(self, ident, saved=False):
        if saved:
            path = self.active/ident/'endpoint.json'
            return json.loads(path.read_text()) if path.exists() else None
        return next(e for e in self.config['endpoints'] if e['id']==ident)

    def environment(self, e, action='', phase='', assets=None):
        env = os.environ.copy()
        env.pop('TUNTOM_SECRET', None)
        env.update(e['env'])
        env.update(TUNTOM_ACTION=action, TUNTOM_PHASE=phase,
                   TUNTOM_BIN=str(self.root/'bin'/'main'), TUNTOM_CTL=str(self.root/'bin'/'tuntomctl'),
                   TUNTOM_CONTROL_SOCKET=e['control'], TUNTOM_PID_FILE=str(self.run/f'{e["id"]}.pid'),
                   TUNTOM_LOG_FILE=str(self.root/'logs'/f'{e["id"]}.log'), TUNTOM_GROUP_MANIFEST=str(self.root/'manifest.tsv'))
        if assets:
            for key in ('TUNTOM_FILES_DIR','TUNTOM_RULES_FILE'):
                if env.get(key):
                    env[key] = env[key].replace(str(self.root/'assets'), str(assets), 1)
            manifest=Path(assets).parent/'manifest.tsv'
            if manifest.is_file(): env['TUNTOM_GROUP_MANIFEST']=str(manifest)
        return env

    def hook(self, e, phase, action, assets=None):
        assets = assets or self.root/'assets'
        relative = e.get(f'{phase}_hook','')
        if relative:
            path = assets/relative
            if not path.is_file():
                raise RuntimeError(f'missing installed hook: {path}')
            command(['bash',str(path)], env=self.environment(e,action,phase,assets))

    def prepare_run(self):
        if self.run.is_symlink():
            raise RuntimeError('symlink runtime directory')
        self.run.mkdir(parents=True, exist_ok=True)
        os.chown(self.run, 0, grp.getgrnam('tuntom').gr_gid)
        self.run.chmod(0o2770)
        self.active.mkdir(mode=0o700, exist_ok=True)

    def pre(self, ident):
        self.prepare_run()
        if self.endpoint(ident, saved=True):
            self.cleanup(ident)
        e = self.endpoint(ident)
        if e['interface'] and command(['ip','link','show','dev',e['interface']],check=False,capture=True).returncode==0:
            raise RuntimeError(f'interface already exists: {e["interface"]}')
        idle_socket(e['control'])
        if self.config['kind']=='switch':
            path = e['env']['TUNTOM_SWITCH_SOCKET']
            if any(p.is_symlink() for p in Path(path).parents): raise RuntimeError('symlink switch socket directory')
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            idle_socket(path)
        snapshot = self.active/ident
        snapshot.mkdir(mode=0o700, exist_ok=True)
        shutil.copytree(self.root/'assets',snapshot/'assets',dirs_exist_ok=True)
        atomic_json(snapshot/'endpoint.json',e)
        if (self.root/'manifest.tsv').exists(): shutil.copyfile(self.root/'manifest.tsv',snapshot/'manifest.tsv')
        if self.config['kind']=='switch':
            # Switch pre/up may generate its rules. Run against the live assets;
            # snapshot the resulting configuration for down hooks afterwards.
            self.hook(e,'pre','up')
            shutil.copytree(self.root/'assets',snapshot/'assets',dirs_exist_ok=True)

    def execute(self, ident):
        e = self.endpoint(ident)
        env = self.environment(e)
        if self.config['kind']=='tunnel':
            credentials = os.environ.get('CREDENTIALS_DIRECTORY')
            if not credentials:
                raise RuntimeError('missing systemd credentials directory')
            secret = (Path(credentials)/'master').read_text().strip()
            if len(secret)!=32 or any(c not in '0123456789abcdefABCDEF' for c in secret):
                raise RuntimeError('invalid tunnel credential')
            env['TUNTOM_SECRET'] = secret
        options = switch_rules(e['args'], self.config['implementation']) if self.config['kind']=='switch' else e['args']
        args = [str(self.root/'bin'/'main'), *options, '--control-socket',e['control']]
        path=self.run/f'{ident}.pid'
        fd=os.open(path,os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'w') as stream: stream.write(str(os.getpid())+'\n')
        if self.config['kind']=='tunnel':
            args += ['--stats-file',e['stats'],'--stats-format','txt']
        if self.config.get('auto_pool'):
            planner = [str(self.root/'bin'/'planner'),*options,'--auto-pool']
            if self.config.get('reserve_cpus') is not None:
                planner += ['--reserve-cpus',str(self.config['reserve_cpus'])]
            output = command(planner,capture=True).stdout
            # The planner emits validated numeric options. Never evaluate shell.
            for line in output.splitlines():
                if line.startswith('option.'):
                    key, value = line[7:].split('=',1)
                    if key not in ('workers','work-per-thread','rx-weight','tx-weight','adapter-weight','trunk-weight') or not value.isdigit():
                        raise RuntimeError('invalid CPU planner output')
                    if '--'+key in args:
                        args[args.index('--'+key)+1]=value
                    else: args += ['--'+key,value]
        os.execve(args[0],args,env)

    def ready(self,e):
        deadline = time.monotonic()+10
        while time.monotonic()<deadline:
            result = command([str(self.root/'bin'/'tuntomctl'),e['control'],'show','stats'],capture=True,check=False)
            expected = [f'mode={e["role"]}',f'tunnel_id={e["env"]["TUNTOM_INSTANCE_KEY"]}'] if self.config['kind']=='tunnel' else [f'component={self.config["kind"]}']
            if result.returncode==0 and all(line in result.stdout.splitlines() for line in expected):
                return
            time.sleep(0.1)
        raise RuntimeError(f'control socket not ready: {e["id"]}')

    def net(self,e,action):
        if self.config['kind']=='tunnel' and e['interface']:
            command(['bash','-c','source "$1"; "tuntom_net_$2"','tuntom-net',str(self.root/'tuntom-net.sh'),action],env=self.environment(e,action))

    def post(self,ident):
        e = self.endpoint(ident)
        self.ready(e)
        user,group = e['socket_owner'].split(':')
        uid = int(user) if user.isdigit() else pwd.getpwnam(user).pw_uid
        gid = int(group) if group.isdigit() else grp.getgrnam(group).gr_gid
        for path in [e['control']] + ([e['env']['TUNTOM_SWITCH_SOCKET']] if self.config['kind']=='switch' else []):
            if Path(path).is_symlink() or not stat.S_ISSOCK(Path(path).lstat().st_mode):
                raise RuntimeError(f'invalid ready socket: {path}')
            os.chown(path,uid,gid)
            os.chmod(path,0o660)
        if e['interface']:
            command(['ip','link','show','dev',e['interface']],capture=True)
            env = e['env']
            if self.config['kind']=='tunnel' and env['TUNTOM_LOCAL_IP']:
                command(['ip','address','replace',env['TUNTOM_LOCAL_IP'],'peer',env['TUNTOM_PEER_IP'],'dev',e['interface']])
                command(['ip','-6','address','replace',env['TUNTOM_LOCAL_IPV6'],'peer',env['TUNTOM_PEER_IPV6'],'dev',e['interface'],'nodad'])
                command(['ip','-6','route','replace',env['TUNTOM_PEER_IPV6']+'/128','dev',e['interface'],'metric','256'])
            command(['ip','link','set','dev',e['interface'],'mtu',env['TUNTOM_MTU'],'up'])
            self.hook(e,'pre','up')
            self.net(e,'up')
        if self.config['kind']!='tunnel' or e['interface']:
            self.hook(e,'post','up')
        # Compatibility bookkeeping for custom hooks; systemd owns the PID.
        atomic_json(self.active/ident/'ready.json', {'ready':True})

    def stop(self,ident):
        e = self.endpoint(ident,saved=True)
        if e and (self.config['kind']!='tunnel' or e['interface']):
            marker = self.active/ident/'pre-down.done'
            if not marker.exists():
                self.hook(e,'pre','down',self.active/ident/'assets')
                marker.touch(mode=0o600)

    def cleanup(self,ident):
        e = self.endpoint(ident,saved=True)
        if not e:
            return
        errors = []
        for action in (lambda:self.stop(ident),lambda:self.net(e,'down'),
                       lambda:self.hook(e,'post','down',self.active/ident/'assets') if self.config['kind']!='tunnel' or e['interface'] else None):
            try: action()
            except Exception as error: errors.append(str(error))
        for path in [e['control']] + ([e['env']['TUNTOM_SWITCH_SOCKET']] if self.config['kind']=='switch' else []):
            try: idle_socket(path)
            except Exception as error: errors.append(str(error))
        for path in (e['stats'],str(self.run/f'{ident}.pid')):
            Path(path).unlink(missing_ok=True)
        if errors:
            raise RuntimeError('; '.join(errors))
        shutil.rmtree(self.active/ident)

    def group(self,phase):
        directory = self.active/'group'
        if phase=='pre':
            self.prepare_run()
            if directory.exists():
                self.group('cleanup')
            directory.mkdir(mode=0o700)
            shutil.copytree(self.root/'assets',directory/'assets')
            if (self.root/'manifest.tsv').exists(): shutil.copyfile(self.root/'manifest.tsv',directory/'manifest.tsv')
            contexts=[]
            for e in self.config['endpoints']:
                if any(c['role']==e['role'] for c in contexts): continue
                e=json.loads(json.dumps(e))
                e['pre_hook']=self.config['group_pre_hook']
                e['post_hook']=self.config['group_post_hook']
                e['env'].update(TUNTOM_SCOPE='group')
                for key in ('IF','INSTANCE','INSTANCE_KEY','MEMBER_INDEX','LOCAL_IP','PEER_IP','LOCAL_IPV6','PEER_IPV6','CLIENT_IP','SERVER_IP','CLIENT_IPV6','SERVER_IPV6','UDP_PORT','MARK','MARK_MASK','TABLE','CHAIN','NAT_CHAIN','SNAT_CHAIN','MANGLE_CHAIN','FORWARD_CHAIN'):
                    e['env']['TUNTOM_'+key]=''
                contexts.append(e)
            atomic_json(directory/'contexts.json',contexts)
            for e in contexts: self.hook(e,'pre','up',directory/'assets')
            return
        if not (directory/'contexts.json').exists(): return
        contexts=json.loads((directory/'contexts.json').read_text())
        if phase=='post':
            restore=self.root/'restore-members.json'
            expected=json.loads(restore.read_text()) if restore.exists() else [e['id'] for e in self.config['endpoints']]
            for e in self.config['endpoints']:
                if e['id'] in expected: self.ready(e)
            for e in contexts: self.hook(e,'post','up',directory/'assets')
        elif phase=='stop':
            if not (directory/'stopped').exists():
                for e in contexts: self.hook(e,'pre','down',directory/'assets')
                (directory/'stopped').touch(mode=0o600)
        elif phase=='cleanup':
            self.group('stop')
            for e in contexts: self.hook(e,'post','down',directory/'assets')
            shutil.rmtree(directory)


def main():
    if os.geteuid()!=0:
        raise RuntimeError('lifecycle helper must run as root')
    os.umask(0o077)
    runtime=Runtime(Path(__file__).resolve().parent)
    action=sys.argv[1]
    if action=='cleanup-all':
        for path in sorted(runtime.active.glob('*/endpoint.json')):
            runtime.cleanup(path.parent.name)
        runtime.group('cleanup')
    elif action.startswith('group-'):
        runtime.group(action[6:])
    elif action in ('pre','execute','post','stop','cleanup'):
        getattr(runtime,action)(sys.argv[2])
    else:
        raise RuntimeError('unknown lifecycle action')


if __name__=='__main__':
    try: main()
    except Exception as error:
        print(f'tuntom-deploy lifecycle: {error}',file=sys.stderr)
        sys.exit(1)
