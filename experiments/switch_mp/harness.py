#!/usr/bin/env python3
"""Bounded child-process helpers shared by MP correctness and load runs."""
import contextlib
import json
import os
from pathlib import Path
import resource
import select
import socket
import subprocess
import tempfile
import time


def snapshot(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
        peer.settimeout(8)
        peer.connect(str(path))
        peer.sendall(b"show stats")
        result = {}
        for line in peer.recv(1 << 20).decode().splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            try:
                value = int(value)
            except ValueError:
                try:
                    value = float(value)
                except ValueError:
                    pass
            result[key] = value
        assert result.get("component") == "switch", result
        return result


def physical_cpus():
    cores = {}
    for cpu in sorted(os.sched_getaffinity(0)):
        base = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        key = tuple((base / item).read_text().strip() for item in ("physical_package_id", "core_id"))
        cores.setdefault(key, cpu)
    return list(cores.values())


def metrics(pid):
    root = Path(f"/proc/{pid}")
    fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
    status = dict(line.split(":", 1) for line in (root / "status").read_text().splitlines() if ":" in line)
    tasks = {}
    for task in (root / "task").iterdir():
        try:
            parts = (task / "stat").read_text().rsplit(")", 1)[1].split()
            details = dict(line.split(":", 1) for line in (task / "status").read_text().splitlines() if ":" in line)
            tasks[task.name] = dict(name=(task / "comm").read_text().strip(),
                                   cpu_s=(int(parts[11]) + int(parts[12])) / os.sysconf("SC_CLK_TCK"),
                                   voluntary=int(details["voluntary_ctxt_switches"]),
                                   involuntary=int(details["nonvoluntary_ctxt_switches"]),
                                   runqueue_wait_ns=int((task / "schedstat").read_text().split()[1]))
        except (FileNotFoundError, ProcessLookupError):
            pass
    return dict(cpu_s=(int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
                rss_kib=int(status.get("VmRSS", "0 kB").split()[0]),
                fd_count=len(list((root / "fd").iterdir())), threads=int(status["Threads"]), tasks=tasks)


def until(check, timeout=12, description="condition", interval=.01):
    deadline = time.monotonic() + timeout
    while True:
        value = check()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise AssertionError(f"Timeout: {description}")
        time.sleep(interval)


def readline(process, timeout, monitor=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], max(0, min(1.0, deadline - time.monotonic())))[0]:
            line = process.stdout.readline()
            if not line:
                raise AssertionError(f"child stdout closed (exit={process.poll()})")
            return line.strip()
        if monitor:
            monitor()
        if process.poll() is not None:
            raise AssertionError(f"child exited: {process.returncode}")
    raise AssertionError("Child response timed out")


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError(f"test child {process.pid} did not terminate")


class Switch:
    def __init__(self, binary, options=(), cpus=None, log_path=None, env=None, fd_limit=None):
        self.temp = tempfile.TemporaryDirectory(prefix="mp-stress.")
        self.root = Path(self.temp.name)
        self.data, self.ctl = self.root / "data", self.root / "ctl"
        self.peers = []
        self.command = [str(binary), "--socket", str(self.data), "--control-socket", str(self.ctl), *map(str, options)]
        self.log_path = Path(log_path) if log_path else self.root / "stderr"
        self.log = open(self.log_path, "w+")
        def setup():
            if cpus:
                os.sched_setaffinity(0, set(cpus))
            if fd_limit:
                resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))
        self.process = subprocess.Popen(self.command, stdout=subprocess.DEVNULL, stderr=self.log,
                                        env=env, preexec_fn=setup, start_new_session=True)
        try:
            def ready():
                self.alive()
                if not self.ctl.exists():
                    return False
                try:
                    self.stats()
                    return True
                except (ConnectionRefusedError, FileNotFoundError):
                    return False
            until(ready, description="switch startup")
        except BaseException:
            self.close(check=False)
            raise

    def alive(self):
        if self.process.poll() is not None:
            self.log.flush()
            raise AssertionError(f"switch exit={self.process.returncode}: {self.log_path.read_text()}")

    def stats(self):
        self.alive()
        return snapshot(self.ctl)

    def connect(self, name=None):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.peers.append(peer)
        peer.settimeout(8)
        peer.connect(str(self.data))
        if name is not None:
            before = self.stats()["registrations_ok"]
            value = name.encode()
            peer.sendall(b"TTP\x01" + bytes([len(value), 0, 0, 0]) + value)
            until(lambda: self.stats()["registrations_ok"] > before, description="port registration")
        return peer

    def close(self, check=True):
        try:
            stop(self.process)
        finally:
            for peer in self.peers:
                peer.close()
            self.log.flush()
            error = self.log_path.read_text()
            self.log.close()
            removed = not self.data.exists() and not self.ctl.exists()
            self.temp.cleanup()
        if check:
            assert self.process.returncode == 0, error
            assert removed, "socket path leaked"
            for marker in ("ThreadSanitizer", "AddressSanitizer", "LeakSanitizer", "runtime error:"):
                assert marker not in error, error

    def __enter__(self):
        return self

    def __exit__(self, kind, value, trace):
        self.close(check=kind is None)


def routes(tunnels, adapters):
    options = []
    for adapter in range(adapters):
        options += ["--exit-port", f"adapter{adapter}"]
    for tunnel in range(tunnels):
        adapter = tunnel % adapters
        options += ["--route", f"tunnel{tunnel}:18=adapter{adapter}:{1000+tunnel}",
                    "--route", f"adapter{adapter}:{100+tunnel}=tunnel{tunnel}:{1000+tunnels+adapter}"]
    return options


def load_case(binary, driver_binary, case, cpus, directory, tag, extra_options=(), during=None):
    """Run a fixed traffic topology; optional callback mutates additional ports."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    tunnels, adapters = case["tunnels"], case["adapters"]
    options = routes(tunnels, adapters) + list(extra_options)
    if case.get("workers"):
        options += ["--workers", str(case["workers"])]
    switch_cpus = cpus[:case.get("switch_cores", 6)]
    with Switch(binary, options, switch_cpus, directory / (tag + ".switch.log")) as switch:
        idle_ports = case.get("idle_ports", 0)
        for index in range(idle_ports):
            switch.connect(f"bench-idle{index}")
        driver = None
        with open(directory / (tag + ".driver.log"), "w+") as errors:
            try:
                csv = lambda values: ",".join(map(str, values))
                driver_threads = case.get("driver_threads")
                source_cpus = cpus[4:4+driver_threads] if driver_threads else cpus[6:7]
                sink_cpus = cpus[6:6+driver_threads] if driver_threads else cpus[7:8]
                assert not (set(source_cpus) & set(sink_cpus))
                assert not driver_threads or not (set(switch_cpus) & set(source_cpus + sink_cpus))
                command = [str(driver_binary), str(switch.data), str(case["size"]), str(case["duration"]),
                           str(case["rate"]), csv(source_cpus), csv(sink_cpus), str(int(case.get("verify_all", False))),
                           str(tunnels), str(adapters), case.get("direction", "duplex"), case.get("shape", "equal")]
                driver = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=errors, text=True, start_new_session=True)
                assert readline(driver, 15) == "READY"
                until(lambda: switch.stats()["connections_current"] == tunnels + adapters + idle_ports,
                      description="all driver registrations")
                initial = switch.stats()
                before = metrics(switch.process.pid)
                samples = [before]
                host_load = os.getloadavg()
                started = time.monotonic()
                driver.stdin.write("START\n")
                driver.stdin.flush()
                def monitor():
                    switch.alive()
                    sample = metrics(switch.process.pid)
                    samples.append(sample)
                    if case["duration"] >= 60:
                        with open(directory / (tag + ".telemetry.jsonl"), "a") as telemetry:
                            telemetry.write(json.dumps(dict(elapsed=time.monotonic()-started, **sample)) + "\n")
                    if during:
                        during(switch, time.monotonic() - started)
                draining = json.loads(readline(driver, case["duration"] + 20, monitor))
                assert draining["phase"] == "DRAINING", draining
                sent = draining["sent"]
                def drained():
                    stats = switch.stats()
                    assert stats["send_errors"] == stats["target_disconnected"] == stats["route_misses"] == 0, stats
                    if stats["frames_rx"] != sent or stats.get("buffers_in_use", 0):
                        return False
                    losses = stats.get("queue_full_drops", 0) + stats["send_backpressure_drops"]
                    assert stats.get("reconfiguration_drops", 0) == 0, stats
                    return stats if stats["frames_tx"] + losses == sent else False
                final = until(drained, timeout=20, description="switch drain/accounting")
                driver.stdin.write(f"REPORT {final['frames_tx']}\n")
                driver.stdin.flush()
                row = json.loads(readline(driver, 15))
                # Packet/byte counters are separate relaxed atomic observations.
                # At this quiet boundary wait for both before comparing bytes.
                expected_bytes = row["received_bytes"] + 16 * sum(row["received_by_port"])
                final = until(lambda: (value if (value := switch.stats())["bytes_tx"] == expected_bytes else False),
                              description="final byte counters")
                (directory / (tag + ".raw.json")).write_text(json.dumps(
                    dict(driver=row, switch=final, initial=initial, case=case), indent=2) + "\n")
                after = metrics(switch.process.pid)
                samples.append(after)
                received = sum(row["received_by_port"])
                assert row["invalid"] == 0 and sum(row["sent_by_port"]) == sent
                assert final["frames_tx"] == received and final["bytes_tx"] == row["received_bytes"] + 16 * received
                assert switch.stats()["connections_current"] == tunnels + adapters + idle_ports or during
                assert row["offered"] == sent + sum(row["source_backpressure_by_port"])
                for index, count in enumerate(row["received_by_port"]):
                    if row.get("sent_to_port", row["output_weights"])[index]:
                        assert count > 0, ("starved output", index, case)
                    if "sent_to_port" in row and sent == received:
                        assert count == row["sent_to_port"][index], ("per-output accounting", index, row)
                driver.stdin.write("STOP\n")
                driver.stdin.flush()
                driver.wait(timeout=10)
                errors.flush()
                assert driver.returncode == 0, errors.name
                text = Path(errors.name).read_text()
                assert not text, text
                cpu_s = after["cpu_s"] - before["cpu_s"]
                row.update(case=case, command=switch.command, driver_command=command,
                           initial_stats=initial, final_stats=final, before=before, after=after,
                           peak_rss_kib=max(item["rss_kib"] for item in samples),
                           peak_fds=max(item["fd_count"] for item in samples),
                           cpu_s=cpu_s, cpu_cores=cpu_s / row["elapsed_s"],
                           pps=received / row["elapsed_s"], received=received, sent=sent,
                           offered_loss_percent=100 * (row["offered"] - received) / max(1, row["offered"]),
                           switch_loss_percent=100 * (sent - received) / max(1, sent),
                           switch_ns_per_frame=cpu_s / max(1, received) * 1e9,
                           host_load_before=host_load, host_load_after=os.getloadavg(),
                           affinity=dict(switch=switch_cpus, source=source_cpus, sink=sink_cpus),
                           control_observation_s=time.monotonic() - started)
                return row
            finally:
                stop(driver)
