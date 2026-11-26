"""Run as root with a build directory; requires the tuntom test account and acl tools.
Only temporary deployment directories are modified.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / 'fabric'))
sys.path.insert(0, str(repo / 'fabric/tests'))
from tunnel_deployments import render_bundle, validate
from test_tunnel_deployments import draft, switch_draft


for spec in (draft(), switch_draft()):
    config, _ = validate(spec)
    config['deployment_id'] = 'd' * 32
    for side in ('side_a', 'side_b'):

        if config[side].get('switch_id'):
            config[side]['local_binaries'] = {'tuntom': '/unused', 'tuntomctl': '/unused'}
            config[side]['attachment']['switch_socket'] = '/unused/switch.sock'

    with tempfile.TemporaryDirectory(prefix='tt-state-perms-') as directory:
        root = pathlib.Path(directory) / 'deployment'
        root.mkdir()
        addresses = {config[side]['endpoint_id']: '127.0.0.1' for side in ('side_a', 'side_b')}
        files = render_bundle(config, addresses, '', root=str(root))
        side = next(
            (side for side in ('side_a', 'side_b') if config[side].get('switch_id')),
            'side_a',
        )
        for name, content in files.items():

            if not name.startswith(side + '/'):
                continue

            path = root / name.split('/', 1)[1]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

        (root / 'bin').mkdir(exist_ok=True)
        (root / 'secrets').mkdir(exist_ok=True)
        (root / 'secrets/master.env').write_text('TUNTOM_SECRET=' + 'ab' * 16 + '\n')
        for name in ('tuntom', 'tuntomctl'):
            shutil.copy(pathlib.Path(sys.argv[1]) / name, root / 'bin' / name)

        install = ['bash', str(root / 'scripts/30-install-config.sh')]
        subprocess.run(install, check=True)

        if config[side]['attachment']['type'] == 'switch':
            initial = list((root / 'config').glob('*.classifier'))
            assert len(initial) == config['count']
            assert all(path.read_text() == '# classifier disabled\n' for path in initial)

        # Exercise the same write permission that the daemon needs after dropping root.

        probe = initial[0] if config[side]['attachment']['type'] == 'switch' else root / 'config/probe'
        previous = probe.with_name(probe.name + '.previous')
        previous.write_text('old configuration')
        write_probe = (
            'import pathlib, sys; '
            'path = pathlib.Path(sys.argv[1]); '
            'path.write_text("preserved"); '
            'assert path.read_text() == "preserved"'
        )
        subprocess.run(
            ['runuser', '-u', 'tuntom', '--', 'python3', '-c', write_probe, str(probe)],
            check=True,
        )

        subprocess.run(install, check=True)
        assert probe.read_text() == 'preserved'
        assert previous.read_text() == 'old configuration'
        print('PASS: target state ownership, traversal and repeated installation', side)
