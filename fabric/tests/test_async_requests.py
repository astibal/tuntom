"""Async collector lifecycle and actual routed SOCK_SEQPACKET framing."""
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from async_requests import Requests
from collector import CollectorServer, RemoteFabric
from control import query, encode_route, ControlError, ResponseTooLarge
from errors import APIError
from server import Fabric


class AsyncTests(unittest.TestCase):
    def test_collector_rpc_does_not_wait_for_result(self):
        gate = threading.Event()
        started = threading.Event()
        fabric = Fabric(discover_fn=lambda: ([], {}))
        fabric.endpoint = lambda key: type("Origin", (), {"source":"local"})()
        def control(*args, on_accepted, **kwargs):
            on_accepted('a' * 32)
            started.set()
            gate.wait(3)
            return 'result'
        fabric.control_query = control
        with tempfile.TemporaryDirectory() as directory:
            server = CollectorServer(directory + '/collector', fabric, os.geteuid())
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                client = RemoteFabric(directory + '/collector')
                job = client.submit_request('origin', {'operation': 'discover'})
                self.assertTrue(started.wait(1))
                state = client.request_status(job['id'])
                self.assertEqual(state['state'], 'running')
                self.assertEqual(state['request_id'], 'a' * 32)
                self.assertIn('collector', client.snapshot())
                gate.set()
                for _ in range(100):
                    state = client.request_status(job['id'])
                    if state['state'] != 'running':
                        break
                    time.sleep(.01)
                self.assertEqual(state['result'], {'text': 'result'})
                with self.assertRaises(APIError) as error:
                    client.submit_request('origin', {'operation': 'load'})
                self.assertEqual(error.exception.status, 400)
            finally:
                gate.set()
                server.shutdown()
                server.server_close()
                fabric.close()
                thread.join()

    def test_capacity_expiry_failure_and_shutdown(self):
        jobs = Requests(limit=1, concurrency=1, retention=.02)
        gate = threading.Event()
        job = jobs.submit(lambda accepted: gate.wait(1))
        with self.assertRaises(APIError) as error:
            jobs.submit(lambda accepted: None)
        self.assertEqual(error.exception.status, 429)
        gate.set()
        jobs.close()
        time.sleep(.03)
        with self.assertRaises(APIError) as error:
            jobs.get(job['id'])
        self.assertEqual(error.exception.status, 404)
        with self.assertRaises(APIError) as error:
            jobs.submit(lambda accepted: None)
        self.assertEqual(error.exception.status, 503)
        jobs = Requests()
        def fail(accepted):
            raise TimeoutError()
        job = jobs.submit(fail)
        jobs.close()
        self.assertEqual(jobs.get(job['id'])['status'], 504)
        for failure, message in [(ControlError("target_not_found"), "target_not_found"),
                                 (ResponseTooLarge(), "control response exceeds 1 MiB")]:
            jobs = Requests()
            def rejected(accepted):
                raise failure
            job = jobs.submit(rejected)
            jobs.close()
            self.assertEqual(jobs.get(job['id'])['error'], message)

    def test_slow_target_isolated_and_results_do_not_block_other_targets(self):
        jobs = Requests(limit=2, concurrency=2)
        gate = threading.Event()
        try:
            slow = jobs.submit(lambda accepted: gate.wait(3), target=("origin", "slow"))
            for _ in range(5):
                with self.assertRaises(APIError) as error:
                    jobs.submit(lambda accepted: self.fail("must not run"), target=("origin", "slow"))
                self.assertEqual(error.exception.status, 429)
                self.assertIn("this target", str(error.exception))
            for index in range(5):
                fast = jobs.submit(lambda accepted: "ok", target=("origin", str(index)))
                deadline = time.monotonic() + 2
                while jobs.get(fast['id'])['state'] == 'running' and time.monotonic() < deadline:
                    time.sleep(.005)
                self.assertEqual(jobs.get(fast['id'])['result'], 'ok')
                self.assertEqual(jobs.get(slow['id'])['state'], 'running')
            self.assertEqual(len(jobs.jobs), 2)
        finally:
            gate.set()
            jobs.close()

    def test_completed_target_can_run_again_and_has_bounded_cache(self):
        jobs = Requests()
        try:
            for _ in range(12):
                job = jobs.submit(lambda accepted: "ok", target="same")
                deadline = time.monotonic() + 2
                while jobs.get(job['id'])['state'] == 'running' and time.monotonic() < deadline:
                    time.sleep(.005)
                self.assertEqual(jobs.get(job['id'])['state'], 'succeeded')
            self.assertEqual(len(jobs.jobs), 8)
        finally:
            jobs.close()

    def test_route_validation(self):
        self.assertEqual(encode_route([]), '-')
        self.assertEqual(encode_route([{'port': 'abc'}, {'peer': True}]), '02036162630100')
        for route in [None, 'peer', [{'port': 'a b'}], [{'peer': False}], [{'peer': 1}], [{'link': 'a'}], [{'peer': True}] * 17]:
            with self.assertRaises(ValueError):
                encode_route(route)

    def test_routed_framing_rejection_limits_and_timeout(self):
        for mode in ('ok', 'reject', 'oversize', 'timeout', 'bad_ack'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                path = directory + '/control'
                listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                listener.bind(path)
                listener.listen(1)
                commands = []
                def daemon():
                    with listener.accept()[0] as conn:
                        commands.append(conn.recv(1024))
                        conn.send(b'WRONG' if mode == 'bad_ack' else b'REQUEST ' + b'a' * 32 + b'\n')
                        if mode == 'bad_ack':
                            return
                        if mode == 'timeout':
                            time.sleep(.15)
                            return
                        conn.send(b'REJECTED 4\n' if mode == 'reject' else b'OK 4\n')
                        conn.send(b'test')
                thread = threading.Thread(target=daemon)
                thread.start()
                accepted = []
                try:
                    kwargs = dict(route=[{'peer': True}], on_accepted=accepted.append,
                                  timeout=.05 if mode == 'timeout' else 1,
                                  max_response_bytes=3 if mode == 'oversize' else 100)
                    if mode == 'ok':
                        self.assertEqual(query(path, 'stats', **kwargs), 'test')
                    else:
                        expected = {'reject': ControlError, 'oversize': ResponseTooLarge,
                                    'timeout': TimeoutError, 'bad_ack': ControlError}[mode]
                        with self.assertRaises(expected):
                            query(path, 'stats', **kwargs)
                    self.assertEqual(commands, [b'routed 5 250 0100 show stats'])
                    self.assertEqual(accepted, [] if mode == 'bad_ack' else ['a' * 32])
                finally:
                    thread.join()
                    listener.close()


if __name__ == '__main__':
    unittest.main()
