"""Disposable telemetry cache. No framework configuration, logs or credentials."""
from datetime import datetime
import json
import math
import os
from pathlib import Path
import sqlite3
import stat
import threading
import time
import zlib

from errors import APIError
from telemetry import health, switch_detail

RETENTION = 86400
PAGE_SIZE = 250


def default_path():
    if os.geteuid() == 0:
        return Path('/var/cache/tuntom-fabric/history.sqlite')
    return Path(os.environ.get('XDG_CACHE_HOME', str(Path.home() / '.cache'))) / 'tuntom-fabric/history.sqlite'


def chart_sample(endpoint, sample):
    metrics = sample['metrics']
    prefix = 'udp' if endpoint.kind == 'tunnel' else 'switch'
    def rate(direction):
        try:
            value = float(metrics[f'{prefix}_{direction}_bps_5s'])
            return value if math.isfinite(value) else None
        except (KeyError, ValueError, TypeError):
            return None
    point = {'time': round(datetime.fromisoformat(sample['sampled_at']).timestamp() * 1000),
             'rx': rate('rx'), 'tx': rate('tx')}
    if endpoint.kind == 'switch':
        detail = switch_detail(metrics, sample['changes'])
        point.update({f"cpu_{worker['index']}": worker['cpu_percent'] for worker in detail['workers']})
    issues = [check for check in health(endpoint, sample)['checks'] if check['state'] == 'warn']
    if sample['status'] == 'unavailable':
        issues.append({'key': 'telemetry', 'code': 'telemetry_missing'})
    if issues:
        point.update(issues=issues, interval=sample['changes'].get('interval_seconds'))
    return point


class History:
    def __init__(self, path):
        path = Path(path).absolute()
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        parent = path.parent.stat()
        if parent.st_uid != os.geteuid() or parent.st_mode & 0o077:
            raise ValueError('history directory must belong to collector UID and have mode 0700')
        if path.is_symlink():
            raise ValueError('history database must not be a symlink')
        fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
                raise ValueError('history database must be a private regular file owned by collector UID')
        finally:
            os.close(fd)
        self.lock = threading.Lock()
        self.db = sqlite3.connect(path, timeout=3, check_same_thread=False)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('PRAGMA synchronous=NORMAL')
        self.db.execute('CREATE TABLE IF NOT EXISTS samples (endpoint TEXT NOT NULL, time INTEGER NOT NULL, chart TEXT NOT NULL, telemetry BLOB NOT NULL, PRIMARY KEY(endpoint,time)) WITHOUT ROWID')
        self.db.execute('CREATE INDEX IF NOT EXISTS samples_time ON samples(time)')
        self.last_prune = 0
        self.prune()

    def prune(self):
        # Deleted pages are reused; no full VACUUM or growing append-only archive.
        with self.lock, self.db:
            self.db.execute('DELETE FROM samples WHERE time < ?', (int((time.time()-RETENTION)*1000),))
        self.last_prune = time.monotonic()

    def record(self, endpoint, sample):
        point = chart_sample(endpoint, sample)
        # Preserve uint64 counter strings exactly; do not persist endpoint arguments/errors/logs.
        telemetry = zlib.compress(json.dumps({'metrics': sample['metrics'], 'changes': sample['changes']}).encode())
        with self.lock, self.db:
            self.db.execute('INSERT OR IGNORE INTO samples VALUES (?,?,?,?)',
                            (endpoint.id, point['time'], json.dumps(point, allow_nan=False), telemetry))
        if time.monotonic() - self.last_prune > 60:
            self.prune()

    def read(self, endpoint, after=0, until=None):
        now_ms = int(time.time()*1000)
        until = now_ms if until is None else until
        if (type(after) is not int or type(until) is not int or
                not 0 <= after <= 2**53-1 or not 0 <= until <= 2**53-1):
            raise APIError(400, 'history timestamps must be nonnegative integer milliseconds')
        with self.lock:
            rows = self.db.execute('SELECT time,chart FROM samples WHERE endpoint=? AND time>? AND time>=? AND time<=? ORDER BY time LIMIT ?',
                                   (endpoint, after, now_ms-RETENTION*1000, min(until, now_ms), PAGE_SIZE+1)).fetchall()
        page, size = [], 0
        for row in rows[:PAGE_SIZE]:
            row_size = len(row[1].encode())
            if size + row_size > 4 * 1024 * 1024:
                if not page:
                    raise APIError(503, 'history sample exceeds response limit')
                break
            page.append(row)
            size += row_size
        return {'samples': [json.loads(row[1]) for row in page],
                'next_after': page[-1][0] if len(rows)>len(page) else None, 'until': until}

    def close(self):
        with self.lock:
            self.db.close()
