#!/usr/bin/env python3
"""Produce medians and full ranges; do not silently hide failed/partial runs."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    groups = defaultdict(list)
    for row in json.loads(Path(args.results).read_text()):
        groups[(row["profile"], row["payload_size"], row["offered_pps"], row["variant"])].append(row)
    result = []
    keys = ("pps", "mbps", "switch_cpu_percent", "source_cpu_percent", "sink_cpu_percent",
            "switch_cpu_ns_per_frame", "total_cpu_ns_per_frame", "offered_loss_percent",
            "switch_loss_percent", "latency_p50_us", "latency_p99_us", "latency_max_us", "switch_peak_rss_kib")
    for (profile, payload, offered, variant), rows in groups.items():
        item = dict(profile=profile, payload=payload, offered_pps=offered, variant=variant, runs=len(rows))
        for key in keys:
            values = [r[key] for r in rows]
            item[key] = dict(median=statistics.median(values), min=min(values), max=max(values))
        item["source_peak_thread_cpu_percent"] = max(100*t/r["elapsed_s"] for r in rows for t in r["source_cpu_by_thread"])
        item["sink_peak_thread_cpu_percent"] = max(100*t/r["elapsed_s"] for r in rows for t in r["sink_cpu_by_thread"])
        item["send_eagain"] = [r["switch_stats"].get("send_eagain", 0) for r in rows]
        item["pool_stalls"] = [r["switch_stats"].get("pool_stalls", 0) for r in rows]
        item["wake_per_delivered_frame"] = [r["switch_stats"].get("wake_calls", 0)/r["received"] for r in rows]
        if variant != "reference":
            item["adapter_rx_cpu_percent"] = [100*r["switch_stats"]["port_0_rx_cpu_s"]/r["elapsed_s"] for r in rows]
            item["adapter_tx_cpu_percent"] = [100*r["switch_stats"]["port_0_tx_cpu_s"]/r["elapsed_s"] for r in rows]
        result.append(item)
        print(f"{profile:3s} {payload:5s} {offered:6.0f} {variant:10s} n={len(rows)} "
              f"kpps={item['pps']['median']/1000:.1f} [{item['pps']['min']/1000:.1f},{item['pps']['max']/1000:.1f}] "
              f"CPU={item['switch_cpu_percent']['median']:.0f}% P99={item['latency_p99_us']['median']/1000:.2f}ms "
              f"ns/frame={item['switch_cpu_ns_per_frame']['median']:.0f}")
    Path(args.output).write_text(json.dumps(result, indent=2)+"\n")


if __name__ == "__main__":
    main()
