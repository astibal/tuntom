#!/usr/bin/env python3
"""Compare shared RX/TX worker ratios and reserved adapter owners on four physical CPUs.

Pairs: N ports in N/2 independent full-duplex pairs, equal offered rates.
Hub: one adapter plus N-1 tunnels, paced aggregate offered directions 50:50.
Saturation remains backpressure-dependent and is not a fixed traffic matrix.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "switch_mt"))
from bench import physical_cpus, run



def summarize(rows):
    groups = {}
    for row in rows:
        key = (row["layout"], row["profile"], row["payload_size"], row["offered_pps"],
               row["port_count"], row["variant"])
        groups.setdefault(key, []).append(row)
    result = []
    for key, samples in groups.items():
        entry = dict(zip(("layout", "profile", "payload_size", "offered_pps", "port_count", "variant"), key))
        for metric in ("pps", "offered_loss_percent", "switch_loss_percent", "latency_p50_us",
                       "latency_p99_us", "switch_cpu_percent", "switch_cpu_ns_per_frame",
                       "total_cpu_ns_per_frame", "source_peak_cpu_percent", "sink_peak_cpu_percent",
                       "worker_runqueue_cpu_equivalents", "worker_context_switches_per_second",
                       "wake_syscalls_per_frame", "pool_stalls_per_frame", "pool_probes_per_rx",
                       "actual_offered_pps", "to_adapter_pps", "from_adapter_pps"):
            values = [row[metric] for row in samples if metric in row]
            if values:
                entry[metric] = dict(median=statistics.median(values), min=min(values), max=max(values))
        entry["runs"] = len(samples)
        result.append(entry)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True)
    p.add_argument("--resume", action="store_true", help="keep completed raw runs and retry missing runs after a harness interruption")
    p.add_argument("--ports", default="8")
    p.add_argument("--layouts", default="physical4")
    p.add_argument("--variants", default="reference,legacy,ports,g11,g22,g31,g13,g22-tx,g22-both")
    p.add_argument("--case", action="append")
    p.add_argument("--duration", type=float, default=4)
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--verify-all", action="store_true")
    p.add_argument("--pool-size", type=int, default=128)
    p.add_argument("--queue-size", type=int, default=128)
    p.add_argument("--reference", default="/tmp/tuntom-switch-reference")
    p.add_argument("--draft", default="/tmp/tuntom-switch-workers")
    p.add_argument("--legacy", default="/tmp/tuntom-switch-mt")
    p.add_argument("--driver", default="/tmp/tuntom-switch-mt-load")
    args = p.parse_args()
    args.single_driver = False
    args.source_saturation = "poll"
    args.placement = "round-robin"
    counts = [int(n) for n in args.ports.split(",")]
    if not counts or any(n < 2 or n > 128 or n % 2 for n in counts):
        p.error("port counts must be even and between 2 and 128")
    variants = args.variants.split(",")
    if any(v not in ("reference", "legacy", "ports", "g11", "g12", "g21", "g22", "g31", "g13", "g22-tx", "g22-both") for v in variants):
        p.error("unknown variant")
    layouts = args.layouts.split(",")
    if layouts != ["physical4"]:
        p.error("unknown layout")
    if args.duration <= 0 or args.repeat < 1:
        p.error("duration and repeat must be positive")
    cases = []
    for case in args.case or ["pairs:64:0", "pairs:1500:0", "pairs:9000:0", "pairs:9000:160000", "pairs:9000:240000", "hub:9000:160000", "hub:9000:240000"]:
        profile, size, rate = case.split(":")
        if profile not in ("pairs", "hub"):
            p.error("profile must be pairs or hub")
        cases.append((profile, int(size), float(rate)))
    physical = physical_cpus()
    allowed = sorted(os.sched_getaffinity(0))
    # Physical-first order makes the two hardware threads of a core explicit.
    logical = physical + [cpu for cpu in allowed if cpu not in physical]
    if len(physical) != 8 or len(logical) != 16:
        p.error("these named layouts require the documented 8-core/16-thread host")
    args.source_cpus, args.sink_cpus = physical[4:6], physical[6:8]
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and not args.resume:
        p.error("output exists; select a new output to preserve raw measurements")
    if args.resume and not output.exists():
        p.error("resume needs an existing raw result file")
    root = Path(__file__).resolve().parents[2]
    files = [Path(args.reference), Path(args.draft), Path(args.legacy), Path(args.driver),
             root / "src/switch/main.cpp", *root.joinpath("src").rglob("*.hpp"),
             *Path(__file__).parent.glob("*.cpp"), *Path(__file__).parent.glob("*.hpp"),
             *Path(__file__).parent.glob("*.py"), Path(__file__).parent / "build.sh",
             root/"experiments/switch_mt/bench.py", root/"experiments/switch_mt/queues.hpp", root/"experiments/switch_mt/load.cpp", root/"experiments/switch_mt/main.cpp"]
    cpu_topology = {}
    for cpu in allowed:
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        cpu_topology[cpu] = {name: (path / name).read_text().strip()
                             for name in ("core_id", "physical_package_id", "thread_siblings_list")}
    metadata = dict(arguments=vars(args).copy(), utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    uname=list(platform.uname()), cpu_topology=cpu_topology, logical_order=logical,
                    cpu_model=next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                                   if line.startswith("model name")),
                    layout_notes={"physical4": "Switch uses physical CPUs0-3; sources4,5 and sinks6,7; no workload SMT sibling overlap. The OS and other host tasks are not isolated."},
                    git_head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                    git_status=subprocess.check_output(["git", "status", "--short"], cwd=root, text=True),
                    sha256={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
                    compiler_flags="-std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -Isrc")
    metadata_path = Path(str(output) + ".metadata.json")
    if args.resume:
        previous = json.loads(metadata_path.read_text())
        for key in ("duration", "repeat", "ports", "layouts", "variants", "case", "pool_size", "queue_size", "verify_all", "driver", "draft", "legacy", "reference"):
            if previous["arguments"][key] != vars(args)[key]:
                p.error(f"resume changes measured setting {key}")
        for path, digest in previous["sha256"].items():
            if path.endswith((".cpp", ".hpp")) or path in (args.driver, args.draft, args.legacy, args.reference):
                if metadata["sha256"].get(path) != digest:
                    p.error(f"resume changes measured source/binary {path}")
        metadata_path = Path(str(output) + f".resume-{time.time_ns()}.metadata.json")
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    rows = json.loads(output.read_text()) if args.resume else []
    completed = {(r["repeat"], r["layout"], r["profile"], r["payload_size"], r["offered_pps"], r["port_count"], r["variant"]) for r in rows}
    if args.resume:
        print(f"Resume: preserving {len(rows)} completed runs; measured sources and binaries unchanged", flush=True)
    shared_binary = args.draft
    started = time.monotonic()
    for repeat in range(args.repeat):
        # Rotation alone also alternates a two-count sweep. Reversing odd
        # repeats would cancel it and always measure the first count first.
        order = counts[repeat % len(counts):] + counts[:repeat % len(counts)]
        for layout in layouts:
            args.switch_cpus = physical[:4]
            for profile, size, rate in cases:
                for index, count in enumerate(order):
                    args.port_count = count
                    shift = (repeat + index) % len(variants)
                    variant_order = variants[shift:] + variants[:shift]
                    for variant in variant_order:
                        if "-" in variant and profile != "hub":
                            continue
                        if (repeat, layout, profile, size, rate, count, variant) in completed:
                            continue
                        args.draft_extra = []
                        args.worker_placement = physical[:4]
                        args.draft = shared_binary
                        actual_workers = 1
                        if variant == "legacy":
                            args.draft = args.legacy
                            args.worker_placement = [physical[i] for i in (0,2,1,3)]
                            actual_workers = 2*count
                        elif variant == "ports":
                            args.draft_extra = ["--rx-workers",str(count),"--tx-workers",str(count)]
                            args.worker_placement = [physical[i%2] for i in range(count)]+[physical[2+i%2] for i in range(count)]
                            actual_workers = 2*count
                        elif variant != "reference":
                            rx,tx = int(variant[1]),int(variant[2])
                            actual_workers = rx+tx
                            args.draft_extra = ["--rx-workers",str(rx),"--tx-workers",str(tx)]
                            if variant.endswith("-tx"):
                                args.draft_extra += ["--dedicated-tx","adapter"]
                            elif variant.endswith("-both"):
                                args.draft_extra += ["--dedicated-rx","adapter","--dedicated-tx","adapter"]
                        row = dict(layout=layout, profile=profile, payload_size=size, offered_pps=rate,
                                   port_count=count, worker_count=actual_workers,
                                   repeat=repeat, variant=variant,
                                   **run(args, variant, profile, size, rate, physical))
                        row["metadata_file"] = metadata_path.name
                        elapsed = row["elapsed_s"]
                        row["actual_offered_pps"] = row["offered"] / elapsed
                        row["source_peak_cpu_percent"] = max(row["source_cpu_by_thread"]) * 100 / elapsed
                        row["sink_peak_cpu_percent"] = max(row["sink_cpu_by_thread"]) * 100 / elapsed
                        workers = [t for t in row["switch_tasks"].values() if t["name"].startswith(("mt-","sw-")) or variant=="reference"]
                        row["worker_runqueue_cpu_equivalents"] = sum(t["runqueue_wait_s"] for t in workers) / elapsed
                        row["worker_context_switches_per_second"] = sum(t["context_switches"] for t in workers) / elapsed
                        stats = row["switch_stats"]
                        row["wake_syscalls_per_frame"] = stats.get("wake_calls", 0) / max(1, row["received"])
                        row["pool_stalls_per_frame"] = stats.get("pool_stalls", 0) / max(1, row["received"])
                        row["pool_probes_per_rx"] = stats.get("pool_probes", 0) / max(1, row["sent"])
                        row["per_port_received_pps"] = [n / elapsed for n in row["received_by_port"]]
                        if profile == "hub":
                            row["to_adapter_pps"] = row["received_by_port"][-1] / elapsed
                            row["from_adapter_pps"] = sum(row["received_by_port"][:-1]) / elapsed
                        row["rx_worker_cpu_s"] = sum(v for k,v in stats.items() if k.startswith("worker_rx_") and k.endswith("_cpu_s"))
                        row["tx_worker_cpu_s"] = sum(v for k,v in stats.items() if k.startswith("worker_tx_") and k.endswith("_cpu_s"))
                        if variant=="legacy":
                            row["rx_worker_cpu_s"] = sum(stats[f"port_{i}_rx_cpu_s"] for i in range(count))
                            row["tx_worker_cpu_s"] = sum(stats[f"port_{i}_tx_cpu_s"] for i in range(count))
                        rows.append(row)
                        temporary = output.with_suffix(output.suffix+".tmp")
                        temporary.write_text(json.dumps(rows, indent=2)+"\n")
                        temporary.replace(output)
                        print(f"{repeat} {layout:10s} N={count:2d} {variant:9s} {profile}:{size}:{rate:g} "
                              f"delivered={row['pps']:9.0f}pps loss={row['offered_loss_percent']:6.2f}% "
                              f"CPU={row['switch_cpu_percent']:5.0f}% P99={row['latency_p99_us']:8.0f}us "
                              f"runqueue={row['worker_runqueue_cpu_equivalents']:.2f}", flush=True)
    Path(str(output) + ".summary.json").write_text(json.dumps(summarize(rows), indent=2) + "\n")
    print(f"PASS: {len(rows)} runs, accounting and payload identities valid; {time.monotonic()-started:.1f}s", flush=True)


if __name__ == "__main__":
    main()
