"""Persistent operator audit and component condition transitions (not syslog)."""
from contextvars import ContextVar
from functools import wraps
import json
import os
from pathlib import Path
import sqlite3
import stat
import threading
import time
import uuid
from errors import APIError

actor = ContextVar('fabric_actor', default={'username':'system','role':'internal'})
operation_id = ContextVar('fabric_operation_id', default=None)


def default_journal_path():
    base = Path('/var/lib') if os.geteuid() == 0 else Path(os.environ.get('XDG_STATE_HOME', str(Path.home()/'.local/state')))
    return base/'tuntom-fabric/journal.sqlite'


def clipped(value, limit=65536):
    text = str(value)
    return text[:limit], len(text)>limit


class Journal:
    def __init__(self, path=None, retention_days=90, missing_grace_seconds=0):
        if not 1 <= retention_days <= 3650:
            raise ValueError('journal retention must be 1..3650 days')
        if path:
            path=Path(path).absolute()
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            info=path.parent.stat()
            if info.st_uid!=os.geteuid() or info.st_mode & 0o077:
                raise ValueError('journal directory must belong to the process UID and have mode 0700')
            fd=os.open(path,os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
            try:
                info=os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.geteuid() or info.st_mode & 0o077:
                    raise ValueError('journal must be a private regular file')
            finally:os.close(fd)
        self.lock=threading.RLock()
        self.db=sqlite3.connect(str(path) if path else ':memory:',check_same_thread=False)
        self.db.row_factory=sqlite3.Row
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('PRAGMA synchronous=FULL')
        self.db.executescript('''
          CREATE TABLE IF NOT EXISTS entries(id INTEGER PRIMARY KEY, at REAL NOT NULL, category TEXT NOT NULL,
            action TEXT NOT NULL, target TEXT NOT NULL, actor TEXT NOT NULL, role TEXT NOT NULL,
            operation_id TEXT, outcome TEXT NOT NULL, details TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS entries_category_id ON entries(category,id);
          CREATE TABLE IF NOT EXISTS conditions(target TEXT NOT NULL, code TEXT NOT NULL, name TEXT NOT NULL,
            details TEXT NOT NULL, since REAL NOT NULL, seen REAL NOT NULL, PRIMARY KEY(target,code));
          CREATE TABLE IF NOT EXISTS components(target TEXT PRIMARY KEY, name TEXT NOT NULL, seen REAL NOT NULL);
        ''')
        self.retention_days=retention_days
        self.persistent=bool(path)
        self.error=None
        self.pruned=0
        self.missing_after=time.monotonic()+missing_grace_seconds

    def _insert(self, category, action, target, outcome, details, identity, op):
        encoded=json.dumps(details,ensure_ascii=False)
        if len(encoded.encode())>131072:
            encoded=json.dumps({'truncated':True,'preview':encoded[:32768]})
        return self.db.execute('INSERT INTO entries(at,category,action,target,actor,role,operation_id,outcome,details) VALUES(?,?,?,?,?,?,?,?,?)',
            (time.time(),category,action,str(target)[:1024],str(identity.get('username','unknown'))[:128],
             str(identity.get('role','unknown'))[:32],op,outcome,encoded)).lastrowid

    def append(self, action, target='', outcome='started', details=None, op=None, identity=None):
        try:
            with self.lock,self.db:
                result=self._insert('audit',action,target,outcome,details or {},identity or actor.get(),op or operation_id.get())
            self.error=None
            return result
        except (sqlite3.Error,OSError) as error:
            self.error='journal write failed'
            raise APIError(503,'audit storage unavailable; operation not started or completion not recorded') from error

    def observe(self, endpoints):
        tick=time.time()
        with self.lock,self.db:
            known={r['target']:r['name'] for r in self.db.execute('SELECT target,name FROM components')}
            seen=set()
            for e in endpoints:
                target=e['id'];name=e.get('name',target);seen.add(target)
                self.db.execute('INSERT INTO components VALUES(?,?,?) ON CONFLICT(target) DO UPDATE SET name=excluded.name,seen=excluded.seen',(target,name,tick))
                old={r['code']:dict(r) for r in self.db.execute('SELECT * FROM conditions WHERE target=?',(target,))}
                desired={}
                resolved={'disappeared'}
                if e.get('status')=='unavailable':
                    desired['telemetry']={'code':'telemetry_missing'}
                elif e.get('status')=='reachable':
                    resolved.add('telemetry')
                for check in e.get('health',{}).get('checks',[]):
                    key=check['key']
                    if check['state']=='warn':desired[key]=check
                    elif check['state']=='ok':resolved.add(key)
                for code,detail in desired.items():
                    if code not in old:self._insert('event',code,target,'raised',{'name':name,'condition':detail},{'username':'collector','role':'system'},None)
                    else:
                        previous=json.loads(old[code]['details'])
                        signature=lambda d:(d.get('code'),sorted(d.get('counters',{})),d.get('values'))
                        if signature(previous)!=signature(detail):self._insert('event',code,target,'updated',{'name':name,'condition':detail},{'username':'collector','role':'system'},None)
                    self.db.execute('INSERT INTO conditions VALUES(?,?,?,?,?,?) ON CONFLICT(target,code) DO UPDATE SET name=excluded.name,details=excluded.details,seen=excluded.seen',
                        (target,code,name,json.dumps(detail),tick,tick))
                for code in resolved-desired.keys():
                    if code in old:
                        self._insert('event',code,target,'resolved',{'name':name,'since':old[code]['since']},{'username':'collector','role':'system'},None)
                        self.db.execute('DELETE FROM conditions WHERE target=? AND code=?',(target,code))
            for target in known.keys()-seen:
                # A fresh collector must rebuild its discovered inventory first.
                if time.monotonic()<self.missing_after:continue
                if not self.db.execute('SELECT 1 FROM conditions WHERE target=? AND code=?',(target,'disappeared')).fetchone():
                    self._insert('event','disappeared',target,'raised',{'name':known[target]},{'username':'collector','role':'system'},None)
                    self.db.execute('INSERT INTO conditions VALUES(?,?,?,?,?,?)',(target,'disappeared',known[target],'{}',tick,tick))
            if tick-self.pruned>3600:
                cutoff=tick-self.retention_days*86400
                self.db.execute('DELETE FROM entries WHERE at<?',(cutoff,))
                self.db.execute('DELETE FROM conditions WHERE target IN (SELECT target FROM components WHERE seen<?)',(cutoff,))
                self.db.execute('DELETE FROM components WHERE seen<?',(cutoff,))
                self.pruned=tick
        self.error=None

    def page(self, params=None):
        params=params or {}
        if not isinstance(params,dict) or set(params)-{'category','before','target','actor','active'}:
            raise APIError(400,'invalid journal query')
        category=params.get('category','audit')
        if category not in {'audit','event'}:raise APIError(400,'invalid journal category')
        if params.get('active') not in (None,'1'):raise APIError(400,'invalid active filter')
        try:before=int(params.get('before',2**63-1))
        except (ValueError,TypeError):raise APIError(400,'invalid journal cursor')
        if not 0<before<=2**63-1:raise APIError(400,'invalid journal cursor')
        with self.lock:
            if params.get('active')=='1':
                target=params.get('target')
                if target and (not isinstance(target,str) or len(target)>1024):raise APIError(400,'invalid target')
                rows=[dict(r) for r in self.db.execute('SELECT * FROM conditions'+(' WHERE target=?' if target else '')+' ORDER BY since DESC LIMIT 500',(target,) if target else ())]
                total=self.db.execute('SELECT COUNT(*) FROM conditions'+(' WHERE target=?' if target else ''),(target,) if target else ()).fetchone()[0]
                size=0;bounded=[]
                for row in rows:
                    size+=len(row['details'].encode())+2048
                    if size>2*1024*1024:break
                    row['details']=json.loads(row['details']);bounded.append(row)
                return {'conditions':bounded,'total':total,'limit':500,'persistent':self.persistent,'retention_days':self.retention_days,'error':self.error}
            where=['category=?','id<?'];args=[category,before]
            for field in ('target','actor'):
                if params.get(field):
                    if not isinstance(params[field],str) or len(params[field])>1024:raise APIError(400,'invalid filter')
                    where.append(field+'=?');args.append(params[field])
            rows=[dict(r) for r in self.db.execute('SELECT * FROM entries WHERE '+' AND '.join(where)+' ORDER BY id DESC LIMIT 101',args)]
            more=len(rows)>100;rows=rows[:100]
            size=0;bounded=[]
            for row in rows:
                size+=len(row['details'].encode())+2048
                if size>2*1024*1024:more=True;break
                row['details']=json.loads(row['details']);bounded.append(row)
            rows=bounded
            return {'entries':rows,'before':rows[-1]['id'] if more else None,'retention_days':self.retention_days,
                    'persistent':self.persistent,'error':self.error}

    def close(self):
        with self.lock:self.db.close()


def audited(family):
    def decorate(fn):
        @wraps(fn)
        def run(self,*args,**kwargs):
            target=args[0] if args and family!='refresh' else ''
            op=args[1] if family in {'rules','classifier'} and len(args)>1 else ''
            action=family+('.'+op if op else '')
            token=operation_id.set(uuid.uuid4().hex)
            try:
                self.journal.append(action,target)
                try:
                    result=fn(self,*args,**kwargs)
                except Exception as error:
                    try:self.journal.append(action,target,'unknown' if 'outcome may be unknown' in str(error) or 'route changed during request' in str(error) else 'failed',{'error':clipped(error,2048)[0],'status':getattr(error,'status',500)})
                    except APIError:pass
                    raise
                details={}
                if isinstance(result,dict):
                    for key in ('sha256','generation','sampled_at','persistence','id','state'):
                        if key in result:details[key]=result[key]
                    if isinstance(result.get('result'),str):details['result']=clipped(result['result'],2048)[0]
                    if 'diff' in result:details['diff'],details['diff_truncated']=clipped(result['diff'])
                try:self.journal.append(action,target,'submitted' if family=='request' else 'succeeded',details)
                except APIError:
                    if isinstance(result,dict):result['audit_error']='operation completed but audit completion could not be stored; do not retry writes'
                    else:raise
                return result
            finally:operation_id.reset(token)
        return run
    return decorate
