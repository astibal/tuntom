#!/usr/bin/env python3
"""Summarize repeated IPC trials; retain every raw run in the source JSONL."""
import argparse
import collections
import json
from pathlib import Path
import statistics


def summarize(rows):
    groups = collections.defaultdict(list)
    for row in rows:
        case = row["case"]
        groups[(row["variant"], case["tunnels"], case["adapters"], case["size"], case["rate"],
                case.get("driver_threads", 1))].append(row)
    result = []
    for (variant, tunnels, adapters, size, rate, driver_threads), trials in sorted(groups.items()):
        value = dict(variant=variant, tunnels=tunnels, adapters=adapters, size=size, rate=rate,
                     driver_threads=driver_threads, n=len(trials))
        for field in ("pps", "cpu_cores", "latency_p99_us", "offered_loss_percent",
                      "switch_loss_percent", "switch_ns_per_frame", "peak_rss_kib"):
            values = [trial[field] for trial in trials]
            value[field] = statistics.median(values)
            if field == "pps":
                value["pps_min"], value["pps_max"] = min(values), max(values)
        for direction in ("source", "sink"):
            value[direction + "_cores"] = statistics.median(
                trial[direction + "_cpu_s"] / trial["elapsed_s"] for trial in trials)
        value["offered_pps"] = statistics.median(trial["offered"] / trial["elapsed_s"] for trial in trials)
        value["accepted_pps"] = statistics.median(trial["sent"] / trial["elapsed_s"] for trial in trials)
        value["source_loss_percent"] = statistics.median(
            100 * (trial["offered"] - trial["sent"]) / max(1, trial["offered"]) for trial in trials)
        roles = collections.defaultdict(list)
        for trial in trials:
            occupied = collections.Counter()
            initial, final = trial["initial_stats"], trial["final_stats"]
            for worker in range(initial.get("workers_pool", 0)):
                key = f"worker_{worker}_cpu_ns"
                occupied[initial[f"worker_{worker}_roles"]] += (
                    final[key] - initial[key]) / 1e9 / trial["elapsed_s"]
            for role, amount in occupied.items():
                roles[role].append(amount)
        value["role_cores"] = {role: statistics.median(amounts) for role, amounts in roles.items()}
        result.append(value)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.input.read_text().splitlines() if line]
    groups = summarize(rows)
    result = dict(source=str(args.input.resolve()), runs=len(rows), groups=groups,
                  accepted=sum(row["sent"] for row in rows),
                  delivered=sum(row["received"] for row in rows),
                  invalid=sum(row["invalid"] for row in rows))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "performance-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = ["# IPC performance: repeated-trial summary", "",
             f"{len(rows)} trials; {len(groups)} groups. Each cell is the median of the group's trials.", "",
             "Switch and driver affinity is recorded in every raw trial. No TUN, encryption or NIC.",
             "CPU is occupied cores. Source/sink CPU near the driver thread count suggests a driver bottleneck.",
             "CPU includes the short drain and is divided by the sending interval; occupied-core values are approximate.",
             "Target rate can exceed what a CPU-limited source actually offers. Offered kpps is measured, not requested.",
             "Saturation waits for POLLOUT after source EAGAIN; paced trials schedule weighted rounds without retries.",
             "Offered loss includes source EAGAIN. Internal loss counts only accepted frames.", "",
             "| Impl. | Tunnels | Adapters | Payload B | Target kpps | Driver threads per direction | n | Actual offered kpps | Delivered kpps (min–max) | Switch CPU | Source CPU | Sink CPU | P99 µs | Offered loss % | Internal loss % |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in groups:
        size = str(row["size"]) if row["size"] else "mix"
        rate = f"{row['rate']/1000:g}" if row["rate"] else "saturation"
        lines.append(f"| {row['variant']} | {row['tunnels']} | {row['adapters']} | {size} | {rate} | {row['driver_threads']} | {row['n']} | "
                     f"{row['offered_pps']/1000:.1f} | "
                     f"{row['pps']/1000:.1f} ({row['pps_min']/1000:.1f}–{row['pps_max']/1000:.1f}) | "
                     f"{row['cpu_cores']:.2f} | {row['source_cores']:.2f} | {row['sink_cores']:.2f} | "
                     f"{row['latency_p99_us']:.0f} | {row['offered_loss_percent']:.3f} | {row['switch_loss_percent']:.3f} |")
    (args.output / "PERFORMANCE.md").write_text("\n".join(lines) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "groups"}))


if __name__ == "__main__":
    main()
