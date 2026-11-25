#!/usr/bin/env python3
"""Exercise the real IPv4 policy fallback in a disposable network namespace."""
import errno
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys

HELPER = Path(__file__).resolve().parents[1] / 'tuntom-net.sh'
ENV = dict(os.environ, TUNTOM_ID='42', TUNTOM_IF='tt-test', TUNTOM_MSS_CLAMP='0')
MARK = str(42 << 16)
TABLE = '10042'


def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, capture_output=True, env=ENV)


def lifecycle(action):
    run('bash', '-c', 'source "$1"; tuntom_net_' + action, 'test', str(HELPER))


def route(mark=None):
    args = ['ip', '-4', 'route', 'get', '198.18.0.10']
    if mark is not None:
        args += ['mark', mark]
    return run(*args, check=False)


def expect_blocked():
    result = route(MARK)
    assert result.returncode != 0, result
    assert 'blackhole default' in run('ip', '-4', 'route', 'show', 'table', TABLE).stdout
    assert 'fallback' in route().stdout, 'ordinary host route must remain usable'
    # Unrelated low mark bits (such as TPROXY) must not bypass our mask.
    assert route(str(int(MARK) | 1)).returncode != 0


def expect_live():
    result = route(MARK)
    assert result.returncode == 0 and 'tt-test' in result.stdout, result


def send_marked_original():
    # A transparent upstream socket may share the original connection mark.
    # ORIGINAL must use normal routing even when the return tunnel is down.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sent = sock.sendto(b'original-route-test', ('198.18.0.10', 41942))
        assert sent == len(b'original-route-test')


def inside():
    run('ip', 'link', 'set', 'lo', 'up')
    for name in ('fallback', 'tt-test'):
        run('ip', 'link', 'add', name, 'type', 'dummy')
        run('ip', 'link', 'set', name, 'up')
    run('ip', 'addr', 'add', '192.0.2.1/24', 'dev', 'fallback')
    run('ip', 'route', 'add', 'default', 'dev', 'fallback')
    lifecycle('up')
    expect_live()
    before = run('ip', '-4', 'rule', 'show').stdout
    lifecycle('up')
    assert run('ip', '-4', 'rule', 'show').stdout == before, 'duplicate policy rule'
    expect_live()
    print('PASS: live route and repeated start')

    run('ip', 'route', 'del', 'table', TABLE, 'default', 'dev', 'tt-test', 'metric', '0')
    expect_blocked()
    print('PASS: missing tunnel route cannot fall through to main')
    lifecycle('up')
    run('ip', 'link', 'del', 'tt-test')
    expect_blocked()
    print('PASS: disappearing interface retains blackhole fallback')

    lifecycle('down')
    lifecycle('down')
    expect_blocked()
    assert run('ip', '-4', 'rule', 'show').stdout == before
    run('iptables', '-t', 'mangle', '-C', 'PREROUTING', '!', '-i', 'tt-test', '-j', 'TUNTOM_42_R')
    run('iptables', '-t', 'mangle', '-I', 'OUTPUT', '1', '-p', 'udp', '--dport', '41942',
        '-j', 'CONNMARK', '--set-xmark', MARK + '/0x00ff0000')
    send_marked_original()
    print('PASS: repeated teardown retains reply restoration and allows original-direction upstream traffic')

    run('ip', 'link', 'add', 'tt-test', 'type', 'dummy')
    run('ip', 'link', 'set', 'tt-test', 'up')
    lifecycle('up')
    expect_live()
    lifecycle('down')
    expect_blocked()
    send_marked_original()
    print('PASS: restart and teardown with interface still present')


if __name__ == '__main__':
    if '--inside' in sys.argv:
        inside()
    else:
        required = ('ip', 'iptables', 'unshare', 'bash')
        if os.geteuid() != 0 or any(not shutil.which(command) for command in required):
            print('SKIP: requires root, iproute2, iptables and unshare')
            sys.exit(77)
        if run('unshare', '--net', 'true', check=False).returncode:
            print('SKIP: network namespace creation unavailable')
            sys.exit(77)
        result = subprocess.run(['unshare', '--net', sys.executable, __file__, '--inside'])
        sys.exit(result.returncode)
