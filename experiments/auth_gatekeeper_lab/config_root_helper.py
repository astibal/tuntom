#!/usr/bin/env python3
"""Minimal Linux CONFIG broker for the AUTH lab."""
import ipaddress
import json
import os
import pwd
import re
import socket
import struct
import subprocess
import sys

MAX_CONFIG = 16384

def decode(wire):
    if len(wire) < 16 or len(wire) > MAX_CONFIG or wire[:4] != b"\x01\0\0\0":
        raise ValueError("bad CONFIG header")
    config_id, count, reserved = struct.unpack_from("!QHH", wire, 4)
    if not config_id or reserved:
        raise ValueError("bad CONFIG id")
    items, at = [], 16
    for _ in range(count):
        if at + 8 > len(wire): raise ValueError("short CONFIG item")
        kind, flags, size, reserved = struct.unpack_from("!HHHH", wire, at)
        at += 8
        if flags > 1 or reserved or at + size > len(wire): raise ValueError("bad CONFIG item")
        items.append((kind, bool(flags), wire[at:at + size]))
        at += size
    if at != len(wire): raise ValueError("trailing CONFIG bytes")
    return config_id, items

def network(value, version):
    size = 4 if version == 4 else 16
    if len(value) != size + 1: raise ValueError("bad network value")
    return str(ipaddress.ip_network((ipaddress.ip_address(value[:size]), value[-1]), strict=False))

def address(value, version):
    size = 4 if version == 4 else 16
    if len(value) != size + 1: raise ValueError("bad address value")
    if value[-1] > (32 if version == 4 else 128): raise ValueError("bad prefix")
    return f"{ipaddress.ip_address(value[:size])}/{value[-1]}"

def run(*args):
    result = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{' '.join(args)}: {result.stderr.strip() or 'exit '+str(result.returncode)}")

def preserve_endpoint(endpoint):
    result = subprocess.run(["ip", "-j", "route", "get", endpoint], check=True,
                            capture_output=True, text=True)
    route = json.loads(result.stdout)[0]
    command = ["ip", "route", "replace", endpoint + "/32"]
    if route.get("gateway"): command += ["via", route["gateway"]]
    command += ["dev", route["dev"]]
    if route.get("prefsrc"): command += ["src", route["prefsrc"]]
    run(*command)

def apply_config(interface, endpoint, wire):
    _, items = decode(wire)
    actions, default4 = [], False
    for kind, required, value in items:
        if kind == 1: actions.append(("address", 4, address(value, 4)))
        elif kind == 2: actions.append(("address", 6, address(value, 6)))
        elif kind == 3:
            route = network(value, 4); actions.append(("route", 4, route)); default4 |= route == "0.0.0.0/0"
        elif kind == 4: actions.append(("route", 6, network(value, 6)))
        elif kind in (5, 6, 7, 8, 9):
            if required: raise ValueError(f"required CONFIG type {kind} is not supported by lab helper")
        elif kind == 10:
            if len(value) != 2: raise ValueError("bad MTU")
            mtu = struct.unpack("!H", value)[0]
            if mtu < 576: raise ValueError("bad MTU")
            actions.append(("mtu", 0, str(mtu)))
        elif required: raise ValueError(f"unknown required CONFIG type {kind}")
    if default4: preserve_endpoint(endpoint)
    for action, version, value in actions:
        if action == "address": run("ip", f"-{version}", "address", "replace", value, "dev", interface)
        elif action == "mtu": run("ip", "link", "set", "dev", interface, "mtu", value)
    run("ip", "link", "set", "dev", interface, "up")
    for action, version, value in actions:
        if action == "route": run("ip", f"-{version}", "route", "replace", value, "dev", interface)

def serve(path, interface, endpoint):
    if os.geteuid() != 0: raise SystemExit("daemon mode requires root")
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", interface): raise SystemExit("invalid interface")
    try: os.unlink(path)
    except FileNotFoundError: pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(path)
    gid = pwd.getpwnam("tuntom").pw_gid
    os.chown(path, 0, gid); os.chmod(path, 0o660); server.listen(4)
    allowed_uid = pwd.getpwnam("tuntom").pw_uid
    while True:
        connection, _ = server.accept()
        try:
            _, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if uid not in (0, allowed_uid): raise PermissionError("unauthorized CONFIG client")
            wire = connection.recv(MAX_CONFIG + 1)
            apply_config(interface, endpoint, wire)
            connection.sendall(b"OK")
        except Exception as error:
            connection.sendall(("ERROR " + str(error))[:512].encode())
        finally: connection.close()

def submit(path):
    wire = sys.stdin.buffer.read(MAX_CONFIG + 1)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); client.connect(path)
    client.sendall(wire); result = client.recv(512)
    if result != b"OK":
        print(result.decode(errors="replace"), file=sys.stderr); raise SystemExit(1)

def main():
    if len(sys.argv) == 2 and sys.argv[1] == "config": submit(os.environ["TUNTOM_CONFIG_SOCKET"])
    elif len(sys.argv) == 5 and sys.argv[1] == "daemon": serve(sys.argv[2], sys.argv[3], sys.argv[4])
    else: raise SystemExit("usage: config_root_helper.py daemon SOCKET IFACE ENDPOINT | config")

if __name__ == "__main__": main()
