#!/usr/bin/env python3
"""Paired production-target IPC check, reusing the multi-adapter C++ driver."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import select
import socket
import statistics
import subprocess
import tempfile
import time


def snapshot(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
        peer.settimeout(5)
        peer.connect(str(path))
        peer.sendall(b"show stats")
        result = {}
        for line in peer.recv(1 << 20).decode().splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                result[key] = int(value) if value.isdigit() else value
        return result


def cpu(pid):
    values = Path(f"/proc/{pid}/stat").read_text().split()
    return (int(values[13]) + int(values[14])) / os.sysconf("SC_CLK_TCK")


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError("test child did not stop")


def run(args, binary, cpus, rate):
    with tempfile.TemporaryDirectory(prefix="tomtom-mp-bench.") as directory:
        data, ctl = Path(directory) / "data", Path(directory) / "control"
        command = [binary, "--socket", str(data), "--control-socket", str(ctl)]
        for adapter in range(args.adapters):
            command += ["--exit-port", f"adapter{adapter}"]
        for tunnel in range(args.tunnels):
            adapter = tunnel % args.adapters
            command += ["--route", f"tunnel{tunnel}:18=adapter{adapter}:{1000+tunnel}",
                        "--route", f"adapter{adapter}:{100+tunnel}=tunnel{tunnel}:{1000+args.tunnels+adapter}"]
        switch = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                  preexec_fn=lambda: os.sched_setaffinity(0, set(cpus[:4])))
        driver = None
        try:
            deadline = time.monotonic() + 8
            while not ctl.exists():
                if switch.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("switch startup failed")
                time.sleep(.01)
            csv = lambda values: ",".join(map(str, values))
            driver_command = [args.driver, str(data), "9000", str(args.duration), str(rate),
                              csv(cpus[4:6]), csv(cpus[6:8]), "0", str(args.tunnels),
                              str(args.adapters), "duplex", "equal"]
            driver = subprocess.Popen(driver_command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)
            if not select.select([driver.stdout], [], [], 8)[0] or driver.stdout.readline().strip() != "READY":
                raise RuntimeError("driver startup failed")
            deadline = time.monotonic() + 8
            while snapshot(ctl)["connections_current"] != args.tunnels + args.adapters:
                assert time.monotonic() < deadline
                time.sleep(.01)
            initial_stats = snapshot(ctl)
            if initial_stats.get("implementation") == "tomtom-switch-mp":
                assert initial_stats["workers_pool"] <= 4
                if args.tunnels == 8 and args.adapters == 4:
                    assert initial_stats["workers_active"] == min(4, initial_stats["workers_pool"])
            before = cpu(switch.pid)
            output, error = driver.communicate("START\n", timeout=args.duration + 12)
            assert driver.returncode == 0, (output, error)
            row = json.loads(output)
            row["switch_cpu_s"] = cpu(switch.pid) - before
            stats = snapshot(ctl)
            row["initial_switch_stats"] = initial_stats
            row["switch_stats"] = stats
            row["command"], row["driver_command"] = command, driver_command
            sent, received = sum(row["sent_by_port"]), sum(row["received_by_port"])
            assert row["invalid"] == 0 and stats["frames_rx"] == sent and stats["frames_tx"] == received
            assert stats.get("buffers_in_use", 0) == 0 and stats.get("reconfiguration_drops", 0) == 0
            assert sent - received == stats.get("queue_full_drops", 0) + stats["send_backpressure_drops"]
            row.update(sent=sent, received=received, pps=received / row["elapsed_s"],
                       switch_ns_per_frame=row["switch_cpu_s"] / received * 1e9,
                       offered_loss_percent=100 * (row["offered"] - received) / row["offered"],
                       switch_loss_percent=100 * (sent - received) / sent)
            return row
        finally:
            stop(driver)
            stop(switch)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--switch", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=float, default=3)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--tunnels", type=int, default=8)
    parser.add_argument("--adapters", type=int, default=4)
    parser.add_argument("--rates", type=int, nargs="+", default=[160000, 240000])
    args = parser.parse_args()
    if min(args.duration, args.repeats, args.tunnels, args.adapters, *args.rates) <= 0:
        parser.error("positive counts, duration and rates required")
    cores = {}
    for number in sorted(os.sched_getaffinity(0)):
        root = Path(f"/sys/devices/system/cpu/cpu{number}/topology")
        key = (root.joinpath("physical_package_id").read_text(), root.joinpath("core_id").read_text())
        cores.setdefault(key, number)
    cpus = list(cores.values())[:8]
    assert len(cpus) == 8, "eight available physical cores required"
    source_root = Path(__file__).resolve().parents[2]
    sources = list(source_root.joinpath("src/switch_mp").glob("*.*")) + [source_root / "src/switch/main.cpp", Path(__file__)]
    hashes = {str(path): hashlib.sha256(Path(path).read_bytes()).hexdigest()
              for path in [args.switch, args.reference, args.driver, *sources]}
    result = dict(metadata=dict(uname=list(platform.uname()), cpus=cpus, sha256=hashes,
                                duration=args.duration, repeats=args.repeats, payload_size=9000,
                                compiler_flags="-std=c++17 -pthread -O3 -march=native -mtune=native"), rows=[])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    variants = [("single", args.reference), ("mp", args.switch)]
    for rate in args.rates:
        for repeat in range(args.repeats):
            for variant, binary in (variants if repeat % 2 == 0 else variants[::-1]):
                row = dict(variant=variant, offered_pps=rate, repeat=repeat, **run(args, binary, cpus, rate))
                result["rows"].append(row)
                args.output.write_text(json.dumps(result, indent=2) + "\n")
                print(f"{variant} rate={rate} delivered={row['pps']:.0f} "
                      f"switch_ns/frame={row['switch_ns_per_frame']:.0f} "
                      f"p99_us={row['latency_p99_us']:.1f} loss={row['offered_loss_percent']:.3f}%", flush=True)
    for rate in args.rates:
        for variant, _ in variants:
            selected = [row for row in result["rows"] if row["variant"] == variant and row["offered_pps"] == rate]
            print(rate, variant, {key: statistics.median(row[key] for row in selected)
                                  for key in ("pps", "switch_ns_per_frame", "latency_p99_us", "offered_loss_percent")})


if __name__ == "__main__":
    main()
