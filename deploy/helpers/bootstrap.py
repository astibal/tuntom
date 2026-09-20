#!/usr/bin/env python3
"""Import the familiar mk_tunnel options into a new private Ansible inventory."""
from __future__ import annotations
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

import yaml
from controller import DEPLOY, REPO, executable, main as deploy_main, private_json, private_text
from model import Instance, Invalid, name, read_yaml


def integer(text):
    return int(text,16) if str(text).lower().startswith('0x') else int(text,10)


def parse(argv=None):
    p=argparse.ArgumentParser(description='Persist a tested mk_tunnel setup using Ansible (new installations only).')
    p.add_argument('id',type=int)
    p.add_argument('remote',help='host or user@host; SSH is used only during deployment')
    p.add_argument('--name',help='Deployment alias, e.g. psx2; defaults to the remote hostname')
    p.add_argument('--inventory',type=Path,default=Path(os.environ.get('TUNTOM_INVENTORY',str(Path.home()/'.config/tuntom/inventory'))))
    p.add_argument('--count',type=int)
    p.add_argument('--crypto-auth-only',action='store_true')
    p.add_argument('--no-address',action='store_true')
    p.add_argument('--no-stats',action='store_true')
    p.add_argument('--all-tools',action='store_true')
    p.add_argument('--stop',action='store_true',help='Stop only local client member zero, without changing autostart')
    p.add_argument('--prepare-only',action='store_true',help='Create central configuration without installing services')
    p.add_argument('--check',action='store_true',help='Prepare central configuration and inspect the install plan; no host changes')
    for positive,negative,default in [('snat','no-snat',False),('mss-clamp','no-mss-clamp',True)]:
        group=p.add_mutually_exclusive_group()
        group.add_argument('--'+positive,action='store_true',dest=positive.replace('-','_'))
        group.add_argument('--'+negative,action='store_false',dest=positive.replace('-','_'))
        p.set_defaults(**{positive.replace('-','_'):default})
    for side in ('client','server'):
        p.add_argument(f'--{side}-switch',nargs=3,metavar=('SOCKET','PORT','LABEL'))
        p.add_argument(f'--{side}-switch-exit-node',action='store_true')
        p.add_argument(f'--{side}-switch-ipc',choices=('auto','v1','inline'))
        p.add_argument(f'--{side}-switch-ipc-batch',type=int)
        p.add_argument(f'--{side}-classifier-file')
    return p.parse_args(argv)


def copy_source(source,target):
    source=Path(source).expanduser().absolute()
    if source.is_symlink(): raise Invalid(f'symlink source: {source}')
    target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    try: content=source.read_bytes()
    except PermissionError:
        content=subprocess.run(['sudo','cat','--',str(source)],check=True,stdout=subprocess.PIPE).stdout
    target.write_bytes(content)
    target.chmod(0o600)


def remote_classifier(hosts,host,source,target,work):
    inventory=work/'fetch-hosts.json'
    private_json(inventory,{'all':{'hosts':{host:hosts['all']['hosts'][host]}}})
    variables=work/'fetch-vars.json'
    private_json(variables,{'tt_fetch_source':source,'tt_fetch_destination':str(target)})
    env=dict(os.environ,ANSIBLE_LOCAL_TEMP=str(work/'ansible-local'))
    env.pop('TUNTOM_SECRET',None)
    subprocess.run([executable('ansible-playbook'),'-i',str(inventory),str(DEPLOY/'ansible'/'fetch.yml'),'-e','@'+str(variables)],check=True,env=env)


def main(argv=None):
    args=parse(argv)
    remote=args.remote
    if any(c.isspace() for c in remote) or remote.startswith('-'):
        raise Invalid('invalid SSH target')
    user,host=remote.split('@',1) if '@' in remote else ('root',remote)
    if not re.fullmatch(r'[A-Za-z0-9_.-]+',user): raise Invalid('invalid SSH user')
    alias=name(args.name or re.sub(r'[^A-Za-z0-9_-]','-',host)[:32])
    remote_alias='remote-'+alias if alias=='local' else alias
    name(remote_alias)
    inventory=args.inventory.expanduser().resolve()
    if args.stop:
        return deploy_main(['--inventory',str(inventory),'--host','local','--instance',f'tunnel:{args.id}c','--stop'])
    directory=inventory/'instances'/'tunnels'/alias
    if directory.exists(): raise Invalid('central definition already exists; edit it and use --update-config')
    if os.environ.get('TUNTOM_STATS_FORMAT','txt')!='txt': raise Invalid('only txt statistics are supported')
    # Existing deployment path overrides cannot be silently lost during adoption.
    for key in ('TUNTOM_RUN_DIR','TUNTOM_STATE_DIR','TUNTOM_BIN_DIR'):
        if key in os.environ: raise Invalid(f'{key} is not a persistent deployment path override')
    count=args.count
    if count is None:
        path=Path(f'/var/lib/tuntom-mk/client/{args.id}/active/manifest.tsv')
        try: count=len(path.read_text().splitlines())-1
        except FileNotFoundError: count=1
        except PermissionError:
            result=subprocess.run(['sudo','-n','cat','--',str(path)],text=True,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
            if result.returncode: raise Invalid('cannot read saved count; supply --count explicitly')
            count=len(result.stdout.splitlines())-1
    hosts=read_yaml(inventory/'hosts.yml') if (inventory/'hosts.yml').exists() else {'all':{'hosts':{}}}
    # Bootstrap writes only this simple layout; richer Ansible inventories can
    # be used directly with tt_deploy and hand-written instance documents.
    if set(hosts)!={'all'} or not isinstance(hosts['all'].get('hosts'),dict):
        raise Invalid('bootstrap requires all.hosts in hosts.yml; use tt_deploy for richer inventories')
    additions={'local':{'ansible_connection':'local','ansible_become':True},
               remote_alias:{'ansible_host':host,'ansible_user':user,'ansible_become':user!='root'}}
    created_hosts=[host for host in additions if host not in hosts['all']['hosts']]
    for key,value in additions.items():
        existing=hosts['all']['hosts'].get(key)
        if existing is not None and any(existing.get(k)!=v for k,v in value.items()):
            raise Invalid(f'host {key} already has different connection settings')
        hosts['all']['hosts'].setdefault(key,value)
    with tempfile.TemporaryDirectory(prefix='tuntom-bootstrap-') as temporary:
        work=Path(temporary)
        data={'schema':1,'tunnel_id':args.id,'count':count,'mtu':integer(os.environ.get('TUNTOM_MTU','1500')),
              'transport_mtu':integer(os.environ.get('TUNTOM_TRANSPORT_MTU','1400')),
              'prefix16':os.environ.get('TUNTOM_PREFIX16','10.254'),'no_address':args.no_address,
              'crypto_auth_only':args.crypto_auth_only,'no_stats':args.no_stats,'snat':args.snat,'mss_clamp':args.mss_clamp,
              'all_tools':args.all_tools,'secret_file':'secrets/master.key',
              'client':{'host':'local','peer_address':host},'server':{'host':remote_alias}}
        for field,variable in [('mark','TUNTOM_MARK'),('mask','TUNTOM_MARK_MASK'),('table','TUNTOM_TABLE')]:
            if variable in os.environ: data[field]=integer(os.environ[variable])
        if 'TUNTOM_CHAIN' in os.environ: data['chain']=os.environ['TUNTOM_CHAIN']
        for field,variable,default in [('pre_hook','TUNTOM_PRE_HOOK','/etc/tuntom/tuntom-pre.sh'),('post_hook','TUNTOM_POST_HOOK','/etc/tuntom/tuntom-post.sh'),('group_pre_hook','TUNTOM_GROUP_PRE_HOOK',''),('group_post_hook','TUNTOM_GROUP_POST_HOOK','')]:
            source=os.environ.get(variable,default)
            if source and Path(source).exists():
                relative='hooks/'+field+'.sh'
                copy_source(source,work/relative)
                data[field]=relative
            elif source and variable in os.environ:
                raise Invalid(f'missing requested hook: {source}')
        for side in ('client','server'):
            switch=getattr(args,side+'_switch')
            endpoint=data[side]
            if switch:
                endpoint.update(switch_socket=switch[0],port_id=switch[1],label=integer(switch[2]))
            for field,flag in [('exit_node','switch_exit_node'),('ipc','switch_ipc'),('ipc_batch','switch_ipc_batch')]:
                value=getattr(args,side+'_'+flag)
                if value is not None and value is not False: endpoint[field]=value
            source=getattr(args,side+'_classifier_file')
            if source:
                relative=f'files/{side}/classifier.rules'
                (work/relative).parent.mkdir(mode=0o700,parents=True,exist_ok=True)
                if side=='client': copy_source(source,work/relative)
                else: remote_classifier(hosts,remote_alias,source,work/relative,work)
                endpoint['classifier_file']=relative
        secret=os.environ.get('TUNTOM_SECRET','')
        if len(secret)!=32 or any(c not in '0123456789abcdefABCDEF' for c in secret):
            raise Invalid('TUNTOM_SECRET must contain exactly 32 hexadecimal characters')
        (work/'secrets').mkdir(mode=0o700)
        (work/'secrets'/'master.key').write_text(secret+'\n')
        (work/'secrets'/'master.key').chmod(0o600)
        (work/'instance.yml').write_text(yaml.safe_dump({'tuntom_instance':data},sort_keys=False))
        Instance('tunnel',alias,work,data)
        inventory.mkdir(mode=0o700,parents=True,exist_ok=True)
        lock=os.open(inventory,os.O_RDONLY | os.O_DIRECTORY)
        fcntl.flock(lock,fcntl.LOCK_EX | fcntl.LOCK_NB)
        current=read_yaml(inventory/'hosts.yml') if (inventory/'hosts.yml').exists() else {'all':{'hosts':{}}}
        expected={key:value for key,value in hosts['all']['hosts'].items() if key not in created_hosts}
        if current.get('all',{}).get('hosts',{})!=expected:
            os.close(lock)
            raise Invalid('inventory changed during import; retry')
        directory.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
        os.mkdir(directory,0o700)
        try:
            # Copy only public instance assets and the explicitly handled secret;
            # temporary Ansible inventory/vars must not become deployment assets.
            for entry in ['instance.yml','hooks','files','secrets']:
                source=work/entry
                if source.is_dir(): shutil.copytree(source,directory/entry)
                elif source.is_file(): shutil.copyfile(source,directory/entry)
            for file in directory.rglob('*'):
                file.chmod(0o700 if file.is_dir() else 0o600)
            private_json(inventory/'.state'/('tunnel-'+alias)/'identity.json',{
                'owner':str(uuid.uuid4()),'created_hosts':created_hosts,
                'adoption_command':['bash',str(REPO/'mk_tunnel.sh'),str(args.id),remote,'--stop']})
            private_text(inventory/'hosts.yml',yaml.safe_dump(hosts,sort_keys=False))
        except Exception:
            shutil.rmtree(directory)
            shutil.rmtree(inventory/'.state'/('tunnel-'+alias),ignore_errors=True)
            raise
        finally:
            os.close(lock)
    print('Central definition created: '+str(directory))
    if not args.prepare_only:
        deploy_main(['--inventory',str(inventory),'--deployment','tunnel:'+alias,'--install']+(['--check'] if args.check else []))


if __name__=='__main__':
    try:
        os.umask(0o077)
        main()
    except (Invalid,OSError,ValueError,subprocess.CalledProcessError) as error:
        print(f'tt_tunnel: {error}',file=sys.stderr)
        sys.exit(1)
