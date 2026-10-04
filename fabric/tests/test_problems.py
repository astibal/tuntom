import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from journal import Journal

class ProblemsTests(unittest.TestCase):
    def setUp(self):
        self.journal=Journal();self.addCleanup(self.journal.close)
    def sample(self, at, counters=None, status='reachable', state=None):
        return {'id':'node','name':'switch','status':status,'sampled_at':datetime.fromtimestamp(at,timezone.utc).isoformat(),
            'health':{'checks':[{'key':'errors','state':state or ('warn' if counters else 'ok'),'counters':counters or {}}]}}
    def observe(self, at, counters=None, **kwargs):
        with patch('journal.time.time',return_value=at):self.journal.observe([self.sample(at,counters,**kwargs)])
    def rows(self):return self.journal.page({'category':'problem'})['problems']
    def test_grouping_and_confirmed_end(self):
        self.observe(100,{'rtt_lost':'1'});self.observe(100,{'rtt_lost':'1'})
        self.observe(105,{'rtt_lost':'2','route_misses':'3'})
        self.assertEqual(len(self.rows()),2)
        lost=next(r for r in self.rows() if r['code']=='errors:rtt_lost')
        self.assertEqual(lost['count'],2);self.assertEqual(lost['since'],100)
        for at in range(110,141,5):self.observe(at)
        self.assertTrue(all(r['state']=='resolved' and r['ended']==140 for r in self.rows()))
        self.observe(145,{'rtt_lost':'1'});self.assertEqual(len(self.rows()),3)
    def test_missing_unknown_and_stale_do_not_resolve(self):
        self.observe(100,{'rtt_lost':'1'})
        self.observe(105)
        self.observe(110,status='unavailable')
        self.observe(140,state='unknown')
        with patch('journal.time.time',return_value=300):self.journal.observe([self.sample(145)])
        self.assertEqual(self.rows()[0]['state'],'active');self.assertIsNone(self.rows()[0]['ended'])
        for at in range(305,331,5):self.observe(at)
        self.assertIsNone(self.rows()[0]['ended'])
        self.observe(335);self.assertEqual(self.rows()[0]['ended'],335)
    def test_disappearance_and_restart_are_interrupted_not_resolved(self):
        self.observe(100,{'rtt_lost':'1'})
        with patch('journal.time.time',return_value=105):self.journal.observe([])
        self.assertEqual(self.rows()[0]['state'],'unknown');self.assertIsNone(self.rows()[0]['ended'])
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'journal.sqlite'
            j=Journal(path)
            with patch('journal.time.time',return_value=100):j.observe([self.sample(100,{'rtt_lost':'1'})])
            j.close();j=Journal(path)
            try:
                rows=j.page({'category':'problem'})['problems']
                self.assertEqual(rows[0]['state'],'unknown');self.assertIsNone(rows[0]['ended'])
                with patch('journal.time.time',return_value=105):j.observe([self.sample(105,{'rtt_lost':'1'})])
                self.assertEqual(len(j.page({'category':'problem'})['problems']),2)
            finally:j.close()
    def test_filters_and_pagination(self):
        for i in range(105):
            sample=self.sample(100,{'rtt_lost':'1'});sample['id']=str(i)
            with patch('journal.time.time',return_value=100):self.journal.observe([sample])
        page=self.journal.page({'category':'problem'})
        self.assertEqual(len(page['problems']),100)
        self.assertEqual(len(self.journal.page({'category':'problem','before':str(page['before'])})['problems']),5)
        self.assertEqual(len(self.journal.page({'category':'problem','target':'42'})['problems']),1)
