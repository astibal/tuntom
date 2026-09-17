"""Cache persistence, retention, exact counters and collector/HTTP boundaries."""
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
import zlib
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from history import History, chart_sample, PAGE_SIZE
from server import Fabric, now, APIError
from test_observer import endpoint
import test_observer
import test_fabric


class HistoryTests(unittest.TestCase):
    def test_restart_retention_identity_pagination_and_precision(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'history.sqlite'
            e=endpoint()
            f=Fabric(history_path=path)
            raw='format=txt\nformat_version=1\nswitch_rx_bps_5s=123.5\nqueue_full_drops=18446744073709551615\n'
            with patch.object(f,'control_query',return_value=raw):
                sample=f._sample(e)
            point=chart_sample(e,sample)
            self.assertEqual(point['rx'],123.5)
            f.close()
            cache=History(path)
            self.assertEqual(cache.read(e.id)['samples'],[point])
            self.assertEqual(cache.read('different-boot:123:100')['samples'],[])
            stored=cache.db.execute('SELECT telemetry FROM samples').fetchone()[0]
            self.assertEqual(json.loads(zlib.decompress(stored))['metrics']['queue_full_drops'],'18446744073709551615')
            current=int(time.time()*1000)
            with cache.db:
                for i in range(PAGE_SIZE+5):
                    t=current-10000+i
                    cache.db.execute('INSERT INTO samples VALUES (?,?,?,?)',('paged',t,json.dumps({'time':t}),b''))
                cache.db.execute('INSERT INTO samples VALUES (?,?,?,?)',('expired',current-86400001,'{}',b''))
            first=cache.read('paged');second=cache.read('paged',first['next_after'],first['until'])
            self.assertEqual(len(first['samples']),PAGE_SIZE)
            self.assertEqual(len(second['samples']),5)
            self.assertIsNone(second['next_after'])
            cache.prune()
            self.assertEqual(cache.db.execute("SELECT count(*) FROM samples WHERE endpoint='expired'").fetchone()[0],0)
            cache.close()
            # Cache is disposable: removing it while stopped starts clean.
            path.unlink()
            cache=History(path)
            self.assertEqual(cache.read(e.id)['samples'],[])
            cache.close()

    def test_failure_gap_incident_and_storage_failure_does_not_break_live(self):
        with tempfile.TemporaryDirectory() as root:
            f=Fabric(history_path=Path(root)/'history.sqlite')
            with patch.object(f,'control_query',side_effect=OSError('secret path')):
                sample=f._sample(endpoint())
            point=f.history(endpoint().id)['samples'][0]
            self.assertIsNone(point['rx'])
            self.assertEqual(point['issues'][0]['code'],'telemetry_missing')
            self.assertNotIn('secret',json.dumps(point))
            with patch.object(f.history_store,'record',side_effect=OSError('disk full')):
                self.assertEqual(f._sample(endpoint())['status'],'unavailable')
            self.assertIsNotNone(f.snapshot()['history']['error'])
            for body in ({'after':True},{'until':-1},{'after':'1'},{'path':'/tmp'}):
                with self.assertRaises(APIError): f.history(endpoint().id,body)
            f.close()

    def test_reject_shared_directory_and_symlink(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'history.sqlite'
            path.symlink_to(Path(root)/'target')
            with self.assertRaises(ValueError): History(path)
            path.unlink()
            os.chmod(root,0o755)
            with self.assertRaises(ValueError): History(path)


class HistoryCollectorTests(unittest.TestCase):
    setUp = test_observer.CollectorTests.setUp
    tearDown = test_observer.CollectorTests.tearDown
    def test_history_rpc(self):
        self.fabric.history_store=History(Path(self.root.name)/'history.sqlite')
        try:
            self.fabric._sample(endpoint())
            self.assertEqual(len(self.remote.history(endpoint().id)['samples']),1)
            self.assertTrue(self.remote.snapshot()['history']['enabled'])
            with self.assertRaises(APIError): self.remote.history(endpoint().id,{'after':-1})
        finally:
            self.fabric.history_store.close()


class HistoryAPITests(unittest.TestCase):
    setUp = test_fabric.APITests.setUp
    tearDown = test_fabric.APITests.tearDown
    request = test_fabric.APITests.request
    def test_history_route_auth_and_validation(self):
        # Reuse the API fixture's authenticated request helper.
        root=tempfile.TemporaryDirectory()
        self.fabric.history_store=History(Path(root.name)/'history.sqlite')
        try:
            self.fabric._sample(endpoint())
            self.assertEqual(self.request('GET','/api/v1/endpoints/boot%3A123%3A100/history')[0],200)
            self.assertEqual(self.request('GET','/api/v1/endpoints/id/history?after=-1')[0],400)
            self.assertEqual(self.request('GET','/api/v1/endpoints/id/history',auth=False)[0],401)
        finally:
            self.fabric.history_store.close()
            self.fabric.history_store=None
            root.cleanup()
