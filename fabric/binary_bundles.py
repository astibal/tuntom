"""Immutable, explicitly uploaded tunnel binaries; never execute uploads on Fabric."""
import base64
import hashlib
import io
import json
import re
import secrets
import sqlite3
import tarfile
import threading
from controlled_endpoints import SUPPORTED_PLATFORMS, timestamp

MAX_BINARY = 32 * 1024 * 1024


class BinaryBundles:
    def __init__(self, path):
        self.lock = threading.RLock()
        self.build_lock = threading.Lock()
        self.db = sqlite3.connect(str(path) if path is not None else ':memory:', check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute('CREATE TABLE IF NOT EXISTS binary_bundle(id TEXT PRIMARY KEY, metadata TEXT NOT NULL, tuntom BLOB NOT NULL, tuntomctl BLOB NOT NULL)')
        self.db.execute('CREATE TABLE IF NOT EXISTS binary_bundle_extra(id TEXT PRIMARY KEY, divert BLOB NOT NULL)')
        self.db.commit()

    def close(self):
        with self.lock: self.db.close()

    def list(self):
        with self.lock:
            return {'binary_bundles': [json.loads(row[0]) for row in self.db.execute('SELECT metadata FROM binary_bundle ORDER BY rowid DESC')]}

    def get(self, ident, files=False):
        with self.lock:
            row = self.db.execute('SELECT * FROM binary_bundle WHERE id=?', (ident,)).fetchone()
        if row is None: raise KeyError('unknown binary bundle')
        metadata = json.loads(row['metadata'])
        if not files: return metadata
        content = {name: bytes(row[name]) for name in ('tuntom', 'tuntomctl')}
        with self.lock:
            extra = self.db.execute('SELECT divert FROM binary_bundle_extra WHERE id=?', (ident,)).fetchone()
        if extra: content['tuntom-divert-adapter'] = bytes(extra[0])
        return metadata, content

    def create(self, data):
        if not isinstance(data, dict) or set(data) != {'name', 'revision', 'os', 'version', 'architecture', 'files'}:
            raise ValueError('expected bundle name, revision, platform and both binaries')
        for key in ('name', 'revision'):
            if not isinstance(data[key], str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._/-]{0,95}', data[key]):
                raise ValueError('invalid ' + key)
        platform = tuple(data[key] for key in ('os', 'version', 'architecture'))
        if not all(isinstance(value, str) for value in platform) or platform not in SUPPORTED_PLATFORMS:
            raise ValueError('unsupported target platform')
        if not isinstance(data['files'], dict) or set(data['files']) not in ({'tuntom', 'tuntomctl'}, {'tuntom', 'tuntomctl', 'tuntom-divert-adapter'}):
            raise ValueError('both tuntom and tuntomctl are required')
        files = {}
        for name, encoded in data['files'].items():
            if not isinstance(encoded, str) or len(encoded) > (MAX_BINARY + 2) // 3 * 4:
                raise ValueError('binary too large (32 MiB limit per file)')
            try: content = base64.b64decode(encoded, validate=True)
            except ValueError as error: raise ValueError('invalid binary encoding') from error
            # ELF64 little endian x86-64, executable or PIE/shared-object format.
            if len(content) < 64 or len(content) > MAX_BINARY or content[:6] != b'\x7fELF\x02\x01' or int.from_bytes(content[18:20], 'little') != 62 or int.from_bytes(content[16:18], 'little') not in (2, 3):
                raise ValueError(name + ' must be an x86-64 Linux ELF executable')
            files[name] = content
        metadata = {key: data[key] for key in ('name', 'revision', 'os', 'version', 'architecture')}
        metadata.update(id=secrets.token_hex(16), created_at=timestamp(), component='divert' if 'tuntom-divert-adapter' in files else 'tunnel', compatibility='declared',
            files={name: {'sha256': hashlib.sha256(content).hexdigest(), 'size': len(content)} for name, content in files.items()})
        with self.lock, self.db:
            self.db.execute('INSERT INTO binary_bundle VALUES(?,?,?,?)', (metadata['id'], json.dumps(metadata), files['tuntom'], files['tuntomctl']))
            if 'tuntom-divert-adapter' in files:
                self.db.execute('INSERT INTO binary_bundle_extra VALUES(?,?)', (metadata['id'], files['tuntom-divert-adapter']))
        return metadata

    def compatible(self, ident, system):
        metadata = self.get(ident)
        if any(metadata[key] != system.get(key) for key in ('os', 'version', 'architecture')):
            raise ValueError('binary bundle platform does not match endpoint discovery')
        return metadata

    def archive(self, ident):
        metadata, files = self.get(ident, files=True)
        files['bundle.json'] = json.dumps(metadata, indent=2).encode()
        files['manifest.sha256'] = ''.join(f'{hashlib.sha256(content).hexdigest()}  {name}\n' for name, content in files.items()).encode()
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz') as archive:
            for name, content in files.items():
                info = tarfile.TarInfo(name); info.size = len(content); info.mode = 0o755 if name in ('tuntom', 'tuntomctl', 'tuntom-divert-adapter') else 0o644
                archive.addfile(info, io.BytesIO(content))
        return 'binary_bundle_' + ident + '.tar.gz', output.getvalue()

    def build(self, data, endpoints, git_url):
        import shlex
        if not isinstance(data, dict) or set(data) not in ({'name', 'revision', 'endpoint_id'}, {'name', 'revision', 'endpoint_id', 'component'}):
            raise ValueError('expected name, revision and build endpoint')
        if not isinstance(data['name'], str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._/-]{0,95}', data['name']):
            raise ValueError('invalid bundle name')
        revision = data['revision']
        if not isinstance(revision, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._/-]{0,127}', revision) or '..' in revision:
            raise ValueError('invalid Git revision')
        component = data.get('component', 'tunnel')
        if component not in ('tunnel', 'divert'): raise ValueError('invalid component')
        endpoint = endpoints.get(data['endpoint_id'])
        system = (endpoint.get('snapshot') or {}).get('system', {})
        if endpoint['status'] != 'supported' or tuple(system.get(key) for key in ('os', 'version', 'architecture')) not in SUPPORTED_PLATFORMS:
            raise ValueError('build requires a supported discovered SSH endpoint')
        # Dedicated temporary tree. No installation or changes to running deployments.
        program = '''set -euo pipefail
command -v git >/dev/null
command -v g++ >/dev/null
command -v python3 >/dev/null
work=$(mktemp -d /tmp/fabric-bundle.XXXXXXXX)
trap 'rm -rf -- "$work"' EXIT
cd "$work"
git clone --quiet --no-checkout -- REPO source >&2
cd source
git checkout --quiet --detach REV >&2
g++ -std=c++17 -pthread -O2 -march=x86-64 -mtune=generic src/main.cpp -o ../tuntom >&2
g++ -std=c++17 -pthread -O2 -march=x86-64 -mtune=generic src/control/main.cpp -o ../tuntomctl >&2
python3 - <<'FABRIC_RESULT'
import base64, json, pathlib, platform, subprocess
system = dict(line.split('=', 1) for line in pathlib.Path('/etc/os-release').read_text().splitlines() if '=' in line)
files = {}
for name in ('tuntom', 'tuntomctl'):
    path = pathlib.Path('..') / name
    if path.stat().st_size > 32 * 1024 * 1024: raise ValueError('binary exceeds 32 MiB')
    files[name] = base64.b64encode(path.read_bytes()).decode()
print(json.dumps({'revision': subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip(),
    'os': system['ID'].strip('"'), 'version': system['VERSION_ID'].strip('"'), 'architecture': platform.machine(), 'files': files}))
FABRIC_RESULT
'''.replace('REPO', shlex.quote(git_url)).replace('REV', shlex.quote(revision))
        if component == 'divert':
            program = program.replace("python3 - <<'FABRIC_RESULT'", "g++ -std=c++17 -pthread -O2 -march=x86-64 -mtune=generic src/divert/main.cpp -o ../tuntom-divert-adapter >&2\npython3 - <<'FABRIC_RESULT'")
            program = program.replace("('tuntom', 'tuntomctl'):", "('tuntom', 'tuntomctl', 'tuntom-divert-adapter'):")
        # One build at a time on this Fabric instance, independent of inventory reads.
        if not self.build_lock.acquire(blocking=False): raise RuntimeError('another binary bundle build is running')
        try:
            output = endpoints.run(endpoint['id'], 'bash -s', program.encode(), timeout=900)
            result = json.loads(output)
            if component == 'divert' and 'tuntom-divert-adapter' not in result.get('files', {}):
                raise ValueError('build result lacks divert adapter')
            if any(result.get(key) != system.get(key) for key in ('os', 'version', 'architecture')):
                raise ValueError('build host platform changed; refresh discovery')
            result['name'] = data['name']
            return self.create(result)
        finally:
            self.build_lock.release()
