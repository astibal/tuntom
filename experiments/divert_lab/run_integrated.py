#!/usr/bin/env python3
"""Opt-in isolated network lab for the integrated ST/MP divert implementation."""
import argparse
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import run as lab_helpers
from run import Lab, run, require, wait_for, records, CLIENT, SERVER, PORT

HERE = Path(__file__).resolve().parent


def capture(interface):
    # SOCK_DGRAM removes the link header, including on TUN devices.
    with socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(0x0003)) as sock:
        sock.bind((interface, 0))
        print(json.dumps(dict(event="ready")), flush=True)
        while True:
            p = sock.recv(65535)
            if len(p) < 40 or p[0] >> 4 != 4 or p[9] != 6:
                continue
            ihl = (p[0] & 15) * 4
            if len(p) < ihl + 20:
                continue
            sport, dport, seq = struct.unpack_from("!HHI", p, ihl)
            if PORT not in (sport, dport):
                continue
            print(json.dumps(dict(sport=sport, dport=dport, seq=seq, flags=p[ihl + 13], ttl=p[8])), flush=True)


def inside(args):
    mapping = Path("/proc/self/uid_map").read_text().split()
    require(len(mapping) == 3 and mapping[0] == "0" and mapping[1] != "0" and mapping[2] == "1", "requires single-ID user namespace")
    require(os.readlink("/proc/self/ns/net") != os.readlink("/proc/1/ns/net"), "refusing initial network namespace")
    output, build = Path(args.output).resolve(), Path(args.build).resolve()
    require(not output.exists(), "output must be a new directory")
    output.mkdir(parents=True)
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
        for name, address, ns in (("lan0", "10.77.2.1/24", "exit"), ("lan1", SERVER + "/24", "server")):
            lab.ip("link", "set", name, "netns", str(lab.holders[ns].pid))
            lab.ip("addr", "add", address, "dev", name, ns=ns)
            lab.ip("link", "set", name, "mtu", args.mtu, "up", ns=ns)
        lab.ip("route", "add", CLIENT + "/32", "via", "10.77.2.1", ns="server")
        for ns in ("router", "exit"):
            lab.configure_router(ns)
        cookie = "".join(secrets.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(3))
        rules, config = lab.runtime / "switch.rules", lab.runtime / "divert.conf"
        rules.write_text("format 2\nserial 1\nexit exit\nswitch edge,[17,42] to exit,[99,42] allow bidir\n")
        config.write_text(f"cookie {cookie}\nports divert-in divert-out\norigin edge 123\nmatch edge,[17,42]\n")
        adapter_options = ["--cookie", cookie]
        if args.via:
            rules.write_text("format 3\nserial 1\nport edge id 123\nservice router {\n"
                " client-side divert-in\n server-side divert-out\n stickiness hash\n unavailable drop\n}\n"
                "exit exit\nswitch edge,[17,42] to exit,[99,42] allow bidir\n")
            config.write_text("format 3\nmatch edge,[17,42] via [router]\n")
            adapter_options = ["--via-instance", "router#0", "--admission", "warmup"]
        (output / "switch.rules").write_text(rules.read_text())
        (output / "divert.conf").write_text(config.read_text())
        classifier = lab.runtime / "ingress.classifier"
        classifier.write_text("format 1\nclassify ip4 to [17,42]\n")
        path = lab.runtime / "switch.sock"
        switch = build / ("tuntom-switch" if args.switch == "st" else "tomtom-switch-mp")
        extra = [] if args.switch == "st" else ["--workers", "2"]
        lab.start("switch", [switch, "--socket", path, "--control-socket", lab.runtime / "switch.control",
                            "--rules-file", rules, "--divert-file", config, *extra])
        wait_for(path.exists, lab.processes)
        lab.start("divert-adapter", [build / "tuntom-divert-adapter", "di0", "do0", "--switch-socket", path,
                  *adapter_options, "--mtu", args.mtu, "--control-socket", lab.runtime / "divert.control"], ns="router")
        lab.start("exit-adapter", [build / "tuntom-switch-adapter", "ex0", "--switch-socket", path,
                  "--switch-port-id", "exit", "--mtu", args.mtu, "--l4-only", "--l4-timeout", "86400",
                  "--control-socket", lab.runtime / "exit.control"], ns="exit")
        environment = os.environ | {"TUNTOM_SECRET": secrets.token_hex(16)}
        common = ["--mtu", args.mtu, "--transport-mtu", "1500", "--no-pmtud", "--no-ttl-compensate", "--no-stats", "--quiet"]
        lab.start("tuntom-server", [build / "tuntom-userns", "server", "231", "-", *common,
                  "--switch-socket", path, "--switch-port-id", "edge", "--switch-label", "17", "--classifier-file", classifier,
                  "--control-socket", lab.runtime / "server.control"], env=environment)
        lab.start("tuntom-client", [build / "tuntom-userns", "client", "231", "tc0", "192.0.2.1", *common,
                  "--control-socket", lab.runtime / "client.control"], ns="client", env=environment)
        def interface(ns, name):
            return subprocess.run(lab.command(["ip", "link", "show", name], ns), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        wait_for(lambda: all(interface(ns, name) for ns, name in (("client", "tc0"), ("router", "di0"), ("router", "do0"), ("exit", "ex0"))), lab.processes)
        if args.vrf:
            lab.ip("link", "add", "vrf-divert", "type", "vrf", "table", "4123", ns="router")
            lab.ip("link", "set", "vrf-divert", "up", ns="router")
            for name in ("di0", "do0"):
                lab.ip("link", "set", name, "master", "vrf-divert", ns="router")
            lab.ip("route", "add", "table", "4123", "unreachable", "default", ns="router")
        for ns, name, address, destination in (("client", "tc0", CLIENT + "/32", SERVER),
                ("router", "di0", "10.77.254.1/32", CLIENT), ("router", "do0", "10.77.254.2/32", SERVER),
                ("exit", "ex0", "10.77.254.3/32", CLIENT)):
            lab.ip("addr", "add", address, "dev", name, ns=ns)
            lab.ip("link", "set", name, "up", ns=ns)
            table = ["table", "4123"] if args.vrf and ns == "router" else []
            lab.ip("route", "add", *table, destination + "/32", "dev", name, ns=ns)
        def stats(which):
            text = run([build / "tuntomctl", lab.runtime / f"{which}.control", "show", "stats"]).stdout
            return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
        wait_for(lambda: (lab.runtime / "client.control").exists() and (lab.runtime / "server.control").exists(), lab.processes)
        wait_for(lambda: all(stats(which).get("session_confirmed") == "1" for which in ("client", "server")), lab.processes)
        wait_for(lambda: stats("switch").get("connections_current") == "4", lab.processes)
        captures = [lab.start(name, [sys.executable, __file__, "--capture", name], ns="router") for name in ("di0", "do0")]
        wait_for(lambda: all(records(output / f"{name}.log") for name in ("di0", "do0")), lab.processes)
        lab.start("tcp-server", [sys.executable, HERE / "run.py", "--server"], ns="server")
        wait_for(lambda: bool(records(output / "tcp-server.log")), lab.processes)
        driver = subprocess.run(lab.command([sys.executable, __file__, "--client", lab.runtime / "switch.control", "--build", build], "client"),
                                capture_output=True, text=True, timeout=45)
        (output / "tcp-client.log").write_text(driver.stdout + driver.stderr)
        require(driver.returncode == 0, "TCP failed: " + driver.stderr)
        time.sleep(.2)
        for capture in captures:
            capture.send_signal(signal.SIGTERM)
            capture.wait(timeout=3)
        di, do = [[r for r in records(output / f"{name}.log") if "sport" in r] for name in ("di0", "do0")]
        require(not any(40000 in (p["sport"], p["dport"]) for p in di + do), "existing TCP entered router")
        for port in (40001, 40002):
            for before, after, sport, dport, flags in ((di, do, port, PORT, 2), (do, di, PORT, port, 18)):
                a = [p for p in before if p["sport"] == sport and p["dport"] == dport and p["flags"] & 18 == flags]
                b = [p for p in after if p["sport"] == sport and p["dport"] == dport and p["flags"] & 18 == flags]
                require(any(x["seq"] == y["seq"] and x["ttl"] == y["ttl"] + 1 for x in a for y in b), "missing router TTL decrement")
        snapshots = {which: stats(which) for which in ("client", "server", "switch", "divert", "exit")}
        for which in ("switch", "divert"):
            for key, value in snapshots[which].items():
                if key.endswith("_drops") or key in ("tun_errors", "send_errors"):
                    require(value == "0", f"unexpected {which} {key}={value}")
        require(int(snapshots["divert"]["bypass_packets"]) > 0, "no existing TCP bypass")
        require(int(snapshots["divert"]["flow_entries"]) == 2, "wrong diverted flow count")
        if args.switch == "mp":
            require(snapshots["divert"]["divert_in_ipc_version"] == "2", "adapter did not negotiate IPC v2")
        accepted = [r["peer"] for r in records(output / "tcp-server.log") if r["event"] == "accepted"]
        require({tuple(p) for p in accepted} == {(CLIENT, p) for p in (40000,40001,40002)}, "source identity changed")
        if args.mtu > 1500:
            require(int(snapshots["client"]["fragments_tx"]) > 0, "no jumbo transport fragments")
        report = dict(status="PASS", via=args.via, switch=args.switch, mtu=args.mtu, vrf=args.vrf, tcp=json.loads(driver.stdout), server_peers=accepted, stats=snapshots)
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(dict(status="PASS", switch=args.switch, mtu=args.mtu, output=str(output))))
    except Exception as error:
        (output / "failure.txt").write_text(str(error) + "\n")
        raise
    finally:
        lab.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="/tmp/tuntom-divert-build")
    parser.add_argument("--output")
    parser.add_argument("--switch", choices=("st", "mp"), default="mp")
    parser.add_argument("--mtu", type=int, choices=(1500,9000), default=1500)
    parser.add_argument("--via", action="store_true", help="exercise VIA service mode with adapter warmup")
    parser.add_argument("--vrf", action="store_true", help="put the router TUNs in VRF table 4123 inside the isolated namespace")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--client", help=argparse.SUPPRESS)
    parser.add_argument("--capture", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.capture:
        return capture(args.capture)
    if args.client:
        return lab_helpers.client(None, lambda: run([Path(args.build) / "tuntomctl", args.client, "divert", "enable"]))
    require(args.output, "--output must name a new directory")
    if args.inside:
        return inside(args)
    subprocess.run(["unshare", "--user", "--map-root-user", "--net", sys.executable, __file__, "--inside",
                    "--build", str(Path(args.build).resolve()), "--output", str(Path(args.output).resolve()),
                    "--switch", args.switch, "--mtu", str(args.mtu), *(["--vrf"] if args.vrf else []), *(["--via"] if args.via else [])], check=True)


if __name__ == "__main__":
    main()
