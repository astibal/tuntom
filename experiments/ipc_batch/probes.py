#!/usr/bin/env python3
"""Short end-to-end validation; requires the separately built benchmark binary."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cpus', default='2,3')
    args = parser.parse_args()
    cpu_a, other_cpu = map(int, args.cpus.split(','))
    cases = []
    for mode in ('inline', 'mmap', 'inline-mmsg', 'mmap-mmsg', 'mmap-batch'):
        for profile in ('mixed', '65535'):
            cases.append((mode, profile, 128, 1 if mode in ('inline', 'mmap') else 16, 0, other_cpu))
    for mode in ('mmap-mmsg', 'mmap-batch'):
        for slots in (1, 3):
            cases.append((mode, 'mixed', slots, 16, 0, other_cpu))
    for mode in ('inline-mmsg', 'mmap-mmsg', 'mmap-batch'):
        # Sharing one CPU and a tiny send buffer forces partial sends/backpressure.
        cases.append((mode, '64', 128, 16, 4096, cpu_a))
    rows = []
    for mode, profile, slots, batch, sndbuf, cpu_b in cases:
        command = [str(args.binary), mode, profile, '.25', '0', str(cpu_a), str(cpu_b),
                   str(slots), str(batch), str(sndbuf)]
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            raise
        if proc.returncode:
            raise RuntimeError(f'{command}: {stdout}\n{stderr}')
        row = json.loads(stdout)
        assert row['sent'] == row['received'] and row['send_records'] == row['recv_records']
        assert not row['invalid'] and row['sent'] > 0
        row['command'] = command
        rows.append(row)
        print(mode, profile, slots, 'packets', row['received'], 'partial', row['send_partial_calls'],
              'EAGAIN', row['send_eagain'], 'inline', row['inline_packets'], flush=True)
    assert any(r['send_partial_calls'] for r in rows if r['mode'] == 'mmap-mmsg')
    assert any(r['send_eagain'] for r in rows if r['mode'] == 'mmap-batch')
    args.output.write_text(json.dumps(rows, indent=2)+'\n')


if __name__ == '__main__':
    main()
