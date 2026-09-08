# tuntom label switching

[Back to the tuntom README](README.md)

Label switching connects tuntom tunnel links through explicit relay and exit
paths. Forwarding uses the incoming port and label, so a relay can choose the
next tunnel without creating a TUN or configuring kernel IP routes for each
link. This keeps the forwarding topology separate from the payload's IP
addresses: tuntom handles encrypted UDP transport, the switch selects links,
and exit adapters hand IP traffic to Linux networking where needed.

```text
UDP link -> tuntom -> (port, label) -> switch -> tuntom -> UDP link
                                        |
                                        +-> exit adapter -> TUN -> Linux routing
```

Labels are local Unix IPC metadata; they do not change the tuntom v5 UDP wire
format. The switch forwards by this rule:

```text
(input port, top label) -> (output port, replacement top label)
```

[Local setup](#local-switch-and-adapter) · [Hooks](#lifecycle-hooks) ·
[Tunnel attachment](#connect-tunnel-endpoints) · [Flow rules](#flow-rules-and-exit-adapters) ·
[Statistics](#runtime-statistics-control)

## Local switch and adapter

The local helpers need Linux, C++17 `g++`, Bash, `flock`, `timeout` and standard
system utilities. The adapter also needs `iproute2` and `/dev/net/tun`.
Commands below run from the repository root; the scripts build their own
binaries and obtain root privileges through `sudo -E` when needed.

For local build/start/restart/stop, use the companion bootstrap scripts:

```bash
./mk_switch.sh switch --rules-file examples/switch.rules
./mk_adapter.sh exit0 --switch-socket /run/tuntom/switch.sock \
  --switch-port-id internet --post-hook examples/adapter-post.example.sh

# Generate flow rules in a hook instead of supplying a static rules file:
./mk_switch.sh switch --pre-hook examples/switch-pre.example.sh

./mk_adapter.sh exit0 --stop
./mk_switch.sh switch --stop
```

Adjust the example routes first. Both scripts operate only on the local host,
use `sudo -E` when needed, and require no `TUNTOM_SECRET`. Repeat the start
command with the desired options to rebuild and restart. `--stop` needs only
the switch name or adapter interface; saved endpoints and hook paths are used
for teardown. Run `--help` for options, including socket paths, owner, adapter
cache settings and inline switch `--route` / `--exit-port` arguments.

### Restart and socket ownership

Builds finish before the running process is stopped. A per-instance lock
serializes changes. Cleanup finds orphaned/duplicate managed processes even
without a valid PID file, checks process start times, sends TERM and escalates
to KILL if necessary. Socket cleanup refuses live listeners, symlinks and
regular files. Startup must answer `tuntomctl show stats`; failing startup/up
hooks stop the new process and run down hooks. This is an explicit restart
helper, not a background supervisor: after switchover, failure does not
automatically restore the old binary or arbitrary hook side effects.

Default sockets are `/run/tuntom/<name>.sock` (switch data) and
`/run/tuntom/<name>.control` (either component). Use distinct names or custom
control paths when running both components. Created sockets are chowned to
`tuntom:tuntom` and chmodded to `0660` before the post/up hook. Override with
`--socket-owner user:group` or `TUNTOM_SOCKET_OWNER`. The adapter changes only
its own control socket. The shared socket directory is `root:tuntom`, mode
`2770`; custom socket parent directories must already exist with suitable
traversal permissions. Both managed processes run as root.

Private PID, saved endpoints, rules and log live under
`/run/tuntom-mk/switch-<name>/` or `/run/tuntom-mk/adapter-<ifname>/` (root only).
For example, `sudo tail -f /run/tuntom-mk/adapter-exit0/log`.
Each daemon's regular-file log is capped at 16 MiB; external `copytruncate`
rotation allows further logging. Full or broken output drops logs while packet
forwarding continues; `log_*` control counters expose logging failures.
The binaries (`main` and `tuntomctl`) live under
`/var/lib/tuntom-mk/switch-<name>/` or `/var/lib/tuntom-mk/adapter-<ifname>/`;
builds are staged on that same filesystem so installation uses atomic renames.
`mk_tunnel.sh --all-tools` does not replace these private binaries.
`TUNTOM_RUN_DIR` and `TUNTOM_STATE_DIR` override `/run/tuntom` and
`/run/tuntom-mk`; retain the same state directory for subsequent restarts and stops. `TUNTOM_BIN_DIR`
overrides `/var/lib/tuntom-mk`; retain it for subsequent operations as well.

`/run` can stay mounted `noexec`: only runtime files and sockets are stored
there. The binary directory must allow execution. The scripts check this
before compiling or stopping a running instance and report a specific error
if execution is blocked. Existing instances using the former binary location
under `/run/tuntom-mk` are also recognized during stop/restart.

### Lifecycle hooks

Default local hooks are `/etc/tuntom/switch-{pre,post}.sh` and
`/etc/tuntom/adapter-{pre,post}.sh`. Missing default files are skipped. Use
`--pre-hook` / `--post-hook` or `TUNTOM_SWITCH_PRE_HOOK`,
`TUNTOM_SWITCH_POST_HOOK`, `TUNTOM_ADAPTER_PRE_HOOK`,
`TUNTOM_ADAPTER_POST_HOOK` to override them. Hooks run as root through Bash:

| Component | Restart hook order and available resources |
| --- | --- |
| Switch | Build; `pre/up` prepares rules while the old switch runs; validate rules on isolated sockets; `pre/down`; stop old process; `post/down`; start; chown sockets; `post/up` |
| Adapter | Build; `pre/down` while the old TUN exists; stop old process; `post/down`; start with TUN up; chown control socket; `pre/up`; `post/up` |

Down hooks are skipped on the first start and their failures warn without
preventing process cleanup. `pre/down` is suitable for removing policy rules
while the old interface exists. The adapter reconnects independently when the
switch becomes available; its readiness check confirms the local TUN/control
service and does not require the upstream switch to be connected.

Hooks receive:

- `TUNTOM_COMPONENT`: switch, adapter - component type (`switch` or `adapter`).
- `TUNTOM_ID`: switch, adapter - switch instance name or adapter interface name.
- `TUNTOM_SIDE`: switch, adapter - execution side, always `local`.
- `TUNTOM_PHASE`: switch, adapter - hook phase (`pre` or `post`).
- `TUNTOM_ACTION`: switch, adapter - lifecycle action (`up` or `down`).
- `TUNTOM_IF`: adapter - TUN interface name for addresses and routes; empty for switch.
- `TUNTOM_MTU`: adapter - configured TUN MTU in bytes; unused by switch.
- `TUNTOM_SWITCH_PORT_ID`: adapter - registered switch port name usable in flow rules; empty for switch.
- `TUNTOM_SWITCH_SOCKET`: switch, adapter - switch data socket path to listen on or connect to.
- `TUNTOM_CONTROL_SOCKET`: switch, adapter - component's control socket path for stats queries.
- `TUNTOM_SOCKET_OWNER`: switch, adapter - `user:group` assigned to the component's own sockets.
- `TUNTOM_BIN`: switch, adapter - installed component binary path.
- `TUNTOM_CTL`: switch, adapter - installed `tuntomctl` binary path.
- `TUNTOM_PID_FILE`: switch, adapter - component's PID file path.
- `TUNTOM_LOG_FILE`: switch, adapter - component's stdout/stderr log file path.

For switch `pre/up`, `TUNTOM_RULES_FILE` points to a fresh private staging file,
initially empty or copied from `--rules-file` / `TUNTOM_SWITCH_RULES_FILE`.
The hook may replace or append to it. Lines contain `route in:label=out:label`,
`exit-port port`, or `default-back off|on`; blank lines and full-line `#`
comments are ignored. No shell code is evaluated. These entries follow inline
CLI rules; duplicate routes fail validation. On success the file is saved as
the instance's `rules`. Other switch hooks see that saved file. Flow changes
require restarting the switch because its control protocol only exposes stats.

## Connect tunnel endpoints

Use these `mk_tunnel.sh` options to attach either endpoint to a switch. The
ordinary tunnel setup, shared secret, encryption and SSH requirements remain
as described in the [tuntom quick start](README.md#quick-start).

| Option | Effect |
| --- | --- |
| `--all-tools` | Build and atomically install `tuntom-switch`, `tuntom-switch-adapter`, and `tuntomctl` in `/tmp` on both hosts |
| `--client-switch <socket> <port-id> <label>` | Connect the local/client side to an existing switch listener |
| `--server-switch <socket> <port-id> <label>` | Connect the remote/server side to an existing switch listener |
| `--client-switch-exit-node` | Retain the client TUN and permit IPC `EXIT` delivery |
| `--server-switch-exit-node` | Retain the server TUN and permit IPC `EXIT` delivery |

Use `--all-tools` during deployment when the standalone switch utilities should
be rebuilt together with the tunnel. They are installed as `/tmp/tuntom-switch`,
`/tmp/tuntom-switch-adapter`, and `/tmp/tuntomctl` locally and remotely. A failed
direct tunnel ping is reported as a warning in switch mode because the selected
switch topology may intentionally route packets away from the peer TUN; both
tunnel processes are still checked.

The SSH bootstrap can independently attach either endpoint to a switch that is
already running on that endpoint host:

```bash
./mk_tunnel.sh 42 site-router \
  --server-switch /run/tuntom/switch.sock edge-42 17
```

A pure switch side creates no TUN and skips its address, hook and kernel-network
setup. Adding `--server-switch-exit-node` (or its client counterpart) retains
that side's TUN. The switch has an independent lifecycle and is never started or
restarted by `mk_tunnel.sh`. While it is unavailable, tuntom keeps the UDP
control plane alive, drops DATA, and retries connection and registration once
per second after a failed attempt. Connection and registration are nonblocking,
with one shared five-second deadline; a full accept queue cannot stall the
packet loop. The exit adapter uses the same connection state machine.

### Direct tunnel invocation

`--switch-socket` replaces the TUN data path with a Unix `SOCK_SEQPACKET`
connection. Authenticated DATA received over UDP is emitted as a `SWITCH` frame
with the configured ingress label; a `SWITCH` frame received from IPC is sent
as ordinary V5 DATA to the UDP peer. No TUN device is created, so a pure relay
can run without root after its socket and UDP access are available.

```bash
TUNTOM_SECRET=... tuntom client 42 - server.example \
  --switch-socket /run/tuntom/switch.sock \
  --switch-port-id client-42 \
  --switch-label 17
```

The positional interface name is ignored in pure switch mode; `-` is the
recommended placeholder. `--switch-exit-node` retains the TUN and permits only
an explicit IPC `EXIT` frame to write into it. Packets read from that TUN go
directly to the instance's UDP peer.

## Flow rules and exit adapters

The following direct invocation examples assume the binaries are on `PATH`;
see [Build and further reading](#build-and-further-reading). For managed local
instances, use the bootstrap scripts above with the same flow rules.

The companion switch has named static ports and routes:

```bash
tuntom-switch \
  --socket /run/tuntom/switch.sock \
  --route client-42:17=proxy-7:83 \
  --route proxy-7:91=client-42:44 \
  --default-back=off
```

The standalone exit adapter is a switch client backed by a Linux TUN:

```bash
tuntom-switch-adapter exit0 \
  --switch-socket /run/tuntom/switch.sock \
  --switch-port-id internet

tuntom-switch --socket /run/tuntom/switch.sock \
  --exit-port internet \
  --route client-42:17=internet:1001 \
  --route internet:1001=client-42:44
```

The adapter creates the named TUN and brings its link state up. IP addresses,
routes, forwarding and any NAT rules remain explicit host configuration.

Routes targeting an `--exit-port` are delivered as `EXIT`. The adapter learns
the reverse label stack from every valid IPv4/IPv6 packet, using an L4 LRU cache
with an L3 fallback for fragments and non-port protocols. Return traffic from
the TUN is sent as `SWITCH`; packets missing both caches are dropped. The
adapter performs no NAT, TCP state tracking or default-label routing.

On a route miss, `--default-back=on` returns an `EXIT` frame to the ingress
port. The IPC format and exact fail-closed behavior are specified in
[switch protocol v1](docs/SWITCH_PROTOCOL_V1.md).

## Runtime statistics control

The switch and adapter expose live statistics through their own Unix control
sockets:

```bash
tuntomctl /run/tuntom/switch.control show stats
tuntomctl /run/tuntom/exit0.control show stats
```

For direct invocation, pass `--control-socket <path>` to `tuntom-switch` or
`tuntom-switch-adapter`. The bootstrap scripts configure this automatically
and print the command using their private `tuntomctl` binary. Sockets use mode
`0660`; filesystem permissions control access. Only `show stats` is supported;
flow rules are configured at startup.

Tunnel control sockets are covered in the
[tuntom operations guide](README.md#runtime-statistics-control). See also the
[statistics field definitions](docs/DETAILS.md#session-suite-and-rekey-statistics).

## Build and further reading

To build the standalone utilities with CMake:

```bash
cmake -S . -B /tmp/tuntom-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/tuntom-build --target tuntom-switch tuntom-switch-adapter tuntomctl
```

The resulting binaries are in `/tmp/tuntom-build/`. The local bootstrap scripts
compile directly and do not require CMake.

- [mk_switch.sh](mk_switch.sh) and [mk_adapter.sh](mk_adapter.sh) - local lifecycle helpers.
- [Switch protocol v1](docs/SWITCH_PROTOCOL_V1.md) - IPC frames, registration and forwarding semantics.
- [Nonblocking switch connection](docs/DETAILS.md#nonblocking-switch-connection) - reconnect state machine and deadlines.
- [Test coverage](tests/README.md) - switching, adapters and lifecycle regression tests.
- [tuntom README](README.md) - UDP tunnel setup, transport, security and operations.
