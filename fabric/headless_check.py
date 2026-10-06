#!/usr/bin/env python3
"""Read-only readiness report for an offline kit, inside the intended namespace."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def check(root, config):
    failures = []
    def report(name, ok, detail):
        print(('OK   ' if ok else 'FAIL ') + name + ': ' + detail)
        if not ok: failures.append(name)
    print('Network namespace: ' + os.readlink('/proc/self/ns/net'))
    print('Architecture: ' + platform.machine())
    print('Role: ' + config['side'] + '; revision: ' + config['revision'])
    report('directory', len(str(root)) < 75 and all(c.isascii() and (c.isalnum() or c in '/_.-') for c in str(root)), str(root))
    for command in ('python3','screen','flock','ip','ss','timeout','runuser','getfacl','setfacl','getent','groupadd','useradd'):
        found=shutil.which(command)
        report(command, bool(found), found or 'missing runtime dependency')
    for name in config['binaries']:
        path=root/'bin'/name
        report(name, path.is_file() and os.access(path,os.X_OK), str(path)+' (run make first for a source kit)')
    if config['format']=='binary':
        values={}
        for line in Path('/etc/os-release').read_text().splitlines():
            if '=' in line:
                key,value=line.split('=',1);values[key]=value.strip('"')
        report('platform', (values.get('ID'),values.get('VERSION_ID'),platform.machine())==('ubuntu','26.04','x86_64'), 'requires Ubuntu 26.04 x86-64')
    if config['side']=='endpoint':
        report('TUN device', Path('/dev/net/tun').is_char_device(), '/dev/net/tun')
        cap_line=next(line for line in Path('/proc/self/status').read_text().splitlines() if line.startswith('CapEff:'))
        report('CAP_NET_ADMIN', bool(int(cap_line.split()[1],16)&(1<<12)), 'required in this namespace when starting')
        if shutil.which('ip'):
            result=subprocess.run(['ip','route','get',config['peer']],capture_output=True,text=True,timeout=5)
            report('route to peer', result.returncode==0, (result.stdout or result.stderr).strip())
    else:
        report('switch socket', Path(config['switch_socket']).is_socket(), config['switch_socket'])
    print('No processes started, secrets read, packets sent or configuration changed.')
    print('This checks local prerequisites, not remote reachability or data-plane readiness.')
    return bool(failures)


if __name__=='__main__':
    root=Path(__file__).resolve().parent
    try: sys.exit(check(root,json.loads((root/'check.json').read_text())))
    except (OSError,ValueError,subprocess.SubprocessError) as error:
        sys.exit('Readiness check failed: '+str(error))
