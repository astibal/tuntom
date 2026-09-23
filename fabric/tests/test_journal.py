import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from journal import Journal,actor
from server import Fabric,digest
from errors import APIError
from collector import CollectorServer,RemoteFabric
from types import SimpleNamespace


def endpoint(status='reachable',warn=False):
    return {'id':'node','name':'edge','status':status,'health':{'checks':[
        {'key':'session','state':'warn' if warn else 'ok','code':'session_down' if warn else 'session_up'}] if status=='reachable' else []}}

class JournalTests(unittest.TestCase):
    def test_persisted_transitions_deduplicated_and_unknown_does_not_resolve(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'journal.sqlite'
            journal=Journal(path)
            journal.observe([endpoint(warn=True)])
            journal.observe([endpoint(warn=True)])
            self.assertEqual(len(journal.page({'category':'event'})['entries']),1)
            journal.observe([endpoint(status='unavailable')])
            self.assertEqual(len(journal.page({'active':'1'})['conditions']),2)
            journal.close();journal=Journal(path)
            journal.observe([endpoint(status='unavailable')])
            self.assertEqual(len(journal.page({'category':'event'})['entries']),2)
            journal.observe([endpoint()])
            self.assertEqual(journal.page({'active':'1'})['conditions'],[])
            self.assertEqual([r['outcome'] for r in journal.page({'category':'event'})['entries']].count('resolved'),2)
            journal.observe([endpoint(warn=True)])
            self.assertEqual(len(journal.page({'category':'event'})['entries']),5)
            journal.close()
            self.assertEqual(path.stat().st_mode & 0o077,0)

    def test_disappearance_and_return_are_explicit(self):
        journal=Journal();self.addCleanup(journal.close)
        journal.observe([endpoint()]);journal.observe([]);journal.observe([])
        self.assertEqual(len(journal.page({'category':'event'})['entries']),1)
        self.assertEqual(journal.page({'active':'1','target':'other'})['conditions'],[])
        journal.observe([endpoint()])
        self.assertEqual(journal.page({'category':'event'})['entries'][0]['outcome'],'resolved')

    def test_actor_filters_pagination_and_retention(self):
        journal=Journal(retention_days=1);self.addCleanup(journal.close)
        token=actor.set({'username':'alice','role':'admin'})
        try:
            for i in range(105):journal.append('classifier.check','node','succeeded',{'generation':str(i)})
        finally:actor.reset(token)
        first=journal.page({'actor':'alice'});self.assertEqual(len(first['entries']),100)
        last=journal.page({'before':first['before'],'actor':'alice'});self.assertEqual(len(last['entries']),5)
        self.assertEqual(journal.page({'actor':'bob'})['entries'],[])
        with journal.db:journal.db.execute('UPDATE entries SET at=0')
        journal.observe([])
        self.assertEqual(journal.page()['entries'],[])
        for args in ({'category':'invalid'},{'before':'-1'},{'oops':1}):
            with self.assertRaises(APIError):journal.page(args)

    def test_restart_inventory_grace(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'journal.sqlite'
            journal=Journal(path);journal.observe([endpoint()]);journal.close()
            journal=Journal(path,missing_grace_seconds=150)
            self.addCleanup(journal.close)
            journal.observe([])
            self.assertEqual(journal.page({'category':'event'})['entries'],[])
            journal.missing_after=0
            journal.observe([])
            self.assertEqual(journal.page({'category':'event'})['entries'][0]['action'],'disappeared')

    def test_write_diff_actor_and_fail_closed(self):
        f=Fabric(allow_write=True);self.addCleanup(f.close)
        f.endpoint=lambda key:SimpleNamespace(kind='adapter',name='ex0',id=key)
        calls=[];rules='format 1\nclassify to [17]\n'
        def control(e,op,body=''):
            calls.append(op)
            return {'stats':'format=txt\nformat_version=1\nclassifier_generation=1\n',
                    'classifier-show':rules,'classifier-check':'valid=1\n'}.get(op,'classifier_generation=2\n')
        f.control_query=control
        token=actor.set({'username':'alice','role':'admin'})
        try:f.classifier('node','load',{'rules':rules.replace('17','42'),'expected_sha256':digest(rules),'expected_generation':'1'})
        finally:actor.reset(token)
        records=f.journal.page()['entries']
        self.assertEqual({r['actor'] for r in records},{'alice'})
        self.assertEqual(len({r['operation_id'] for r in records}),1)
        prepared=next(r for r in records if r['outcome']=='prepared')
        self.assertIn('-classify to [17]',prepared['details']['diff'])
        self.assertIn('+classify to [42]',prepared['details']['diff'])
        calls.clear()
        with patch.object(f.journal,'append',side_effect=APIError(503,'disk full')):
            with self.assertRaises(APIError):f.classifier('node','disable',{})
        self.assertEqual(calls,[])

    def test_verified_actor_survives_unix_collector_boundary(self):
        import threading
        f=Fabric();self.addCleanup(f.close)
        with tempfile.TemporaryDirectory() as root:
            server=CollectorServer(root+'/socket',f,os.getuid())
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                proxy=RemoteFabric(root+'/socket')
                context=actor.set({'username':'alice','role':'admin-ro'})
                try:proxy.refresh()
                finally:actor.reset(context)
                rows=proxy.journal_entries({'category':'audit'})['entries']
                self.assertEqual(rows[0]['actor'],'alice')
                self.assertEqual(rows[0]['role'],'admin-ro')
            finally:server.shutdown();server.server_close();thread.join()
