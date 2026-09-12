#!/usr/bin/env python3
"""Paired manual IPC benchmark of real switch binaries, no root or TUN needed.

Arguments: old-switch old-producer new-switch new-producer output.json
Producer builds: g++ -std=c++17 -pthread -O3 -march=native -I<old-or-new-src>
tests/ipc_bench.cpp -o <producer> (add -DTUNTOM_IPC_GATHER for the new one).
"""
import json
import hashlib
import os
from pathlib import Path
import statistics
import platform
import subprocess
import sys
import tempfile
import time


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().split()
    return (int(fields[13]) + int(fields[14])) / os.sysconf("SC_CLK_TCK")


def run(switch, producer, size, rate, cpus):
    with tempfile.TemporaryDirectory(prefix="tuntom-ipc-bench.") as directory:
        path = directory + "/switch.sock"
        process = subprocess.Popen([switch, "--socket", path,
                                    "--route", "source:17=sink:99"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   preexec_fn=lambda: os.sched_setaffinity(0, {cpus[0]}))
        try:
            deadline = time.monotonic() + 3
            while not Path(path).exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("switch startup failed")
                time.sleep(0.01)
            before = cpu_seconds(process.pid)
            result = subprocess.run([producer, path, str(size), "3", str(rate),
                                     str(cpus[1]), str(cpus[2])],
                                    capture_output=True, text=True, timeout=10, check=True)
            row = json.loads(result.stdout)
            row["switch_cpu_s"] = cpu_seconds(process.pid) - before
            row["pps"] = row["received"] / row["elapsed_s"]
            row["mbps"] = row["pps"] * size * 8 / 1e6
            row["cpu_ns_per_delivered"] = sum(row[k] for k in
                ("source_cpu_s", "switch_cpu_s", "sink_cpu_s")) / row["received"] * 1e9
            row["switch_ns_per_received"] = row["switch_cpu_s"] / row["sent"] * 1e9
            return row
        finally:
            process.terminate()
            process.wait(timeout=3)


def main():
    old_switch, old_producer, new_switch, new_producer, output = sys.argv[1:]
    cores = {}
    for cpu in sorted(os.sched_getaffinity(0)):
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        core = (topology.joinpath("physical_package_id").read_text().strip(),
                topology.joinpath("core_id").read_text().strip())
        cores.setdefault(core, cpu)
    if len(cores) < 3:
        raise RuntimeError("benchmark needs three available physical cores")
    cpus = list(cores.values())[:3]
    metadata = dict(uname=list(platform.uname()), cpus=dict(zip(("switch", "source", "sink"), cpus)),
                    sha256={p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
                            for p in (old_switch, old_producer, new_switch, new_producer)})
    Path(output + ".metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    variants = [("before", old_switch, old_producer), ("after", new_switch, new_producer)]
    for size, rate in ((64, 0), (1500, 0), (9000, 0), (9000, 15000)):
        for repeat in range(3):
            for name, switch, producer in (variants if repeat % 2 == 0 else variants[::-1]):
                row = dict(variant=name, payload_size=size, offered_pps=rate, repeat=repeat,
                           **run(switch, producer, size, rate, cpus))
                rows.append(row)
                Path(output).write_text(json.dumps(rows, indent=2) + "\n")
                print(f"{name} size={size} rate={rate} pps={row['pps']:.0f} "
                      f"cpu_ns/frame={row['cpu_ns_per_delivered']:.0f} "
                      f"lost={row['sent']-row['received']}", flush=True)
    for size, rate in ((64, 0), (1500, 0), (9000, 0), (9000, 15000)):
        selected = [r for r in rows if r["payload_size"] == size and r["offered_pps"] == rate]
        summary = {v: {k: statistics.median(r[k] for r in selected if r["variant"] == v)
                       for k in ("pps", "cpu_ns_per_delivered", "switch_ns_per_received")}
                   for v in ("before", "after")}
        print(size, rate, json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
