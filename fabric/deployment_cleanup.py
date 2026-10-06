#!/usr/bin/env python3
"""Standalone, deployment-scoped teardown; also included in undeploy runbooks."""
from pathlib import Path
import fcntl
import os
import select
import shutil
import signal
import stat
import sys
import time


def erase_secret(path):
    """Overwrite the live file, sync, unlink. Not a storage-level erase guarantee."""
    path = Path(path)
    try: fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
    except FileNotFoundError: return
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise RuntimeError('refusing to erase a non-regular or hard-linked secret')
        remaining = info.st_size
        while remaining:
            block = os.urandom(min(65536, remaining))
            written = os.write(fd, block); remaining -= written
        os.fsync(fd)
        os.ftruncate(fd, 0); os.fsync(fd)
    finally: os.close(fd)
    path.unlink()


def matches_process(directory, root, role, members):
    try:
        executable = os.readlink(directory / 'exe').removesuffix(' (deleted)')
        args = (directory / 'cmdline').read_bytes().decode().split('\0')
        if executable == str(root / 'bin/tuntom-divert-adapter') and '--control-socket' in args:
            control = args[args.index('--control-socket') + 1]
            return control in {str(root / 'run' / f'via-{i}.control') for i in range(16)}
        return (executable == str(root / 'bin/tuntom') and len(args) > 3 and args[1] == role
                and args[2] in members and '--control-socket' in args
                and args[args.index('--control-socket') + 1] == str(root / 'run' / (args[2] + ('c' if role == 'client' else 's') + '.control')))
    except (FileNotFoundError, ProcessLookupError): return False


SYSTEMD_UNITS = Path('/etc/systemd/system')


def systemd_prefix(ident):
    return 'tuntomfabric_' + '_'.join(ident[i:i+8] for i in range(0, len(ident), 8))


def systemd_manifest(root, ident):
    import hashlib
    import json
    import re
    path = root / 'systemd/manifest.json'
    if path.is_symlink() or (root / 'systemd').is_symlink(): raise RuntimeError('invalid systemd manifest')
    data = json.loads(path.read_text())
    prefix = systemd_prefix(ident)
    if data.get('deployment_id') != ident or not re.fullmatch('[0-9a-f]{32}', ident):
        raise RuntimeError('systemd deployment identity mismatch')
    units = data.get('units', {})
    if not isinstance(units, dict) or not 3 <= len(units) <= 83:
        raise RuntimeError('invalid systemd unit manifest')
    for name, digest in units.items():
        if not re.fullmatch(re.escape(prefix) + r'(?:_[A-Za-z0-9_]+)?\.(?:service|slice|target)', name):
            raise RuntimeError('unit is outside this deployment')
        source = root / 'systemd' / name
        if source.is_symlink() or not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise RuntimeError('generated systemd unit no longer matches its manifest: ' + name)
    if data.get('target') not in units or not data['target'].endswith('.target') or data.get('slice') not in units or not data['slice'].endswith('.slice'):
        raise RuntimeError('invalid systemd target or slice')
    return data


def systemctl(*args, check=True):
    import subprocess
    result = subprocess.run(['systemctl', '--no-pager', *args], capture_output=True, text=True, timeout=90)
    if check and result.returncode:
        raise RuntimeError('systemctl ' + args[0] + ' failed (exit ' + str(result.returncode) + '): ' + (result.stderr or result.stdout)[-2048:])
    return result


def check_installed_units(data):
    import hashlib
    for name, digest in data['units'].items():
        dest = SYSTEMD_UNITS / name
        if dest.is_symlink() or (dest.exists() and (not dest.is_file() or hashlib.sha256(dest.read_bytes()).hexdigest() != digest)):
            raise RuntimeError('refusing to replace/remove a changed or foreign unit: ' + name)
        if (SYSTEMD_UNITS / (name + '.d')).exists() or (SYSTEMD_UNITS / (name + '.d')).is_symlink():
            raise RuntimeError('unit has external overrides; retain deployment for review: ' + name)
        loaded = systemctl('show', name, '--property=FragmentPath', '--property=DropInPaths', '--property=LoadState', check=False)
        values = dict(line.split('=', 1) for line in loaded.stdout.splitlines() if '=' in line)
        if loaded.returncode and values.get('LoadState') != 'not-found':
            raise RuntimeError('cannot inspect systemd unit: ' + name)
        if values.get('FragmentPath') not in ('', None, str(dest)) or values.get('DropInPaths'):
            raise RuntimeError('unit has a foreign fragment or overrides: ' + name)
    enabled = SYSTEMD_UNITS / 'multi-user.target.wants' / data['target']
    if enabled.exists() or enabled.is_symlink():
        if not enabled.is_symlink() or enabled.resolve() != (SYSTEMD_UNITS / data['target']).resolve():
            raise RuntimeError('autostart link belongs to a different unit')


def systemd_stop(data):
    # Disable before stopping; a partial teardown must not restart after reboot.
    if (SYSTEMD_UNITS / data['target']).exists():
        systemctl('disable', data['target'])
    link = SYSTEMD_UNITS / 'multi-user.target.wants' / data['target']
    if link.is_symlink(): link.unlink()
    names = [data['target']] + [n for n in data['units'] if n.endswith('.service')] + [data['slice']]
    for name in names:
        result = systemctl('show', name, '--property=LoadState', '--value', check=False)
        if result.returncode and result.stdout.strip() != 'not-found': raise RuntimeError('cannot inspect systemd unit: ' + name)
        if result.stdout.strip() != 'not-found': systemctl('stop', name)
    for name in names:
        result = systemctl('show', name, '--property=LoadState', '--property=ActiveState', '--property=MainPID', '--property=ControlPID', check=False)
        values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
        if values.get('LoadState') == 'not-found': continue
        if result.returncode: raise RuntimeError('cannot verify stopped systemd unit: ' + name)
        if values.get('ActiveState') not in ('inactive', 'failed') or values.get('MainPID', '0') != '0' or values.get('ControlPID', '0') != '0':
            raise RuntimeError('unit has not stopped; files and keys retained: ' + name)


def systemd_action(root, ident, action):
    import subprocess
    import tempfile
    root = Path(root)
    marker = root / '.fabric-deployment-id'
    if root.is_symlink() or marker.is_symlink() or marker.read_text().strip() != ident:
        raise RuntimeError('deployment identity no longer matches')
    data = systemd_manifest(root, ident)
    if action == 'remove' and not Path('/run/systemd/system').is_dir() and not any((SYSTEMD_UNITS / n).exists() or (SYSTEMD_UNITS / n).is_symlink() for n in data['units']):
        return 'SYSTEMD_REMOVE_OK: no units installed'
    check_installed_units(data)
    if action == 'install':
        SYSTEMD_UNITS.mkdir(parents=True, exist_ok=True)
        for name in data['units']:
            dest = SYSTEMD_UNITS / name
            if dest.exists(): continue
            fd, temporary = tempfile.mkstemp(prefix='.fabric-unit-', dir=SYSTEMD_UNITS)
            try:
                with os.fdopen(fd, 'wb') as stream:
                    stream.write((root / 'systemd' / name).read_bytes()); stream.flush(); os.fsync(stream.fileno())
                os.chmod(temporary, 0o644)
                # Link is exclusive: do not overwrite a unit created since preflight.
                os.link(temporary, dest)
            finally: os.unlink(temporary)
        systemctl('daemon-reload')
    elif action == 'start':
        systemctl('start', data['target'])
    elif action == 'enable':
        systemctl('enable', data['target'])
    elif action == 'stop':
        systemd_stop(data)
    elif action == 'remove':
        systemd_stop(data)
        # Recheck ownership immediately before removing any installed fragment.
        check_installed_units(data)
        for name in data['units']:
            (SYSTEMD_UNITS / name).unlink(missing_ok=True)
        systemctl('daemon-reload')
        systemctl('reset-failed', *data['units'], check=False)
    elif action == 'status':
        result = systemctl('show', *data['units'], '--property=Id', '--property=LoadState', '--property=ActiveState', '--property=SubState', '--property=MainPID', '--property=Result', '--property=ExecMainStatus', '--property=UnitFileState', '--property=NRestarts')
        print(result.stdout)
        args = ['journalctl', '--no-pager', '--lines=80', '--output=short-iso']
        for name in data['units']:
            if name.endswith('.service'): args += ['--unit', name]
        result = subprocess.run(args, capture_output=True, text=True, timeout=20)
        print(result.stdout[-16384:] if result.returncode == 0 else 'Journal unavailable: ' + result.stderr[-2048:])
    else:
        raise ValueError('unknown systemd operation')
    return 'SYSTEMD_' + action.upper() + '_OK'


def cleanup_systemd(root, ident):
    if (root / 'systemd/manifest.json').exists():
        systemd_action(root, ident, 'remove')
    elif list(SYSTEMD_UNITS.glob(systemd_prefix(ident) + '*')):
        raise RuntimeError('systemd units remain but their ownership manifest is missing; retain deployment')


def undeploy(root, ident, role, members):
    root = Path(root)
    if not root.exists() and not root.is_symlink():
        cleanup_systemd(root, ident)
        return
    if root.is_symlink() or not root.is_dir(): raise RuntimeError('invalid deployment directory')
    marker = root / '.fabric-deployment-id'
    if marker.is_symlink() or marker.read_text().strip() != ident:
        raise RuntimeError('deployment identity no longer matches')
    if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise RuntimeError('safe undeploy requires Linux pidfd support')
    cleanup_systemd(root, ident)
    handles = []
    locks = []
    try:
        for directory in Path('/proc').iterdir():
            if not directory.name.isdigit(): continue
            try: fd = os.pidfd_open(int(directory.name))
            except ProcessLookupError: continue
            try:
                if matches_process(directory, root, role, members):
                    signal.pidfd_send_signal(fd, signal.SIGTERM)
                    handles.append(fd); fd = None
            finally:
                if fd is not None: os.close(fd)
        deadline = time.monotonic() + 20
        pending = list(handles)
        while pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0: raise RuntimeError('tunnel did not stop; files and secrets were retained')
            ready, _, _ = select.select(pending, [], [], remaining)
            pending = [fd for fd in pending if fd not in ready]
        # Refuse cleanup while any generated runner still owns its instance lock.
        suffix = 'c' if role == 'client' else 's'
        for member in members:
            path = root / 'run' / (member + suffix + '.lock')
            if path.exists():
                fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW); locks.append(fd)
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for path in (root / 'run').glob('via-*.lock'):
            fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW); locks.append(fd)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        secret_dir = root / 'secrets'
        if secret_dir.is_symlink(): raise RuntimeError('invalid secrets directory')
        if secret_dir.exists():
            for path in secret_dir.iterdir(): erase_secret(path)
        # rmtree does not follow symlinks; only this marker-verified deployment is removed.
        shutil.rmtree(root)
    finally:
        for fd in handles + locks: os.close(fd)


if __name__ == '__main__':
    if sys.argv[1] == 'systemd':
        print(systemd_action(sys.argv[3], sys.argv[4], sys.argv[2]))
    else:
        undeploy(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
        print('UNDEPLOYED: processes stopped; deployment files and secrets removed')
