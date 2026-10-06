#!/usr/bin/env python3
"""Real TUN/netns/fd-transfer and privilege-drop regression test."""

import grp
import os
import pwd
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True


SKIP = 77
TUNNEL_ID = "246"
CLIENT_IF = "ut246c"
SERVER_IF = "ut246s"


def skip(reason):
    print(f"SKIP: {reason}")
    raise SystemExit(SKIP)


def id_is_mapped(path, wanted):
    try:
        for line in Path(path).read_text().splitlines():
            inside, _outside, count = map(int, line.split())
            if inside <= wanted < inside + count:
                return True
    except (OSError, ValueError):
        return False
    return False


def prerequisites():
    if sys.platform != "linux" or os.geteuid() != 0:
        skip("requires Linux root privileges")
    for command in ("ip", "nsenter", "ping", "unshare"):
        if not shutil.which(command):
            skip(f"requires {command}")
    device = Path("/dev/net/tun")
    if not device.exists() or not stat.S_ISCHR(device.stat().st_mode):
        skip("requires /dev/net/tun")
    try:
        user = pwd.getpwnam("tuntom")
        group = grp.getgrnam("tuntom")
    except KeyError:
        skip("requires the tuntom user and group")
    if not id_is_mapped("/proc/self/uid_map", user.pw_uid):
        skip("tuntom UID is not mapped in this user namespace")
    if not id_is_mapped("/proc/self/gid_map", group.gr_gid):
        skip("tuntom GID is not mapped in this user namespace")
    probe = subprocess.run(["unshare", "--net", "true"], capture_output=True)
    if probe.returncode:
        skip("requires permission to create and enter network namespaces")
    return user.pw_uid, group.gr_gid


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def ns_command(holder, *arguments, **kwargs):
    kwargs.setdefault("check", True)
    return subprocess.run(
        ["nsenter", f"--net=/proc/{holder.pid}/ns/net", *arguments],
        **kwargs)


def process_error(process, log):
    if process.poll() is None:
        return ""
    log.flush()
    return f"exited with {process.returncode}:\n{Path(log.name).read_text(errors='replace')}"


def wait_for_interface(holder, name, processes):
    for _ in range(300):
        errors = [process_error(process, log) for process, log in processes]
        if any(errors):
            raise RuntimeError(next(error for error in errors if error))
        result = ns_command(holder, "ip", "link", "show", "dev", name,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            check=False)
        if result.returncode == 0:
            return
        time.sleep(0.02)
    raise RuntimeError(f"interface {name} was not created in target namespace")


def status_fields(pid):
    fields = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            fields[key] = value.strip()
    return fields


def wait_for_privilege_drop(process, expected_uid, expected_gid, log):
    for _ in range(300):
        error = process_error(process, log)
        if error:
            raise RuntimeError(error)
        fields = status_fields(process.pid)
        uids = [int(value) for value in fields["Uid"].split()]
        gids = [int(value) for value in fields["Gid"].split()]
        if all(value == expected_uid for value in uids) and all(value == expected_gid for value in gids):
            if fields.get("NoNewPrivs") != "1":
                raise RuntimeError("tuntom dropped UID/GID without NoNewPrivs")
            if "0" in fields.get("Groups", "").split():
                raise RuntimeError("tuntom retained root supplementary group")
            return
        time.sleep(0.02)
    raise RuntimeError("tuntom did not drop to the runtime UID/GID")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <tuntom>")
    tuntom = str(Path(sys.argv[1]).resolve())
    expected_uid, expected_gid = prerequisites()
    holders = []
    tunnels = []
    logs = []
    with tempfile.TemporaryDirectory(prefix="tuntom-netns-test.") as directory:
        try:
            for _ in range(2):
                holder = subprocess.Popen(["unshare", "--net", "sleep", "300"])
                holders.append(holder)
            time.sleep(0.05)
            if any(holder.poll() is not None for holder in holders):
                raise RuntimeError("network namespace holder failed")

            environment = os.environ.copy()
            environment["TUNTOM_SECRET"] = "00112233445566778899aabbccddeeff"
            server_log = open(Path(directory) / "server.log", "w+")
            client_log = open(Path(directory) / "client.log", "w+")
            logs += [server_log, client_log]
            server = subprocess.Popen([
                tuntom, "server", TUNNEL_ID, SERVER_IF,
                "--quiet", "--no-stats", "--no-pmtud",
                "--tun-netns", f"pid:{holders[1].pid}", "--tun-up",
            ], env=environment, stdout=subprocess.DEVNULL, stderr=server_log)
            tunnels.append(server)
            client = subprocess.Popen([
                tuntom, "client", TUNNEL_ID, CLIENT_IF, "127.0.0.1",
                "--quiet", "--no-stats", "--no-pmtud",
                "--tun-netns", f"pid:{holders[0].pid}", "--tun-up",
            ], env=environment, stdout=subprocess.DEVNULL, stderr=client_log)
            tunnels.append(client)
            pairs = [(server, server_log), (client, client_log)]

            wait_for_interface(holders[0], CLIENT_IF, pairs)
            wait_for_interface(holders[1], SERVER_IF, pairs)
            wait_for_privilege_drop(server, expected_uid, expected_gid, server_log)
            wait_for_privilege_drop(client, expected_uid, expected_gid, client_log)

            ns_command(holders[0], "ip", "address", "add", "10.253.246.1",
                       "peer", "10.253.246.2", "dev", CLIENT_IF)
            ns_command(holders[1], "ip", "address", "add", "10.253.246.2",
                       "peer", "10.253.246.1", "dev", SERVER_IF)
            ns_command(holders[0], "ip", "link", "set", "dev", CLIENT_IF, "up")
            ns_command(holders[1], "ip", "link", "set", "dev", SERVER_IF, "up")

            result = ns_command(
                holders[0], "ping", "-n", "-c", "3", "-W", "1", "10.253.246.2",
                capture_output=True, text=True, check=False)
            if result.returncode:
                details = "\n".join(Path(log.name).read_text(errors="replace") for log in logs)
                raise RuntimeError(f"ping did not cross tuntom tunnel:\n{result.stdout}{result.stderr}\n{details}")
        finally:
            for process in reversed(tunnels):
                stop(process)
            for process in reversed(holders):
                stop(process)
            for log in logs:
                log.close()
    print("PASS: forked netns TUN fd transfer, privilege drop and encrypted tunnel traffic")


if __name__ == "__main__":
    main()
