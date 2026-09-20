#!/usr/bin/env python3
"""Serial, alternating A/B trials; only the benchmark's own child processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cpus', default='2,3')
    parser.add_argument('--profiles', default='64,1500,9000,mixed')
    parser.add_argument('--rates', default='25000,0')
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
                modes = ('inline', 'mmap') if repeat % 2 == 0 else ('mmap', 'inline')
                for mode in modes:
                    command = [str(args.binary), mode, profile, str(args.duration), rate,
                               *map(str, cpus), str(args.slots)]
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
                    row.update(repeat=repeat, command=command)
                    rows.append(row)
                    (args.output/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
                    print(f'{profile:>5} {rate:>5} {repeat} {mode:>6}: '
                          f'{row["pps"]:9.0f} pps, {row["cpu_us_per_frame"]:6.2f} us CPU/frame, '
                          f'P99 {row["p99_us"]:8.1f} us, '
                          f'mmap {100*row["mmap_packets"]/row["received"]:5.1f}%', flush=True)
    summary = []
    for profile in args.profiles.split(','):
        for rate in map(float, args.rates.split(',')):
            item = {'profile': profile, 'rate': rate}
            for mode in ('inline', 'mmap'):
                values = [r for r in rows if (r['profile'], r['rate'], r['mode']) == (profile, rate, mode)]
                metrics = ('pps', 'cpu_us_per_frame', 'p50_us', 'p99_us',
                           'a_user_s', 'a_sys_s', 'b_user_s', 'b_sys_s', 'a_vol_cs', 'b_vol_cs')
                item[mode] = {k: statistics.median(r[k] for r in values) for k in metrics}
                item[mode]['cpu_range'] = [min(r['cpu_us_per_frame'] for r in values),
                                            max(r['cpu_us_per_frame'] for r in values)]
                item[mode]['mmap_percent'] = 100*sum(r['mmap_packets'] for r in values)/sum(r['received'] for r in values)
            item['cpu_change_percent'] = 100*(item['mmap']['cpu_us_per_frame']/item['inline']['cpu_us_per_frame']-1)
            item['pps_change_percent'] = 100*(item['mmap']['pps']/item['inline']['pps']-1)
            summary.append(item)
    (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print('All trials delivered every accepted frame in order, with no validation errors.')


if __name__ == '__main__':
    main()
