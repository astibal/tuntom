"""Bounded, ephemeral collector jobs, independent of periodic sampling."""
import secrets
import threading
import time

from errors import APIError
from control import ControlError, ResponseTooLarge


class Requests:
    def __init__(self, limit=64, concurrency=16, retention=60):
        self.limit, self.concurrency, self.retention = limit, concurrency, retention
        self.lock = threading.Lock()
        self.jobs = {}
        self.threads = set()
        self.closed = False

    def _prune(self):
        cutoff = time.monotonic() - self.retention
        self.jobs = {k: v for k, v in self.jobs.items() if v["finished"] is None or v["finished"] > cutoff}

    def submit(self, work, *, target="local"):
        with self.lock:
            self._prune()
            if self.closed:
                raise APIError(503, "collector is shutting down")
            if any(job["target"] == target and job["finished"] is None for job in self.jobs.values()):
                raise APIError(429, "async request already running for this target")
            if len(self.threads) >= self.concurrency:
                raise APIError(429, "async request capacity reached; retry later")
            # Results are a cache, not a queue: make room without holding up
            # other targets. Bound each target's share of retained records.
            completed = sorted((job for job in self.jobs.values() if job["finished"] is not None),
                               key=lambda job: job["finished"])
            own = [job for job in completed if job["target"] == target]
            if len(own) >= 8:
                del self.jobs[own[0]["id"]]
            if len(self.jobs) >= self.limit:
                oldest = next((job for job in completed if job["id"] in self.jobs), None)
                if oldest is None:
                    raise APIError(429, "async request capacity reached; retry later")
                del self.jobs[oldest["id"]]
            key = secrets.token_hex(16)
            self.jobs[key] = {"id": key, "state": "running", "request_id": None, "finished": None, "target": target}
            thread = threading.Thread(target=self._run, args=(key, work), daemon=True)
            self.threads.add(thread)
            thread.start()
            return self._public(self.jobs[key])

    @staticmethod
    def _public(job):
        return {k: v for k, v in job.items() if k not in {"finished", "target"}}

    def _run(self, key, work):
        def accepted(request_id):
            with self.lock:
                self.jobs[key]["request_id"] = request_id
        try:
            outcome = {"state": "succeeded", "result": work(accepted)}
        except APIError as error:
            outcome = {"state": "failed", "status": error.status, "error": str(error)}
        except ControlError as error:
            outcome = {"state": "failed", "status": 502, "error": str(error)}
        except ResponseTooLarge:
            outcome = {"state": "failed", "status": 502, "error": "control response exceeds 1 MiB"}
        except TimeoutError:
            outcome = {"state": "failed", "status": 504, "error": "control request timed out"}
        except Exception:
            outcome = {"state": "failed", "status": 502, "error": "control request failed"}
        with self.lock:
            self.jobs[key].update(outcome, finished=time.monotonic())
            self.threads.discard(threading.current_thread())

    def get(self, key):
        with self.lock:
            self._prune()
            if key not in self.jobs:
                raise APIError(404, "async request expired or unknown")
            return self._public(self.jobs[key])

    def close(self):
        with self.lock:
            self.closed = True
            threads = list(self.threads)
        for thread in threads:
            thread.join()
