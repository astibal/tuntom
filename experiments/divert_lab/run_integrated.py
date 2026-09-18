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
        multipath = args.relay_paths > 1 or args.shared_flows
        paths = [(side, i) for side in (("in", "out") if args.split_sides else ("",)) for i in range(args.relay_paths)]
        pattern = "*" if multipath else ""
        new_ports = list(range(40001, 40003 + (16 if args.relay_paths > 1 else 0)))
        if args.via:
            rules.write_text("format 3\nserial 1\nport edge id 123\nservice router {\n"
                f" client-side divert-in{pattern}\n server-side divert-out{pattern}\n" +
                (" client-relay proxy-in-link*\n server-relay proxy-out-link*\n" if args.split_sides else
                 f" relay proxy-link{pattern}\n" if args.relay else "") +
                " stickiness hash\n unavailable drop\n}\n"
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
        adapter_connection = ["--switch-socket", path]
        relay_names = []
        if args.relay:
            lab.ip("link", "add", "relay0", "type", "veth", "peer", "name", "relay1")
            lab.ip("link", "set", "relay1", "netns", str(lab.holders["router"].pid))
            for name, address, ns in (("relay0", "192.0.2.5/30", None), ("relay1", "192.0.2.6/30", "router")):
                lab.ip("addr", "add", address, "dev", name, ns=ns)
                lab.ip("link", "set", name, "up", ns=ns)
            relay_env = os.environ | {"TUNTOM_SECRET": secrets.token_hex(16)}
            adapter_connection = []
            for tunnel_index, (side, i) in enumerate(paths):
                suffix = side + str(i) if multipath else ""
                parent = f"proxy-{side}-link{i}" if args.split_sides else "proxy-link" + suffix
                relay_names.append("relay" + suffix)
                adapter_socket = lab.runtime / f"relay{suffix}.sock"
                lab.start("relay-hub" + suffix, [build / "tuntom-userns", "server", str(232 + tunnel_index), "-",
                    "--relay-connect", path, "--relay-port-id", parent, "--no-stats"], env=relay_env)
                lab.start("relay-remote" + suffix, [build / "tuntom-userns", "client", str(232 + tunnel_index), "-", "192.0.2.5",
                    "--relay-listen", adapter_socket, "--control-socket", lab.runtime / f"relay{suffix}.control", "--no-stats"], ns="router", env=relay_env)
                wait_for(adapter_socket.exists, lab.processes)
                adapter_connection += ["--relay-path", f"{i}={adapter_socket}"] if multipath else ["--switch-socket", adapter_socket]
        divert_names = [f"divert{side}{i}" for side, i in paths] if args.shared_flows else ["divert"]
        for worker_index, name in enumerate(divert_names):
            side, i = paths[worker_index]
            connection = ["--relay-path", f"{i}={lab.runtime / f'relay{side}{i}.sock'}",
                          "--shared-flows", lab.runtime / "shared.flows"] if args.shared_flows else adapter_connection
            if args.split_sides: connection += ["--side", side]
            lab.start(name + "-adapter", [build / "tuntom-divert-adapter", "di0", "do0", *connection,
                      *adapter_options, "--mtu", args.mtu, "--control-socket", lab.runtime / f"{name}.control"], ns="router")
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
        wait_for(lambda: stats("switch").get("connections_current") == (str(2 + len(paths)) if args.relay else "4"), lab.processes)
        if args.relay:
            wait_for(lambda: all(stats(name).get("relay_acknowledged") == "1" and stats(name).get("relay_channels") == ("1" if args.split_sides else "2") for name in relay_names), lab.processes)
        if args.shared_flows:
            wait_for(lambda: all(stats(name).get("tun_queues_active") == "1" for name in divert_names), lab.processes)
        captures = [lab.start(name, [sys.executable, __file__, "--capture", name], ns="router") for name in ("di0", "do0")]
        wait_for(lambda: all(records(output / f"{name}.log") for name in ("di0", "do0")), lab.processes)
        lab.start("tcp-server", [sys.executable, HERE / "run.py", "--server"], ns="server")
        wait_for(lambda: bool(records(output / "tcp-server.log")), lab.processes)
        driver = subprocess.run(lab.command([sys.executable, __file__, "--client", lab.runtime / "switch.control", "--build", build,
                                "--relay-paths", args.relay_paths], "client"),
                                capture_output=True, text=True, timeout=45)
        (output / "tcp-client.log").write_text(driver.stdout + driver.stderr)
        require(driver.returncode == 0, "TCP failed: " + driver.stderr)
        time.sleep(.2)
        for capture in captures:
            capture.send_signal(signal.SIGTERM)
            capture.wait(timeout=3)
        di, do = [[r for r in records(output / f"{name}.log") if "sport" in r] for name in ("di0", "do0")]
        require(not any(40000 in (p["sport"], p["dport"]) for p in di + do), "existing TCP entered router")
        for port in new_ports:
            for before, after, sport, dport, flags in ((di, do, port, PORT, 2), (do, di, PORT, port, 18)):
                a = [p for p in before if p["sport"] == sport and p["dport"] == dport and p["flags"] & 18 == flags]
                b = [p for p in after if p["sport"] == sport and p["dport"] == dport and p["flags"] & 18 == flags]
                require(any(x["seq"] == y["seq"] and x["ttl"] == y["ttl"] + 1 for x in a for y in b), "missing router TTL decrement")
        snapshots = {which: stats(which) for which in ("client", "server", "switch", *divert_names, "exit")}
        for which in ("switch", *divert_names):
            for key, value in snapshots[which].items():
                if key.endswith("_drops") or key in ("tun_errors", "send_errors"):
                    require(value == "0", f"unexpected {which} {key}={value}")
        require(sum(int(snapshots[name]["bypass_packets"]) for name in divert_names) > 0, "no existing TCP bypass")
        require(all(int(snapshots[name]["flow_entries"]) == len(new_ports) for name in divert_names), "wrong diverted flow count")
        if multipath:
            key = "connected_paths" if args.split_sides else "connected_pairs"
            require(all(snapshots[name][key] == str(1 if args.shared_flows else args.relay_paths)
                        for name in divert_names), "missing relay path")
            for worker_side, i in paths:
                snapshot = snapshots[f"divert{worker_side}{i}" if args.shared_flows else "divert"]
                for side in ((worker_side,) if args.split_sides else ("in", "out")):
                    prefix = f"relay_path_{i}_{side}_ipc_"
                    require(snapshot[prefix + "version"] == "2", "relay did not negotiate IPC v2")
                    require(int(snapshot[prefix + "rx_inline_frames"]) > 0, "unused relay path")
                    require(int(snapshot[prefix + "tx_inline_frames"]) > 0, "unused return path")
        elif args.switch == "mp" or args.relay:
            require(snapshots["divert"]["divert_in_ipc_version"] == "2", "adapter did not negotiate IPC v2")
        accepted = [r["peer"] for r in records(output / "tcp-server.log") if r["event"] == "accepted"]
        require({tuple(p) for p in accepted} == {(CLIENT, p) for p in [40000, *new_ports]}, "source identity changed")
        if args.mtu > 1500:
            require(int(snapshots["client"]["fragments_tx"]) > 0, "no jumbo transport fragments")
        report = dict(status="PASS", relay=args.relay, relay_paths=args.relay_paths if args.relay else 0, shared_flows=args.shared_flows,
                      split_sides=args.split_sides, via=args.via, switch=args.switch, mtu=args.mtu, vrf=args.vrf, tcp=json.loads(driver.stdout), server_peers=accepted, stats=snapshots)
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
    parser.add_argument("--relay", action="store_true", help="put the VIA adapter behind a tuntom tunnel in the router namespace")
    parser.add_argument("--relay-paths", type=int, choices=range(1, 17), default=1, help="independent relay tunnels; values above 1 imply --relay")
    parser.add_argument("--shared-flows", action="store_true", help="one adapter process per relay tunnel, shared flow state and multiqueue TUNs")
    parser.add_argument("--split-sides", action="store_true", help="separate IN/OUT workers and relays; --relay-paths counts paths per side")
    parser.add_argument("--via", action="store_true", help="exercise VIA service mode with adapter warmup")
    parser.add_argument("--vrf", action="store_true", help="put the router TUNs in VRF table 4123 inside the isolated namespace")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--client", help=argparse.SUPPRESS)
    parser.add_argument("--capture", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.split_sides:
        require(args.relay_paths <= 8, "split groups support at most 16 workers in total")
        args.shared_flows = True
    if args.relay_paths > 1 or args.shared_flows: args.relay = True
    if args.relay: args.via = True
    if args.capture:
        return capture(args.capture)
    if args.client:
        return lab_helpers.client(None, lambda: run([Path(args.build) / "tuntomctl", args.client, "divert", "enable"]),
                                  extra_flows=16 if args.relay_paths > 1 else 0)
    require(args.output, "--output must name a new directory")
    if args.inside:
        return inside(args)
    subprocess.run(["unshare", "--user", "--map-root-user", "--net", sys.executable, __file__, "--inside",
                    "--build", str(Path(args.build).resolve()), "--output", str(Path(args.output).resolve()),
                    "--switch", args.switch, "--mtu", str(args.mtu), "--relay-paths", str(args.relay_paths),
                    *(["--vrf"] if args.vrf else []), *(["--via"] if args.via else []), *(["--relay"] if args.relay else []),
                    *(["--shared-flows"] if args.shared_flows else []), *(["--split-sides"] if args.split_sides else [])], check=True)


if __name__ == "__main__":
    main()
