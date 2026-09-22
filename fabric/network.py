"""Bounded CONTROL discovery and polling, independent of local /proc sampling."""
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
from dataclasses import asdict
from datetime import datetime, timezone
import re
import json
import threading
import time
from urllib.parse import unquote

from control import encode_route
from discovery import Endpoint
from errors import APIError
from telemetry import changes, health, switch_detail


def stamp():
    return datetime.now(timezone.utc).isoformat(timespec='milliseconds')


def parse_discovery(text):
    if not isinstance(text, str) or len(text.encode()) > 1024 * 1024:
        raise ValueError('discovery response exceeds limit')
    lines = text.splitlines()
    if not lines or lines.pop(0) != 'path\tstate\tinstance\tcomponent\tcapabilities' or len(lines) > 1024:
        raise ValueError('invalid discovery header or row limit')
    result = []
    for line in lines:
        fields = line.split('\t')
        if len(fields) != 5:
            raise ValueError('invalid discovery row')
        path, state, instance, component, capabilities = fields
        if state == 'NO_RESPONSE':
            continue
        if state not in {'FOUND', 'ALT_PATH'} or not re.fullmatch('[0-9a-f]{32}', instance):
            raise ValueError('invalid discovery identity')
        route = []
        if path != 'self':
            for part in path.split('/'):
                if part == 'peer':
                    route.append({'peer': True})
                elif part.startswith('port:') and not re.search(r'%(?![0-9a-fA-F]{2})', part):
                    route.append({'port': unquote(part[5:], errors='strict')})
                else:
                    raise ValueError('invalid discovery path')
        encode_route(route)
        if not re.fullmatch('[a-zA-Z0-9_-]{1,64}', component):
            raise ValueError('invalid discovery component')
        result.append((instance, component, path, route))
    return result


class Network:
    def __init__(self, probe, parse_stats, history=None, interval=5, discovery_interval=120):
        self.probe, self.parse_stats, self.history = probe, parse_stats, history
        self.interval, self.discovery_interval = interval, discovery_interval
        self.lock = threading.RLock()
        self.wake = threading.Event()
        self.stop = threading.Event()
        self.thread = None
        self.nodes, self.origins, self.local = {}, {}, {}
        self.local_instances = {}
        self.next_discovery, self.discovery_status = {}, {}
        self.active = set()
        self.origin_active = {}
        self.limit = 256
        self.truncated = False
        self.history_error = None

    def start(self):
        self.thread = threading.Thread(target=self._run, name='fabric-network', daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.wake.set()
        if self.thread:
            self.thread.join()

    def refresh(self):
        with self.lock:
            self.next_discovery.clear()
        self.wake.set()

    def update(self, endpoints, samples):
        with self.lock:
            self.local = {e.id: e for e in endpoints}
            self.origins = {e.id: e for e in endpoints if e.control and
                samples.get(e.id, {}).get('status') == 'reachable' and all(
                samples[e.id]['metrics'].get(k) == '1' for k in
                ('control_enabled', 'control_discover_enabled', 'control_can_initiate'))}
            # Bound origin work as well as retained bookkeeping.
            self.origins = dict(sorted(self.origins.items(), key=lambda item:item[1].kind != "switch")[:8])
            self.next_discovery = {k:v for k,v in self.next_discovery.items() if k in self.origins}
            self.discovery_status = {k:v for k,v in self.discovery_status.items() if k in self.origins}
            self.local_instances = {k:v for k,v in self.local_instances.items() if k in self.origins}
        self.wake.set()

    def ingest(self, origin, text):
        with self.lock:
            if origin not in self.origins:
                return
        rows = parse_discovery(text)
        tick, at = time.monotonic(), stamp()
        with self.lock:
            if origin not in self.origins:
                return
            entry = self.origins[origin]
            local_ids = set()
            # Match only direct attachments in the same known local namespace.
            for instance, component, path, route in rows:
                if not route:
                    local_ids.add(instance)
                elif entry.kind == 'switch' and len(route) == 1 and 'port' in route[0]:
                    matches = [e for e in self.local.values() if e.switch_socket and
                        e.switch_socket == entry.switch_socket and e.port_id == route[0]['port'] and
                        e.mount_namespace and e.mount_namespace == entry.mount_namespace]
                    if len(matches) == 1:
                        local_ids.add(instance)
            self.local_instances[origin] = local_ids
            all_local = set().union(*self.local_instances.values())
            for instance in all_local:
                self.nodes.pop(instance, None)
            for instance, component, path, route in rows:
                if instance in all_local or not route:
                    continue
                if instance not in self.nodes:
                    if len(self.nodes) >= self.limit:
                        self.truncated = True
                        continue
                    kind = {'tunnel':'tunnel', 'switch':'switch', 'switch-mp':'switch',
                            'adapter':'adapter', 'divert-adapter':'divert'}.get(component, 'process')
                    name = next((hop['port'] for hop in reversed(route) if 'port' in hop), component)
                    if route[-1] == {'peer': True}:
                        name += ' / peer'
                    endpoint = Endpoint('control:' + instance, None, 0, '', kind, name, '', None,
                                        '', None, None, None, source='discovered', instance=instance)
                    self.nodes[instance] = {'endpoint': endpoint, 'routes': {}, 'next': 0,
                        'failures': 0, 'baseline': None, 'seen': tick, 'last_seen': at,
                        'success': 0, 'sample_bytes':0, 'sample': {'status':'pending','sampled_at':None,'metrics':{},'error':None}}
                node = self.nodes[instance]
                node['seen'], node['last_seen'] = tick, at
                token = (origin, encode_route(route))
                # Never bind an old instance to a path now announcing a new one.
                for old_id, old in self.nodes.items():
                    if old_id != instance:
                        old['routes'].pop(token, None)
                if token in node['routes'] or len(node['routes']) < 8:
                    node['routes'][token] = {'origin':origin,'path':path,'route':route,'seen':tick}
            self.discovery_status[origin] = {'status':'ok','sampled_at':at,'responses':len(rows)}
        self.wake.set()

    def _reserve(self, key, origin):
        if key in self.active or len(self.active) >= 16 or self.origin_active.get(origin, 0) >= 4:
            return False
        self.active.add(key)
        self.origin_active[origin] = self.origin_active.get(origin, 0) + 1
        return True

    def _release(self, key, origin):
        with self.lock:
            self.active.discard(key)
            count = self.origin_active.get(origin, 1) - 1
            if count:
                self.origin_active[origin] = count
            else:
                self.origin_active.pop(origin, None)
        self.wake.set()

    def _routes(self, node):
        return [(token, route) for token, route in node['routes'].items()
                if route['origin'] in self.origins and time.monotonic() - route['seen'] < 600]

    def _discover(self, origin):
        try:
            self.ingest(origin, self.probe(origin, 'discover', []))
        except Exception as error:
            with self.lock:
                self.discovery_status[origin] = {'status':'unavailable','sampled_at':stamp(),'error':str(error)}
        finally:
            self._release(('discovery', origin), origin)

    def _poll_node(self, instance, token, route):
        try:
            metrics = self.parse_stats(self.probe(route['origin'], 'stats', route['route']))
            sample = {'status':'reachable','metrics':metrics,'error':None,'sampled_at':stamp()}
        except Exception as error:
            sample = {'status':'unavailable','metrics':{},'error':str(error),'sampled_at':stamp()}
        try:
            with self.lock:
                node = self.nodes.get(instance)
                # A concurrent discovery may have rebound this path; discard its result.
                if node is None or token not in node['routes']:
                    return
                tick = time.monotonic()
                size = len(json.dumps(sample['metrics']).encode())
                other_bytes = sum(n['sample_bytes'] for n in self.nodes.values()) - node['sample_bytes']
                if size > 65536 or other_bytes + size > 2 * 1024 * 1024:
                    sample.update(status='unavailable', metrics={}, error='network metrics cache limit exceeded')
                    size = 0
                node['sample_bytes'] = size
                previous = node['baseline']
                elapsed = tick - previous[0] if previous else None
                sample['changes'] = changes(sample['metrics'], previous[1] if previous and elapsed <= 15 else None,
                                             elapsed if previous else None)
                node['sample'] = sample
                if sample['status'] == 'reachable':
                    node['baseline'] = (tick, sample['metrics'])
                    node['failures'], node['success'] = 0, tick
                    node['preferred'] = token
                else:
                    node['baseline'] = None
                    node['failures'] += 1
                    # Rotate to an alternative on the next attempt, without duplicate requests.
                    failed = node['routes'].pop(token)
                    node['routes'][token] = failed
                    node.pop('preferred', None)
                delay = min(120, self.interval * 2 ** min(node['failures'], 5))
                node['next'] = tick + delay
                endpoint = node['endpoint']
            if self.history:
                try:
                    self.history.record(endpoint, sample)
                    self.history_error = None
                except Exception:
                    self.history_error = 'network history cache write failed'
        finally:
            self._release(instance, route['origin'])

    def _run(self):
        with ThreadPoolExecutor(max_workers=16, thread_name_prefix='fabric-control') as pool:
            while not self.stop.is_set():
                self.wake.clear()
                tick = time.monotonic()
                with self.lock:
                    for origin in self.origins:
                        if tick >= self.next_discovery.get(origin, 0) and self._reserve(('discovery',origin), origin):
                            self.next_discovery[origin] = tick + self.discovery_interval
                            pool.submit(self._discover, origin)
                    for instance, node in sorted(self.nodes.items(), key=lambda item:item[1]["next"]):
                        if tick - max(node['seen'], node['success']) > 600 and instance not in self.active:
                            del self.nodes[instance]
                            continue
                        if tick < node['next']:
                            continue
                        routes = self._routes(node)
                        preferred = node.get('preferred')
                        routes.sort(key=lambda item:item[0] != preferred)
                        for token, route in routes:
                            if self._reserve(instance, route['origin']):
                                pool.submit(self._poll_node, instance, token, deepcopy(route))
                                break
                self.wake.wait(.5)

    def endpoint(self, key):
        with self.lock:
            node = self.nodes.get(key.removeprefix('control:'))
            if not node:
                raise APIError(404, 'discovered component expired; refresh discovery')
            return node['endpoint']

    def read(self, key, operation, body=""):
        if operation not in {'stats', 'flows', 'show', 'classifier-show', 'classifier-check', 'classifier-load', 'classifier-load-flush', 'classifier-disable'}:
            raise APIError(403, 'discovered components support read-only operations')
        instance = key.removeprefix('control:')
        with self.lock:
            node = self.nodes.get(instance)
            if not node:
                raise APIError(404, 'discovered component expired')
            routes = self._routes(node)
            if not routes:
                raise APIError(503, 'no current CONTROL route')
            routes.sort(key=lambda item:item[0] != node.get('preferred'))
            token, route = routes[0]
            route = deepcopy(route)
            if not self._reserve(instance, route['origin']):
                raise APIError(429, 'CONTROL request already running; retry shortly')
        try:
            result = self.probe(route['origin'], operation, route['route'], body) if body else self.probe(route['origin'], operation, route['route'])
            with self.lock:
                if instance not in self.nodes or token not in self.nodes[instance]['routes']:
                    raise APIError(409, 'CONTROL route changed during request')
            return result
        finally:
            self._release(instance, route['origin'])

    def snapshot(self):
        with self.lock:
            tick = time.monotonic()
            endpoints = []
            for node in self.nodes.values():
                endpoint, sample = node['endpoint'], deepcopy(node['sample'])
                stale = tick - node['seen'] > self.discovery_interval * 2
                if sample['status'] == 'reachable' and (not node['success'] or tick - node['success'] > 15):
                    sample.update(status='unavailable', metrics={}, error='CONTROL sample is stale')
                endpoints.append({**asdict(endpoint), **sample, 'discovery_stale':stale,
                    'discovered_at':node['last_seen'],
                    'control_routes':[{'origin':r['origin'],'path':r['path']} for r in node['routes'].values()],
                    'health':health(endpoint, sample),
                    'switch_detail':switch_detail(sample['metrics'],sample.get('changes',{})) if endpoint.kind=='switch' else None})
            return endpoints, {'interval_seconds':self.discovery_interval,'origins':deepcopy(self.discovery_status),
                               'truncated':self.truncated,'limit':self.limit,'history_error':self.history_error}
