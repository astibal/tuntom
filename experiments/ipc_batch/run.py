#!/usr/bin/env python3
"""Serial, reproducibly shuffled batching trials; only the benchmark's own child processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import signal
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cpus', default='2,3')
    parser.add_argument('--profiles', default='mixed,9000')
    parser.add_argument('--variants', default='inline:1,mmap:1,inline-mmsg:4,inline-mmsg:8,inline-mmsg:16,mmap-mmsg:4,mmap-mmsg:8,mmap-mmsg:16,mmap-batch:4,mmap-batch:8,mmap-batch:16')
    parser.add_argument('--sndbuf', type=int, default=0)
    parser.add_argument('--rates', default='25000,300000,0')
    parser.add_argument('--duration', type=float, default=2)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--slots', type=int, default=128)
    args = parser.parse_args()
    cpus = [int(value) for value in args.cpus.split(',')]
    assert len(cpus) == 2 and len(set(cpus)) == 2
    assert set(cpus).issubset(os.sched_getaffinity(0))
    assert 0 < args.duration <= 60 and 0 < args.repeats <= 20
    args.binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output/'results.json').exists():
        raise RuntimeError('Refusing to overwrite an existing measurement')
    variants = [(mode, int(batch)) for mode, batch in (v.split(':') for v in args.variants.split(','))]
    rng = random.Random(20260912)
    sources = [args.binary, Path(__file__).resolve(), Path(__file__).with_name('bench.cpp'),
               ROOT / 'src/ipc/switch_protocol.hpp']
    topology = {}
    for cpu in cpus:
        base = Path(f'/sys/devices/system/cpu/cpu{cpu}')
        topology[cpu] = {key: (base / 'topology' / key).read_text().strip()
                         for key in ('physical_package_id', 'core_id', 'thread_siblings_list')}
        topology[cpu]['caches'] = [
            {'level': (p/'level').read_text().strip(),
             'shared_cpus': (p/'shared_cpu_list').read_text().strip()}
            for p in sorted((base/'cache').glob('index*'))]
    assert len({(t['physical_package_id'], t['core_id']) for t in topology.values()}) == 2
    metadata = {'uname': list(platform.uname()), 'topology': topology,
                'cgroup': Path('/proc/self/cgroup').read_text(),
                'cpuinfo': Path('/proc/cpuinfo').read_text().split('\n\n')[0],
                'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                'parameters': {key: str(value) if isinstance(value, Path) else value
                               for key, value in vars(args).items()}}
    (args.output/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
    rows = []
    for profile in args.profiles.split(','):
        for rate in args.rates.split(','):
            for repeat in range(args.repeats):
                order = variants.copy()
                rng.shuffle(order)
                for mode, batch in order:
                    command = [str(args.binary), mode, profile, str(args.duration), rate,
                               *map(str, cpus), str(args.slots), str(batch), str(args.sndbuf)]
                    proc = subprocess.Popen(command, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, text=True, start_new_session=True)
                    try:
                        stdout, stderr = proc.communicate(timeout=15+args.duration*4)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.communicate()
                        raise
                    if proc.returncode:
                        raise RuntimeError(f'{command}: {stdout}\n{stderr}')
                    row = json.loads(stdout)
                    assert row['sent'] == row['received'] > 0 and row['invalid'] == 0
                    assert row['mmap_packets'] + row['inline_packets'] == row['received']
                    assert row['send_records'] == row['recv_records']
                    row.update(repeat=repeat, command=command)
                    n = row['received']
                    row['sys_us_per_frame'] = 1e6*(row['a_sys_s']+row['b_sys_s'])/n
                    row['user_us_per_frame'] = 1e6*(row['a_user_s']+row['b_user_s'])/n
                    row['calls_per_frame'] = (row['send_calls']+row['recv_calls'])/n
                    row['polls_per_frame'] = (row['a_polls']+row['b_polls'])/n
                    row['send_batch_mean'] = n/row['send_success_calls']
                    row['recv_batch_mean'] = n/row['recv_success_calls']
                    row['frames_per_record'] = n/row['send_records']
                    row['mmap_percent'] = 100*row['mmap_packets']/n
                    rows.append(row)
                    (args.output/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
                    print(f'{profile:>5} {rate:>5} {repeat} {mode:>11}/{batch:2}: '
                          f'{row["pps"]:9.0f} pps, {row["cpu_us_per_frame"]:6.2f} us CPU/frame, '
                          f'P99 {row["p99_us"]:8.1f} us, '
                          f'batch A/B {row["send_batch_mean"]:.2f}/{row["recv_batch_mean"]:.2f}, '
                          f'mmap {row["mmap_percent"]:5.1f}%', flush=True)
    summary = []
    for profile in args.profiles.split(','):
        for rate in map(float, args.rates.split(',')):
            for mode, batch in variants:
                values = [r for r in rows if (r['profile'], r['rate'], r['mode'], r['batch']) == (profile, rate, mode, batch)]
                metrics = ('pps', 'cpu_us_per_frame', 'p50_us', 'p99_us', 'sys_us_per_frame',
                           'user_us_per_frame', 'calls_per_frame', 'polls_per_frame',
                           'send_batch_mean', 'recv_batch_mean', 'frames_per_record', 'mmap_percent')
                item = dict(profile=profile, rate=rate, mode=mode, batch=batch)
                item['median'] = {k: statistics.median(r[k] for r in values) for k in metrics}
                item['range'] = {k: [min(r[k] for r in values), max(r[k] for r in values)] for k in metrics}
                summary.append(item)
    (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print('All trials delivered every accepted frame in order, with no validation errors.')


if __name__ == '__main__':
    main()
