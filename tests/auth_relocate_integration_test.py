#!/usr/bin/env python3
import os, signal, struct, subprocess, sys, tempfile, time
from pathlib import Path

tuntom, switch, ctl = sys.argv[1:4]

def stop(p):
    if p.poll() is None:
        p.send_signal(signal.SIGTERM)
        try: p.wait(timeout=3)
        except subprocess.TimeoutExpired: p.kill(); p.wait()

with tempfile.TemporaryDirectory(prefix="tuntom-auth-relocate.") as tmp:
    root=Path(tmp); data=root/'switch.sock'; control=root/'switch.control'; relay=root/'relay.sock'
    responder=root/'respond.py'; verifier=root/'verify.py'
    responder.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdin.buffer.read()\nsys.stdout.buffer.write(b"secret")\n')
    principal=b'alice'; port=b'auth-session'; labels=(1001,7)
    result=b'TTR\x01'+bytes((1,len(principal),len(port),len(labels)))+struct.pack('!I',0)+principal+port+b''.join(struct.pack('!Q',x) for x in labels)
    verifier.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdin.buffer.read()\nsys.stdout.buffer.write('+repr(result)+' )\n')
    responder.chmod(0o700); verifier.chmod(0o700)
    env=os.environ|{'TUNTOM_SECRET':'00112233445566778899aabbccddeeff'}; processes=[]
    try:
        sw=subprocess.Popen([switch,'--socket',str(data),'--control-socket',str(control)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE);processes.append(sw)
        deadline=time.monotonic()+3
        while time.monotonic()<deadline and not data.exists(): time.sleep(.02)
        if not data.exists(): raise AssertionError('switch did not start')
        tid=232
        server=subprocess.Popen([tuntom,'server',str(tid),'-','--switch-socket',str(data),'--switch-port-id','listener','--switch-label','1','--auth-command',str(verifier),'--no-stats','--no-pmtud'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE);processes.append(server)
        client_log=open(root/'client.log','w+')
        client=subprocess.Popen([tuntom,'client',str(tid),'-','127.0.0.1','--relay-listen',str(relay),'--auth-username','alice','--auth-response-command',str(responder),'--control-socket',str(root/'client.control'),'--no-stats','--no-pmtud'],env=env,stdout=client_log,stderr=client_log);processes.append(client)
        deadline=time.monotonic()+12;seen=False
        while time.monotonic()<deadline:
            for name,p in zip(('switch','server','client'),processes):
                if p.poll() is not None: raise AssertionError(f'{name} exited rc={p.returncode}: '+(p.stderr.read().decode(errors='replace') if p.stderr else ''))
            stats=subprocess.run([ctl,str(control),'show','stats'],capture_output=True,text=True,timeout=2)
            if stats.returncode==0 and 'port_auth-session-' in stats.stdout and '_retry_' in stats.stdout:
                client_log.flush();client_log.seek(0)
                if client_log.read().count('V5 session confirmed')>=2: seen=True;break
            time.sleep(.05)
        if not seen: raise AssertionError('AUTH relocation child/session did not become active')
        print('PASS: AUTH helper, child spawn, RELOCATE, binder and switch registration')
    finally:
        for p in reversed(processes): stop(p)
        if 'client_log' in locals(): client_log.close()
