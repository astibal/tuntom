"""Grant runtime directory traversal while preserving other users' effective ACLs."""
import os
from pathlib import Path
import pwd
import subprocess
import sys


def grant_traversal(directory, uid):
    result = subprocess.run(['getfacl', '-acpn', '--', str(directory)], check=True, capture_output=True, text=True)
    entries = [line.split('#', 1)[0].strip().split(':') for line in result.stdout.splitlines()
               if line.strip() and not line.startswith('#')]
    acl = {':'.join(parts[:2]): parts[2] for parts in entries if len(parts) == 3}
    mask = acl.get('mask:', acl['group:'])
    def effective(permissions):
        return ''.join(p if m != '-' else '-' for p, m in zip(permissions, mask))
    changes = [key + ':' + effective(permissions) for key, permissions in acl.items()
               if key.startswith('group:') or (key.startswith('user:') and key != 'user:')]
    previous = acl.get('user:' + str(uid))
    if previous is not None:
        rights = effective(previous)
    else:
        user = pwd.getpwuid(uid)
        groups = set(os.getgrouplist(user.pw_name, user.pw_gid))
        matches = [effective(permissions) for key, permissions in acl.items()
                   if key.startswith('group:') and
                   (os.stat(directory).st_gid if key == 'group:' else int(key.split(':')[1])) in groups]
        rights = (''.join(char if any(p[index] == char for p in matches) else '-'
                          for index, char in enumerate('rwx')) if matches else acl['other:'])
    rights = rights[:2] + 'x'
    new_mask = ''.join(char if old == char or new == char else '-'
                       for char, old, new in zip('rwx', mask, rights))
    changes.extend(['user:' + str(uid) + ':' + rights, 'mask::' + new_mask])
    subprocess.run(['setfacl', '-n', '-m', ','.join(changes), '--', str(directory)], check=True)


def prepare(root):
    uid = pwd.getpwnam('tuntom').pw_uid
    for directory in reversed((Path(root), *Path(root).parents)):
        check = subprocess.run(['runuser', '-u', 'tuntom', '-g', 'tuntom', '--', 'python3', '-c',
                               'import os,sys; os.chdir(sys.argv[1])', str(directory)])
        if check.returncode == 0:
            continue
        grant_traversal(directory, uid)
        subprocess.run(['runuser', '-u', 'tuntom', '-g', 'tuntom', '--', 'python3', '-c',
                               'import os,sys; os.chdir(sys.argv[1])', str(directory)], check=True)
        print('Granted runtime traversal to tuntom: ' + str(directory), flush=True)


if __name__ == '__main__':
    try:
        prepare(sys.argv[1])
    except (OSError, subprocess.CalledProcessError, KeyError) as error:
        sys.exit('Cannot prepare runtime directory access: ' + str(error))
