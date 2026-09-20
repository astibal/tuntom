"""Render units, with one service per process and ordered group hooks."""
import shlex


def render(spec):
    root=spec['root']
    prefix='tuntom-'+spec['key']
    target=spec['target']
    pre=prefix+'-prepare.service'
    post=prefix+'-ready.service'
    failed=prefix+'-failed.service'
    members=' '.join(e['unit'] for e in spec['endpoints'])
    marker=f'# tuntom-deploy owner={spec["owner"]}\n'
    def call(action):
        return '/usr/bin/python3 '+shlex.quote(root+'/runtime.py')+' '+action
    result={}
    for e in spec['endpoints']:
        credential=f'LoadCredential=master:{root}/{spec["secret_file"]}\n' if spec['kind']=='tunnel' else ''
        result[e['unit']]=marker+f'''[Unit]
Description=Tuntom {spec['key']} endpoint {e['id']}
PartOf={target}
Requires={pre}
After={pre} network.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=exec
UMask=0077
{credential}ExecStartPre={call('pre '+e['id'])}
ExecStart={call('execute '+e['id'])}
ExecStartPost={call('post '+e['id'])}
ExecStop={call('stop '+e['id'])}
ExecStopPost={call('cleanup '+e['id'])}
Restart=on-failure
RestartSec=3
TimeoutStartSec=60
TimeoutStopSec=60
KillMode=control-group
LimitCORE=0
StandardOutput=append:{root}/logs/{e['id']}.log
StandardError=inherit
'''
    result[pre]=marker+f'''[Unit]
Description=Tuntom {spec['key']} group preparation
PartOf={target}
Before={members} {post}
After=network.target
OnFailure={failed}

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart={call('group-pre')}
ExecStopPost={call('group-cleanup')}
TimeoutStartSec=60
TimeoutStopSec=60
StandardOutput=append:{root}/logs/group.log
StandardError=inherit
'''
    result[post]=marker+f'''[Unit]
Description=Tuntom {spec['key']} group readiness
PartOf={target}
Requires={pre}
After={pre} {members}
OnFailure={failed}

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart={call('group-post')}
ExecStopPost={call('group-stop')}
TimeoutStartSec=120
TimeoutStopSec=60
StandardOutput=append:{root}/logs/group.log
StandardError=inherit
'''
    result[failed]=marker+f'''[Unit]
Description=Stop incomplete Tuntom {spec['key']} startup

[Service]
Type=oneshot
ExecStart=/usr/bin/systemctl --no-block stop {target}
'''
    result[target]=marker+f'''[Unit]
Description=Tuntom deployment {spec['key']}
Wants={pre} {members} {post}
After={post}

[Install]
WantedBy=multi-user.target
'''
    return result
