#!/usr/bin/env python3
"""Real TCP + encrypted tuntom + Linux TUN router, in disposable namespaces."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import secrets
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent
CLIENT, SERVER, PORT = "10.77.1.2", "10.77.2.2", 18080


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, text=True,
                          capture_output=True, timeout=15, **kwargs)


def records(path):
    if not path.exists():
        return []
    text = path.read_text()
    lines = text.splitlines()
    if text and not text.endswith("\n"):
        lines = lines[:-1]  # Writer may still be finishing its last JSON line.
    return [json.loads(line) for line in lines if line]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(predicate, processes=(), timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(f"process {process.args} exited: {process.returncode}")
        if predicate():
            return
        time.sleep(0.02)
    raise RuntimeError("readiness timeout")


def server():
    def echo(conn):
        with conn:
            while data := conn.recv(65536):
                conn.sendall(data)
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((SERVER, PORT))
        listener.listen()
        print(json.dumps({"event": "ready"}), flush=True)
        while True:
            conn, peer = listener.accept()
            print(json.dumps({"event": "accepted", "peer": peer}), flush=True)
            threading.Thread(target=echo, args=(conn,), daemon=True).start()


def client(switch_pid, activate=None, extra_flows=0):
    results = []
    def connect(port):
        sock = socket.socket()
        sock.settimeout(10)
        sock.bind((CLIENT, port))
        sock.connect((SERVER, PORT))
        return sock
    def exchange(sock, name, size):
        payload = (bytes(range(256)) * ((size + 255) // 256))[:size]
        received = bytearray()
        # Interleave bounded writes/reads, independent of socket buffer sizes.
        for offset in range(0, size, 65536):
            piece = payload[offset:offset + 65536]
            sock.sendall(piece)
            remaining = len(piece)
            while remaining:
                data = sock.recv(remaining)
                require(data, "unexpected TCP EOF")
                received.extend(data)
                remaining -= len(data)
        require(bytes(received) == payload, f"corrupted echo: {name}")
        results.append({"name": name, "bytes_each_direction": size,
                        "sha256": hashlib.sha256(received).hexdigest(),
                        "client_port": sock.getsockname()[1]})
    with connect(40000) as old:
        exchange(old, "before_divert", 4096)
        if activate:
            activate()
        else:
            os.kill(switch_pid, signal.SIGUSR1)
        time.sleep(0.1)
        exchange(old, "existing_after_activation", 32768)
        with connect(40001) as new:
            exchange(new, "new_diverted", 1024 * 1024)
        exchange(old, "existing_after_new_flow", 32768)
        with connect(40002) as second:
            exchange(second, "second_diverted", 131072)
        for port in range(40003, 40003 + extra_flows):
            with connect(port) as extra:
                exchange(extra, "multipath_diverted", 32768)
    print(json.dumps(results), flush=True)


class Lab:
    def __init__(self, build, output):
        self.build, self.output = build, output
        self.processes, self.logs, self.holders = [], [], {}
        self.temp = tempfile.TemporaryDirectory(prefix="tt-divert.")
        self.runtime = Path(self.temp.name)

    def command(self, command, ns=None):
        prefix = ["nsenter", "--target", str(self.holders[ns].pid), "--net"] if ns else []
        return prefix + [str(x) for x in command]

    def ip(self, *args, ns=None):
        return run(self.command(["ip", *args], ns))

    def start(self, name, command, ns=None, env=None):
        log = (self.output / f"{name}.log").open("w")
        self.logs.append(log)
        process = subprocess.Popen(self.command(command, ns), stdout=log, stderr=subprocess.STDOUT,
                                   env=env, start_new_session=True)
        self.processes.append(process)
        return process

    def namespace(self, name):
        process = subprocess.Popen(["unshare", "--net", sys.executable, str(HERE / "run.py"), "--holder"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        self.processes.append(process)
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            require(selector.select(timeout=5), "namespace holder did not start")
            line = process.stdout.readline()
            if line.strip() != "ready":
                raise RuntimeError(f"namespace failed: {process.stderr.read()}")
        self.holders[name] = process
        for setting in ("net.ipv6.conf.all.disable_ipv6=1", "net.ipv6.conf.default.disable_ipv6=1"):
            run(self.command(["sysctl", "-q", "-w", setting], name))
        self.ip("link", "set", "lo", "up", ns=name)

    def configure_router(self, name):
        for setting in ("net.ipv4.ip_forward=1", "net.ipv4.conf.all.rp_filter=0",
                        "net.ipv4.conf.default.rp_filter=0", "net.ipv4.conf.all.send_redirects=0"):
            run(self.command(["sysctl", "-q", "-w", setting], name))

    def close(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=2)
            for pipe in (process.stdout, process.stderr):
                if pipe:
                    pipe.close()
        for log in self.logs:
            log.close()
        self.temp.cleanup()


def verify(output, cookie, tcp_results, mtu, tunnel_stats):
    sw, adapter = records(output / "switch.jsonl"), records(output / "adapter.jsonl")
    errors = [row for row in sw + adapter if "drop" in row["event"]]
    require(not errors, f"unexpected datapath drops: {errors[:3]}")
    require(any(r["event"] == "existing" and r.get("sport") == 40000 for r in adapter), "old TCP was not classified EXISTING")
    require(not any(r["event"] == "to-router" and 40000 in (r.get("sport"), r.get("dport")) for r in adapter), "existing TCP entered router")
    for port in (40001, 40002):
        route = [r for r in sw if r["event"] == "normal" and r["from"] == "divert-out" and r.get("sport") == port]
        require(route and all(r["logical"] == "edge" and r["to"] == "exit" for r in route), "origin port was not restored")
        require(all(r["labels"][:2] == [99, 42] and len(r["labels"]) == 8 and r["labels"][4] == 123 for r in route), "exit lost variable divert envelope or stable origin")
        require(any(r["event"] == "server_return" and r.get("dport") == port for r in sw), "exit did not restore DVRT on response")
        returns = [r for r in sw if r["event"] == "client_return" and r.get("dport") == port]
        require(returns and all(r["to"] == "edge" and r["labels"] == [17, 42] for r in returns), "client return failed to strip envelope/restore original stack")
        for sport, dport, flags, direction in ((port, PORT, 2, "SYN"), (PORT, port, 18, "SYN/ACK")):
            before = [r for r in adapter if r["event"] == "to-router" and r.get("sport") == sport and r.get("dport") == dport and r.get("flags", 0) & 18 == flags]
            after = [r for r in adapter if r["event"] == "from-router" and r.get("sport") == sport and r.get("dport") == dport and r.get("flags", 0) & 18 == flags]
            require(any(a["seq"] == b["seq"] and a["ttl"] + 1 == b["ttl"] for a in after for b in before), f"no real Linux TTL decrement for {direction} on {port}")
    accepted = [r["peer"] for r in records(output / "tcp-server.log") if r["event"] == "accepted"]
    require({tuple(peer) for peer in accepted} == {(CLIENT, 40000), (CLIENT, 40001), (CLIENT, 40002)}, "server observed changed source identity")
    if mtu > 1500:
        require(int(tunnel_stats["client"].get("fragments_tx", "0")) > 0 and
                int(tunnel_stats["server"].get("fragments_rx", "0")) > 0,
                "jumbo run did not expose transport fragmentation counters")
    report = {"status": "PASS", "mtu": mtu, "cookie": cookie,
              "tcp": tcp_results, "server_peers": accepted,
              "switch_events": dict(Counter(r["event"] for r in sw)),
              "adapter_events": dict(Counter(r["event"] for r in adapter)),
              "checks": ["existing TCP bypasses router", "new TCP traverses both divert TUNs",
                         "Linux decrements SYN and SYN/ACK TTL", "stable origin 123 restored",
                         "exact normal rules see original stack", "exit adapter restores DVRT",
                         "client gets original stack", "source IP/port unchanged", "bidirectional echo hashes match"],
              "tuntom_stats": tunnel_stats}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# Divert router lab — PASS", "", f"Inner MTU: {mtu}; cookie: `{cookie}`.", "",
             "Uses real Linux namespaces/TUN routing, tuntom encryption and the production exit adapter.", "",
             "The rootless tuntom launcher changes only privilege dropping; this is not a CPU benchmark.", "",
             *[f"- {item}" for item in report["checks"]], "", "## TCP exchanges", "",
             "| Exchange | Bytes each direction | Client port |", "| --- | ---: | ---: |"]
    lines += [f"| {r['name']} | {r['bytes_each_direction']} | {r['client_port']} |" for r in tcp_results]
    (output / "RESULTS.md").write_text("\n".join(lines) + "\n")
    return report


def inside(args):
    mapping = Path("/proc/self/uid_map").read_text().split()
    require(len(mapping) == 3 and mapping[0] == "0" and mapping[1] != "0" and mapping[2] == "1",
            "run through unshare --user --map-root-user")
    require(os.readlink("/proc/self/ns/net") != os.readlink("/proc/1/ns/net"), "refusing initial network namespace")
    output = Path(args.output).resolve()
    require(not output.exists(), "output already exists; use a new directory")
    output.mkdir(parents=True)
    build = Path(args.build).resolve()
    lab = Lab(build, output)
    def terminated(signum, _frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, terminated)
    try:
        lab.ip("link", "set", "lo", "up")
        for name in ("client", "router", "exit", "server"):
            lab.namespace(name)
        lab.ip("link", "add", "under0", "type", "veth", "peer", "name", "under1")
        lab.ip("link", "set", "under1", "netns", str(lab.holders["client"].pid))
        for name, address, ns in (("under0", "192.0.2.1/30", None), ("under1", "192.0.2.2/30", "client")):
            lab.ip("addr", "add", address, "dev", name, ns=ns)
            lab.ip("link", "set", name, "up", ns=ns)
        lab.ip("link", "add", "lan0", "type", "veth", "peer", "name", "lan1")
        lab.ip("link", "set", "lan0", "netns", str(lab.holders["exit"].pid))
        lab.ip("link", "set", "lan1", "netns", str(lab.holders["server"].pid))
        for name, address, ns in (("lan0", "10.77.2.1/24", "exit"), ("lan1", "10.77.2.2/24", "server")):
            lab.ip("addr", "add", address, "dev", name, ns=ns)
            lab.ip("link", "set", name, "mtu", str(args.mtu), "up", ns=ns)
        lab.ip("route", "add", CLIENT + "/32", "via", "10.77.2.1", ns="server")
        lab.configure_router("router")
        lab.configure_router("exit")
        cookie = "".join(secrets.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(3))
        rules = lab.runtime / "switch.rules"
        rules.write_text('format 2\nserial 1\nexit exit\nswitch edge, [17, 42] to exit, [99, 42] allow bidir\n')
        (output / "switch.rules").write_text(rules.read_text())
        classifier = lab.runtime / "ingress.classifier"
        classifier.write_text("format 1\nclassify ip4 to [17, 42]\n")
        path = lab.runtime / "switch.sock"
        sw = lab.start("switch", [build / "divert-switch", "--socket", path, "--rules", rules,
                       "--cookie", cookie, "--trace", output / "switch.jsonl",
                       "--port", "divert-out=402", "--port", "exit=700", "--port", "edge=123", "--port", "divert-in=401"])
        wait_for(path.exists, lab.processes)
        lab.start("divert-adapter", [build / "divert-adapter", path, "di0", "do0", cookie, output / "adapter.jsonl", args.mtu], ns="router")
        lab.start("exit-adapter", [build / "exit-adapter", "ex0", "--switch-socket", path, "--switch-port-id", "exit",
                                   "--switch-ipc", "v1", "--mtu", args.mtu, "--control-socket", lab.runtime / "exit.control"], ns="exit")
        environment = os.environ | {"TUNTOM_SECRET": secrets.token_hex(16)}
        common = ["--mtu", args.mtu, "--transport-mtu", "1500", "--no-pmtud", "--no-ttl-compensate", "--no-stats", "--quiet"]
        lab.start("tuntom-server", [build / "tuntom-userns", "server", "231", "-", *common,
                  "--switch-socket", path, "--switch-port-id", "edge", "--switch-label", "17", "--switch-ipc", "v1",
                  "--classifier-file", classifier, "--control-socket", lab.runtime / "server.control"], env=environment)
        lab.start("tuntom-client", [build / "tuntom-userns", "client", "231", "tc0", "192.0.2.1", *common,
                  "--control-socket", lab.runtime / "client.control"], ns="client", env=environment)
        def interface(ns, name):
            return subprocess.run(lab.command(["ip", "link", "show", name], ns), stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
        wait_for(lambda: all(interface(ns, name) for ns, name in (("client", "tc0"), ("router", "di0"), ("router", "do0"), ("exit", "ex0"))), lab.processes)
        for ns, name, address, destination in (("client", "tc0", CLIENT + "/32", SERVER),
                ("router", "di0", "10.77.254.1/32", CLIENT), ("router", "do0", "10.77.254.2/32", SERVER),
                ("exit", "ex0", "10.77.254.3/32", CLIENT)):
            lab.ip("addr", "add", address, "dev", name, ns=ns)
            lab.ip("link", "set", name, "up", ns=ns)
            lab.ip("route", "add", destination + "/32", "dev", name, ns=ns)
        def stats(which):
            text = run([build / "tuntomctl", lab.runtime / f"{which}.control", "show", "stats"]).stdout
            return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
        wait_for(lambda: (lab.runtime / "client.control").exists() and (lab.runtime / "server.control").exists(), lab.processes)
        wait_for(lambda: all(stats(which).get("session_confirmed") == "1" for which in ("client", "server")), lab.processes)
        wait_for(lambda: {r["from"] for r in records(output / "switch.jsonl") if r["event"] == "registered"} == {"edge", "exit", "divert-in", "divert-out"}, lab.processes)
        lab.start("tcp-server", [sys.executable, HERE / "run.py", "--server"], ns="server")
        wait_for(lambda: bool(records(output / "tcp-server.log")), lab.processes)
        driver = subprocess.run(lab.command([sys.executable, HERE / "run.py", "--client", sw.pid], "client"),
                                capture_output=True, text=True, timeout=45)
        (output / "tcp-client.log").write_text(driver.stdout + driver.stderr)
        require(driver.returncode == 0, "TCP client failed: " + driver.stderr)
        tcp_results = json.loads(driver.stdout)
        time.sleep(0.2)
        report = verify(output, cookie, tcp_results, args.mtu, {which: stats(which) for which in ("client", "server")})
        print(json.dumps({"status": report["status"], "mtu": args.mtu, "output": str(output)}, indent=2))
    except Exception as error:
        (output / "failure.txt").write_text(str(error) + "\n")
        raise
    finally:
        lab.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="/tmp/tuntom-divert-lab-build")
    parser.add_argument("--output")
    parser.add_argument("--mtu", type=int, choices=(1500, 9000), default=1500)
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--holder", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--server", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--client", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.holder:
        print("ready", flush=True)
        while True:
            signal.pause()
    if args.server:
        return server()
    if args.client:
        return client(args.client)
    require(args.output, "--output is required and must name a new directory")
    if args.inside:
        return inside(args)
    command = ["unshare", "--user", "--map-root-user", "--net", sys.executable, str(HERE / "run.py"),
               "--inside", "--build", str(Path(args.build).resolve()), "--output", str(Path(args.output).resolve()), "--mtu", str(args.mtu)]
    result = subprocess.run(command)
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
