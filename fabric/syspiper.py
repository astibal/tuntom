"""Optional IP-only system telemetry. A separate poller never blocks Fabric probes."""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import http.client
import ipaddress
import json
import math
import os
import socket
import sqlite3
import subprocess
import threading
import time

MAX_NODES = 64
MAX_BODY = 1024 * 1024
PATHS = ('cpu', 'ram', 'disk', 'net', 'system', 'interfaces', 'filesystems', 'pressure')


def ip_literal(value):
    try:
        ip = ipaddress.ip_address(value)
        if ip.is_unspecified or ip.is_multicast or ip.is_link_local or '%' in str(ip) or str(ip) == '255.255.255.255':
            return None
        return str(ip)
    except ValueError:
        return None


def add_arguments(parser):
    parser.add_argument('--syspiper-key', help='backend-only shared X-API-Key; alternatively TUNTOM_SYSPIPER_KEY')
    parser.add_argument('--syspiper-node', action='append', default=[], metavar='IP', help='additional known Tuntom IP (repeatable; no DNS or subnet scan)')
    parser.add_argument('--syspiper-port', type=int, default=8181)
    parser.add_argument('--syspiper-interval', type=float, default=30, help='independent system poll interval, seconds (default 30)')


def options(args):
    key = args.syspiper_key if args.syspiper_key is not None else os.environ.get('TUNTOM_SYSPIPER_KEY', '')
    if key and (len(key) > 256 or any(ord(c) < 33 or ord(c) > 126 for c in key)):
        raise ValueError('Syspiper key must contain 1..256 printable ASCII characters without spaces')
    if not 1 <= args.syspiper_port <= 65535 or not 5 <= args.syspiper_interval <= 3600:
        raise ValueError('invalid Syspiper port or interval (5..3600 seconds)')
    nodes = []
    for value in args.syspiper_node:
        node = ip_literal(value)
        if node is None:
            raise ValueError('Syspiper nodes must be unicast IP literals, not names or subnets')
        if node not in nodes:
            nodes.append(node)
    if len(nodes) > MAX_NODES:
        raise ValueError('at most 64 Syspiper nodes are supported')
    if nodes and not key:
        raise ValueError('Syspiper nodes require a backend API key')
    return dict(key=key, nodes=nodes, port=args.syspiper_port, interval=args.syspiper_interval)


def interface_addresses():
    # Read only the local network namespace. No route/neighbor enumeration or scanning.
    try:
        result = subprocess.run(['ip', '-j', 'address', 'show'], capture_output=True, timeout=2, check=True)
        if len(result.stdout) > MAX_BODY:
            return [], 'interface_discovery_failed'
        return json.loads(result.stdout), None
    except (OSError, ValueError, subprocess.SubprocessError):
        return [], 'interface_discovery_failed'


def candidates(endpoints, addresses, explicit, namespace):
    found = {}
    def add(address, source, endpoint=None):
        ip = ip_literal(address)
        if not ip:
            return
        item = found.setdefault(ip, {'ip': ip, 'sources': [], 'processes': []})
        if source not in item['sources']:
            item['sources'].append(source)
        if endpoint and endpoint.id not in item['processes']:
            item['processes'].append(endpoint.id)
    add('127.0.0.1', 'local_host')
    for ip in explicit:
        add(ip, 'manual')
    by_interface = {a.get('ifname'): a for a in addresses if isinstance(a, dict)}
    for e in endpoints:
        # Only addresses in the collector's namespace can be reached as discovered.
        if e.net_namespace and e.net_namespace != namespace:
            continue
        if e.kind == 'tunnel' and e.role == 'client':
            add(e.peer, 'tunnel_peer', e)
        if e.kind not in {'tunnel', 'adapter', 'divert'}:
            continue
        for address in by_interface.get(e.interface, {}).get('addr_info', []):
            if not isinstance(address, dict):
                continue
            add(address.get('local', ''), 'interface_local', e)
            # iproute2 renders an explicit point-to-point peer as IP[/prefix].
            add(str(address.get('peer', '')).split('/')[0], 'interface_peer', e)
    return found


class ReadError(Exception):
    def __init__(self, code):
        self.code = code


def fetch(ip, port, path, key):
    # Literal IP, fixed path, no proxy environment, no redirects, no remote scripts.
    conn = http.client.HTTPConnection(ip, port, timeout=2)
    deadline = time.monotonic() + 4
    try:
        conn.request('GET', '/' + path, headers={'X-API-Key': key, 'Accept': 'application/json', 'Accept-Encoding': 'identity'})
        response = conn.getresponse()
        if response.status != 200:
            raise ReadError('unauthorized' if response.status in (401, 403) else
                            'unsupported' if response.status in (400, 404) else
                            'redirect_rejected' if 300 <= response.status < 400 else 'http_error')
        if response.getheader('Content-Encoding', 'identity') != 'identity':
            raise ReadError('invalid_response')
        raw = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ReadError('timeout')
            # read1 returns available bytes; deadline also bounds trickle responses.
            if conn.sock:
                conn.sock.settimeout(min(2, remaining))
            chunk = response.read1(min(65536, MAX_BODY + 1 - len(raw)))
            if not chunk:
                break
            raw.extend(chunk)
            if len(raw) > MAX_BODY:
                raise ReadError('response_too_large')
        data = json.loads(raw)
        if not isinstance(data, dict) or data.get('status') not in ('ok', 'partial'):
            raise ReadError('invalid_response')
        return data
    except ReadError:
        raise
    except (TimeoutError, socket.timeout):
        raise ReadError('timeout') from None
    except (OSError, ValueError, http.client.HTTPException, RecursionError):
        raise ReadError('unreachable') from None
    finally:
        conn.close()


def number(value, *, percent=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value) or value < 0 or (percent and value > 100):
        return None
    return value


def section(data, name):
    value = data.get(name, {})
    return value.get('data') if isinstance(value, dict) and value.get('status') in ('ok', 'partial') else None


def clean_snapshot(results):
    # Explicit schema: never forward response headers, arbitrary fields or error bodies.
    values = {name: number(results.get(path, {}).get('percent'), percent=True)
              for name, path in [('cpu', 'cpu'), ('ram', 'ram'), ('disk', 'disk')]}
    for path in ('ram', 'disk'):
        for field in ('total', 'used', 'free', 'available'):
            values[path + '_' + field] = number(results.get(path, {}).get(field))
    for field in ('sent', 'recv'):
        value = results.get('net', {}).get(field)
        values['net_' + field] = str(value) if type(value) is int and 0 <= value < 2**64 else None
    system = results.get('system', {})
    identity = section(system, 'identity') or {}
    values['hostname'] = identity.get('hostname', '')[:255] if isinstance(identity.get('hostname'), str) else ''
    boot = section(system, 'boot') or {}
    values['boot_time'] = number(boot.get('boot_time'))
    values['uptime_seconds'] = number(boot.get('uptime_seconds'))
    load = section(system, 'load') or {}
    values['load'] = {k: number(load.get(k)) for k in ('avg1', 'avg5', 'avg15')}
    # PSI and interface counters are flattened into a bounded, numeric table.
    details = []
    for resource in ('cpu', 'memory', 'io'):
        pressure = section(results.get('pressure', {}), resource) or {}
        if not isinstance(pressure, dict):
            continue
        for level in ('some', 'full'):
            for field, value in (pressure.get(level, {}) or {}).items():
                if field in ('avg10', 'avg60', 'avg300', 'total_us') and number(value) is not None:
                    details.append([f'psi.{resource}.{level}.{field}', str(value)])
    interfaces = results.get('interfaces', {})
    counters = section(interfaces, 'counters') or {}
    links = section(interfaces, 'links') or {}
    if isinstance(counters, dict):
        for name, fields in list(counters.items())[:128]:
            if not isinstance(fields, dict):
                continue
            for field in ('bytes_sent', 'bytes_recv', 'packets_sent', 'packets_recv', 'errin', 'errout', 'dropin', 'dropout'):
                value = fields.get(field)
                if type(value) is int and 0 <= value < 2**64:
                    details.append([f'{name[:100]}.{field}', str(value)])
            link = links.get(name, {}) if isinstance(links, dict) else {}
            if isinstance(link, dict):
                for field in ('mtu', 'speed_mbps'):
                    if number(link.get(field)) is not None:
                        details.append([f'{name[:100]}.{field}', str(link[field])])
    filesystems = section(results.get('filesystems', {}), 'filesystems') or []
    if isinstance(filesystems, list):
        for fs in filesystems[:128]:
            if not isinstance(fs, dict) or not isinstance(fs.get('mountpoint'), str):
                continue
            for metric in ('usage', 'inodes'):
                usage = section(fs, metric) or {}
                if isinstance(usage, dict):
                    for field in ('total', 'used', 'free', 'percent'):
                        if number(usage.get(field)) is not None:
                            details.append([f"{fs['mountpoint'][:100]}.{metric}.{field}", str(usage[field])])
    values['details'] = details[:128]
    values['details_truncated'] = len(details) > 128
    return values


class Syspiper:
    def __init__(self, *, key='', nodes=(), port=8181, interval=30, history=None, fetch_fn=fetch):
        self.key, self.explicit, self.port, self.interval = key, nodes, port, interval
        self.history, self.fetch = history, fetch_fn
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.wake = threading.Event()
        self.endpoints = []
        self.thread = None
        self.targets, self.samples, self.baselines, self.unsupported = {}, {}, {}, {}
        self.skipped = 0
        self.discovery_error = None
        self.history_error = False

    def update(self, endpoints):
        if not self.key:
            return
        with self.lock:
            self.endpoints = list(endpoints)

    def discover(self):
        addresses, error = interface_addresses()
        with self.lock:
            found = candidates(self.endpoints, addresses, self.explicit, os.readlink('/proc/self/ns/net'))
            self.targets = dict(list(found.items())[:MAX_NODES])
            self.skipped = max(0, len(found)-MAX_NODES)
            self.discovery_error = error

    def start(self):
        if self.key:
            self.thread = threading.Thread(target=self._poll, name='fabric-syspiper', daemon=True)
            self.thread.start()

    def close(self):
        self.stop.set()
        self.wake.set()
        if self.thread:
            self.thread.join()

    def snapshot(self):
        with self.lock:
            return {'enabled': bool(self.key), 'interval_seconds': self.interval,
                    'port': self.port, 'skipped': self.skipped, 'discovery_error': self.discovery_error,
                    'history_error': self.history_error,
                    'nodes': [{**target, 'id': self.identity(ip), **self.samples.get(ip, {'status':'pending', 'values':{}, 'errors':{}})}
                              for ip, target in self.targets.items()]}

    def identity(self, ip):
        return f'syspiper:{ip}:{self.port}'

    def sample(self, ip):
        results, errors = {}, {}
        for path in PATHS:
            if self.stop.is_set():
                return
            if self.unsupported.get((ip, path), 0) > time.monotonic():
                errors[path] = 'unsupported'
                continue
            try:
                results[path] = self.fetch(ip, self.port, path, self.key)
            except ReadError as error:
                errors[path] = error.code
                if error.code == 'unsupported':
                    self.unsupported[ip, path] = time.monotonic() + 600
                elif error.code in ('unauthorized', 'unreachable', 'timeout'):
                    break
        try:
            values = clean_snapshot(results)
        except (ValueError, TypeError, AttributeError, OverflowError):
            values, errors = {}, {'response': 'invalid_response'}
        for path in ('cpu', 'ram', 'disk'):
            if path in results and values.get(path) is None:
                errors[path] = 'invalid_response'
        if 'net' in results and (values.get('net_sent') is None or values.get('net_recv') is None):
            errors['net'] = 'invalid_response'
        sampled = time.time()
        tick = time.monotonic()
        previous = self.baselines.pop(ip, None)
        rx = tx = None
        if values.get('net_sent') is not None and values.get('net_recv') is not None:
            if previous:
                old_tick, old = previous
                elapsed = tick-old_tick
                if 0 < elapsed <= self.interval*3 and old.get('boot_time') == values.get('boot_time'):
                    recv = int(values['net_recv'])-int(old['net_recv'])
                    sent = int(values['net_sent'])-int(old['net_sent'])
                    if recv >= 0 and sent >= 0:
                        rx, tx = recv*8/elapsed, sent*8/elapsed
            self.baselines[ip] = (tick, values)
        point = {'time': int(sampled*1000), 'rx': rx, 'tx': tx,
                 **{k: values.get(k) for k in ('cpu', 'ram', 'disk')}}
        if errors and any(v != 'unsupported' for v in errors.values()):
            point['issues'] = [{'key':'syspiper', 'code':'syspiper_'+code} for code in sorted(set(errors.values())-{'unsupported'})]
        sample = {'status': 'unavailable' if not results or not values else 'partial' if errors or any(v.get('status')=='partial' for v in results.values()) else 'ok',
                  'sampled_at': datetime.fromtimestamp(sampled, timezone.utc).isoformat(),
                  'values': values, 'errors': errors, 'chart': point}
        with self.lock:
            self.samples[ip] = sample
        if self.history:
            try:
                self.history.record_point(self.identity(ip), point, values)
                self.history_error = False
            except (OSError, sqlite3.Error, ValueError):
                self.history_error = True

    def _poll(self):
        with ThreadPoolExecutor(max_workers=4, thread_name_prefix='syspiper-probe') as pool:
            while not self.stop.is_set():
                started = time.monotonic()
                self.wake.clear()
                self.discover()
                with self.lock:
                    targets = list(self.targets)
                for mapping in (self.samples, self.baselines):
                    with self.lock:
                        for ip in list(mapping):
                            if ip not in targets:
                                del mapping[ip]
                for key in list(self.unsupported):
                    if key[0] not in targets:
                        del self.unsupported[key]
                list(pool.map(self.sample, targets))
                self.wake.wait(max(1, self.interval-(time.monotonic()-started)))
