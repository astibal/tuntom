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
        for arguments in (("--workers", "0"), ("--pool-size", "65536"), ("--queue-size", "-1"),
                          ("--tx-weight",), ("--reserve-cpus", "1"), ("--stop",),
                          ("--auto-pool", "--reserve-cpus", str(available)),
                          ("--exit-port", "same", "--trunk-port", "same"),
                          ("--route", "a:1=b:2", "--route", "a:1=c:2"), ("--route", "bad"),
                          ("--unknown", "1")):
            run("--dry-run", *arguments, ok=False)
        rules.write_text("unknown nope\n")
        run("--dry-run", "--rules-file", rules, ok=False)
        print("PASS: MP dry-run, options, rules, manual precedence and no deployment side effects")


if __name__ == "__main__":
    main()
