#!/usr/bin/env python3
"""Read-only MP helper planning and CLI checks; never invoke real sudo or daemons."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix="tmk-mp-plan-") as tmp:
        directory = Path(tmp)
        planner = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else directory / "planner"
        if len(sys.argv) != 2:
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-pthread", "-O2",
                            str(ROOT / "tools/switch_mp_plan.cpp"), "-o", str(planner)], check=True)
        compiler = directory / "compiler"
        compiler.write_text('''#!/usr/bin/env bash
set -euo pipefail
[[ "$*" == *tools/switch_mp_plan.cpp* ]] || exit 91
cp -- "$TEST_PLANNER" "${@: -1}"
''')
        compiler.chmod(0o700)
        sudo = directory / "sudo"
        sudo.write_text('#!/bin/sh\necho "unexpected sudo" >&2\nexit 92\n')
        sudo.chmod(0o700)
        marker = directory / "hook-ran"
        hook = directory / "hook with spaces.sh"
        hook.write_text(f"touch {shlex.quote(str(marker))}\n")
        env = os.environ | {"CXX": str(compiler), "TEST_PLANNER": str(planner),
                           "PATH": f"{directory}:{os.environ['PATH']}", "TMPDIR": str(directory),
                           "TUNTOM_RUN_DIR": str(directory / "run"),
                           "TUNTOM_STATE_DIR": str(directory / "state"),
                           "TUNTOM_BIN_DIR": str(directory / "bin"),
                           "TUNTOM_SWITCH_RULES_FILE": ""}

        def run(*args, ok=True, affinity=None):
            result = subprocess.run(["bash", str(ROOT / "mk_switch_mp.sh"), "demo", *map(str, args)],
                                    env=env, capture_output=True, text=True, timeout=10,
                                    preexec_fn=(lambda: os.sched_setaffinity(0, affinity)) if affinity else None)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            assert not marker.exists()
            assert all(not (directory / name).exists() for name in ("run", "state", "bin"))
            assert not list(directory.glob("tuntom-mp-plan.*")), "Planner temporary files leaked"
            return dict(line.split("=", 1) for line in result.stdout.splitlines()
                        if "=" in line and not line.startswith("Command:")), result.stdout

        fields, _ = run("--dry-run", "--auto-pool", "--pre-hook", hook)
        available = int(fields["hardware.worker_limit"])
        assert int(fields["workers.pool"]) == max(1, available // 2)
        assert fields["option.adapter-weight"] == "4" and fields["option.trunk-weight"] == "8"
        allowed = sorted(os.sched_getaffinity(0))
        fields, _ = run("--dry-run", "--auto-pool", affinity={allowed[0]})
        assert fields["hardware.logical_cpus"] == fields["hardware.physical_cores"] == fields["workers.pool"] == "1"
        cores = {}
        for cpu in allowed:
            base = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
            try:
                key = ((base / "physical_package_id").read_text(), (base / "core_id").read_text())
            except OSError:
                continue
            cores.setdefault(key, []).append(cpu)
        siblings = next((cpus[:2] for cpus in cores.values() if len(cpus) >= 2), None)
        if siblings:
            fields, _ = run("--dry-run", "--auto-pool", affinity=set(siblings))
            assert fields["hardware.logical_cpus"] == "2" and fields["hardware.physical_cores"] == "1"
            assert fields["workers.pool"] == "1"
        fields, _ = run("--dry-run", "--auto-pool", "--reserve-cpus", "0", "--workers", "1",
                        "--work-per-thread", "6", "--rx-weight", "2", "--tx-weight", "3",
                        "--adapter-weight", "7", "--trunk-weight", "9", "--pool-size", "4",
                        "--queue-size", "8", "--max-ports", "40", "--max-pending", "2",
                        "--exit-port", "adapter0", "--trunk-port", "trunk0", "--default-back=on")
        assert fields["workers.pool"] == "1" and fields["worker.0"] == "RX,TX,RXa,TXa"
        assert [fields[f"option.{key}"] for key in ("work-per-thread", "rx-weight", "tx-weight",
                                                    "adapter-weight", "trunk-weight")] == ["6", "2", "3", "7", "9"]
        rules = directory / "rules with spaces"
        rules.write_text("# test\nroute t0:1=a0:2\nroute t0:3=a0:4\nexit-port a0\ntrunk-port trunk0\n")
        fields, output = run("--dry-run", "--auto-pool", "--rules-file", rules,
                             "--route", "t1:1=a0:2", "--pre-hook", hook)
        assert [fields[f"configured.{key}"] for key in ("tunnels", "adapters", "trunks")] == ["2", "1", "1"]
        assert "pre/up hooks are not executed" in output
        command = next(line for line in output.splitlines() if line.startswith("Command:"))
        assert shlex.split(command)[1] == str(directory / "bin/switch-demo/main")
        fields, _ = run("--dry-run", "--workers", "1")
        assert fields["option.adapter-weight"] == "2" and fields["reserved_cpus"] == "0"
        _, output = run("--dry-run", "--auto-pool", "--ipc-mode", "inline", "--ipc-batch", "16",
                        "--ipc-slots", "32", "--ipc-frame-capacity", "65607", "--ipc-memory-mib", "512")
        command = shlex.split(next(line for line in output.splitlines() if line.startswith("Command:")))
        for option, value in (("--ipc-mode", "inline"), ("--ipc-batch", "16"), ("--ipc-slots", "32"),
                              ("--ipc-frame-capacity", "65607"), ("--ipc-memory-mib", "512")):
            assert command[command.index(option) + 1] == value
        for option, value in (("--ipc-mode", "wrong"), ("--ipc-batch", "17"), ("--ipc-slots", "129"),
                              ("--ipc-frame-capacity", "16"), ("--ipc-frame-capacity", "65608")):
            run("--dry-run", option, value, ok=False)
        for arguments in (("--workers", "0"), ("--pool-size", "65536"), ("--queue-size", "-1"),
                          ("--tx-weight",), ("--reserve-cpus", "1"), ("--stop",),
                          ("--auto-pool", "--reserve-cpus", str(available)),
                          ("--exit-port", "same", "--trunk-port", "same"),
                          ("--route", "a:1=b:2", "--route", "a:1=c:2"), ("--route", "bad"),
                          ("--unknown", "1")):
            run("--dry-run", *arguments, ok=False)
        rules.write_text("route internet:1001=edge-42*:44\nroute edge-42*:17=internet:1001\nexit-port internet\n")
        fields, output = run("--dry-run", "--rules-file", rules)
        assert fields["configured.wildcard_patterns"] == "1"
        assert fields["configured.port_counts"] == "lower_bound" and fields["configured.tunnels"] == "0"
        command = shlex.split(next(line for line in output.splitlines() if line.startswith("Command:")))
        assert "internet:1001=edge-42*:44" in command and "edge-42*:17=internet:1001" in command
        rules.write_text("# format-1\nformat 1 # inline\nserial 10\nexit internet*\ntrunk backbone\n"
                         "label client,17 to internet*, [1001,...]\nswitch allow\n")
        fields, output = run("--dry-run", "--auto-pool", "--rules-file", rules)
        assert fields["configured.wildcard_patterns"] == "1"
        assert fields["configured.tunnels"] == "1" and fields["configured.trunks"] == "1"
        command = shlex.split(next(line for line in output.splitlines() if line.startswith("Command:")))
        assert command[command.index("--rules-file") + 1] == str(rules)
        run("--dry-run", "--rules-file", rules, "--route", "a:1=b:2", ok=False)
        rules.write_text("format 2\nserial 11\nexit internet*\ntrunk backbone\n"
                         "switch client,[42,&16,...] to internet*,[99,*,...] allow bidir\n")
        fields, output = run("--dry-run", "--auto-pool", "--rules-file", rules)
        assert fields["configured.wildcard_patterns"] == "1"
        assert fields["configured.tunnels"] == "1" and fields["configured.trunks"] == "1"
        command = shlex.split(next(line for line in output.splitlines() if line.startswith("Command:")))
        assert command[command.index("--rules-file") + 1] == str(rules)
        divert = directory / "divert with spaces"
        divert.write_text("cookie hTX\nports divert-in divert-out\norigin client 123\n"
                          "origin other 456\nmatch client,[42,...]\n")
        for automatic in ((), ("--auto-pool",)):
            fields, output = run("--dry-run", *automatic, "--rules-file", rules,
                                 "--divert-file", divert, "--pre-hook", hook)
            assert fields["configured.adapters"] == "2" and fields["configured.tunnels"] == "2"
            assert fields["configured.trunks"] == "1"
            command = shlex.split(next(line for line in output.splitlines() if line.startswith("Command:")))
            assert command[command.index("--divert-file") + 1] == str(divert)
            assert command[command.index("--control-socket") + 1] == str(directory / "run/demo.control")
        run("--dry-run", "--divert-file", ok=False)
        run("--dry-run", "--divert-file", divert, ok=False)
        run("--dry-run", "--rules-file", rules, "--divert-file", directory / "missing", ok=False)
        divert.write_text("cookie invalid\nports divert-in divert-out\norigin client 123\nmatch client\n")
        run("--dry-run", "--rules-file", rules, "--divert-file", divert, ok=False)
        rules.write_text('format 3\nserial 12\nport client id 123\nservice proxy {\n'
                         ' client-side proxy-in\n server-side proxy-out\n stickiness failover\n'
                         ' instances ["proxy#0", "proxy#1"]\n unavailable drop\n}\n'
                         'exit internet\nswitch client,[17,42] to internet,[99,42] via [proxy] allow bidir\n')
        divert.write_text('format 3\nmatch client,[17,42] via [proxy]\n')
        fields, output = run("--dry-run", "--auto-pool", "--rules-file", rules, "--divert-file", divert)
        assert fields["configured.adapters"] == "5" and fields["configured.tunnels"] == "1"
        rules.write_text("format 1\nserial 12\nswitch capture\n")
        run("--dry-run", "--rules-file", rules, ok=False)
        rules.write_text("unknown nope\n")
        run("--dry-run", "--rules-file", rules, ok=False)
        print("PASS: MP dry-run, options, rules, manual precedence and no deployment side effects")


if __name__ == "__main__":
    main()
