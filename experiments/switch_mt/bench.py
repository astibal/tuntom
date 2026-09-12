#!/usr/bin/env python3
"""Paired reference / per-port RX+TX switch benchmark, using real IPC v1.

Each case is PROFILE:PAYLOAD:PPS; PROFILE=mix|hub, PAYLOAD=mixed|64|1500|9000.
PPS=0 means offered saturation (not a guaranteed isolated switch limit).
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


def parse_stats(text):
    result = {}
    for line in text.splitlines():
        key, value = line.split("=", 1)
        try:
            value = int(value)
        except ValueError:
            try:
                value = float(value)
            except ValueError:
                pass
        result[key] = value
    return result


def snapshot(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
        sock.settimeout(3)
        sock.connect(str(path))
        sock.sendall(b"show stats\n")
        return parse_stats(sock.recv(65536).decode())


def physical_cpus():
    cores = {}
    for cpu in sorted(os.sched_getaffinity(0)):
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        key = tuple((path / part).read_text().strip() for part in ("physical_package_id", "core_id"))
        cores.setdefault(key, cpu)
    return list(cores.values())


def process_cpu(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def task_samples(pid):
    result = {}
    for path in Path(f"/proc/{pid}/task").iterdir():
        try:
            fields = (path / "stat").read_text().rsplit(")", 1)[1].split()
            status = (path / "status").read_text().splitlines()
            result[path.name] = dict(
                name=(path / "comm").read_text().strip(),
                cpu_s=(int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
                runqueue_wait_s=int((path / "schedstat").read_text().split()[1]) / 1e9,
                context_switches=sum(int(line.split()[1]) for line in status if "ctxt_switches:" in line),
                voluntary_switches=next(int(line.split()[1]) for line in status if line.startswith("voluntary_ctxt_switches:")),
                involuntary_switches=next(int(line.split()[1]) for line in status if line.startswith("nonvoluntary_ctxt_switches:")),
            )
        except (FileNotFoundError, ProcessLookupError):
            pass
    return result


def host_cpu():
    return {parts[0]: list(map(int, parts[1:9])) for line in Path("/proc/stat").read_text().splitlines()
            if (parts := line.split())[0].startswith("cpu") and parts[0] != "cpu"}


def rss_kib(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return 0


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run(args, variant, profile, size, rate, cpus):
    variable_ports = hasattr(args, "port_count")
    port_count = getattr(args, "port_count", 11)
    if variable_ports:
        worker_cpus = args.switch_cpus[:1] if variant == "reference" else args.switch_cpus
        worker_count = len(worker_cpus)
        source_cpus, sink_cpus = args.source_cpus, args.sink_cpus
    else:
        worker_count = 1 if variant == "reference" else int(variant[2])
        worker_cpus, source_cpus, sink_cpus = cpus[:worker_count], cpus[4:6], cpus[6:8]
    if args.single_driver:
        source_cpus, sink_cpus = source_cpus[:1], sink_cpus[:1]
    csv = lambda cpus: ",".join(map(str, cpus))
    weight = port_count - 1 if profile == "hub" else (1 if profile == "pairs" else 2)
    with tempfile.TemporaryDirectory(prefix="tuntom-mt-bench.") as temp:
        data, ctl, final_stats = (Path(temp) / part for part in ("data", "control", "final"))
        command = [args.reference if variant == "reference" else args.draft, "--socket", str(data),
                   "--control-socket", str(ctl)]
        if profile != "pairs":
            command += ["--exit-port", "adapter"]
        for i in range(port_count if profile == "pairs" else port_count - 1):
            if profile in ("pairs", "mix") or not variable_ports:
                command += ["--route", f"tunnel{i}:17=tunnel{i ^ 1}:{1000+i}"]
            if profile != "pairs":
                command += ["--route", f"tunnel{i}:18=adapter:{1000+i}",
                            "--route", f"adapter:{100+i}=tunnel{i}:{1000+port_count-1}"]
        if variant != "reference":
            placement = getattr(args, "worker_placement", worker_cpus)
            if args.placement == "adapter-dedicated":
                if worker_count != 4:
                    raise ValueError("adapter-dedicated needs mt4")
                # Adapter is CLI port 0. Give its RX/TX separate cores; group
                # the ten other RXs/TXs on the two remaining cores.
                placement = worker_cpus[:2] + worker_cpus[2:4] * 10
            command += ["--worker-cpus", csv(placement), "--pool-size", str(args.pool_size),
                        "--queue-size", str(args.queue_size), "--stats-file", str(final_stats),
                        "--backpressure", "drop" if "drop" in variant else "retry",
                        "--idle", "spin" if "spin" in variant else "sleep"]
            command += getattr(args, "draft_extra", [])
        switch = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                  preexec_fn=lambda: os.sched_setaffinity(0, set(worker_cpus)))
        driver = None
        try:
            deadline = time.monotonic() + 10
            while not ctl.exists():
                if switch.poll() is not None:
                    raise RuntimeError("switch startup: " + switch.stderr.read())
                if time.monotonic() > deadline:
                    raise RuntimeError("control startup timeout")
                time.sleep(.01)
            driver_cmd = [args.driver, str(data), str(size), str(args.duration), str(rate),
                          csv(source_cpus), csv(sink_cpus), str(weight), str(int(args.verify_all)), args.source_saturation]
            if variable_ports:
                driver_cmd += [str(port_count), profile]
            driver = subprocess.Popen(driver_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)
            if not select.select([driver.stdout], [], [], 10)[0] or driver.stdout.readline().strip() != "READY":
                raise RuntimeError("driver startup failed")
            deadline = time.monotonic() + 10
            while True:
                stats = snapshot(ctl)
                if stats["connections_current"] == port_count and stats.get("workers_started", 1):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("registration timeout")
                time.sleep(.01)
            before_cpu, before_host = process_cpu(switch.pid), host_cpu()
            before_tasks = task_samples(switch.pid)
            tasks = dict(before_tasks)
            before_driver_tasks = task_samples(driver.pid)
            driver_tasks = dict(before_driver_tasks)
            load_before = os.getloadavg()
            began = time.monotonic()
            driver.stdin.write("START\n")
            driver.stdin.close()
            driver.stdin = None
            peak_rss = 0
            while driver.poll() is None:
                if switch.poll() is not None:
                    raise RuntimeError("switch exited: " + switch.stderr.read())
                if time.monotonic() - began > args.duration + 15:
                    raise RuntimeError("driver timed out")
                tasks.update(task_samples(switch.pid))
                try:
                    driver_tasks.update(task_samples(driver.pid))
                except (FileNotFoundError, ProcessLookupError):
                    pass
                peak_rss = max(peak_rss, rss_kib(switch.pid))
                time.sleep(.1)
            output, error = driver.communicate()
            if driver.returncode:
                raise RuntimeError(f"driver failed: {output} {error}")
            row = json.loads(output)
            after_host = host_cpu()
            row.update(switch_cpu_s=process_cpu(switch.pid)-before_cpu,
                       cpu_observation_s=time.monotonic()-began, switch_peak_rss_kib=peak_rss,
                       host_loadavg_before=load_before, host_loadavg_after=os.getloadavg(),
                       affinity=dict(switch=worker_cpus, source=source_cpus, sink=sink_cpus),
                       command=command, driver_command=driver_cmd)
            row["host_cpu_busy_percent"] = {}
            for cpu, first in before_host.items():
                delta = [last-first for first, last in zip(first, after_host[cpu])]
                row["host_cpu_busy_percent"][cpu] = 100*(sum(delta)-delta[3]-delta[4])/max(1, sum(delta))
            row["switch_tasks"] = {}
            for tid, last in tasks.items():
                first = before_tasks.get(tid, {})
                row["switch_tasks"][tid] = {key: value-first.get(key, 0) if key != "name" else value
                                             for key, value in last.items()}
            row["driver_tasks"] = {}
            for tid, last in driver_tasks.items():
                first = before_driver_tasks.get(tid, {})
                row["driver_tasks"][tid] = {key: value-first.get(key, 0) if key != "name" else value
                                            for key, value in last.items()}
            stats = snapshot(ctl)
            stop(switch)
            if switch.returncode:
                raise RuntimeError("switch shutdown: " + switch.stderr.read())
            if final_stats.exists():
                stats = parse_stats(final_stats.read_text())
            row["switch_stats"] = stats
            row["sent"], row["received"] = sum(row["sent_by_port"]), sum(row["received_by_port"])
            row["switch_drops"] = row["sent"]-row["received"]
            for key in ("malformed_frames", "send_errors", "route_misses", "target_disconnected", "rx_errors", "shutdown_drops", "buffers_in_use"):
                assert stats.get(key, 0) == 0, (key, row)
            assert row["invalid"] == 0 and row["sent"] == stats["frames_rx"], row
            assert row["received"] == stats["frames_tx"], row
            assert row["switch_drops"] == stats["send_backpressure_drops"]+stats.get("queue_full_drops", 0), row
            row["pps"] = row["received"] / row["elapsed_s"]
            row["mbps"] = row["received_bytes"]*8/row["elapsed_s"]/1e6
            row["offered_loss_percent"] = 100*(row["offered"]-row["received"])/max(1, row["offered"])
            row["switch_loss_percent"] = 100*row["switch_drops"]/max(1, row["sent"])
            for who in ("switch", "source", "sink"):
                row[who+"_cpu_percent"] = 100*row[who+"_cpu_s"]/row["elapsed_s"]
            row["switch_cpu_ns_per_frame"] = row["switch_cpu_s"]*1e9/max(1,row["received"])
            row["total_cpu_ns_per_frame"] = sum(row[k+"_cpu_s"] for k in ("switch", "source", "sink"))*1e9/max(1,row["received"])
            return row
        finally:
            stop(driver)
            stop(switch)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--reference", default="/tmp/tuntom-switch-reference")
    p.add_argument("--draft", default="/tmp/tuntom-switch-mt")
    p.add_argument("--driver", default="/tmp/tuntom-switch-mt-load")
    p.add_argument("--output", required=True)
    p.add_argument("--case", action="append")
    p.add_argument("--variants", default="reference,mt1,mt2,mt4")
    p.add_argument("--duration", type=float, default=4)
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--pool-size", type=int, default=128)
    p.add_argument("--queue-size", type=int, default=128)
    p.add_argument("--single-driver", action="store_true")
    p.add_argument("--verify-all", action="store_true")
    p.add_argument("--source-saturation", choices=("poll", "flood"), default="poll")
    p.add_argument("--placement", choices=("round-robin", "adapter-dedicated"), default="round-robin")
    args = p.parse_args()
    cpus = physical_cpus()
    if len(cpus) < 8:
        raise RuntimeError("this layout requires 8 physical cores; choose a new documented layout")
    variants = args.variants.split(",")
    if any(v not in ("reference", "mt1", "mt2", "mt4", "mt4-drop", "mt4-spin") for v in variants):
        raise ValueError("unknown variant")
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    files = [Path(args.reference), Path(args.draft), Path(args.driver), *root.joinpath("src").rglob("*.hpp"),
             root/"src/switch/main.cpp", *Path(__file__).parent.glob("*.cpp"), Path(__file__).parent/"queues.hpp", Path(__file__)]
    metadata = dict(arguments=vars(args), uname=list(platform.uname()), physical_cpus=cpus,
                    cpu_siblings={str(cpu): Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip() for cpu in cpus},
                    cpu_model=next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
                    git_head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                    git_status=subprocess.check_output(["git", "status", "--short"], cwd=root, text=True),
                    sha256={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
                    compiler_flags="-std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -Isrc")
    Path(str(output)+".metadata.json").write_text(json.dumps(metadata, indent=2)+"\n")
    rows = []
    for repeat in range(args.repeat):
        for case in args.case or ["mix:mixed:200000", "mix:mixed:240000", "mix:9000:0", "hub:9000:0", "mix:64:0"]:
            profile, payload, rate = case.split(":")
            for variant in variants if repeat%2 == 0 else variants[::-1]:
                row = dict(variant=variant, repeat=repeat, profile=profile, payload_size=payload, offered_pps=float(rate),
                           **run(args, variant, profile, 0 if payload == "mixed" else int(payload), float(rate), cpus))
                rows.append(row)
                output.write_text(json.dumps(rows, indent=2)+"\n")
                print(f"{repeat} {variant:10s} {case:20s} delivered={row['pps']:9.0f}pps "
                      f"CPU sw/src/sink={row['switch_cpu_percent']:.0f}/{row['source_cpu_percent']:.0f}/{row['sink_cpu_percent']:.0f}% "
                      f"loss all/sw={row['offered_loss_percent']:.3f}/{row['switch_loss_percent']:.3f}% "
                      f"P99={row['latency_p99_us']:.0f}us", flush=True)


if __name__ == "__main__":
    main()
