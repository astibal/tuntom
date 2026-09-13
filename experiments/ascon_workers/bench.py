#!/usr/bin/env python3
"""Run randomized, pinned codec/queue comparisons and save raw measurements."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def physical_cpus():
    seen, result = set(), []
    for cpu in sorted(os.sched_getaffinity(0)):
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        identity = tuple((topology / name).read_text().strip()
                         for name in ("physical_package_id", "core_id"))
        if identity not in seen:
            result.append(cpu)
            seen.add(identity)
    return result


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_hashes():
    paths = list((ROOT / "src").rglob("*.hpp"))
    paths += [p for p in HERE.iterdir() if p.suffix in (".cpp", ".py", ".sh")]
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(paths)}


def invoke(binary, cpus, mode, size, workers, batch, packets, *, slots=256,
           warmup=4096, verify=False, tamper=False, delay=0):
    command = [str(binary), "--mode", mode, "--size", str(size),
               "--workers", str(workers), "--batch", str(batch),
               "--slots", str(slots), "--packets", str(packets),
               "--warmup", str(warmup), "--delay-us", str(delay),
               "--cpus", ",".join(map(str, cpus[:workers + 1]))]
    if verify:
        command.append("--verify")
    if tamper:
        command.append("--tamper")
    completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if completed.returncode:
        raise RuntimeError(f"benchmark failed ({completed.returncode}): {' '.join(command)}\n"
                           f"{completed.stdout}{completed.stderr}")
    row = json.loads(completed.stdout)
    assert row["tx"] + row["rx"] == packets
    assert 0 <= row["rejected"] <= row["rx"]
    assert row["max_inflight"] <= slots
    assert all(math.isfinite(row[key]) and row[key] >= 0
               for key in ("pps", "seconds", "cpu_cores", "p50_us", "p99_us"))
    row["cpus"] = cpus[:workers + 1]
    return row


def summarize(data):
    grouped = {}
    for row in data["runs"]:
        key = (row["mode"], row["size"], row["workers"], row["batch"])
        grouped.setdefault(key, []).append(row)
    summary = []
    for (mode, size, workers, batch), rows in sorted(grouped.items()):
        entry = dict(mode=mode, size=size, workers=workers, batch=batch, repeats=len(rows))
        for field in ("pps", "payload_gbps", "cpu_cores", "cpu_ns_per_packet", "p50_us", "p99_us"):
            entry[field] = statistics.median(row[field] for row in rows)
        entry["min_gbps"] = min(row["payload_gbps"] for row in rows)
        entry["max_gbps"] = max(row["payload_gbps"] for row in rows)
        baseline = grouped.get((mode, size, 0, 1))
        entry["speedup"] = (entry["pps"] / statistics.median(r["pps"] for r in baseline)
                            if baseline else None)
        summary.append(entry)
    return summary


def report(data):
    lines = ["# Ascon worker mock benchmark", "",
             f"Host: {data['metadata']['cpu_model']}; CPUs: {data['metadata']['cpus']}; "
             f"{data['options']['repeat']} repeats, target {data['options']['seconds']} s/run.", "",
             "Medians below; Gbit/s counts payload processed once per crypto operation. "
             "Mixed is aggregate 50:50 TX/RX, not per-direction throughput. "
             "Workers are additional to the single coordinator. All threads busy-poll.", "",
             "| Mode | Bytes | Workers | Batch | Gbit/s | Range | Speedup | CPU cores | p50 µs | p99 µs |",
             "|---|---:|---:|---:|---:|---|---:|---:|---:|---:|"]
    for row in data["summary"]:
        speed = f"{row['speedup']:.2f}x" if row["speedup"] is not None else "n/a"
        lines.append(f"| {row['mode']} | {row['size']} | {row['workers']} | {row['batch']} | "
                     f"{row['payload_gbps']:.3f} | {row['min_gbps']:.3f}–{row['max_gbps']:.3f} | "
                     f"{speed} | {row['cpu_cores']:.2f} | {row['p50_us']:.1f} | {row['p99_us']:.1f} |")
    lines += ["", "No TUN, UDP, IPC, replay window, fragmentation, reassembly, handshake or rekey. "
              "RX repeats a bounded corpus of authenticated datagrams; it is a codec workload, "
              "not a live session. Synthetic source buffers are reused. "
              "Latency is sampled from input preparation to ordered completion under saturation "
              "and includes queueing; it is not network RTT. "
              "Mixed uses one global completion order, stricter than independent TX/RX orders. "
              "CPU affinity does not isolate cores from other host workloads.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="/tmp/tuntom-ascon-workers")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=.5)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--sizes", default="64,512,1400")
    parser.add_argument("--modes", default="tx,rx,mixed")
    parser.add_argument("--workers", default="0,1,2,4")
    parser.add_argument("--batches", default="1,8")
    parser.add_argument("--slots", type=int, default=256)
    parser.add_argument("--cpus")
    parser.add_argument("--seed", type=int, default=20260913)
    args = parser.parse_args()
    workers = list(map(int, args.workers.split(',')))
    cpus = list(map(int, args.cpus.split(','))) if args.cpus else physical_cpus()[:max(workers) + 1]
    if len(cpus) < max(workers) + 1:
        parser.error("not enough physical CPUs; reduce --workers or provide --cpus")
    if args.seconds <= 0 or args.repeat < 1:
        parser.error("positive seconds and repeats required")
    if args.output.exists() or args.output.with_suffix('.md').exists():
        parser.error("output already exists; choose a new path")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    cpu_model = next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                     if line.startswith('model name'))
    metadata = dict(started=datetime.now(timezone.utc).isoformat(), cpu_model=cpu_model,
                    uname=list(platform.uname()), cpus=cpus, physical_cpus=physical_cpus(),
                    binary_sha256=digest(args.binary), source_sha256=source_hashes(),
                    git_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    compiler=subprocess.check_output([os.environ.get('CXX', 'g++'), '--version'], text=True).splitlines()[0],
                    load_start=list(os.getloadavg()))
    data = dict(metadata=metadata, options={**vars(args), 'output': str(args.output)},
                calibrations=[], runs=[])
    configs = [(mode, size, worker, batch) for mode in args.modes.split(',')
               for size in map(int, args.sizes.split(',')) for worker in workers
               for batch in ([1] if worker == 0 else map(int, args.batches.split(',')))]
    rng = random.Random(args.seed)
    counts = {}
    for repetition in range(args.repeat):
        order = configs.copy()
        rng.shuffle(order)
        for config in order:
            if config not in counts:
                pilot = invoke(args.binary, cpus, *config, 16384, slots=args.slots)
                counts[config] = max(10000, min(100000000, round(pilot['pps'] * args.seconds)))
                data['calibrations'].append(pilot)
            row = invoke(args.binary, cpus, *config, counts[config], slots=args.slots)
            assert not row['verify'] and not row['tamper'] and row['delay_us'] == 0 and row['rejected'] == 0
            row['repeat'] = repetition
            data['runs'].append(row)
            data['summary'] = summarize(data)
            args.output.write_text(json.dumps(data, indent=2) + '\n')
            print(f"{len(data['runs'])}/{len(configs) * args.repeat} "
                  f"{config}: {row['payload_gbps']:.3f} Gbit/s, "
                  f"CPU {row['cpu_cores']:.2f}, p99 {row['p99_us']:.1f} us", flush=True)
    if metadata['source_sha256'] != source_hashes() or metadata['binary_sha256'] != digest(args.binary):
        raise RuntimeError("sources/binary changed during measurement; discard mixed results")
    metadata.update(finished=datetime.now(timezone.utc).isoformat(), load_end=list(os.getloadavg()))
    data['complete'] = True
    args.output.write_text(json.dumps(data, indent=2) + '\n')
    args.output.with_suffix('.md').write_text(report(data))


if __name__ == '__main__':
    main()
