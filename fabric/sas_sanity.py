"""Read-only origin-host probe, sent over verified SSH; never executes payload binaries."""
import grp
import json
import os
from pathlib import Path
import platform
import pwd
import shutil
import socket
import stat
import sys


def inspect(config):
    origin = config['origin'].rstrip('.').lower()
    if origin not in {socket.gethostname().rstrip('.').lower(), socket.getfqdn().rstrip('.').lower()}:
        return {'status': 'unavailable', 'reason': 'origin_mismatch', 'scope': 'origin_host'}
    work = Path(config['work_dir'])
    installation = Path(config['microservices_dir'])
    user, group = config['user'], config['group']
    checks = []
    def add(name, status, detail):
        checks.append({'name': name, 'status': status, 'detail': str(detail)})
    add('architecture', 'info', platform.machine())
    add('libc', 'info', ' '.join(platform.libc_ver()) or 'unknown')
    try:
        account = pwd.getpwnam(user)
        add('user', 'ok' if account.pw_uid != 0 else 'warning', f'{user}: UID={account.pw_uid}, GID={account.pw_gid}')
    except KeyError:
        add('user', 'missing', user)
    try:
        account_group = grp.getgrnam(group)
        add('group', 'ok' if account_group.gr_gid != 0 else 'warning', f'{group}: GID={account_group.gr_gid}')
    except KeyError:
        add('group', 'missing', group)
    add('workspace', 'info' if work.is_dir() else 'missing', work)
    add('installation', 'ok' if installation.is_dir() else 'missing', installation)
    # This is a conservative host-side check, not proof of SAS mount isolation.
    paths_safe = False
    try:
        protected = [installation, *installation.parents]
        paths_safe = all(not p.is_symlink() and p.stat().st_uid == 0 and not p.stat().st_mode & 0o022 for p in protected)
        resolved_install = installation.resolve(strict=True)
        resolved_work = work.resolve(strict=True)
        paths_safe = paths_safe and resolved_install != resolved_work and resolved_work not in resolved_install.parents and resolved_install not in resolved_work.parents
    except OSError:
        pass
    add('installation_paths', 'ok' if paths_safe else 'warning', 'protected host ancestors; SAS mount isolation still unverified' if paths_safe else 'missing, symlinked, untrusted or writable installation ancestors')
    add('namespace', 'ok' if Path('/run/netns', config['namespace']).exists() else 'missing', config['namespace'])
    payload_safe = paths_safe and not (installation/'tuntom').is_symlink()
    for name, path in [('installation_binary', installation/'tuntom/tuntom'), ('path_binary', shutil.which('tuntom'))]:
        present = bool((name != 'installation_binary' or payload_safe) and path and not Path(path).is_symlink() and Path(path).is_file() and os.access(path, os.X_OK))
        add(name, 'ok' if present else 'missing', path or 'PATH')
    for name in ('version.txt', 'version-commit.txt'):
        path = installation/'tuntom'/name
        try:
            if not payload_safe or path.is_symlink() or not stat.S_ISREG(path.stat().st_mode): raise OSError('not regular')
            with path.open() as stream: value = stream.read(256).strip()
            add(name, 'info', value)
        except (OSError, UnicodeError):
            add(name, 'missing', path)
    for tool in ('sh', 'mv', 'ip', 'getent', 'useradd', 'groupadd'):
        path = shutil.which(tool)
        add(tool, 'ok' if path else 'missing', path or tool)
    tun = Path('/dev/net/tun')
    try: tun_present = stat.S_ISCHR(tun.stat().st_mode)
    except OSError: tun_present = False
    add('tun_device', 'ok' if tun_present else 'missing', tun)
    can_create = os.geteuid() == 0 and all(shutil.which(tool) for tool in ('useradd', 'groupadd', 'getent')) and os.access('/etc', os.W_OK)
    add('account_creation', 'info' if can_create else 'warning', 'host_prerequisites_present' if can_create else 'host_prerequisites_missing')
    return {'status': 'checked', 'scope': 'origin_host', 'checks': checks,
            'host_account_creation_possible': bool(can_create), 'runtime_verified': False}


if __name__ == '__main__':
    print(json.dumps(inspect(json.loads(sys.argv[1]))))
