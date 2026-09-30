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
        return (executable == str(root / 'bin/tuntom') and len(args) > 3 and args[1] == role
                and args[2] in members and '--control-socket' in args
                and args[args.index('--control-socket') + 1] == str(root / 'run' / (args[2] + ('c' if role == 'client' else 's') + '.control')))
    except (FileNotFoundError, ProcessLookupError): return False


def undeploy(root, ident, role, members):
    root = Path(root)
    if not root.exists() and not root.is_symlink(): return
    if root.is_symlink() or not root.is_dir(): raise RuntimeError('invalid deployment directory')
    marker = root / '.fabric-deployment-id'
    if marker.is_symlink() or marker.read_text().strip() != ident:
        raise RuntimeError('deployment identity no longer matches')
    if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise RuntimeError('safe undeploy requires Linux pidfd support')
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
        secret_dir = root / 'secrets'
        if secret_dir.is_symlink(): raise RuntimeError('invalid secrets directory')
        if secret_dir.exists():
            for path in secret_dir.iterdir(): erase_secret(path)
        # rmtree does not follow symlinks; only this marker-verified deployment is removed.
        shutil.rmtree(root)
    finally:
        for fd in handles + locks: os.close(fd)


if __name__ == '__main__':
    undeploy(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
    print('UNDEPLOYED: processes stopped; deployment files and secrets removed')
