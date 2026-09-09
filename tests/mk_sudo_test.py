#!/usr/bin/env python3
"""Check bootstrap sudo environment selection without root, SSH or networking."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    if os.geteuid() == 0:
        print('SKIP: sudo environment tests must run as a normal user')
        return
    with tempfile.TemporaryDirectory(prefix='tuntom-sudo-test-') as tmp:
        directory = Path(tmp)
        sudo = directory / 'sudo'
        # Accept the named-preservation CLI shared by classic sudo and sudo-rs.
        # Stop at the privilege boundary, before any real deployment command.
        sudo.write_text('''#!/usr/bin/python3
import json, os, sys
args = sys.argv[1:]
names = []
while args and args[0].startswith('--preserve-env='):
    names.extend(args.pop(0).split('=', 1)[1].split(','))
assert args and not args[0].startswith('-'), args
assert all(name.startswith('TUNTOM_') for name in names), names
print(json.dumps({'args': args, 'env': {k: os.environ[k] for k in names if k in os.environ}}))
sys.exit(77)
''')
        sudo.chmod(0o700)
        env = {'PATH': f'{directory}:/usr/bin:/bin', 'LC_ALL': 'C',
               'CXX': 'must-not-be-preserved', 'UNRELATED_SECRET': 'must-not-be-preserved',
               'TUNTOM': 'must-not-be-preserved', 'XTUNTOM_SECRET': 'must-not-be-preserved'}
        settings = {'TUNTOM_SECRET': '00112233445566778899aabbccddeeff',
                    'TUNTOM_MTU': '1400', 'TUNTOM_PREFIX16': '10.123',
                    'TUNTOM_CUSTOM': "spaces, equals= and 'quotes'\nsecond line", 'TUNTOM_EMPTY': ''}

        def run(script, args, values, expected):
            result = subprocess.run(['bash', str(ROOT / script), *args],
                                    env=env | values, capture_output=True, text=True, timeout=5)
            assert result.returncode == 77, result.stdout + result.stderr
            report = json.loads(result.stdout)
            assert report['env'] == expected, report
            assert settings['TUNTOM_SECRET'] not in '\0'.join(report['args']), report
            return report['args']

        run('mk_tunnel.sh', ['42', 'example.invalid'], settings, settings)
        run('mk_tunnel.sh', ['42', 'example.invalid', '--stop'], {}, {'TUNTOM_SECRET': ''})
        print('PASS: tunnel start/stop preserve only TUNTOM_*; values stay out of argv')
        for kind in ('switch', 'adapter'):
            script = f'mk_{kind}.sh'
            if not (ROOT / script).exists():
                continue
            args = ['sw' if kind == 'switch' else 'exit0']
            if kind == 'adapter':
                args += ['--switch-socket', str(directory / 'switch.sock'), '--switch-port-id', 'internet']
            args += ['--control-socket', str(directory / 'control with spaces.sock')]
            forwarded = run(script, args, settings, settings)
            assert forwarded == ['bash', str(ROOT / script), *args], forwarded
            stop = [args[0], '--stop']
            forwarded = run(script, stop, {}, {})
            assert forwarded == ['bash', str(ROOT / script), *stop], forwarded
            print(f'PASS: {kind} start/stop preserve only TUNTOM_* and argument boundaries')


if __name__ == '__main__':
    main()
