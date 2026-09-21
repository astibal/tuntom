#!/usr/bin/env python3
"""Real classifier paths: encrypted UDP ingress and adapter TUN cache misses."""
import contextlib
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from switch_ruleset_integration_test import Switch, packet as frame
from switch_mp_integration_test import until
from switch_reconnect_test import snapshot
from switch_tunnel_test import terminate


def ipv4(sport=12345, dport=443, proto=6, fragment=0):
    return struct.pack("!BBHHHBBH4s4s", 0x45, 0, 40, 7, fragment, 64, proto, 0,
                       socket.inet_aton("10.0.0.7"), socket.inet_aton("192.0.2.200")) + \
        struct.pack("!HH", sport, dport) + bytes(16)


def ipv6():
    return struct.pack("!IHBB16s16s", 6 << 28, 16, 0, 64,
                       socket.inet_pton(socket.AF_INET6, "2001:db8::1"),
                       socket.inet_pton(socket.AF_INET6, "2001:db8::2")) + \
        bytes([17, 0, 0, 0, 0, 0, 0, 0]) + struct.pack("!HHHH", 12345, 53, 8, 0)


def label(text):
    return int.from_bytes(text.encode().ljust(8, b"\0"), "big")


def rules(origin):
    return f'''format 1
classify ip4 src 10.0.0.0/8 dst 192.0.2.128/25 proto tcp sport <1024,65535> dport 443 to ["{origin}", 99, b1000]
classify ip6 proto udp dport 53 to ["{origin}", 0xff, 6]
'''


def run(tuntom, switch_binary, adapter, ctl, mode):
    with tempfile.TemporaryDirectory(prefix="tuntom-classifier.") as directory, contextlib.ExitStack() as stack:
        root = Path(directory)
        sw = Switch(switch_binary, ctl, root, """format 2
serial 1
exit adapter
switch app,10 to client allow
switch app,[90,...] to adapter allow
switch adapter to sink allow
switch server to sink allow
""")
        stack.callback(sw.log.close); stack.callback(sw.stop)
        app, sink = sw.port("app"), sw.port("sink")
        tunnel_file, adapter_file = root / "tunnel.classifier", root / "adapter.classifier"
        tunnel_file.write_text(rules("TUNT")); adapter_file.write_text(rules("ADPT"))
        processes = []
        controls = {}

        def start(args, name, env=None, pass_fds=()):
            log = stack.enter_context(open(root / (name + ".log"), "w+"))
            child = subprocess.Popen(args, env=env, pass_fds=pass_fds,
                                     stdout=subprocess.DEVNULL, stderr=log)
            processes.append((child, log)); stack.callback(terminate, child)
            return child

        tunnel_env = dict(os.environ, TUNTOM_SECRET="00112233445566778899aabbccddeeff")
        for role, default_label in (("server", "2"), ("client", "1")):
            controls[role] = root / (role + ".ctl")
            args = [tuntom, role, "234", "-"] + (["127.0.0.1"] if role == "client" else [])
            args += ["--quiet", "--no-stats", "--no-pmtud", "--no-ttl-compensate",
                     "--switch-socket", str(sw.data), "--switch-port-id", role,
                     "--switch-label", default_label, "--switch-ipc", mode,
                     "--control-socket", str(controls[role])]
            if role == "server":
                args += ["--classifier-file", str(tunnel_file)]
            start(args, role, tunnel_env)
        tun, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.enter_context(tun); stack.enter_context(endpoint); tun.settimeout(3)
        controls["adapter"] = root / "adapter.ctl"
        start([adapter, "test-classify", "--switch-socket", str(sw.data), "--switch-port-id", "adapter",
               "--control-socket", str(controls["adapter"]), "--classifier-file", str(adapter_file),
               "--switch-ipc", mode, "--l3-capacity", "16", "--l4-capacity", "16"], "adapter",
              dict(os.environ, TUNTOM_TEST_TUN_FD=str(endpoint.fileno())), (endpoint.fileno(),))

        def ready():
            for child, log in processes:
                if child.poll() is not None:
                    log.seek(0); raise AssertionError(log.read())
            if not all(path.exists() for path in controls.values()):
                return False
            stats = {role: snapshot(str(path)) for role, path in controls.items()}
            return all(s["switch_connected"] == "1" for s in stats.values()) and \
                stats["client"]["session_confirmed"] == "1"

        until(ready, timeout=10)

        def stats(role):
            return snapshot(str(controls[role]))

        def expect(payload, labels, source):
            source.sendall(payload if source is tun else frame([10], payload))
            assert sink.recv(70000) == frame(labels, payload), (mode, labels)

        for payload, suffix in ((ipv4(), [99,8]), (ipv6(), [255,6])):
            expect(payload, [label("TUNT"), *suffix], app)
            expect(payload, [label("ADPT"), *suffix], tun)
        expect(ipv4(fragment=0x2000), [label("TUNT"),99,8], app)
        # UDP transport reassembly feeds the same classifier once per whole packet.
        jumbo = bytearray(ipv4()); jumbo.extend(bytes(1400)); struct.pack_into("!H", jumbo, 2, len(jumbo))
        expect(bytes(jumbo), [label("TUNT"),99,8], app)
        for unmatched in (ipv4(dport=80), ipv4(fragment=1), b"opaque payload"):
            expect(unmatched, [2], app)  # Existing tuntom default label.
            tun.sendall(unmatched)
            assert not select.select([sink], [], [], .08)[0], "adapter cache miss must not bypass classifier"
        adapter_stats = stats("adapter")
        assert adapter_stats["l3_entries"] == adapter_stats["l4_entries"] == "0", adapter_stats
        assert adapter_stats["classifier_hits"] == "2", adapter_stats
        assert adapter_stats["classifier_misses"] == "2", adapter_stats
        assert adapter_stats["classifier_parse_errors"] == "1", adapter_stats
        assert adapter_stats["ip_parse_errors"] == "1", adapter_stats
        assert adapter_stats["cache_miss_drops"] == "3", adapter_stats

        # Learn the reverse of a packet that otherwise matches classification.
        reply = ipv4()
        request = reply[:12] + reply[16:20] + reply[12:16] + reply[22:24] + reply[20:22] + reply[24:]
        learned = [90,17,8]
        app.sendall(frame(learned, request))
        assert tun.recv(70000) == request
        expect(reply, learned, tun)                 # L4 reverse hit takes priority.
        expect(ipv4(sport=20000), learned, tun)     # Existing L3 fallback takes priority too.
        after = stats("adapter")
        assert after["classifier_hits"] == adapter_stats["classifier_hits"], after
        assert after["classifier_misses"] == adapter_stats["classifier_misses"], after
        assert int(after["l4_hits"]) >= 1 and int(after["l3_hits"]) >= 1, after
        assert stats("client")["classifier_enabled"] == "0"
        def command(role, operation, text=None, success=True):
            args = [ctl, str(controls[role]), "classifier", operation]
            if text is not None:
                args.append("-")
            result = subprocess.run(args, input=text, text=True, capture_output=True, timeout=5)
            assert (result.returncode == 0) == success, result
            return result.stdout

        replacement = "format 1\nclassify to [77,88]\n"
        command("adapter", "check", replacement)
        assert command("adapter", "show") == rules("ADPT")
        command("adapter", "load-flush", "format 1\nclassify dport 99999 to 1\n", False)
        expect(reply, learned, tun)
        assert stats("adapter")["classifier_generation"] == "1"
        command("adapter", "load", replacement)
        expect(reply, learned, tun)
        command("adapter", "disable")
        expect(reply, learned, tun)
        command("adapter", "load", replacement)
        before_flush = stats("adapter")
        command("adapter", "load-flush", replacement)  # Identical config still flushes.
        flushed = stats("adapter")
        assert flushed["l3_entries"] == flushed["l4_entries"] == "0", flushed
        assert flushed["classifier_hits"] == before_flush["classifier_hits"]
        assert flushed["classifier_flushes"] == "1"
        expect(reply, [77,88], tun)
        expect(ipv4(sport=20000), [77,88], tun)
        app.sendall(frame(learned, request))
        assert tun.recv(70000) == request
        expect(reply, learned, tun)
        command("adapter", "load-flush", "format 1\n")
        tun.sendall(reply)
        assert not select.select([sink], [], [], .08)[0]
        command("server", "load-flush", replacement)
        expect(reply, [77,88], app)
        command("server", "load-flush", "invalid", False)
        expect(reply, [77,88], app)
        command("server", "disable")
        expect(reply, [2], app)
        command("client", "load", replacement)  # No startup classifier file.
        assert command("client", "show") == replacement
        assert stats("client")["classifier_enabled"] == "1"
        rejected = subprocess.run([ctl, str(sw.control), "classifier", "load", "-"],
                                  input=replacement, text=True, capture_output=True, timeout=5)
        assert rejected.returncode != 0
        if mode == "auto" and "mp" in Path(switch_binary).name:
            assert after["switch_ipc_mmap"] == "1", after
    print(f"PASS: classifier in tuntom and adapter, reverse cache precedence, IPC={mode}, {Path(switch_binary).name}")


def invalid_startup(tuntom, adapter):
    with tempfile.TemporaryDirectory(prefix="tuntom-classifier-invalid.") as directory:
        path = Path(directory) / "invalid.rules"
        path.write_text("format 1\nclassify dport 65536 to 1\n")
        common = ["--switch-socket", str(Path(directory) / "absent.sock"), "--switch-port-id", "test",
                  "--classifier-file", str(path)]
        for args in ([adapter, "test", *common], [tuntom, "server", "234", "-", *common, "--switch-label", "1"]):
            result = subprocess.run(args, capture_output=True, text=True, timeout=3)
            assert result.returncode != 0 and "classifier line 2" in result.stderr, result
            assert "TUN" not in result.stderr and "Cannot access" not in result.stderr, result.stderr


if __name__ == "__main__":
    tuntom, st, mp, adapter, ctl = sys.argv[1:6]
    invalid_startup(tuntom, adapter)
    for binary, mode in ((st, "v1"), (mp, "inline"), (mp, "auto")):
        run(tuntom, binary, adapter, ctl, mode)
