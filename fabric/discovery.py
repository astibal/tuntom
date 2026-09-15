"""Discover running daemons from /proc; no inventory, environment or state files."""
from __future__ import annotations

from dataclasses import asdict, dataclass, field
import os
from pathlib import Path
import re
import socket

# Only explicitly public options are returned. In particular, --cookie and any
# future secret/password options never enter the API or a reconstructed command.
VALUE_OPTIONS = set("""control-socket stats-file stats-format socket switch-socket
switch-port-id switch-label switch-ipc switch-ipc-batch classifier-file rules-file
divert-file divert-in-port divert-out-port mtu transport-mtu init-window workers
pool-size queue-size ipc-mode ipc-batch reserve-cpus flow-capacity flow-idle-seconds
admission-capacity default-back port-id label exit-port trunk-port""".split())
FLAG_OPTIONS = set("""no-stats crypto-auth-only pmtud no-pmtud no-ttl-compensate
switch-exit-node quiet debug auto-pool""".split())


@dataclass
class Endpoint:
    id: str
    pid: int
    start_ticks: int
    host: str
    kind: str
    name: str
    executable: str
    uid: int
    state: str
    uptime_seconds: int
    rss_bytes: int
    threads: int
    control: str = ""
    switch_socket: str = ""
    port_id: str = ""
    interface: str = ""
    role: str = ""
    peer: str = ""
    unit: str = ""
    mount_namespace: str = ""
    net_namespace: str = ""
    options: dict = field(default_factory=dict)
    notes: list = field(default_factory=list)


def options(argv):
    result = {}
    index = 1
    while index < len(argv):
        token = argv[index]
        if token.startswith("--"):
            key, equals, value = token[2:].partition("=")
            if key in FLAG_OPTIONS:
                result[key] = True
            elif key in VALUE_OPTIONS:
                if not equals and index + 1 < len(argv):
                    index += 1
                    value = argv[index]
                if equals or value:
                    result[key] = value
        index += 1
    return result


def kind_of(executable, argv, comm):
    names = {Path(executable.removesuffix(" (deleted)")).name, Path(argv[0]).name if argv else "", comm}
    if names & {"tuntomctl", "tuntom-fabric"}:
        return None
    if names & {"tuntom-divert"}:
        return "divert"
    if names & {"tuntom-switch-adapter", "tuntom-adapter"}:
        return "adapter"
    if names & {"tuntom-switch", "tomtom-switch", "tomtom-switch-mp", "tuntom-switch-mp"}:
        return "switch"
    tunnel_name = any(re.fullmatch(r"tuntom(?:_[0-9]+(?:_[0-9]+)?[cs])?", n) for n in names)
    deployed_main = "main" in names and any("tuntom" in p for p in Path(executable).parts)
    if tunnel_name or deployed_main:
        if len(argv) > 1 and argv[1] in ("client", "server"):
            return "tunnel"
        if "--socket" in argv:
            return "switch"
        if "--switch-socket" in argv:
            return "adapter"
        return "process"
    return None


def read_link(path):
    try:
        return os.readlink(path)
    except OSError:
        return ""


def stat_fields(path):
    # comm can contain spaces and parentheses; the final ')' delimits it.
    value = path.read_text()
    return value[value.rindex(")") + 2:].split()


def inspect_process(directory, *, host, boot, uptime, ticks, page_size):
    before = stat_fields(directory / "stat")
    if before[0] in ("Z", "X"):
        return None
    comm = (directory / "comm").read_text().strip()
    try:
        raw = (directory / "cmdline").read_bytes()[:262144]
        argv = raw.rstrip(b"\0").decode("utf-8", "replace").split("\0") if raw else []
    except PermissionError:
        argv = []
    executable = read_link(directory / "exe") or (argv[0] if argv else comm)
    kind = kind_of(executable, argv, comm)
    if not kind:
        return None
    opts = options(argv)
    cwd = read_link(directory / "cwd")
    notes = []

    def path_option(key):
        value = opts.get(key, "")
        if not value:
            return ""
        if os.path.isabs(value):
            return value
        if cwd:
            return os.path.normpath(os.path.join(cwd, value))
        notes.append(f"Cannot resolve relative --{key}: process cwd is inaccessible")
        return ""

    role = argv[1] if kind == "tunnel" and len(argv) > 1 else ""
    interface = argv[3] if kind == "tunnel" and len(argv) > 3 else (
        argv[1] if kind in ("adapter", "divert") and len(argv) > 1 else "")
    peer = argv[4] if role == "client" and len(argv) > 4 else ""
    control = path_option("control-socket")
    switch_socket = path_option("socket" if kind == "switch" else "switch-socket")
    endpoint_name = (argv[2] + ("c" if role == "client" else "s")) if kind == "tunnel" and len(argv) > 2 else (
        Path(switch_socket).stem if kind == "switch" and switch_socket else interface or comm)
    if not argv:
        notes.append("Process arguments are inaccessible")
    if not control:
        notes.append("No accessible --control-socket; process information only")
    try:
        cgroup = (directory / "cgroup").read_text()
        units = re.findall(r"(?:^|/)([^/\n]+\.service)(?:/|$)", cgroup, re.M)
    except OSError:
        units = []
    after = stat_fields(directory / "stat")
    if before[19] != after[19]:
        return None
    return Endpoint(
        id=f"{boot}:{directory.name}:{before[19]}", pid=int(directory.name), start_ticks=int(before[19]),
        host=host, kind=kind, name=endpoint_name, executable=executable, uid=directory.stat().st_uid,
        state=after[0], uptime_seconds=max(0, int(uptime - int(before[19]) / ticks)),
        rss_bytes=max(0, int(after[21])) * page_size, threads=int(after[17]), control=control,
        switch_socket=switch_socket, port_id=opts.get("switch-port-id", opts.get("port-id", "")),
        interface=interface, role=role, peer=peer, unit=units[-1] if units else "",
        mount_namespace=read_link(directory / "ns/mnt"), net_namespace=read_link(directory / "ns/net"),
        options=opts, notes=notes)


def discover(proc=Path("/proc")):
    host = socket.gethostname()
    boot = (proc / "sys/kernel/random/boot_id").read_text().strip()
    uptime = float((proc / "uptime").read_text().split()[0])
    result, denied = [], 0
    for directory in sorted(proc.iterdir(), key=lambda p: int(p.name) if p.name.isdigit() else -1):
        if not directory.name.isdigit():
            continue
        try:
            endpoint = inspect_process(directory, host=host, boot=boot, uptime=uptime,
                                       ticks=os.sysconf("SC_CLK_TCK"), page_size=os.sysconf("SC_PAGE_SIZE"))
            if endpoint:
                result.append(endpoint)
        except PermissionError:
            denied += 1
        except (OSError, ValueError, IndexError):
            # Exiting processes are expected during a scan.
            continue
    return result, {"host": host, "source": "procfs", "permission_denied": denied,
                    "mount_namespace": read_link(proc / "self/ns/mnt")}


if __name__ == "__main__":
    import json
    endpoints, info = discover()
    print(json.dumps({"discovery": info, "endpoints": [asdict(e) for e in endpoints]}, indent=2))
