"""Persistent first-deploy/undeploy orchestration for SAS V3 and collector listener."""
import base64
import contextvars
import hashlib
import json
from pathlib import Path
import re
import secrets
import shlex
import sqlite3
import subprocess
import threading
from datetime import datetime, timezone
from sas_te import preview_request, installed_binary, network_options
from tunnel_deployments import validate
from deployment_diagnostics import safe_output


class SASDeployments:
    def __init__(self,path,endpoints,fabric,secret_dir):
        self.db=sqlite3.connect(str(path) if path is not None else ':memory:',check_same_thread=False)
        self.db.row_factory=sqlite3.Row
        self.db.execute('CREATE TABLE IF NOT EXISTS sas_deployment(id TEXT PRIMARY KEY, config TEXT NOT NULL, state TEXT NOT NULL, progress TEXT NOT NULL, error TEXT, updated_at TEXT NOT NULL)')
        self.db.execute("UPDATE sas_deployment SET state='attention',error='Fabric restarted during operation; inspect SAS status before retrying' WHERE state IN ('deploying','undeploying','applying')")
        self.db.commit();self.lock=threading.RLock();self.endpoints=endpoints;self.fabric=fabric
        self.secret_dir=secret_dir;self.memory={};self.workers=[]

    def close(self):
        for worker in self.workers:worker.join()
        with self.lock:self.db.close()

    def launch(self,fn,*args):
        worker=threading.Thread(target=contextvars.copy_context().run,args=(fn,*args),daemon=True)
        self.workers.append(worker);worker.start()

    def public(self,row):
        result=dict(row);config=json.loads(result.pop('config'));result['progress']=json.loads(result['progress'])
        result.update(instance_id=config['instance_id'],prefix=config['prefix'],origin=config['origin'],name=config['name'],network=config.get('network',{'addresses':[],'routes':[]}),network_revision=hashlib.sha256(json.dumps(config.get('network',{}),sort_keys=True).encode()).hexdigest())
        return result

    def list(self):
        with self.lock:return {'sas_deployments':[self.public(row) for row in self.db.execute('SELECT * FROM sas_deployment ORDER BY updated_at DESC')]}

    def get(self,ident,private=False):
        with self.lock:row=self.db.execute('SELECT * FROM sas_deployment WHERE id=?',(ident,)).fetchone()
        if not row:raise KeyError('unknown SAS deployment')
        return json.loads(row['config']) if private else self.public(row)

    def update(self,ident,state,message,error=None):
        with self.lock,self.db:
            row=self.db.execute('SELECT progress FROM sas_deployment WHERE id=?',(ident,)).fetchone()
            progress=json.loads(row[0]);progress.append({'at':datetime.now(timezone.utc).isoformat(),'message':message})
            self.db.execute('UPDATE sas_deployment SET state=?,progress=?,error=?,updated_at=? WHERE id=?',(state,json.dumps(progress),error,datetime.now(timezone.utc).isoformat(),ident))

    def local(self,config,action,secret=None):
        body={'action':action,'intent':config['intent'],'deployment_id':config['deployment_id']}
        if secret is not None:body['secret']=secret
        return self.fabric.switch_deployment(body)

    def remote(self,config,action,secret=None):
        data={**config,'action':action}
        if action=='prepare':
            metadata,content=installed_binary()
            if metadata['sha256']!=config['sha256']:raise RuntimeError('Installed binary changed; prepare a new package')
            data.update(binary=base64.b64encode(content).decode(),secret=secret)
        script='CONFIG='+repr(data)+'\n'+Path(__file__).with_name('sas_install.py').read_text()+"\nprint(json.dumps(execute(CONFIG)))\n"
        result=self.endpoints.run(config['entry_endpoint_id'],'python3 -',input_data=script.encode(),timeout=120)
        return json.loads(result)

    def create(self,data):
        if not isinstance(data,dict) or set(data)!={'entry_endpoint_id','template','switch_id','expected_sha256'}:raise ValueError('invalid SAS deployment request')
        if not self.fabric.allow_write:raise RuntimeError('Fabric write operations are disabled')
        preview=preview_request({'entry_endpoint_id':data['entry_endpoint_id'],'template':data['template'],'package':True},self.endpoints)
        plan=preview['plan'];template=json.loads(preview['files']['template.json']);location=plan['location_snapshot'];ident=template['deployment_id']
        if plan['binary']['sha256']!=data['expected_sha256']:raise ValueError('Binary changed since preview; prepare a new package')
        profile=location.get('microservice_rootfs') or {}
        if location.get('microservice_mode')!='managed' or location.get('microservice_execution_enabled') is not True or not all(location.get('microservice_capabilities',{}).get(k) is True for k in ('installation','supervision','status','stop')):
            raise ValueError('SAS managed installation, supervision, status and stop are required')
        if template['runtime']['user']!='tuntom' or template['runtime']['group']!='tuntom' or profile.get('runtime_uid')!=65532 or profile.get('runtime_gid')!=65532:
            raise ValueError('SAS schema-1 rootfs requires tuntom UID/GID 65532')
        for endpoint in self.fabric.snapshot().get('endpoints',[]):
            match=re.fullmatch(r'([0-9]+)(?:_[0-9]+)?[cs]',endpoint.get('name',''))
            if endpoint.get('source','local')=='local' and endpoint.get('kind')=='tunnel' and match and int(match[1])==template['tunnel_id']:
                raise ValueError('Tunnel ID is already active on the collector host')
        resolved=self.fabric.switch_deployment({'action':'inspect','switch_id':data['switch_id']})
        if resolved['switch_socket']!=template['switch']['socket']:raise ValueError('Selected switch differs from preview')
        if template['switch']['port_id'] in resolved.get('ports',[]):raise ValueError('Switch port already in use')
        metadata,_=installed_binary()
        if profile.get('architecture')!=metadata['architecture']:raise ValueError('Binary architecture differs from SAS rootfs')
        result=subprocess.run(['readelf','--version-info',metadata['path']],capture_output=True,text=True,timeout=10,check=True)
        abi=sorted(set(re.findall(r'Name: ((?:GLIBC|GLIBCXX|CXXABI)_[0-9.]+)',result.stdout)))
        if not abi:raise ValueError('Unable to determine binary ABI requirements')
        dynamic=subprocess.run(['readelf','--dynamic',metadata['path']],capture_output=True,text=True,timeout=10,check=True).stdout
        needed=re.findall(r'\(NEEDED\).*?\[([^]]+)\]',dynamic)
        intent={'name':'sas-'+ident,'tunnel_id':template['tunnel_id'],'count':1,'software':{'source':'binary','sha256':metadata['sha256']},'secret':{'mode':'generate'},
                'side_a':{'endpoint_id':data['entry_endpoint_id'],'role':'initiator','peer_address':template['peer'],'attachment':{'type':'tun'},'runtime':'screen'},
                'side_b':{'switch_id':data['switch_id'],'role':'listener','attachment':{'type':'switch','port_id':template['switch']['port_id'],'label':template['switch']['label']},'runtime':'screen'}}
        validate(intent)
        config={'deployment_id':ident,'name':intent['name'],'entry_endpoint_id':data['entry_endpoint_id'],'instance_id':template['instance_id'],'prefix':template['prefix'],
                'origin':location['origin'],'namespace':location['namespace'],'installation':location['microservices_dir'],'profile':profile,'sha256':metadata['sha256'],
                'interface':template['interface'],'peer':template['peer'],'network':plan['network'],'needed_libraries':needed,'version':metadata['version'],'commit':metadata.get('commit','unknown'),'abi_versions':abi,'intent':intent,
                'script':preview['files']['payload/microservices/'+template['prefix']+'-start.sh'],'registration':preview['files']['registration.row']}
        with self.lock,self.db:
            for row in self.db.execute("SELECT config FROM sas_deployment WHERE state!='removed'"):
                old=json.loads(row[0])
                if old['instance_id']==config['instance_id'] or old['intent']['tunnel_id']==intent['tunnel_id']:
                    raise ValueError('Existing SAS deployment uses this instance or tunnel ID; undeploy it first')
            self.db.execute('INSERT INTO sas_deployment VALUES(?,?,?,?,?,?)',(ident,json.dumps(config),'deploying','[]',None,datetime.now(timezone.utc).isoformat()))
        secret=secrets.token_hex(16)
        try:
            if self.secret_dir:
                path=self.secret_dir/(ident+'.sas.key')
                import os
                fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
                with os.fdopen(fd,'w') as stream:stream.write(secret)
            else:self.memory[ident]=secret
        except OSError:
            self.update(ident,'attention','Could not store deployment key; no remote changes made')
            raise
        self.launch(self.deploy,config,secret)
        return self.get(ident)

    def deploy(self,config,secret):
        ident=config['deployment_id'];registered=False
        try:
            self.update(ident,'deploying','SAS: verify V3 profile, ABI, ownership and empty slot; stage payload')
            self.remote(config,'prepare',secret)
            self.update(ident,'deploying','Switch: prepare listener and shared key')
            self.local(config,'prepare',secret)
            self.update(ident,'deploying','Switch: start and verify listener')
            self.local(config,'start')
            self.update(ident,'deploying','SAS: register service; SAS will start it on its next scan')
            # A lost response can mean registration succeeded. Do not remove the listener.
            registered=True
            self.remote(config,'activate')
            self.update(ident,'registered','Registered with SAS; use Refresh status to observe startup')
            self.fabric.audit_event({'action':'sas_te.deploy','target':ident,'outcome':'succeeded'})
        except Exception as error:
            # Preserve staged files for verified cleanup; never guess process emptiness.
            if not registered:
                try:self.local(config,'undeploy')
                except Exception:pass
            self.update(ident,'attention','Deployment needs attention; staged files retained',safe_output(error)[:1500])

    def status(self,ident):
        config=self.get(ident,True)
        result=self.remote(config,'status')
        row=self.get(ident)
        if row['state'] not in ('deploying','undeploying','applying','removed'):
            self.update(ident,'running' if result.get('state')=='running' and result.get('registered') else row['state'],'SAS state: '+str(result.get('state')))
        return {**self.get(ident),'sas_status':result}

    def undeploy(self,ident):
        if not self.fabric.allow_write:raise RuntimeError('Fabric write operations are disabled')
        config=self.get(ident,True)
        with self.lock,self.db:
            changed=self.db.execute("UPDATE sas_deployment SET state='undeploying' WHERE id=? AND state IN ('registered','running','attention')",(ident,)).rowcount
            if not changed:raise RuntimeError('SAS deployment operation already active or removed')
        self.launch(self.remove,config)
        return self.get(ident)

    def remove(self,config):
        ident=config['deployment_id']
        try:
            self.update(ident,'undeploying','SAS: withdraw registration and verify exact-run stop')
            self.remote(config,'withdraw')
            self.update(ident,'undeploying','Switch: stop listener and remove its deployment')
            self.local(config,'undeploy')
            self.update(ident,'undeploying','SAS: remove owned installation files; preserve runtime and locks')
            self.remote(config,'remove')
            if self.secret_dir:(self.secret_dir/(ident+'.sas.key')).unlink(missing_ok=True)
            else:self.memory.pop(ident,None)
            self.update(ident,'removed','Undeployed; SAS owns runtime cleanup')
            self.fabric.audit_event({'action':'sas_te.undeploy','target':ident,'outcome':'succeeded'})
        except Exception as error:self.update(ident,'attention','Undeploy incomplete; files retained where state was uncertain',safe_output(error)[:1500])

    def apply(self,ident,data):
        if not self.fabric.allow_write:raise RuntimeError('Fabric write operations are disabled')
        if not isinstance(data,dict) or set(data)!={'network','expected_revision'}:raise ValueError('invalid network update')
        desired=network_options(data['network'])
        with self.lock,self.db:
            row=self.get(ident)
            if row['state'] not in ('registered','running','attention'):raise RuntimeError('Deployment operation active or removed')
            if row['network_revision']!=data['expected_revision']:raise RuntimeError('Network configuration changed; reload before applying')
            config=self.get(ident,True)
            self.db.execute("UPDATE sas_deployment SET state='applying' WHERE id=?",(ident,))
        self.launch(self.apply_worker,config,desired)
        return self.get(ident)

    def apply_worker(self,config,desired):
        ident=config['deployment_id']
        try:
            self.update(ident,'applying','SAS: reconcile owned IP addresses and routes')
            self.remote({**config,'desired_network':desired},'apply')
            updated={**config,'network':desired}
            with self.lock,self.db:self.db.execute('UPDATE sas_deployment SET config=? WHERE id=?',(json.dumps(updated),ident))
            self.update(ident,'registered','Network changes applied; service was not restarted')
            self.fabric.audit_event({'action':'sas_te.apply','target':ident,'outcome':'succeeded','details':{'before':config.get('network',{}),'after':desired}})
        except Exception as error:
            self.update(ident,'attention','Network update incomplete; retry Apply changes to reconcile partial changes',safe_output(error)[:1500])
