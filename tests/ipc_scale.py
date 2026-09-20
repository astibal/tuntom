#!/usr/bin/env python3
"""Capacity probe for 10 tunnel ports + 1 exit adapter (no TUN or crypto).

Example: ipc_scale.py --switch /path/tuntom-switch --driver /path/ipc-scale
  --output /tmp/scale.json --case mixed:180000 --case mixed:240000 --case 9000:0
By default the adapter has twice each tunnel's ingress/egress PPS. Tunnels
send 80% to their paired tunnel and 20% to the adapter; the adapter splits
equally among tunnels. Overall 2/3 tunnel-to-tunnel and 1/3 through adapter.
Use --adapter-weight 1 for equal port rates (81.8% tunnel-to-tunnel).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import select
import socket
import subprocess
import tempfile
import time


def snapshot(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
        peer.settimeout(2)
        peer.connect(path)
        peer.sendall(b"show stats\n")
        return {k: int(v) if v.isdecimal() else v
                for k, v in (line.split("=", 1) for line in peer.recv(65536).decode().splitlines())}


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().split()
    return (int(fields[13]) + int(fields[14])) / os.sysconf("SC_CLK_TCK")


def cpu_ticks():
    return {fields[0]: list(map(int, fields[1:9]))
            for line in Path("/proc/stat").read_text().splitlines()
            if (fields := line.split())[0].startswith("cpu") and fields[0] != "cpu"}


def scheduler_wait(pid):
    return int(Path(f"/proc/{pid}/schedstat").read_text().split()[1])


def physical_cpus():
    cores = {}
    for cpu in sorted(os.sched_getaffinity(0)):
        directory = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        key = tuple(directory.joinpath(f).read_text().strip()
                    for f in ("physical_package_id", "core_id"))
        cores.setdefault(key, cpu)
    if len(cores) < 3:
        raise RuntimeError("three physical CPU cores required")
    return list(cores.values())[:3]


def run(args, size, rate, cpus, switch_binary):
    with tempfile.TemporaryDirectory(prefix="tuntom-scale.") as directory:
        path, ctl = directory + "/data", directory + "/control"
        command = [switch_binary, "--socket", path, "--control-socket", ctl, "--exit-port", "adapter"]
        for i in range(10):
            command.extend(["--route", f"tunnel{i}:17=tunnel{i ^ 1}:{1000 + i}",
                            "--route", f"tunnel{i}:18=adapter:{1000 + i}",
                            "--route", f"adapter:{100+i}=tunnel{i}:1010"])
        switch = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                  text=True, preexec_fn=lambda: os.sched_setaffinity(0, {cpus[0]}))
        driver = None
        try:
            deadline = time.monotonic() + 5
            while not Path(ctl).exists():
                if switch.poll() is not None:
                    raise RuntimeError("switch startup failed: " + switch.stderr.read())
                if time.monotonic() >= deadline:
                    raise RuntimeError("switch startup timed out")
                time.sleep(0.01)
            driver = subprocess.Popen([args.driver, path, str(size), str(args.duration), str(rate),
                                       str(cpus[1]), str(cpus[2]), str(args.adapter_weight)],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True)
            if not select.select([driver.stdout], [], [], 5)[0] or driver.stdout.readline().strip() != "READY":
                raise RuntimeError("driver startup failed")
            deadline = time.monotonic() + 5
            while snapshot(ctl)["connections_current"] != 11:
                if time.monotonic() >= deadline:
                    raise RuntimeError("11 ports did not register")
                time.sleep(0.01)
            before = cpu_seconds(switch.pid)
            waited_before = scheduler_wait(switch.pid)
            host_before = cpu_ticks()
            load_before = list(os.getloadavg())
            wall_before = time.monotonic()
            output, errors = driver.communicate("START\n", timeout=args.duration + 5)
            if driver.returncode:
                raise RuntimeError(f"driver failed: {output} {errors}")
            row = json.loads(output)
            host_after = cpu_ticks()
            row["host_loadavg_before"] = load_before
            row["host_loadavg_after"] = list(os.getloadavg())
            row["switch_runqueue_wait_s"] = (scheduler_wait(switch.pid) - waited_before) / 1e9
            row["cpu_observation_s"] = time.monotonic() - wall_before
            row["host_cpu_busy_percent"] = {}
            for cpu in host_before:
                delta = [b - a for a, b in zip(host_before[cpu], host_after[cpu])]
                row["host_cpu_busy_percent"][cpu] = 100 * (sum(delta) - delta[3] - delta[4]) / max(1, sum(delta))
            row["switch_cpu_s"] = cpu_seconds(switch.pid) - before
            row["switch_stats"] = snapshot(ctl)
            row["sent"] = sum(row["sent_by_port"])
            row["received"] = sum(row["received_by_port"])
            row["switch_drops"] = row["sent"] - row["received"]
            row["pps"] = row["received"] / row["elapsed_s"]
            row["mbps"] = row["received_bytes"] * 8 / row["elapsed_s"] / 1e6
            row["switch_cpu_percent"] = row["switch_cpu_s"] / row["elapsed_s"] * 100
            row["source_cpu_percent"] = row["source_cpu_s"] / row["elapsed_s"] * 100
            row["sink_cpu_percent"] = row["sink_cpu_s"] / row["elapsed_s"] * 100
            row["offered_loss_percent"] = (row["offered"] - row["received"]) / row["offered"] * 100
            row["switch_loss_percent"] = row["switch_drops"] / row["sent"] * 100
            stats = row["switch_stats"]
            assert row["invalid"] == 0 and row["sent"] == stats["frames_rx"], row
            assert row["received"] == stats["frames_tx"], row
            assert row["switch_drops"] == stats["send_backpressure_drops"], row
            assert all(stats[k] == 0 for k in ("malformed_frames", "send_errors", "route_misses", "target_disconnected")), row
            return row
        finally:
            if driver and driver.poll() is None:
                driver.kill()
                driver.wait()
            switch.terminate()
            try:
                switch.wait(timeout=3)
            except subprocess.TimeoutExpired:
                switch.kill()
                switch.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--switch", required=True)
    parser.add_argument("--baseline", help="optional old switch, paired A/B with the same IPC driver")
    parser.add_argument("--driver", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--case", action="append", help="payload:total_pps; payload=mixed or bytes, pps=0 saturates")
    parser.add_argument("--duration", type=float, default=5)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--adapter-weight", type=int, choices=(1, 2), default=2)
    args = parser.parse_args()
    cpus = physical_cpus()
    cases = [case.split(":") for case in (args.case or ["mixed:180000", "mixed:240000", "9000:0"])]
    rows = []
    variants = [("after", args.switch)]
    if args.baseline:
        variants.insert(0, ("before", args.baseline))
    metadata = dict(uname=list(platform.uname()), cpus=dict(zip(("switch", "source", "sink"), cpus)),
                    cpu_siblings={cpu: Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip()
                                  for cpu in cpus},
                    arguments=vars(args), sha256={p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
                                                for p in [args.driver, *(p for _, p in variants)]})
    Path(args.output + ".metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    for repeat in range(args.repeat):
        for size, rate in cases:
            for name, binary in (variants if repeat % 2 == 0 else variants[::-1]):
                row = dict(variant=name, repeat=repeat, payload_size=size, offered_pps=float(rate),
                           **run(args, 0 if size == "mixed" else int(size), float(rate), cpus, binary))
                rows.append(row)
                Path(args.output).write_text(json.dumps(rows, indent=2) + "\n")
                print(f"{name} {size}:{rate} rx={row['pps']:.0f}pps CPU switch/source/sink="
                      f"{row['switch_cpu_percent']:.1f}/{row['source_cpu_percent']:.1f}/{row['sink_cpu_percent']:.1f}% "
                      f"loss offered/switch={row['offered_loss_percent']:.3f}/{row['switch_loss_percent']:.3f}% "
                      f"p99={row['latency_p99_us']:.0f}us", flush=True)


if __name__ == "__main__":
    main()
