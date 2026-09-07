#!/usr/bin/env python3
"""Exercise mk_tunnel switch option parsing without root, SSH or networking."""
import pathlib
import subprocess


root = pathlib.Path(__file__).resolve().parent.parent
source = (root / "mk_tunnel.sh").read_text()
prefix = source[:source.index('if ! [[ "$id" =~')]
report = prefix + r'''
printf '%s\n' \
  "client_socket=$client_switch_socket" \
  "client_port=$client_switch_port_id" \
  "client_label=$client_switch_label" \
  "client_exit=$client_switch_exit_node" \
  "client_tun=$client_has_tun" \
  "server_socket=$server_switch_socket" \
  "server_port=$server_switch_port_id" \
  "server_label=$server_switch_label" \
  "server_exit=$server_switch_exit_node" \
  "server_tun=$server_has_tun" \
  "switch_enabled=$switch_enabled" \
  "all_tools=$all_tools" \
  "crypto_option=$crypto_option"
'''


def run(*arguments, ok=True):
    result = subprocess.run(
        ["bash", "-c", report, "--", "42", "router.example", *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if (result.returncode == 0) != ok:
        raise RuntimeError(
            f"unexpected parser result {result.returncode}: {result.stderr}")
    return result.stdout


output = run(
    "--client-switch", "/run/tuntom/switch.sock", "client-42", "0x11",
    "--server-switch", "/run/tuntom/switch.sock", "server-42", "23",
    "--server-switch-exit-node",
)
expected = {
    "client_socket=/run/tuntom/switch.sock",
    "client_port=client-42",
    "client_label=0x11",
    "client_exit=0",
    "client_tun=0",
    "server_socket=/run/tuntom/switch.sock",
    "server_port=server-42",
    "server_label=23",
    "server_exit=1",
    "server_tun=1",
    "switch_enabled=1",
    "all_tools=0",
    "crypto_option=",
}
if set(output.splitlines()) != expected:
    raise RuntimeError(f"unexpected parsed switch options:\n{output}")

run("--client-switch-exit-node", ok=False)
run("--server-switch", "/tmp/x", "bad port", "1", ok=False)
run("--server-switch", "/tmp/x", "port", "-1", ok=False)
if "crypto_option=--crypto-auth-only" not in run("--crypto-auth-only").splitlines():
    raise RuntimeError("auth-only option was not forwarded")
if "all_tools=1" not in run("--all-tools").splitlines():
    raise RuntimeError("all-tools option was not enabled")
run("--pfs", ok=False)
run("--encrypt-ascon", ok=False)

for required in (
        '"$source_dir/switch/main.cpp" -o "$local_switch_stage"',
        '"$source_dir/adapter/main.cpp" -o "$local_adapter_stage"',
        '"$source_dir/control/main.cpp" -o "$local_control_stage"',
        '"$build_dir/src/switch/main.cpp" -o "$switch_stage"',
        '"$build_dir/src/adapter/main.cpp" -o "$adapter_stage"',
        '"$build_dir/src/control/main.cpp" -o "$control_stage"'):
    if required not in source:
        raise RuntimeError(f"all-tools build is missing: {required}")

print("PASS: mk_tunnel per-side switch options and TUN selection")
