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
  "server_tun=$server_has_tun"1
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
}
if set(output.splitlines()) != expected:
    raise RuntimeError(f"unexpected parsed switch options:\n{output}")

run("--client-switch-exit-node", ok=False)
run("--server-switch", "/tmp/x", "bad port", "1", ok=False)
run("--server-switch", "/tmp/x", "port", "-1", ok=False)

print("PASS: mk_tunnel per-side switch options and TUN selection")
