#!/usr/bin/env python3
"""V2 implementation A/B: identical MP scheduler and offered traffic, real SwitchClient."""
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
sys.dont_write_bytecode = True
from harness import load_case, physical_cpus

VARIANTS = {
    "v1": ("v1", 8),
    "inline": ("inline", 8),
    "mmap1": ("auto", 1),
    "mmap8": ("auto", 8),
    "mmap16": ("auto", 16),
}
TOPOLOGIES = {"single": (1, 1, 0), "sparse": (1, 1, 19), "10x1": (10, 1, 0), "20x3": (20, 3, 0)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--switch", type=Path, required=True)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variants", choices=VARIANTS, nargs="+", default=list(VARIANTS))
    parser.add_argument("--cases", choices=TOPOLOGIES, nargs="+", default=["single", "20x3"])
    parser.add_argument("--rates", type=int, nargs="+", default=[25000, 160000, 0])
    parser.add_argument("--duration", type=float, default=4)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--verify-all", action="store_true")
    args = parser.parse_args()
    assert args.duration > 0 and args.repeats > 0 and min(args.rates) >= 0
    args.output.mkdir(parents=True, exist_ok=True)
    result = args.output / "results.jsonl"
    if result.exists():
        parser.error("use a fresh output directory")
    cpus = physical_cpus()[:8]
    assert len(cpus) == 8, "requires 8 available physical cores"
    metadata = dict(command=sys.argv, started=time.time(), cpus=cpus,
        cpuinfo=Path("/proc/cpuinfo").read_text(), kernel=platform.release(),
        affinity=sorted(os.sched_getaffinity(0)),
        l3={cpu: (Path(f"/sys/devices/system/cpu/cpu{cpu}/cache/index3/shared_cpu_list").read_text().strip()) for cpu in cpus},
        sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.switch, args.driver)})
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    for topology, rate, repeat in itertools.product(args.cases, args.rates, range(args.repeats)):
        variants = args.variants[repeat % len(args.variants):] + args.variants[:repeat % len(args.variants)]
        for variant in variants:
            mode, batch = VARIANTS[variant]
            tunnels, adapters, idle = TOPOLOGIES[topology]
            case = dict(tunnels=tunnels, adapters=adapters, idle_ports=idle, workers=4,
                        size=0, rate=rate, duration=args.duration, verify_all=args.verify_all,
                        switch_cores=4, driver_threads=2, direction="duplex", shape="equal",
                        ipc_mode=mode, ipc_batch=batch, repeat=repeat)
            tag = f"{len(rows):03d}-{topology}-r{rate}-{variant}"
            print(f"START {tag}", flush=True)
            row = load_case(args.switch, args.driver, case, cpus, args.output, tag,
                            extra_options=["--ipc-batch", str(batch), "--ipc-slots", "128"])
            row.update(topology=topology, variant=variant)
            row["total_cpu_cores"] = (row["cpu_s"] + row["source_cpu_s"] + row["sink_cpu_s"]) / row["elapsed_s"]
            row["source_frames_per_record"] = row["sent"] / row["ipc_tx_records"]
            row["switch_frames_per_record"] = row["received"] / row["ipc_rx_records"]
            row["mapped_source_percent"] = 100 * row["ipc_tx_mapped"] / row["sent"]
            row["mapped_switch_percent"] = 100 * row["ipc_rx_mapped"] / row["received"]
            with result.open("a") as out:
                out.write(json.dumps(row) + "\n")
            rows.append(row)
            summary = []
            for key in sorted({(r["topology"], r["case"]["rate"], r["variant"]) for r in rows}):
                group = [r for r in rows if (r["topology"], r["case"]["rate"], r["variant"]) == key]
                item = dict(topology=key[0], rate=key[1], variant=key[2], repeats=len(group))
                for field in ("pps", "cpu_cores", "total_cpu_cores", "switch_ns_per_frame", "latency_p50_us",
                              "latency_p99_us", "source_frames_per_record", "switch_frames_per_record",
                              "source_cpu_s", "sink_cpu_s", "offered_loss_percent", "switch_loss_percent",
                              "mapped_source_percent", "mapped_switch_percent"):
                    item[field] = statistics.median(r[field] for r in group)
                summary.append(item)
            (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            print(f"PASS {tag}: {row['pps']:.0f} pps; switch={row['cpu_cores']:.3f} CPU; "
                  f"total={row['total_cpu_cores']:.3f} CPU; P99={row['latency_p99_us']:.1f} us; "
                  f"frames/record in={row['source_frames_per_record']:.2f}, out={row['switch_frames_per_record']:.2f}; "
                  f"switch drops={row['switch_loss_percent']:.3f}%", flush=True)


if __name__ == "__main__":
    main()
