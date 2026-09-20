#!/usr/bin/env python3
"""Small CLI over Ansible operations. All product-specific code stays in deploy/."""
from __future__ import annotations
import argparse
import contextlib
import difflib
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import uuid

from model import BASE, Invalid, Instance, inside, json_text, load_instances, name, read_yaml
from units import render

DEPLOY=Path(__file__).resolve().parents[1]
REPO=DEPLOY.parent
OPERATIONS=('install','info','update','update-config','start','stop','restart','enable','disable','wipe')


def executable(tool):
    sibling=Path(sys.executable).parent/tool
    found=str(sibling) if sibling.is_file() else shutil.which(tool)
    if not found:
        raise Invalid(f'{tool} is missing; install deploy/requirements.txt in a virtual environment')
    return found


def process(argv, **kwargs):
    return subprocess.run(argv,check=True,text=True,**kwargs)


def inventory_hosts(directory):
    with tempfile.TemporaryDirectory(prefix='tuntom-inventory-') as work:
        env=dict(os.environ,ANSIBLE_LOCAL_TEMP=work)
        env.pop('TUNTOM_SECRET',None)
        data=process([executable('ansible-inventory'),'-i',str(directory/'hosts.yml'),'--list'],capture_output=True,env=env)
    inventory=json.loads(data.stdout)
    hosts=dict(inventory.get('_meta',{}).get('hostvars',{}))
    for value in inventory.values():
        if isinstance(value,dict):
            for host in value.get('hosts',[]): hosts.setdefault(host,{})
    for host in hosts:
        name(host)
        if host in ('localhost','all','ungrouped'):
            raise Invalid(f'reserved host alias: {host}; use local for the controller')
    return hosts


def parser():
    p=argparse.ArgumentParser(description='Manage selected tuntom deployments through Ansible.')
    operations=p.add_mutually_exclusive_group(required=True)
    for op in OPERATIONS: operations.add_argument('--'+op,action='store_const',const=op,dest='operation')
    p.add_argument('--inventory',type=Path,default=Path(os.environ.get('TUNTOM_INVENTORY',str(Path.home()/'.config/tuntom/inventory'))))
    select=p.add_mutually_exclusive_group()
    select.add_argument('--tunnel',help='Tunnel deployment alias or numeric tunnel ID')
    select.add_argument('--deployment',help='kind:alias, e.g. switch:fabric')
    select.add_argument('--instance',help='One daemon, e.g. tunnel:42_1c; requires --host')
    select.add_argument('--all',action='store_true',help='Explicitly select every inventory deployment')
    p.add_argument('--host',help='Inventory host alias')
    p.add_argument('--local',action='store_true',help='Only inventory hosts with ansible_connection=local')
    p.add_argument('--check',action='store_true',help='Read-only plan; no builds, hooks or restarts')
    p.add_argument('--diff',action='store_true',help='Show public configuration diffs; never secrets')
    p.add_argument('--ask-become-pass',action='store_true')
    return p


def select_instances(args,instances):
    selected=[]
    for item in instances:
        if args.tunnel and not (item.kind=='tunnel' and args.tunnel in (item.alias,str(item.definition['tunnel_id']))): continue
        if args.deployment and args.deployment!=f'{item.kind}:{item.alias}': continue
        if args.host and args.host not in item.hosts(): continue
        if args.instance and not any(e['selector']==args.instance for e in item.compile(args.host)['endpoints']): continue
        selected.append(item)
    if not selected: raise Invalid('no matching deployment')
    if args.instance and len(selected)!=1: raise Invalid('ambiguous daemon selector')
    return selected


def private_json(path,value):
    private_text(path,json_text(value))


def private_text(path,value):
    path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    if path.parent.is_symlink() or path.is_symlink(): raise Invalid('symlink controller state')
    fd,temporary=tempfile.mkstemp(prefix=path.name+'.',dir=path.parent)
    try:
        with os.fdopen(fd,'w') as stream: stream.write(value)
        os.replace(temporary,path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def source_archive(path):
    with tarfile.open(path,'w:gz') as archive:
        for entry in ['src','tools','mk_tunnel.sh','mk_switch.sh','mk_switch_mp.sh','mk_adapter.sh','tuntom-net.sh']:
            source=REPO/entry
            for item in ([source] if source.is_file() else sorted(source.rglob('*'))):
                if item.is_symlink(): raise Invalid(f'symlink build source: {item}')
                if '__pycache__' not in item.parts and item.is_file():
                    archive.add(item,arcname=str(item.relative_to(REPO)),recursive=False)


def prepare_assets(item, destination):
    destination.mkdir(mode=0o700,parents=True)
    for source in sorted(item.directory.rglob('*')):
        relative=source.relative_to(item.directory)
        if source.is_symlink(): raise Invalid('symlink instance content')
        if relative.parts[0]=='secrets': continue
        target=destination/relative
        if source.is_dir(): target.mkdir(mode=0o700,exist_ok=True)
        elif source.is_file():
            target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
            shutil.copyfile(source,target)
            target.chmod(0o600)
        else: raise Invalid('special file in instance')


class Session:
    def __init__(self,args,instances,hosts,work):
        self.args,self.instances,self.hosts,self.work=args,instances,hosts,Path(work)
        self.operation_id=str(uuid.uuid4())
        self.identities={}
        self.payload={}
        self.reports={}
        self.adoptions=[]
        for item in instances:
            identity_path=args.inventory/'.state'/item.key/'identity.json'
            for path in [args.inventory/'.state',identity_path.parent,identity_path]:
                if path.is_symlink(): raise Invalid('symlink controller identity: '+str(path))
            identity=json.loads(identity_path.read_text()) if identity_path.is_file() else {'owner':str(uuid.uuid4())}
            self.identities[item.key]=identity
            if identity.get('hosts') and sorted(identity['hosts'])!=item.hosts():
                raise Invalid('moving endpoints between hosts requires a new deployment, not update')
            for host in item.hosts():
                if host not in hosts: raise Invalid(f'unknown inventory host: {host}')
                if args.host and host!=args.host: continue
                if args.local and hosts[host].get('ansible_connection')!='local': continue
                spec=item.compile(host)
                spec.update(owner=identity['owner'],operation_id=self.operation_id)
                self.payload.setdefault(host,[]).append({'spec':spec,'key':item.key,'root':str(item.root),
                    'stage':f'/var/lib/tuntom-deploy/staging/{item.key}-{self.operation_id}',
                    'source_dir':str(item.directory),'units':render(spec)})
        if not self.payload: raise Invalid('no hosts selected (check --local and ansible_connection)')

    def run(self, playbook, *, extra=None, quiet=False):
        results=self.work/'results'
        results.mkdir(exist_ok=True)
        inventory={'all':{'hosts':{}}}
        for host,jobs in self.payload.items():
            variables={k:v for k,v in self.hosts[host].items() if not k.startswith('tt_')}
            variables.update(tt_jobs=jobs)
            inventory['all']['hosts'][host]=variables
        private_json(self.work/'inventory.json',inventory)
        variables={'tt_operation':self.args.operation,'tt_operation_id':self.operation_id,
                   'tt_results':str(results),'tt_helpers':str(DEPLOY/'helpers'),
                   'tt_source_archive':str(self.work/'source.tar.gz'),
                   'tt_repo':str(REPO),'tt_selected_daemon':self.args.instance or '',
                   'tt_confirmed':False,'tt_adoptions':self.adoptions}
        if extra: variables.update(extra)
        private_json(self.work/'vars.json',variables)
        argv=[executable('ansible-playbook'),'-i',str(self.work/'inventory.json'),str(DEPLOY/'ansible'/playbook),'-e','@'+str(self.work/'vars.json')]
        if self.args.ask_become_pass: argv+=['--ask-become-pass']
        if self.args.diff: argv+=['--diff']
        env=os.environ.copy()
        env.pop('TUNTOM_SECRET',None)
        env.update(ANSIBLE_NOCOLOR='1',ANSIBLE_RETRY_FILES_ENABLED='false',
                   ANSIBLE_LOCAL_TEMP=str(self.work/'ansible-local'))
        return process(argv,env=env,stdout=subprocess.DEVNULL if quiet else None)

    def inspect(self):
        self.run('info.yml')
        for host,jobs in self.payload.items():
            for job in jobs:
                path=self.work/'results'/f'{host}--{job["key"]}.json'
                report=json.loads(path.read_text())
                self.reports[(host,job['key'])]=report
                job['before']=report
                if report.get('reachable') is False:
                    print(f'\n{job["key"]} @ {host}: UNKNOWN: {report["error"]}')
                    continue
                print(f'\n{job["key"]} @ {host}: '+('installed' if report['installed'] else 'not installed'))
                if report['installed'] and not report.get('complete', True):
                    print('  Incomplete installation: repair with --update-config or remove with --wipe.')
                for unit,state in report['services'].items():
                    print(f'  {unit}: {state.get("ActiveState")} / {state.get("UnitFileState")}')
                if report['manual_changes']: print('  Manual changes: '+', '.join(report['manual_changes']))
                if report['desired_changed']: print('  Central configuration differs from last deployment.')
                if report['legacy']: print('  Existing mk_* installation detected.')
                print('  Files: '+report['root'])
                print('  Central definition: '+job['source_dir'])
                print('  Owner: '+report['owner']+'; machine: '+report['machine_id'])
                for endpoint in report['spec']['endpoints']:
                    env=endpoint['env']
                    print('    '+endpoint['selector']+': '+(' '.join(endpoint['args'])))
                    print('      control: '+endpoint['control'])
                    if endpoint['interface']:
                        print('      interface: '+endpoint['interface']+'; address: '+env.get('TUNTOM_LOCAL_IP',''))
                if self.args.operation in ('info','wipe'):
                    for path,value in report['files'].items():
                        detail=' [secret; content hidden; mode '+value['mode']+']' if isinstance(value,dict) and value.get('secret') else ' ['+str(value)+']'
                        print('    '+path+detail)
                    for unit,value in report.get('unit_files',{}).items():
                        if value['dropins']: print('    '+unit+' drop-ins: '+', '.join(value['dropins']))
                    for path in report.get('staging',[]): print('    Previous staging: '+path)
                expected=self.identities[job['key']].get('machine_ids',{}).get(host)
                if expected and expected!=report['machine_id']:
                    raise Invalid(f'{host}: machine identity changed; refusing takeover')

    def require_reachable(self):
        unavailable=[f'{host}/{key}' for (host,key),r in self.reports.items() if r.get('reachable') is False]
        if unavailable:
            raise Invalid('no changes made; cannot inspect every selected host: '+', '.join(unavailable))
        seen={}
        for (host,key),report in self.reports.items():
            identity=(report['machine_id'],key)
            if identity in seen and seen[identity]!=host:
                raise Invalid('two inventory aliases address the same machine for '+key+'; use one host alias for both roles')
            seen[identity]=host

    def plan(self):
        self.require_reachable()
        print(f'\nOperation: {self.args.operation}')
        for host,jobs in self.payload.items():
            for job in jobs:
                report=job['before']
                spec=job['spec']
                if self.args.operation=='wipe' and report['legacy']:
                    raise Invalid('this mk_* setup has not been adopted; stop it with its mk_* helper before wiping the prepared definition')
                if self.args.operation=='install' and report['installed']:
                    raise Invalid(f'{job["key"]}: already installed; use --update or --update-config')
                if self.args.operation=='update' and not report.get('complete',True):
                    raise Invalid('incomplete installation requires --update-config or --wipe')
                if self.args.operation in ('update','update-config','start','stop','restart','enable','disable') and not report['installed']:
                    raise Invalid(f'{job["key"]}: not installed on {host}')
                if self.args.operation not in ('install','update-config'):
                    spec=dict(report['spec'])
                    spec.update(operation_id=self.operation_id)
                    job['spec']=spec
                    job['units']=render(spec)
                if self.args.operation=='install' and report['legacy'] and spec['kind']=='tunnel':
                    identity=self.identities[job['key']]
                    command=identity.get('adoption_command')
                    clients=[i for i in self.instances if i.key==job['key']]
                    client=clients[0].definition['client']['host']
                    if not command or self.hosts[client].get('ansible_connection')!='local':
                        raise Invalid('stop the existing mk_tunnel group first, or import it with tt_tunnel.sh on its original client')
                    if command not in self.adoptions: self.adoptions.append(command)
                if self.args.operation=='update-config' and report['installed']:
                    old=report['spec']
                    old_ids={e['env'].get('TUNTOM_GROUP_ID') for e in old['endpoints']}
                    new_ids={e['env'].get('TUNTOM_GROUP_ID') for e in spec['endpoints']}
                    if old_ids!=new_ids: raise Invalid('changing tunnel_id is a migration; create a new deployment')
                if self.args.instance:
                    chosen=[e for e in report['spec']['endpoints'] if e['selector']==self.args.instance]
                    if not chosen: raise Invalid('selected daemon is not installed')
                    job['selected_units']=[e['unit'] for e in chosen]
                else:
                    job['selected_units']=[e['unit'] for e in report['spec']['endpoints']]
                    if self.args.operation in ('start','stop','restart'):
                        job['selected_units']=[report['spec']['target']]
                if self.args.operation in ('enable','disable'):
                    if self.args.instance: raise Invalid('autostart is managed per deployment target; select --deployment or --tunnel')
                    job['selected_units']=[report['spec']['target']]
                print(f'  {host}: {job["key"]} ({len(spec["endpoints"])} endpoints)')
                if self.args.operation=='wipe':
                    print('    REMOVE units, runtime, entire '+job['root'])
                elif self.args.operation=='update':
                    print('    binaries only; preserve configuration, secrets, stopped services and autostart')
                if self.args.diff and self.args.operation in ('install','update-config'):
                    old={k:v for k,v in report['spec'].items() if k not in ('operation_id','source_digest')}
                    new={k:v for k,v in spec.items() if k not in ('operation_id','source_digest')}
                    print(''.join(difflib.unified_diff(json_text(old).splitlines(True) if report['installed'] else [],json_text(new).splitlines(True),fromfile='installed public spec',tofile='desired public spec')),end='')
        if self.args.operation=='wipe':
            for item in self.instances:
                print(f'  REMOVE central definition, secrets and state: {item.directory}')
            if self.args.local: print('  REMOTE INSTALLATIONS WILL REMAIN (--local).')

    def remember(self):
        for item in self.instances:
            identity=self.identities[item.key]
            machines=identity.setdefault('machine_ids',{})
            machines.update({host:self.reports[(host,item.key)]['machine_id'] for host in self.payload if (host,item.key) in self.reports})
            identity.update(hosts=item.hosts())
            private_json(self.args.inventory/'.state'/item.key/'identity.json',identity)

    def prepare(self):
        source_archive(self.work/'source.tar.gz')
        for item in self.instances:
            if self.args.operation!='update': prepare_assets(item,self.work/'assets'/item.key)
            if item.kind=='tunnel' and self.args.operation!='update':
                secret=inside(item.directory,item.definition.get('secret_file','secrets/master.key'))
                if secret.stat().st_mode & 0o077:
                    raise Invalid(f'secret must have mode 0600: {secret}')
                value=secret.read_text().strip()
                if len(value)!=32 or any(c not in '0123456789abcdefABCDEF' for c in value):
                    raise Invalid(f'invalid secret: {secret} (expected 32 hex characters)')
        for jobs in self.payload.values():
            for job in jobs:
                job['assets_src']=str(self.work/'assets'/job['key'])+'/'
                job['secret_src']=str(Path(job['source_dir'])/job['spec']['secret_file']) if job['spec']['secret_file'] else ''
                spec=job['spec']
                source={'tunnel':'src/main.cpp','adapter':'src/adapter/main.cpp','switch':'src/switch_mp/main.cpp' if spec['implementation']=='mp' else 'src/switch/main.cpp'}[spec['kind']]
                job['builds']=[{'source':source,'name':'main'},{'source':'src/control/main.cpp','name':'tuntomctl'}]
                if spec.get('auto_pool'): job['builds'].append({'source':'tools/switch_mp_plan.cpp','name':'planner'})
                if spec.get('all_tools'):
                    job['builds'] += [{'source':'src/switch/main.cpp','name':'tuntom-switch'},
                                      {'source':'src/adapter/main.cpp','name':'tuntom-switch-adapter'}]
                states=job['before']['services']
                job['active_members']=[e['unit'] for e in job['before']['spec']['endpoints'] if states.get(e['unit'],{}).get('ActiveState')=='active']
                old_units={e['unit'] for e in job['before']['spec']['endpoints']}
                job['restore_members']=[e['unit'] for e in spec['endpoints'] if e['unit'] in job['active_members'] or (e['unit'] not in old_units and states.get(spec['target'],{}).get('ActiveState')=='active')]
                job['restore_ids']=[e['id'] for e in spec['endpoints'] if e['unit'] in job['restore_members']]
                job['prepare_active']=states.get('tuntom-'+spec['key']+'-prepare.service',{}).get('ActiveState')=='active'
                job['ready_active']=states.get('tuntom-'+spec['key']+'-ready.service',{}).get('ActiveState')=='active'

    def wipe_central(self):
        remaining=[item for item in load_instances(self.args.inventory) if item.key not in {x.key for x in self.instances}]
        used={host for item in remaining for host in item.hosts()}
        owned={host for item in self.instances for host in self.identities[item.key].get('created_hosts',[])}
        hosts_path=self.args.inventory/'hosts.yml'
        host_data=read_yaml(hosts_path)
        simple=host_data.get('all',{}).get('hosts',{})
        # Connection records explicitly created by the importer are removed when
        # no remaining instance uses them. Existing/shared host records stay.
        for host in owned-used:
            simple.pop(host,None)
        for item in self.instances:
            for path in [item.directory,self.args.inventory/'.state'/item.key]:
                if path.is_symlink(): raise Invalid('refusing symlink during central wipe')
                if path.exists(): shutil.rmtree(path)
        if owned-used:
            import yaml
            private_text(hosts_path,yaml.safe_dump(host_data,sort_keys=False))
        print('Selected deployment definitions, secrets and controller state removed.')


def main(argv=None):
    args=parser().parse_args(argv)
    args.inventory=args.inventory.expanduser().resolve()
    os.umask(0o077)
    # Lock before reading definitions or hosts, not only before writing them.
    # The directory inode survives atomic replacement of individual YAML files.
    with contextlib.ExitStack() as stack:
        lock=os.open(args.inventory,os.O_RDONLY | os.O_DIRECTORY)
        stack.callback(os.close,lock)
        fcntl.flock(lock,fcntl.LOCK_EX | fcntl.LOCK_NB)
        operate(args)


def operate(args):
    if args.instance and not args.host: raise Invalid('--instance requires --host')
    if args.local and args.host: raise Invalid('--local and --host are mutually exclusive')
    if args.instance and args.operation not in ('info','start','stop','restart'):
        raise Invalid('per-daemon selection supports info/start/stop/restart; updates and wipe select a deployment')
    instances=load_instances(args.inventory)
    if not instances: raise Invalid('inventory has no instances')
    if args.operation not in ('info',) and not any((args.tunnel,args.deployment,args.instance,args.all)):
        if args.operation!='wipe' or not sys.stdin.isatty(): raise Invalid('select --tunnel, --deployment, --instance or --all')
        print('\n'.join(f'{i+1}: {x.kind}:{x.alias}' for i,x in enumerate(instances)))
        choice=input('Select one deployment number (empty cancels): ').strip()
        if not choice: return
        if not choice.isdigit() or not 1<=int(choice)<=len(instances): raise Invalid('invalid selection')
        item=instances[int(choice)-1]
        args.deployment=f'{item.kind}:{item.alias}'
    selected=select_instances(args,instances)
    if args.operation=='wipe' and args.host and any(x.kind=='tunnel' for x in selected):
        raise Invalid('tunnel wipe requires both endpoints; use --local for the explicit local exception')
    if args.operation in ('install','update','update-config') and (args.host or args.local) and any(x.kind=='tunnel' for x in selected):
        raise Invalid('tunnel deployment updates require both endpoints')
    hosts=inventory_hosts(args.inventory)
    with tempfile.TemporaryDirectory(prefix='tuntom-deploy-') as work:
        session=Session(args,selected,hosts,work)
        session.inspect()
        if args.operation=='info': return
        session.plan()
        if args.check:
            print('CHECK ONLY: no builds, hooks, service operations or removal performed. Runtime behavior is not validated.')
            return
        if args.operation=='wipe':
            if not sys.stdin.isatty(): raise Invalid('wipe requires an interactive terminal; there is no --yes option')
            if input('Permanently wipe the selected deployments and their central secrets? [ano/NE]: ').strip().lower()!='ano':
                print('Cancelled.'); return
        session.remember()
        try:
            session.run('lock.yml')
            if args.operation in ('install','update','update-config'):
                session.prepare()
                session.run({'install':'install.yml','update':'update.yml','update-config':'update_config.yml'}[args.operation])
            elif args.operation=='wipe':
                session.run('wipe.yml',extra={'tt_confirmed':True})
                session.wipe_central()
            else:
                session.run('service.yml')
        finally:
            try: session.run('unlock.yml',quiet=True)
            except subprocess.CalledProcessError:
                print('Some host cleanup/locks could not be completed; inspect the reported hosts before retrying.',file=sys.stderr)


if __name__=='__main__':
    try: main()
    except (Invalid,OSError,ValueError,KeyError,subprocess.CalledProcessError) as error:
        print(f'tt_deploy: {error}',file=sys.stderr)
        sys.exit(1)
