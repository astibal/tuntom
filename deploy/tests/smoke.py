#!/usr/bin/env python3
"""Exercise the lifecycle helper against real switches over temporary Unix sockets.

Run as an ordinary user after building main, switch, switch-mp, adapter,
tuntomctl and planner into the supplied directory. Does not use systemd or TUN.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

DEPLOY=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(DEPLOY/'helpers'))
from model import Instance
from runtime import Runtime


def main():
    if os.geteuid()==0: raise SystemExit('run this test as an ordinary user')
    binaries=Path(sys.argv[1]).resolve()
    for name in ('main','adapter','switch','switch-mp','tuntomctl','planner'):
        result=subprocess.run([str(binaries/name),'--help'],capture_output=True,text=True)
        expected=1 if name in ('main','adapter','tuntomctl') else 0
        assert result.returncode==expected,(name,result.stderr)
    for implementation,binary in [('single','switch'),('mp','switch-mp')]:
        with tempfile.TemporaryDirectory(prefix='tt-smoke-') as temporary:
            root=Path(temporary)
            (root/'assets/files').mkdir(parents=True)
            (root/'assets/hooks').mkdir()
            (root/'assets/instance.yml').write_text('smoke test\n')
            (root/'assets/files/rules').write_text('format 1\nserial 1\n')
            (root/'assets/hooks/event.sh').write_text('test -z "${TUNTOM_SECRET:-}"\nprintf "%s/%s\\n" "$TUNTOM_PHASE" "$TUNTOM_ACTION" >> "$TUNTOM_TEST_EVENTS"\n')
            data={'schema':1,'host':'local','implementation':implementation,'socket':str(root/'data.sock'),
                  'rules_file':'files/rules','pre_hook':'hooks/event.sh','post_hook':'hooks/event.sh',
                  'group_pre_hook':'hooks/event.sh','group_post_hook':'hooks/event.sh',
                  'socket_owner':f'{os.getuid()}:{os.getgid()}'}
            if implementation=='mp': data['options']={'workers':1}
            item=Instance('switch','smoke',root/'assets',data)
            spec=item.compile('local')
            encoded=json.dumps(spec).replace(str(item.root),str(root)).replace(str(item.run),str(root/'run'))
            (root/'config.json').write_text(encoded)
            (root/'bin').mkdir()
            shutil.copyfile(binaries/binary,root/'bin/main'); (root/'bin/main').chmod(0o700)
            shutil.copyfile(binaries/'tuntomctl',root/'bin/tuntomctl'); (root/'bin/tuntomctl').chmod(0o700)
            (root/'logs').mkdir()
            rt=Runtime(root)
            def prepare():
                rt.run.mkdir(exist_ok=True)
                rt.active.mkdir(exist_ok=True)
            env=dict(os.environ,TUNTOM_TEST_EVENTS=str(root/'events'),TUNTOM_SECRET='must-not-reach-hooks')
            with patch.dict(os.environ,env,clear=True),patch.object(rt,'prepare_run',side_effect=prepare):
                rt.group('pre')
                rt.pre('smoke')
                script='import sys; sys.path.insert(0,sys.argv[1]); from runtime import Runtime; Runtime(sys.argv[2]).execute("smoke")'
                with open(root/'logs/process.log','w') as log:
                    process=subprocess.Popen([sys.executable,'-c',script,str(DEPLOY/'helpers'),str(root)],stdout=log,stderr=log,env=env)
                    try:
                        rt.post('smoke')
                        rt.group('post')
                        # Down uses the saved working hooks, despite a broken
                        # edit to the live configuration after startup.
                        (root/'assets/hooks/event.sh').write_text('exit 91\n')
                        rt.group('stop')
                        rt.stop('smoke')
                    finally:
                        process.terminate()
                        try: process.wait(timeout=10)
                        except subprocess.TimeoutExpired: process.kill(); process.wait()
                assert process.returncode==0,(implementation,(root/'logs/process.log').read_text())
                rt.cleanup('smoke')
                rt.group('cleanup')
                assert not (root/'data.sock').exists()
                assert not list(rt.active.iterdir())
                assert (root/'events').read_text().splitlines()==['pre/up','pre/up','post/up','post/up','pre/down','pre/down','post/down','post/down']
                print(implementation+': control readiness, hooks, saved teardown and socket cleanup PASS',flush=True)


if __name__=='__main__': main()
