# tuntom label switching

[Back to the tuntom README](README.md)

The separate [tomtom-switch-mp target](README_SWITCHING_MP.md) provides a generic
worker pool and runtime RX/TX scheduling. Use `mk_switch_mp.sh` for that target,
including CPU planning with `--auto-pool --dry-run`. `mk_switch.sh` builds the
existing single-thread `tuntom-switch`.

Label switching connects tuntom tunnel links through explicit relay and exit
paths. Forwarding uses the incoming port and label stack, so a relay can choose the
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
(input port, stack match) -> (output port, rewritten stack)
```

[Local setup](#local-switch-and-adapter) · [Hooks](#lifecycle-hooks) ·
[Tunnel attachment](#connect-tunnel-endpoints) · [Flow rules](#flow-rules-and-exit-adapters) ·
[Statistics](#runtime-statistics-control)

## Local switch and adapter

The local helpers need Linux, C++17 `g++`, Bash, `flock`, `timeout` and standard
system utilities. The adapter also needs `iproute2` and `/dev/net/tun`.
Commands below run from the repository root; the scripts build their own
binaries and obtain root privileges through `sudo` when needed. Only `TUNTOM_*`
variables are explicitly preserved by name, with both classic `sudo` and `sudo-rs`.
Other overrides such as `CXX` follow the normal sudo environment policy.

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
use `sudo` when needed, and require no `TUNTOM_SECRET`. Repeat the start
command with the desired options to rebuild and restart. `--stop` needs only
the switch name or adapter interface; saved endpoints and hook paths are used
for teardown. Run `--help` for options, including socket paths, owner, adapter
cache settings and inline switch `--route` / `--exit-port` arguments.

`mk_switch.sh NAME` and `mk_switch_mp.sh NAME` manage the same named instance,
state and lock. Changing the helper replaces the implementation after the new
build and configuration have been checked. Either helper can stop that switch.
Use different names for separate switches; see [MP lifecycle and migration](README_SWITCHING_MP.md#local-helper-and-automatic-pool).

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
- `TUNTOM_CONTROL_SOCKET`: switch, adapter - component's control socket path for stats and switch rules commands.
- `TUNTOM_SOCKET_OWNER`: switch, adapter - `user:group` assigned to the component's own sockets.
- `TUNTOM_BIN`: switch, adapter - installed component binary path.
- `TUNTOM_CTL`: switch, adapter - installed `tuntomctl` binary path.
- `TUNTOM_PID_FILE`: switch, adapter - component's PID file path.
- `TUNTOM_LOG_FILE`: switch, adapter - component's stdout/stderr log file path.

For switch `pre/up`, `TUNTOM_RULES_FILE` points to a fresh private staging file,
initially empty or copied from `--rules-file` / `TUNTOM_SWITCH_RULES_FILE`.
The hook may replace or append to it. Lines contain `route in:label=out:label`,
`exit-port port`, or `default-back off|on`; blank lines are ignored and `#`
begins a comment through the end of its line. No shell code is evaluated. These legacy entries follow inline
CLI rules; duplicate routes fail validation. Hooks may instead write a complete
versioned ruleset, which cannot be combined with legacy CLI rules. On success the
file is saved as the instance's `rules`. Other switch hooks see that saved file.
Versioned rules can also be loaded over the control socket without a restart.

## Versioned rulesets and live reload

For annotated examples, see the [Czech rules showcase](docs/SWITCH_RULES_SHOWCASE_CZ.md)
and its [complete format-2 configuration](examples/switch-showcase.rules).

Both implementations support the ordered configuration in
[examples/switch.rules](examples/switch.rules). In `format 2`, one `switch`
statement matches the input stack, chooses a destination group and rewrites
the stack. Elements support exact values, `*`, inclusive ranges and any-bit
masks; values accept decimal, hex, binary and strings of up to eight bytes.
See the [format-2 specification](docs/SWITCH_RULESET_V2.md) for `bidir`, ordering,
and preserved positions. `exit`/`trunk` declarations and manual serials remain.

`format 1` remains compatible: it matches only the top label and keeps separate
`switch` policies and `label` mappings, each in first-match order. Its original
semantics are documented in the [format-1 guide](docs/SWITCH_RULESET_V1.md).
An unquoted `#` begins a comment; format-2 label strings may contain `#`.

```bash
./mk_switch.sh switch --rules-file examples/switch.rules
sudo /var/lib/tuntom-mk/switch-switch/tuntomctl /run/tuntom/switch.control rules check examples/switch.rules
sudo /var/lib/tuntom-mk/switch-switch/tuntomctl /run/tuntom/switch.control rules load examples/switch.rules
sudo /var/lib/tuntom-mk/switch-switch/tuntomctl /run/tuntom/switch.control rules show > saved.rules
```

The daemon also accepts `--rules-file PATH` directly. `check` prepares without
publishing; `load` applies the complete ruleset atomically and preserves connected
ports. `show` writes a reloadable configuration with its active serial to stdout.
Increase the serial when editing rules. Runtime loads do not overwrite startup
files. Capture syntax is reserved and currently rejected as unsupported.
See [ruleset format and control protocol](docs/SWITCH_RULESET_V1.md) for full
syntax, queue behavior, limits, legacy compatibility, and persistence.

## Connect tunnel endpoints

Use these `mk_tunnel.sh` options to attach either endpoint to a switch. The
ordinary tunnel setup, shared secret, encryption and SSH requirements remain
as described in the [tuntom quick start](README.md#quick-start).

| Option | Effect |
| --- | --- |
| `--all-tools` | Build and atomically install `tuntom-switch`, `tuntom-switch-adapter`, and `tuntomctl` in `/tmp` on both hosts |
| `--client-switch <socket> <port-id> <label>` | Connect the local/client side to an existing switch listener |
| `--server-switch <socket> <port-id> <label>` | Connect the remote/server side to an existing switch listener |
| `--client-classifier-file <path>` / `--server-classifier-file <path>` | Assign ingress stacks from L3/L4 headers using a file already on that host |
| `--client-switch-exit-node` | Retain the client TUN and permit IPC `EXIT` delivery |
| `--server-switch-exit-node` | Retain the server TUN and permit IPC `EXIT` delivery |
| `--no-address` | Skip address assignment and peer address routes on retained TUNs; check processes instead of tunnel pings |
| `--count <1..64>` | Start a group; append member suffixes to switch port IDs while retaining the supplied labels |

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

With `--count 3`, the server ports above are `edge-42`, `edge-42_1` and
`edge-42_2`, all using ingress label `17`. A route targeting `edge-42*` balances
flows across the connected members; bootstrap does not create the switch rules.
Per-group hooks receive a
manifest containing every member's port and address resources. See
[tunnel groups](README.md#tunnel-groups) for lifecycle and stop behavior.

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
with the configured ingress label, or a stack selected by `--classifier-file`;
a `SWITCH` frame received from IPC is sent
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

### Wildcard ports and ECMP

Both switch implementations accept one trailing `*` in either route port ID.
It matches zero or more characters after a non-empty literal prefix; `*` alone
is rejected to prevent accidental forwarding across all ports. Labels remain
exact numbers. Quote CLI rules to prevent shell filename expansion:

```bash
--route 'internet:1001=edge-42*:44' \
--route 'edge-42*:17=internet:1001'
```

The equivalent rules file is:

```text
exit-port internet
route internet:1001=edge-42*:44
route edge-42*:17=internet:1001
```

`edge-42*` includes `edge-42`, `edge-42_1`, `edge-42_2`, and also `edge-420` or
`edge-42-backup`. Matching is case-sensitive. Interior/repeated stars are
rejected; `?`, brackets and other characters are literal. A star in a route
port ID is now reserved for this wildcard syntax; there is no escape syntax.
`exit-port` and MP's `trunk-port` still name individual ports, not patterns.

For each input label, an exact ingress port takes precedence over wildcard
rules, then the longest matching prefix wins. CLI/rules-file order does not
affect precedence. Duplicate input-pattern/label pairs remain an error. An
unavailable target of a more specific rule does not fall through to a broader
rule. Ingress matching is compiled at registration or MP plan preparation.

One matching connected output forwards normally. Multiple outputs use equal
cost multipath (ECMP), sending each packet to exactly one member and replacing
the top label with the rule's output label. The chosen port's `exit-port` role
determines whether delivery uses `EXIT`; labels below the top are preserved.
Zero matching outputs drops the packet and increments `target_disconnected`,
even with default-back enabled. Default-back only applies to an ingress route
miss. A wildcard may match the ingress port itself; such a member is not
implicitly excluded.

ECMP uses vendored Linux SipHash-2-4. TCP/UDP packets use
`H(source IP, source port) XOR H(destination IP, destination port)`; other IP
packets use `H(source IP) XOR H(destination IP)`. Endpoint encoding includes
the IP version and, for L4, the transport protocol. All IPv4 fragments and all
IPv6 packets containing a fragment header use L3, including the first fragment.
Switching between fragmented and unfragmented packets can therefore change a
flow's path. Opaque/non-parseable IP payloads keep a stable path per label stack.
Payload contents, TCP sequence numbers, worker IDs and connection generations
do not affect the flow hash. A single available member skips flow hashing.

Rendezvous selection uses stable public hash keys and the output port's full
name. It gives the same result across ST/MP, IPC versions, restarts and
registration order when the active names and packet key are the same. Adding
a member moves flows only to that member; removing one moves only its flows.
Registration, replacement and disconnection update membership automatically.
Queued packets are not moved to another output on congestion. A membership
change can briefly reorder packets; ECMP balances flows rather than bytes and
one TCP connection uses one member. Symmetric flow keys alone do not guarantee
the same physical return path through independently configured switches.

Availability means a locally registered IPC connection. The switch has no
remote UDP path-health signal; a live tunnel process whose remote link fails
can remain an ECMP member. Legacy CLI rules are fixed at startup; format-1 and format-2 rulesets support live reload.
`ecmp_packets` counts received packets selected with more than one available
member, including packets later dropped by output backpressure.

### Exit adapter behavior

The adapter creates the named TUN and brings its link state up. IP addresses,
routes, forwarding and any NAT rules remain explicit host configuration.

Routes targeting an `--exit-port` are delivered as `EXIT`. The adapter learns
the reverse label stack from every valid IPv4/IPv6 packet, using an L4 LRU cache
with an L3 fallback, including when a TCP/UDP tuple misses L4. Return traffic
from the TUN is sent as `SWITCH`. On a miss in both caches, optional
`--classifier-file` rules assign a stack from IP addresses/CIDR, protocol and
TCP/UDP ports. Without a matching rule, the packet is dropped. The adapter
performs no NAT or TCP state tracking, and classification does not learn flows.
See the [shared ingress classifier](docs/PACKET_CLASSIFIER.md) and
[example rules](examples/ingress.classifier); tuntom supports the same classifier
for authenticated UDP DATA entering its switch interface.

On a route miss, `--default-back=on` returns an `EXIT` frame to the ingress
port. The IPC format and exact fail-closed behavior are specified in
[switch protocol v1](docs/SWITCH_PROTOCOL_V1.md).

## Runtime statistics control

### Connection capacity

The switch defaults to **256 registered ports** and **16 additional pending
registrations**. Set `--max-ports` and `--max-pending` (each 1..65535) on either
`tuntom-switch` or `mk_switch.sh` to change them. At startup it counts open and
inherited descriptors using `/proc/self/fd` and reduces these limits to fit
the soft `RLIMIT_NOFILE`, leaving 16 descriptor slots for control and other
operations. Pending capacity is reserved before port capacity, with at least
one slot of each kind; insufficient startup capacity is an error. Effective
limits are visible in control statistics.

An accepted client must register within five seconds. Registered ports may
remain idle indefinitely. A new connection can replace an existing port ID
even at the port limit; a new distinct ID at that limit is disconnected.
Pending capacity is separate so a full port table alone cannot prevent a
replacement. A full pending pool temporarily defers all new admissions.

Accept attempts have a burst allowance of 16 and refill at 32/s. While pending
capacity or rate allowance is exhausted, the data listener leaves the poll set;
established ports and control continue to run. Accept errors other than normal
nonblocking retries defer that listener for one second. Control listeners use
their own error backoff, including during runtime resource recovery. Under
system-wide file-table exhaustion (`ENFILE`), new control requests may also
fail until resources recover. Limits prevent clients of this switch from
consuming its control headroom; they do not reserve global kernel resources.

The capacity calculation is performed at startup. Later changes to the process
FD limit or descriptors opened by other code are handled by accept-error
backoff, rather than by silently evicting registered ports. Socket access
permissions and port replacement semantics are unchanged.

### Query statistics

The switch and adapter expose live statistics through their own Unix control
sockets:

```bash
tuntomctl /run/tuntom/switch.control show stats
tuntomctl /run/tuntom/exit0.control show stats
```

For direct invocation, pass `--control-socket <path>` to `tuntom-switch` or
`tuntom-switch-adapter`. The bootstrap scripts configure this automatically
and print the command using their private `tuntomctl` binary. Sockets use mode
`0660`; filesystem permissions control access. All components also accept
`tuntomctl <control-socket> show flows` to inspect retained flows and label stacks.
See [flow snapshot fields and semantics](README.md#flow-and-label-snapshots).
Switches support the `rules show|check|load` commands described above.

Switch admission fields are documented in
[switch admission statistics](docs/DETAILS.md#switch-admission-and-fd-capacity).

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

## Optional IPC V2

`tomtom-switch-mp` and new tuntom/adapter clients support negotiated
[V2 shared-memory payloads and grouped references](docs/SWITCH_PROTOCOL_V2.md).
The single-thread `tuntom-switch` continues serving V1; automatic clients probe
without a port name and fall back to V1. `--switch-ipc v1` skips the probe and
`--switch-ipc inline` explicitly selects V2 without shared payloads.

## Optional local divert

Both switches accept the opt-in `--divert-file` configuration. A separate
`tuntom-divert-adapter` connects two TUNs and restores flow labels across a local
Linux router. See [Divert: configuration, limits and manual namespace/VRF commands](docs/DIVERT.md).
The existing eight-label limit and IPC formats remain unchanged.

Both lifecycle helpers also accept `--divert-file PATH`:

```bash
./mk_switch.sh switch --rules-file ./switch.rules --divert-file ./divert.conf
# Or use the MP helper, including its normal planning options:
./mk_switch_mp.sh switch --auto-pool --rules-file ./switch.rules --divert-file ./divert.conf
```

Divert requires versioned rules (format 1 or 2), supplied by `--rules-file` or
the existing `pre/up` hook. The helper stages and validates the divert file
before replacing the running switch, then saves it as the instance's `divert`
file. Every start with divert configured leaves it disabled until both divert
adapter ports are connected and `tuntomctl SOCKET divert enable` is called.
`tuntomctl SOCKET divert stop` immediately stops new offers to the divert adapter:
all subsequent unmarked packets use normal rules. It is idempotent, retains DVRT
return handling, and may interrupt active proxied connections; it does not drain flows.

## VIA service chains (format 3)

Permanent and ad hoc service chains share the VIA label envelope, rendezvous flow
hashing, ordered failover, and unavailable pass/drop policies. The switch keeps no
per-flow VIA state. See [VIA configuration, label diagrams and lab commands](docs/VIA.md).
Existing formats 1/2, legacy DVRT, CLI defaults, and the eight-label limit are unchanged.
