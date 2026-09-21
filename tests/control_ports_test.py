#!/usr/bin/env python3
"""Live port discovery on both switch implementations; no TUN required."""
import contextlib
import io
import os
import threading
import csv
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from switch_ruleset_integration_test import Switch


def listing(ctl, path, *options):
    result = subprocess.run([ctl, 'switch', str(path), *options, '--port-list'],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stderr == '', result.stderr
    rows = list(csv.DictReader(io.StringIO(result.stdout), delimiter='\t'))
    assert result.stdout.startswith('port\tattachment\tvia\n'), result.stdout
    assert [r['port'] for r in rows] == sorted(r['port'] for r in rows)
    return rows


def tree(ctl, path):
    result = subprocess.run([ctl, 'switch', '--socket', str(path), '--port-tree'],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0 and result.stderr == '', result.stderr
    return result.stdout


if __name__ == '__main__':
    ctl, st, mp, tunnel = sys.argv[1:]
    for binary in (st, mp):
        with tempfile.TemporaryDirectory(prefix='tuntom-ports-') as directory:
            sw = Switch(binary, ctl, Path(directory), 'format 1\nserial 1\n')
            try:
                assert listing(ctl, sw.control) == []
                assert tree(ctl, sw.control) == "switch\n`-- (no registered ports)\n"
                def automatic(*extra, root=directory):
                    return subprocess.run([ctl, 'switch', '--port-list', *extra],
                                          env={**os.environ, 'TUNTOM_RUN_DIR': str(root)},
                                          capture_output=True, text=True, timeout=5)
                missing = automatic()
                assert missing.returncode == 1 and 'no switch socket' in missing.stderr, missing.stderr
                assert automatic(root=Path(directory)/'absent').returncode == 1
                (Path(directory)/'switch-regular-file').write_text('not a socket')
                nested = Path(directory)/'nested'; nested.mkdir()
                (nested/'switch.control').symlink_to(sw.control)
                assert automatic().returncode == 1, 'discovery must not recurse or choose regular files'
                discovered = Path(directory)/'only-switch.control'; discovered.symlink_to(sw.control)
                assert automatic().returncode == 0
                assert automatic('---').returncode == 0
                duplicate = Path(directory)/'another-switch.control'; duplicate.symlink_to(sw.control)
                ambiguous = automatic()
                assert ambiguous.returncode == 1 and str(discovered) in ambiguous.stderr and str(duplicate) in ambiguous.stderr
                assert automatic('--socket', str(sw.control)).returncode == 0, 'explicit socket must win'
                duplicate.unlink(); discovered.unlink()
                # A matching name is not proof of the component's identity.
                with contextlib.closing(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)) as impostor:
                    impostor.settimeout(5)
                    impostor.bind(str(discovered)); impostor.listen(1)
                    observed = []
                    def answer():
                        client, _ = impostor.accept()
                        with client:
                            client.settimeout(5)
                            observed.append(client.recv(256))
                            client.sendall(b'format=txt\ncomponent=adapter\n')
                    worker = threading.Thread(target=answer); worker.start()
                    rejected = automatic(); worker.join(timeout=6)
                    assert not worker.is_alive() and observed == [b'show stats']
                    assert rejected.returncode == 1 and 'not a switch' in rejected.stderr, rejected.stderr
                stale = automatic()
                assert stale.returncode == 1 and 'connect(' in stale.stderr, stale.stderr
                discovered.unlink()

                # Pending registrations are not addressable targets.
                with contextlib.closing(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)) as pending:
                    pending.connect(str(sw.data))
                    assert listing(ctl, sw.control) == []
                    z = sw.port('z-exit')
                    sw.port('a-tunnel')
                    expected = [{'port': name, 'attachment': 'direct', 'via': '-'} for name in ('a-tunnel', 'z-exit')]
                    assert listing(ctl, sw.control) == expected
                    assert tree(ctl, sw.control) == "switch\n|-- a-tunnel\n`-- z-exit\n"
                    inline = subprocess.run([tunnel, 'ctl', 'switch', '--port-list', '--socket', str(sw.control)],
                                            capture_output=True, text=True, timeout=5)
                    assert inline.returncode == 0 and 'z-exit\tdirect\t-' in inline.stdout, inline.stderr
                    z.close()
                    deadline = time.monotonic() + 3
                    while time.monotonic() < deadline:
                        if listing(ctl, sw.control) == expected[:1]: break
                        time.sleep(.02)
                    else: raise AssertionError('disconnected port still listed')
                assert sw.stats()['component'] == 'switch'
                for args in (['switch'], ['switch', str(sw.control), '--port-list', 'show', 'stats'],
                             ['switch', str(sw.control), '--remote-wait', '1s', '--port-list'],
                             ['switch', str(sw.control), '--port-list', '--port-list'],
                             ['switch', str(sw.control), '--port-list', '--port-tree'],
                             ['switch', str(sw.control), '--port-tree', '--port-tree'],
                             ['remote', str(sw.control), '--port-tree'],
                             ['remote', str(sw.control), '--port-list']):
                    result = subprocess.run([ctl, *args], capture_output=True, timeout=3)
                    assert result.returncode == 1, args
            finally:
                sw.stop(); sw.log.close()
    print('PASS: switch --port-list, both switches, empty/pending/disconnected ports, CLI compatibility')
