# Local divert adapter

The first integrated version inserts a Linux router into the packet path. A single
`tuntom-divert-adapter` process handles two switch ports and two TUN interfaces:

```text
TCP client -> tuntom -> switch -> divert-in adapter -> di0
                                                        |
                                                   Linux routing
                                                        |
                       switch <- divert-out adapter <- do0
                          |
                     exit adapter -> ex0 -> TCP server

Response:
server -> exit adapter -> switch -> divert-out adapter -> do0
                    Linux routing -> di0 -> divert-in adapter
                                  -> switch -> original tuntom -> client
```

`divert-in` and `divert-out` are two sides of the same process and share flow
context. The switch has no per-flow divert cache. The origin is an explicitly
configured 64-bit port ID, independent of registration order and reconnects.

## Compatibility and scope

- IPC V1/V2, the mmap ABI, the maximum frame size, and the **eight-label limit remain unchanged**.
- Without the new `--divert-file` option, existing commands, rules, and defaults
  retain their behavior. Divert configuration is read separately; the rule format is unchanged.
- `--divert-file` requires `--rules-file` and `--control-socket`. Divert becomes
  active only after the new `tuntomctl SOCKET divert enable` command; repeated enable calls are idempotent.
- Both the ST switch and the MP switch are supported, including MP IPC V1, V2 inline,
  and mmap. Rule continuation uses the original logical ingress, normal policy, and ECMP.
- Unfragmented IPv4/IPv6 TCP and UDP packets are supported. ICMP, inner IP fragments,
  and other protocols selected by divert are currently dropped
  (`unsupported_drops`). Fragmentation of the encrypted tuntom transport still works.
- The configuration describes one local divert. Server responses arrive through a
  port marked as `exit` in the rules. Remote divert and transporting labels through
  a tunnel are separate future extensions.
- The Linux router preserves IP addresses and ports. Smithproxy/TPROXY integration
  has not been verified; it must preserve the flow identity used by the adapter to
  restore labels. Overlapping flows from different origins require separate instances/TUNs.

### DVRT block and the eight-label limit

```text
original: [17,42]
offer:    [17,42,"hTX4","DVRT",123,0,17,42]
onward:   [99,42,"hTX4","DVRT",123,1,17,42]
client:   [17,42]
```

The cookie consists of three ASCII letters and must match in the switch and the
divert adapter. The fourth byte is the body length + 48, excluding the cookie
and DVRT labels. The lower four bytes of the label are zero. The cookie is a
recognition marker, not authentication.

The body contains `origin, action, original stack...`; actions are 0 offer,
1 continue, 2 return to client, and 3 EXISTING bypass. The block's position and
length are variable. This implementation stores the entire original stack: an
offer requires `2*n + 4` labels. Therefore, original stacks of **1 or 2** labels
can be diverted. After a normal rewrite,
`new_length + original_length + 4 <= 8` must hold.

Overflow causes a drop and increments `divert_overflow_drops`; the stack is never
truncated. Longer stacks continue to work in normal switching unless selected
by a divert match.

## Admission and context lifetime

The timed phases start with the first valid packet offered to the divert-in
adapter, rather than at process startup. An idle adapter waiting for activation
therefore retains its full learning interval.

| Protocol / time since first offer | Decision |
| --- | --- |
| TCP 0–60 min | Known EXISTING flows bypass; known DIVERTED flows go through the TUN; unknown SYN without ACK enters DIVERTED, others enter EXISTING |
| TCP 60 min–24 h | Clear DIVERTED; EXISTING flows bypass, others go through the TUN |
| TCP from 24 h | Clear EXISTING; everything goes through the TUN, with no admission lookup |
| UDP 0–60 s | Observed flows enter EXISTING and bypass |
| UDP 60 s–1 h | EXISTING flows bypass, others go through the TUN |
| UDP from 1 h | Clear EXISTING; everything goes through the TUN, with no admission lookup |

Admission uses a bidirectional flow key. Each learning set has a default capacity
of 100000 (`--admission-capacity`). At capacity, new flows that cannot be classified
are dropped; entries are not evicted. This is the agreed heuristic: an old
connection that remains idle throughout the learning interval, or survives beyond
the protection interval, may be interrupted.

**The label flow context is a separate table from admission.** It has one entry
per bidirectional connection, two directional contexts, a default capacity of
100000, and an idle timeout of 86400 s. Configure these with `--flow-capacity` and
`--flow-idle-seconds`. Capacity pressure does not evict active entries; an unknown
return flow, a context collision, or a full table causes a drop with the
corresponding counter. There is no complete TCP state machine.

The exit adapter has a new **optional** `--l4-only` flag: it disables IP-pair
fallback when an exact flow entry is missing. The existing default is unchanged.
For this scenario, use `--l4-only --l4-timeout 86400` and sufficient L4 capacity,
without `--classifier-file`. The exit cache remains LRU; after eviction or expiry,
a return packet is dropped instead of receiving another connection's labels.

Restarting the adapter loses admission and flow context. Restarting the switch
disables activation; retain the stable IDs and cookie in the configuration file.
This version does not support seamless service restarts or graceful draining of
diverted connections.

### Immediate stop

```bash
tuntomctl /run/tuntom/switch.control divert stop
```

`stop` sets `divert_enabled=0`. Subsequent unmarked packets from origin ports
follow normal switch rules, including packets belonging to previously diverted
flows. The command is idempotent and works even if the divert adapter ports are
disconnected. Both ST and MP support it; it does not stop either process.

Packets already carrying DVRT retain their existing routing, and packets already
being processed or queued may still reach the divert adapter. This is an immediate
cutover, not a graceful drain: active proxied connections may break. Adapter flow
contexts and admission timers are not reset. Use `tuntomctl SOCKET divert enable`
to resume diversion once both adapter ports are connected.

## Build and tests

From the repository root, without changes to `deploy/`:

```bash
cmake -S . -B /tmp/tuntom-divert-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE=-O2
cmake --build /tmp/tuntom-divert-build -j 4
ctest --test-dir /tmp/tuntom-divert-build --output-on-failure
```

The compiler setting keeps assertions enabled in the existing tests.
New tests: `divert_test`, `divert_integration_test`. The latter uses real ST/MP
processes and independently constructed IPC frames, covering mmap, rule reloads,
origin reconnects, and rejection of malformed blocks and overflow.

### Isolated end-to-end lab

This **standalone test script** creates and cleans up temporary namespaces and
routes. Production binaries do not manage this network setup. The rootless
launcher only adjusts tuntom's privilege drop for a single-UID user namespace,
as in the original lab.

```bash
g++ -std=c++17 -O2 -pthread -Isrc \
  experiments/divert_lab/tuntom_userns.cpp \
  -o /tmp/tuntom-divert-build/tuntom-userns

python3 experiments/divert_lab/run_integrated.py \
  --build /tmp/tuntom-divert-build --switch st --mtu 1500 \
  --output /tmp/divert-st-results

python3 experiments/divert_lab/run_integrated.py \
  --build /tmp/tuntom-divert-build --switch mp --mtu 9000 --vrf \
  --output /tmp/divert-mp-vrf-results
```

Output directories must be new. The lab verifies existing TCP bypass, two new TCP
connections, 1 MiB and 128 KiB echo transfers, preservation of the source IP/port,
and a TTL decrement in both directions through the router. `--vrf` places both
router TUNs in a VRF **inside** its temporary namespace. The lab does not measure
performance or test an actual smithproxy instance.

## Manually adding divert to a prepared topology

This example assumes that tuntom registers port `edge` with ingress stack
`[17,42]`; the client is `10.77.1.2` and the target server is `10.77.2.2`. The exit
adapter uses port `exit`, with bidirectional routing to the server network already
configured. Processes can access a shared filesystem and Unix socket. The commands
below set up the service and its namespace; use the isolated scenario above for
a complete lab from scratch.

Set these variables in each terminal:

```bash
B=/tmp/tuntom-divert-build
R=/tmp/tuntom-divert-manual
```

### 1. Configuration and switch — terminal A

```bash
mkdir -m 755 "$R"
cat > "$R/switch.rules" <<'EOF'
format 2
serial 1
exit exit
switch edge,[17,42] to exit,[99,42] allow bidir
EOF
cat > "$R/divert.conf" <<'EOF'
cookie hTX
ports divert-in divert-out
origin edge 123
match edge,[17,42]
EOF

"$B/tomtom-switch-mp" --socket "$R/switch.sock" \
  --control-socket "$R/switch.control" --rules-file "$R/switch.rules" \
  --divert-file "$R/divert.conf" --workers 2
```

For ST, use `tuntom-switch` with the same arguments except `--workers 2`.
Alternatively, from the repository root, use a lifecycle helper in place of the
direct switch invocation above:

```bash
./mk_switch_mp.sh divert-lab --socket "$R/switch.sock" \
  --control-socket "$R/switch.control" --rules-file "$R/switch.rules" \
  --divert-file "$R/divert.conf" --workers 2
```

Use `mk_switch.sh` and omit `--workers 2` for ST. The helper copies the divert
configuration into private staging, validates it before stopping the previous
instance, and saves the validated copy as `TUNTOM_STATE_DIR/switch-divert-lab/divert`
(default `/run/tuntom-mk/switch-divert-lab/divert`). Versioned rules are required;
the existing `pre/up` hook may generate them. MP `--auto-pool` and `--dry-run`
include the divert configuration and both adapter ports in the plan. Dry-run
reads the supplied files directly and does not run hooks.

Divert remains disabled after startup or restart; activate it in step 3 once both
adapter ports are connected. Each start must explicitly supply `--divert-file`;
a saved copy is not automatically reused when the option is omitted.

Connect tuntom and the exit adapter to this socket; adjust socket ownership for
the accounts running them. Run the exit adapter in your exit environment:

```bash
sudo "$B/tuntom-switch-adapter" ex0 --switch-socket "$R/switch.sock" \
  --switch-port-id exit --l4-only --l4-timeout 86400 --l4-capacity 1000000
```

### 2. Namespace and divert adapter — terminal B

Create the namespace manually. `ip netns exec` starts the process in that network
environment. See [ip-netns(8)](https://man7.org/linux/man-pages/man8/ip-netns.8.html).

```bash
sudo ip netns add tt-divert
sudo ip -n tt-divert link set lo up
sudo ip netns exec tt-divert sysctl -qw net.ipv4.ip_forward=1
sudo ip netns exec tt-divert sysctl -qw net.ipv4.conf.all.rp_filter=0
sudo ip netns exec tt-divert sysctl -qw net.ipv4.conf.default.rp_filter=0
sudo ip netns exec tt-divert sysctl -qw net.ipv4.conf.all.send_redirects=0

sudo ip netns exec tt-divert "$B/tuntom-divert-adapter" di0 do0 \
  --switch-socket "$R/switch.sock" --cookie hTX --mtu 1500 \
  --control-socket "$R/divert.control"
```

The adapter only creates/opens the TUNs, sets their MTU, and brings the links up.
It does not manage addresses, routes, forwarding, namespaces, or VRFs.

### 3. Routes and activation — terminal C

Once `di0` and `do0` exist:

```bash
sudo ip -n tt-divert addr add 10.77.254.1/32 dev di0
sudo ip -n tt-divert addr add 10.77.254.2/32 dev do0
sudo ip -n tt-divert route add 10.77.1.2/32 dev di0
sudo ip -n tt-divert route add 10.77.2.2/32 dev do0

sudo "$B/tuntomctl" "$R/divert.control" show stats
"$B/tuntomctl" "$R/switch.control" divert show
"$B/tuntomctl" "$R/switch.control" divert enable
```

Both divert adapter ports must be connected before enable. Open the old TCP
connection before enable and new connections afterward. In adapter statistics,
watch `bypass_packets`, `flow_entries`, `tun_rx_packets`, `tun_tx_packets`, and
counters ending in `_drops`. In the switch, watch `divert_overflow_drops` and
`divert_invalid_drops`.

### Alternative: VRF

Create the VRF and assign the TUNs to it manually **before adding routes**;
additional routes may be removed when an interface is assigned. This procedure
follows the [kernel VRF documentation](https://docs.kernel.org/networking/vrf.html).

Inside the same namespace, use this instead of the normal routing table setup
in step 3:

```bash
sudo ip -n tt-divert link add vrf-divert type vrf table 4123
sudo ip -n tt-divert link set vrf-divert up
sudo ip -n tt-divert link set di0 master vrf-divert
sudo ip -n tt-divert link set do0 master vrf-divert
sudo ip -n tt-divert addr add 10.77.254.1/32 dev di0
sudo ip -n tt-divert addr add 10.77.254.2/32 dev do0
sudo ip -n tt-divert route add table 4123 unreachable default
sudo ip -n tt-divert route add table 4123 10.77.1.2/32 dev di0
sudo ip -n tt-divert route add table 4123 10.77.2.2/32 dev do0
```

For a VRF in the native environment, run the divert adapter directly and omit
`-n tt-divert` from the `ip` commands. Configure forwarding in that environment
yourself. Assigning the TUNs to the VRF is sufficient for the Linux forwarding
verified here; sockets opened by an actual proxy need to be handled according
to its VRF support.

After the test, stop the divert adapter first (Ctrl-C in terminal B), then remove
the test namespace manually with `sudo ip netns delete tt-divert`.
