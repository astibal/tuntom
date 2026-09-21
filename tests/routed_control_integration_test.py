#!/usr/bin/env python3
"""Real switch -> relay tunnel -> peer -> exit/divert CONTROL, without root/TUN."""
import contextlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from switch_ruleset_integration_test import Switch
from relay_integration_test import RULES

st, mp, ctl, tunnel, exit_adapter, divert_adapter = sys.argv[1:]
for binary in (st, mp):
    with tempfile.TemporaryDirectory(prefix='tuntom-route-') as tmp, contextlib.ExitStack() as stack:
        root = Path(tmp)
        sw = Switch(binary, ctl, root, RULES, extra_args=['--allow-control-all'])
        stack.callback(sw.log.close); stack.callback(sw.stop)
        processes = []
        def start(args, name, tun_count=0):
            env = {**os.environ, 'TUNTOM_SECRET': '0123456789abcdef0123456789abcdef'}
            inherited = []
            for index in range(tun_count):
                a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                stack.callback(a.close); stack.callback(b.close)
                inherited.append(b.fileno())
                env['TUNTOM_TEST_TUN_FD' if index == 0 else 'TUNTOM_TEST_TUN_OUT_FD'] = str(b.fileno())
            log = stack.enter_context(open(root / (name+'.log'), 'w+'))
            process = subprocess.Popen(args, env=env, pass_fds=inherited, stdout=log, stderr=log)
            processes.append((process, log))
            stack.callback(lambda p: (p.terminate(), p.wait(timeout=5)) if p.poll() is None else None, process)
            return process
        probe = socket.socket(socket.AF_INET6,socket.SOCK_DGRAM)
        for tid in range(210,230):
            try: probe.bind(('::',40000+tid)); break
            except OSError: continue
        else: raise AssertionError('no free UDP port')
        probe.close()
        common = ['--allow-control-all','--no-stats','--no-pmtud','--transport-mtu','600']
        start([tunnel,'server',str(tid),'-','--relay-connect',str(sw.data),'--relay-port-id','proxy-link',
               '--control-socket',str(root/'hub.ctl'),*common], 'hub')
        start([tunnel,'client',str(tid),'-','127.0.0.1','--relay-listen',str(root/'relay'),
               '--control-socket',str(root/'peer.ctl'),*common], 'peer')
        start([exit_adapter,'test-exit','--switch-socket',str(root/'relay'),'--switch-port-id','proxy-in0~via:c:smithproxy#0',
               '--control-socket',str(root/'exit.ctl'),'--allow-control-all','--l4-capacity','16','--l3-capacity','16'], 'exit', 1)
        start([divert_adapter,'test-in','test-out','--switch-socket',str(root/'relay'),'--via-instance','smithproxy#1',
               '--divert-in-port','proxy-in1','--divert-out-port','proxy-out1','--allow-control-all',
               '--flow-capacity','16','--admission-capacity','16'], 'divert', 2)
        def command(*args, target=None):
            route = ['--port','proxy-link','--peer']
            if target: route += ['--peer-port',target]
            return subprocess.run([ctl,'switch',str(sw.control),*route,'--remote-retries','5','--remote-wait','100ms','---',*args],
                                  capture_output=True,text=True,timeout=15)
        start([exit_adapter,'local-exit','--switch-socket',str(sw.data),'--switch-port-id','local-exit',
               '--allow-control-all','--l4-capacity','16','--l3-capacity','16'], 'local-exit', 1)
        deadline = time.monotonic()+10
        while True:
            for p, log in processes:
                if p.poll() is not None:
                    log.seek(0); raise AssertionError(log.read())
            result = command('show','stats',target='proxy-in0~via:c:smithproxy#0')
            if result.returncode == 0: break
            if time.monotonic()>=deadline:
                for p, log in processes:
                    log.flush(); log.seek(0); print(log.read(),file=sys.stderr)
                for socket_name in ('exit.ctl',):
                    print(subprocess.run([ctl,str(root/socket_name),'show','stats'],capture_output=True,text=True).stdout,file=sys.stderr)
                raise AssertionError(result.stderr)
            time.sleep(.05)
        assert 'component=adapter\n' in result.stdout, result
        direct = subprocess.run([ctl,'switch',str(sw.control),'--port','local-exit','---','show','stats'],
                                capture_output=True,text=True,timeout=10)
        assert direct.returncode==0 and 'component=adapter\n' in direct.stdout, direct
        if binary == mp:
            assert 'switch_ipc_mmap=1\n' in direct.stdout, direct.stdout
        request_id = result.stderr.split('request_id=')[1].splitlines()[0]
        saved = command('request','status',request_id,target='proxy-in0~via:c:smithproxy#0')
        assert saved.returncode==0 and saved.stdout==result.stdout, saved
        tunnel_stats = command('show','stats')
        assert tunnel_stats.returncode==0 and 'session_confirmed=1\n' in tunnel_stats.stdout, tunnel_stats
        for target in ('proxy-in1~via:c:smithproxy#1','proxy-out1~via:s:smithproxy#1'):
            result = command('show','stats',target=target)
            assert result.returncode==0 and 'component=divert-adapter\n' in result.stdout, result
        source = root/'classifier.conf'
        body = 'format 1\n' + '# opaque text\n'*400 + 'classify ip4 proto tcp dport 443 to [7]\n'
        source.write_text(body)
        result = command('classifier','load',str(source),target='proxy-in0~via:c:smithproxy#0')
        assert result.returncode==0, result
        shown = command('classifier','show',target='proxy-in0~via:c:smithproxy#0')
        assert shown.returncode==0 and shown.stdout==body, shown
        missing = command('show','stats',target='missing')
        assert missing.returncode==255 and 'target_not_found' in missing.stderr, missing
        rejected = subprocess.run([ctl,'remote',str(root/'peer.ctl'),'---','discover'],
                                  capture_output=True,text=True,timeout=5)
        assert rejected.returncode==1 and 'only in switch mode' in rejected.stderr, rejected
        found = command('discover')
        assert found.returncode==0 and '\tadapter\t' in found.stdout and '\tdivert-adapter\t' in found.stdout, found
        assert '\tALT_PATH\t' in found.stdout, found.stdout
        tree = command('discover','tree')
        assert tree.returncode==0 and tree.stdout.startswith('switch\n`-- proxy-link\n    `-- peer [tunnel]\n'), tree
        assert 'proxy-in0~via:c:smithproxy#0 [adapter]' in tree.stdout, tree.stdout
        assert '[ALT_PATH]' in tree.stdout and '\t' not in tree.stdout, tree.stdout
        print('PASS routed CONTROL, status, multi-block classifier, DISCOVER/ALT_PATH:', Path(binary).name)
