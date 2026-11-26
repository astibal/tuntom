"""Durable switch changes survive process replacement on ST and MP."""
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fabric'))
from control import query, ControlError


for binary in sys.argv[1:]:
    with tempfile.TemporaryDirectory(prefix='switch-state-') as directory:
        root = Path(directory)
        saved = root / 'rules'
        original = 'format 1\nserial 1\n'
        saved.write_text(original)
        control = root / 'control'
        args = [
            binary,
            '--socket', str(root / 'data'),
            '--control-socket', str(control),
            '--rules-file', str(saved),
        ]

        def start():
            process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            for _ in range(100):

                if process.poll() is not None:
                    raise AssertionError(process.stderr.read().decode())

                try:
                    query(str(control), 'stats')
                    return process

                except OSError:
                    time.sleep(.05)

            raise AssertionError('startup timeout')

        def stop(process):
            process.terminate()
            process.communicate(timeout=10)
            assert process.returncode == 0, process.returncode

        process = start()
        try:
            candidate = 'format 1\nserial 2\nswitch a to b allow\n'
            query(str(control), 'load-save', candidate)
            assert saved.read_text() == query(str(control), 'show')
            assert (root / 'rules.previous').read_text() == original
            assert 'matches_active=1' in query(str(control), 'state')
            temporary = candidate.replace('serial 2', 'serial 3')
            query(str(control), 'load', temporary)
            assert 'matches_active=0' in query(str(control), 'state')
            stop(process)
            process = start()
            assert 'serial 2' in query(str(control), 'show')
            try:
                query(str(control), 'load-save', 'bad configuration')

            except ControlError:
                pass

            else:
                raise AssertionError('bad rules accepted')

            assert 'serial 2' in query(str(control), 'show')
            # A persistence failure must not activate the candidate.
            saved.unlink()
            saved.mkdir()
            try:
                query(str(control), 'load-save', temporary)

            except ControlError:
                pass

            else:
                raise AssertionError('directory accepted as state file')

            assert 'serial 2' in query(str(control), 'show')

        finally:
            stop(process)
        # No configured file means runtime-only operation, not an implicit new path.

        index = args.index('--rules-file')
        del args[index:index + 2]
        process = start()
        try:
            assert 'configured=0' in query(str(control), 'state')
            query(str(control), 'load', candidate)
            try:
                query(str(control), 'load-save', temporary)

            except ControlError:
                pass

            else:
                raise AssertionError('save without --rules-file accepted')

            assert 'serial 2' in query(str(control), 'show')

        finally:
            stop(process)

        print('PASS:', binary, 'direct save/backup/restart/temporary/failure/no-file')
