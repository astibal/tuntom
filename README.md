# tuntom

**Linux IP tunneling over UDP, with encrypted forward-secret sessions and SSH
deployment.**

`tuntom` connects two Linux TUN interfaces and carries IPv4 and IPv6 traffic
between them. It combines a self-contained C++17 tunnel engine with a bootstrap
script that builds, deploys, and configures both endpoints. Protocol v5 provides
replay protection, automatic path-MTU discovery, and internal fragmentation;
Ascon-AEAD128 encryption and X25519 rekeying provide confidentiality and forward
secrecy by default; an explicit authentication-only mode is available.

```text
      local / client                         remote / server
   +-------------------+                  +-------------------+
   | Linux networking  |                  | Linux networking  |
   +---------+---------+                  +---------+---------+
          ut42c                                   ut42s
       10.254.42.1                            10.254.42.2
             |                                      |
             +---------- UDP / port 40042 ----------+
                  authenticated v5 session
                  AEAD encryption + PFS
```

Written by **Ales Stibal <astib@mag0.net>**.   
Licensed under [BSD 3-Clause](LICENSE.md).

OpenAI Codex has been used in code and documentation development.
Contributors remain responsible for the changes they submit.

[Quick start](#quick-start) · [Configuration](#configuration) ·
[Security and compatibility](#security-and-compatibility) ·
[Operations](#operations) · [Build and test](#build-and-test)

## At a glance

| Area | What tuntom provides |
| --- | --- |
| Tunnel | IPv4/IPv6 TUN traffic over UDP, NAT-friendly client/server model |
| Sessions | Authenticated v5 handshake, directional keys, replay protection |
| Encryption | Ascon-AEAD128 with X25519 PFS and periodic rekey by default |
| MTU | Independent inner/outer MTUs, automatic PMTUD, balanced fragmentation |
| Deployment | Local and remote compilation, staged restart, start/stop helper |
| Networking | IPv4 policy routing, connection marks, MSS clamping, optional SNAT, lifecycle hooks |
| Observability | Text statistics, signal-controlled snapshots, logs, Wireshark dissector |
| Runtime | No external crypto libraries; drops privileges to `tuntom:tuntom` |
| Switching | Optional userspace label switching over Unix `SOCK_SEQPACKET`, without a TUN on relay links |

The tunnel engine handles transport. Linux networking and the included
`tuntom-net.sh` helper handle routing and firewall policy; custom routes and
DNAT rules can be added through hooks.

## Quick start

### Requirements

For the bootstrap workflow, both hosts need:

- Linux with `/dev/net/tun` and root access.
- `g++` with C++17 support, Bash, `iproute2`, and `iptables`.
- Standard system utilities, including `tar`, `mktemp`, `getent`, `useradd`, and `groupadd`.
- Synchronized clocks for the v5 handshake.

The caller also needs `ssh`, working SSH key authentication, and `flock`.
When started as a normal local user, the script uses `sudo -E` for privileged
local operations. The remote SSH account must already have root privileges:
remote commands do not use `sudo`. A bare hostname selects `root@host`.
The server's UDP port (`40000 + tunnel ID`) must be reachable from the client.

### Start a tunnel

Set a random 128-bit shared secret as exactly 32 hex characters. For example,
if OpenSSL is installed locally:

```bash
export TUNTOM_SECRET="$(openssl rand -hex 16)"
```

From the repository directory, create tunnel `42` to `sx2`:

```bash
./mk_tunnel.sh 42 sx2
```

This builds both endpoints, passes the secret over SSH, creates the runtime
account, configures networking, and starts the processes in the background.
Encryption and forward secrecy are enabled on both ends by default. Use
`--crypto-auth-only` only when the payload must remain visible on the wire; see
the [mode table](#security-and-compatibility).

| Tunnel 42 | Local / client | Remote / server |
| --- | --- | --- |
| Interface | `ut42c` | `ut42s` |
| IPv4 | `10.254.42.1` | `10.254.42.2` |
| IPv6 | `fd42::10:254:42:1` | `fd42::10:254:42:2` |
| UDP port | Server destination: `40042` | Listen: `40042` |

Once the session is established:

```bash
ping 10.254.42.2
ping -6 fd42::10:254:42:2
```

Run the same start command again to rebuild and restart the tunnel. Both staged
builds finish before the running tunnel is stopped; the switchover briefly
interrupts traffic. Keep the secret and desired options when restarting.

```bash
./mk_tunnel.sh 42 sx2 --stop
```

Stopping removes both processes, interfaces, statistics files, and the helper's
per-tunnel networking rules. It does not require `TUNTOM_SECRET`.
For a local smoke test, use `localhost` as the host (root SSH access is still required).

## Configuration

Tunnel IDs range from **1 to 255** and determine interface names, addresses,
and the server UDP port.

### Bootstrap options

| Option | Effect |
| --- | --- |
| `--crypto-auth-only` | Disable payload encryption and PFS; retain AMAC authentication |
| `--no-stats` | Disable automatic stats file writes; keep live metrics and socket queries |
| `--all-tools` | Build and atomically install `tuntom-switch`, `tuntom-switch-adapter`, and `tuntomctl` in `/tmp` on both hosts |
| `--client-switch <socket> <port-id> <label>` | Connect the local/client side to an existing switch listener |
| `--server-switch <socket> <port-id> <label>` | Connect the remote/server side to an existing switch listener |
| `--client-switch-exit-node` | Retain the client TUN and permit IPC `EXIT` delivery |
| `--server-switch-exit-node` | Retain the server TUN and permit IPC `EXIT` delivery |
| `--snat` / `--no-snat` | Enable / disable IPv4 MASQUERADE; default: off |
| `--mss-clamp` / `--no-mss-clamp` | Enable / disable TCP MSS clamping; default: on |
| `--stop` | Stop and clean up the tunnel on both hosts |

### Environment

| Variable | Default | Purpose |
| --- | --- | --- |
| `TUNTOM_SECRET` | Required to start | 128-bit master key, 32 hex characters |
| `TUNTOM_PREFIX16` | `10.254` | First two IPv4 octets; also used in IPv6 addresses |
| `TUNTOM_MTU` | `1500` | Inner/TUN MTU |
| `TUNTOM_TRANSPORT_MTU` | `1400` | Outer IP MTU / initial PMTUD target |
| `TUNTOM_STATS_FORMAT` | `txt` | Statistics format; currently only `txt` |
| `TUNTOM_PRE_HOOK` | `/etc/tuntom/tuntom-pre.sh` | Local source for pre-action hooks |
| `TUNTOM_POST_HOOK` | `/etc/tuntom/tuntom-post.sh` | Local source for post-action hooks |

For example:

```bash
TUNTOM_PREFIX16=10.10 TUNTOM_MTU=9000 TUNTOM_TRANSPORT_MTU=1500 \
    ./mk_tunnel.sh 42 sx2
```

This uses `10.10.42.1` / `10.10.42.2` and
`fd42::10:10:42:1` / `fd42::10:10:42:2`, with a 9000-byte inner MTU.
IPv6 addresses use the prefix text with dots replaced by colons.

Advanced networking overrides are `TUNTOM_MARK`, `TUNTOM_MARK_MASK`,
`TUNTOM_TABLE`, and `TUNTOM_CHAIN`; see [the helper](tuntom-net.sh).

### Label-switch mode

Use `--all-tools` during deployment when the standalone switch utilities should
be rebuilt together with the tunnel. They are installed as `/tmp/tuntom-switch`,
`/tmp/tuntom-switch-adapter`, and `/tmp/tuntomctl` locally and remotely. A failed
direct tunnel ping is reported as a warning in switch mode because the selected
switch topology may intentionally route packets away from the peer TUN; both
tunnel processes are still checked.

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

### Runtime statistics control

All three data-plane processes can expose the same local control command through
an optional Unix socket:

```bash
tuntomctl /run/tuntom/42c.control show stats
tuntomctl /run/tuntom/switch.control show stats
tuntomctl /run/tuntom/exit0.control show stats
```

Pass `--control-socket <path>` to `tuntom`, `tuntom-switch` or
`tuntom-switch-adapter`. `mk_tunnel.sh` configures `<id>c.control` and
`<id>s.control` automatically. The sockets use mode `0660`; filesystem
permissions are the authorization boundary. Only `show stats` is defined in
this first control protocol version. Existing stats signals remain available
for compatibility.

On a route miss, `--default-back=on` returns an `EXIT` frame to the ingress
port. The IPC format and exact fail-closed behavior are specified in
[switch protocol v1](docs/SWITCH_PROTOCOL_V1.md).

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
per second.

### MTU and fragmentation

The TUN MTU is independent of the outer IP MTU, which includes IP, UDP, and
tuntom headers. Oversized inner packets are split into balanced fragments and
reassembled at the receiving endpoint:

```text
1500-byte inner packet -> 750 + 750 bytes of fragment payload
1401-byte inner packet -> 701 + 700 bytes of fragment payload
```

Automatic PMTUD starts at a conservative 500-byte outer MTU. Authenticated
`MTU_PROBE` / `MTU_REPLY` messages search for a working size, first targeting the
configured transport MTU and exploring up to at least 1500 bytes (higher if
configured). A probe times out after two seconds. Discovery restarts when the
peer changes or a data send fails; traffic continues at the last known-good MTU.

The standalone binary accepts `--no-pmtud` to keep `--transport-mtu` fixed.
The bootstrap does not forward arbitrary binary options.

### TTL / Hop-Limit compensation

By default, received IPv4 TTL / IPv6 Hop Limit is incremented by one; the IPv4
header checksum is updated. This compensates for an extra forwarding hop when
connecting routing points. Locally generated packets have not consumed that
hop and may therefore arrive with a value one higher than expected.
Use `--no-ttl-compensate` when running the binary directly to disable this.

## Security and compatibility

All current modes use the v5 session handshake and a shared master secret.
Both endpoints must select the same mode.

| Mode | Suite | Payload encryption | Forward secrecy |
| --- | --- | --- | --- |
| Default | 2 | Ascon-AEAD128 | X25519 exchange, rekey every two minutes |
| `--crypto-auth-only` | 0 | No; AMAC authentication only | No |

Suite 1 (Ascon-AEAD128 without PFS) remains a recognized wire suite for protocol
compatibility and tests, but has no command-line selector. The former `--pfs`
and `--encrypt-ascon` options are rejected.

The authentication primitive is specified in [AMAC v1](docs/AMAC_V1.md).
Suite 2 uses a **project-specific AMAC-based [AKDF v1](docs/AKDF_V1.md)**, not HKDF or a standardized
Ascon KDF. X25519 is vendored from Monocypher. Construction details and security
assumptions are documented in the [v5 wire specification](docs/PROTOCOL_V5.md).
Encryption adds no wire bytes. Mode mismatches fail the handshake without
falling back to plaintext; old receive keys overlap for up to three seconds
during PFS rekeying.

The handshake normally takes one RTT before the client can send DATA. Initial
TUN traffic is not buffered, and DATA arriving before CONFIRM is dropped.
Timestamped INITs require synchronized clocks: the default acceptance window is
300 seconds total (±150 seconds). The binary's `--init-window` accepts an even
value from 2 to 86400 seconds. Expired INITs and previously seen nonces are rejected.

**Update both endpoints together.** V5 is incompatible with V1–V4. There is no
legacy receive path or automatic downgrade; `--allow-v1` and `--allow-v2` are
rejected. DATA headers are 25 bytes, fragmented DATA headers 37 bytes. Tunnel ID
stays in configuration/key derivation; only INIT/RESPONSE transmit the version.

Processes start as root to initialize networking, then drop privileges to
`tuntom:tuntom`, disable core dumps, and set `no_new_privs`.

## Operations

### Logs and statistics

Files live on the respective endpoint hosts:

| File | Client | Server |
| --- | --- | --- |
| Log | `/tmp/tuntom_42c.log` | `/tmp/tuntom_42s.log` |
| PID | `/run/tuntom/42c.pid` | `/run/tuntom/42s.pid` |
| Statistics | `/run/tuntom/42c.stats` | `/run/tuntom/42s.stats` |

```bash
sudo tail -f /tmp/tuntom_42c.log
sudo cat /run/tuntom/42c.stats
```

Statistics include traffic counters, throughput, sampled processing latency,
PMTUD state, active suite, session readiness, and handshake/rekey counters.
See [statistics field definitions](docs/DETAILS.md#session-suite-and-rekey-statistics).
The standalone binary also accepts `--debug` and `--quiet` for logging.

Start with `--no-stats` to pause automatic file writes. Counters and sampled
latency/throughput metrics continue updating in memory. Control each process
separately using its PID:

```bash
sudo kill -USR1 <pid>  # toggle automatic stats file writes
sudo kill -USR2 <pid>  # write one snapshot, even when disabled
```

The bootstrap retains the stats destination when disabled. For direct binary
use, supply `--stats-file <path>` even with `--no-stats` to allow later writes.
The last file stays unchanged while paused, so check `updated_unix` for age.
`stats_enabled` records automatic file export, not metric collection. Toggling
export does not reset throughput or latency history. `tuntomctl <control-socket>
show stats` returns current metrics directly from memory, even without a configured
stats file, and never reads or writes that file.

### Networking and hooks

The bootstrap runs `tuntom-net.sh` on both hosts to set up IPv4 connection
marking, policy routing for replies, forwarding rules, MSS clamping, and optional
MASQUERADE. Configure the routes, forwarding sysctls, and application-specific
policy needed by your topology; IPv6 forwarding/firewall policy is separate.

Optional hook files exist on the caller only. Their content runs locally and
is streamed over SSH for remote execution. Missing hooks are skipped.

```text
pre/down -> network cleanup -> post/down
pre/up   -> network setup   -> post/up
```

Hooks receive `TUNTOM_SIDE=local|remote`, `TUNTOM_ACTION=up|down`,
`TUNTOM_PHASE=pre|post`, plus tunnel addresses, interface names, and networking
settings. Use `post/up` to add custom routes or DNAT rules.
See [hook context](docs/DETAILS.md#lifecycle-hooks) and the
[service ingress example](examples/tuntom-service-ingress-hook.example.sh).

### Wireshark

[tuntom.lua](tuntom.lua) dissects v1, v2, v3, v4, and v5 captures, including handshake
fields, session hints, sequence counters, fragments, authentication tags, and
PMTUD probes. It reassembles unencrypted v5 DATA and passes inner packets to the
IPv4/IPv6 dissector. Encrypted payloads remain encrypted in the capture.

### PMTUD black-hole test

The manual helper simulates silently dropped IPv4 UDP packets above a selected
outer size:

```bash
sudo bash tests/pmtud-iptables-test.sh --size 1200 --interface eth0
# Start or restart the tunnel and inspect logs/statistics.
sudo bash tests/pmtud-iptables-test.sh --size 1200 --interface eth0 --remove
```

These rules affect **all IPv4 UDP traffic** above that size on the selected
interface. Remove them after testing.

## Build and test

Build with CMake (3.16+) or open this directory as a CMake project in CLion:

```bash
cmake -S . -B /tmp/tuntom-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/tuntom-build
ctest --test-dir /tmp/tuntom-build --output-on-failure
```

Or compile directly:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/main.cpp -o /tmp/tuntom
```

The regression runner also checks header self-containment and runs the dissector
test when `tshark` and `python3` are available. It requires no root or live tunnel:

```bash
bash tests/run.sh
```

See [test coverage](tests/README.md) and
[standalone binary setup](docs/DETAILS.md#using-the-binary-without-the-bootstrap-script).
Direct execution requires the `tuntom` user/group and root at startup.

The bootstrap compiles with `-O2 -march=native -mtune=native` independently on
each host. It streams `src/` as a tar archive and removes remote temporary
sources on exit, including compilation failure. CMake is not required for
deployment.

## Project map

| Path | Contents |
| --- | --- |
| [src/](src/README.md) | C++17 engine, ordinary headers, and `main.cpp` |
| [src/vendor/](src/vendor/README.md) | Vendored X25519 implementation and provenance |
| [mk_tunnel.sh](mk_tunnel.sh) | Build, deploy, start, restart, and stop |
| [tuntom-net.sh](tuntom-net.sh) | Linux routing and firewall helper |
| [tuntom.lua](tuntom.lua) | Wireshark Lua dissector |
| [examples/](examples/) | Lifecycle hook example |
| [tests/](tests/README.md) | Regression tests, vectors, and manual PMTUD helper |
| [docs/DETAILS.md](docs/DETAILS.md) | Implementation and operating details |
| [docs/PROTOCOL_V5.md](docs/PROTOCOL_V5.md) | Wire format, handshake, and cryptographic constructions |
| [CMakeLists.txt](CMakeLists.txt) | Local build and CTest targets |
| [LICENSE.md](LICENSE.md) | BSD 3-Clause license |
