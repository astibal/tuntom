#!/usr/bin/env python3
"""Serial A/B socket-phase measurements; no installed service or TUN device."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import statistics
import sys
import time

from harness import load_case, physical_cpus


TOPOLOGIES = {
    "single": dict(tunnels=1, adapters=1, idle_ports=0),
    "sparse": dict(tunnels=1, adapters=1, idle_ports=19),
    "10x1": dict(tunnels=10, adapters=1, idle_ports=0),
    "20x3": dict(tunnels=20, adapters=3, idle_ports=0),
}


def summary(rows):
    groups = {}
    for row in rows:
        key = (row["topology"], row["case"]["size"], row["case"]["rate"], row["variant"])
        groups.setdefault(key, []).append(row)
    result = []
    for (topology, size, rate, variant), group in groups.items():
        item = dict(topology=topology, size=size, rate=rate, variant=variant, repeats=len(group))
        for key in ("pps", "cpu_cores", "switch_ns_per_frame", "latency_p50_us", "latency_p99_us",
                    "offered_loss_percent", "switch_loss_percent", "source_cpu_s", "sink_cpu_s",
                    "voluntary_switches_per_frame", "polls_per_frame"):
            item[key] = statistics.median(row[key] for row in group)
        item["pps_min"], item["pps_max"] = min(r["pps"] for r in group), max(r["pps"] for r in group)
        for key in ("recv_calls", "recv_eagain", "send_calls", "send_eagain", "wake_calls", "wake_reads", "cpu_samples"):
            values = [row["counter_per_frame"][key] for row in group if key in row["counter_per_frame"]]
            if values:
                item[key + "_per_frame"] = statistics.median(values)
        result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--improved", type=Path, required=True)
    parser.add_argument("--readiness", type=Path, help="Optional socket-only build with the original scheduler")
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", choices=TOPOLOGIES, default=list(TOPOLOGIES))
    parser.add_argument("--rates", type=int, nargs="+", default=[25000, 160000, 0])
    parser.add_argument("--sizes", type=int, nargs="+", default=[0], help="0 = two 9000B packets per 64B packet")
    parser.add_argument("--duration", type=float, default=3)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--driver-threads", type=int, choices=[1, 2], default=2)
    args = parser.parse_args()
    if args.duration <= 0 or args.repeats <= 0 or min(args.rates) < 0 or not all(0 <= s <= 65535 for s in args.sizes):
        parser.error("positive duration/repeats, nonnegative rates and sizes 0..65535 required")
    args.output.mkdir(parents=True, exist_ok=True)
    results = args.output / "results.jsonl"
    if results.exists():
        parser.error("use a fresh output directory to avoid mixing runs")
    cpus = physical_cpus()[:8]
    assert len(cpus) == 8, "benchmark needs eight available physical cores"
    binaries = [args.baseline, args.improved, args.driver] + ([args.readiness] if args.readiness else [])
    metadata = dict(started=time.time(), command=sys.argv, kernel=platform.release(), cpus=cpus,
                    cpuinfo=Path("/proc/cpuinfo").read_text(), affinity=sorted(os.sched_getaffinity(0)),
                    sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries})
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    for topology, size, rate in itertools.product(args.cases, args.sizes, args.rates):
        variants = [("baseline", args.baseline), ("improved", args.improved)]
        if args.readiness and topology == "single":
            variants.insert(1, ("readiness", args.readiness))
        for repeat in range(args.repeats):
            # Rotate ordering, so three-way comparisons also change their first variant.
            ordered = variants[repeat % len(variants):] + variants[:repeat % len(variants)]
            for variant, binary in ordered:
                case = dict(**TOPOLOGIES[topology], workers=4, size=size, rate=rate,
                            duration=args.duration, verify_all=False, switch_cores=4,
                            driver_threads=args.driver_threads, direction="duplex", shape="equal", repeat=repeat)
                tag = f"{len(rows):03d}-{topology}-s{size}-r{rate}-{variant}"
                print(f"START {tag}", flush=True)
                row = load_case(binary, args.driver, case, cpus, args.output, tag)
                row.update(topology=topology, variant=variant)
                initial, final = row["initial_stats"], row["final_stats"]
                frames = row["received"]
                row["counter_per_frame"] = {key: (final[key] - initial.get(key, 0)) / frames
                    for key in ("recv_calls", "recv_eagain", "send_calls", "send_eagain", "wake_calls", "wake_reads", "cpu_samples")
                    if key in final}
                row["polls_per_frame"] = sum(value - initial.get(key, 0) for key, value in final.items()
                    if key.startswith("worker_") and key.endswith("_poll_calls")) / frames
                row["voluntary_switches_per_frame"] = sum(task["voluntary"] -
                    row["before"]["tasks"].get(tid, {}).get("voluntary", 0)
                    for tid, task in row["after"]["tasks"].items()) / frames
                with results.open("a") as output:
                    output.write(json.dumps(row) + "\n")
                rows.append(row)
                (args.output / "summary.json").write_text(json.dumps(summary(rows), indent=2) + "\n")
                print(f"PASS {tag}: {row['pps']:.0f} pps, {row['cpu_cores']:.3f} cores, "
                      f"{row['switch_ns_per_frame']/1000:.2f} us/frame, p99={row['latency_p99_us']:.1f} us, "
                      f"switch loss={row['switch_loss_percent']:.3f}%", flush=True)


if __name__ == "__main__":
    main()
