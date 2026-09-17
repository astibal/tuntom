# VIA services and ad hoc divert

VIA inserts an ordered list of local or remote services between an ingress port and an exit
adapter. Both switch implementations use the same routing core. The switch keeps
configuration and registered-port tables, **no per-flow table**. Flow context and
optional admission learning remain in the divert adapter.

## Compatibility

- Opt in with **rules format 3** and adapter `--via-instance ID`.
- Existing format 1/2 rules, legacy CLI routes, DVRT configuration, IPC V1/V2,
  mmap ABI, and the eight-label limit keep their existing meanings.
- The legacy adapter still requires `--cookie ABC` and defaults to warmup.
  VIA cookies are automatic and VIA adapters default to immediate admission.
- Remote adapters can use [IPC relay tunnels](RELAY.md); no remote switch is needed.
- No `strict`, PINs, single-sided/bidir attachment, or RETURN action.
- The adapter creates/opens and raises its two TUNs in its current namespace.
  It does not create namespaces, VRFs, addresses, or routes.

## Complete permanent configuration

See [the runnable example](../examples/via/switch.rules):

```text
format 3
serial 1
port edge id 123

service smithproxy {
    client-side proxy-in*
    server-side proxy-out*
    stickiness hash
    unavailable drop
}

service capture {
    client-side capture-in*
    server-side capture-out*
    stickiness failover
    instances ["capture#0", "capture#1"]
    unavailable pass
}

exit internet
switch edge,[17,42] to internet,[99,42] via [smithproxy,capture] allow bidir
```

`port NAME id N` assigns a nonzero uint64 identity independent of connection or
vector order. Names and IDs must be unique. Every selected VIA ingress needs an
explicit ID, including ports selected by wildcards. An undeclared ingress drops
when it matches a VIA rule; it cannot silently bypass that rule.

A service requires two attachment patterns. Patterns match the adapter's declared
attachment name before its reserved suffix. A trailing `*` uses existing prefix
matching. A registration must match exactly one service on its side. Service
names use letters, digits, `_`, `-`, and `.`. Instance IDs additionally allow `#`;
quote IDs containing `#` in configuration because it starts a comment otherwise.

- `stickiness hash` (default): symmetric flow hash and rendezvous selection over
  available instance IDs, independent of registration order. Flow hash is
  `hash(src:sport) XOR hash(dst:dport)`; protocol/IP version are hash domains.
- `stickiness failover`: requires `instances [...]`, uses the first complete live
  instance in that explicit order. Recovery makes an earlier instance eligible
  again immediately. Connection order never establishes priority.
- `instances [...]` is also an optional allowlist for `hash`.
- `unavailable drop` (default): drop when no complete instance exists.
- `unavailable pass`: skip that service and continue in the current direction.

Selection is repeated from the service definition on both directions. Changing
membership can move a flow; no switch-side session affinity survives a failure.
This suits a stateless capture service. A stateful proxy may lose its session.
One proxy instance with `unavailable drop` avoids migration to another instance.
A service may appear only once in a chain.

The permanent chain is active as soon as its rule is loaded. Ordered rule matching
and destination policy still apply; final forwarding/rewrite uses the original
logical ingress and saved labels. The destination must be an `exit` adapter that
preserves the entire label stack. `allow bidir` generates the ordinary reverse
rule without another VIA offer; VIA itself carries the reverse traversal.

## Registration and running adapters

From the repository root, after building:

```bash
cmake -S . -B /tmp/tuntom-via-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE=-O2
cmake --build /tmp/tuntom-via-build -j4

/tmp/tuntom-via-build/tuntom-switch \
  --socket /tmp/via.sock --control-socket /tmp/via.control \
  --rules-file examples/via/switch.rules
```

Alternatively use `tomtom-switch-mp` with the same options plus `--workers 2`.
The existing installation helpers accept these files unchanged:

```bash
sudo ./mk_switch.sh via --rules-file examples/via/switch.rules
# Or inspect the MP plan before installation:
./mk_switch_mp.sh via --dry-run --auto-pool --rules-file examples/via/switch.rules
```

Run the adapter in another terminal or your process supervisor:

```bash
sudo /tmp/tuntom-via-build/tuntom-divert-adapter di0 do0 \
  --switch-socket /tmp/via.sock --via-instance 'smithproxy#0' \
  --divert-in-port proxy-in0 --divert-out-port proxy-out0 \
  --admission immediate --mtu 1500
```

This registers:

```text
proxy-in0~via:c:smithproxy#0   client-side
proxy-out0~via:s:smithproxy#0  server-side
                   ^ same instance ID
```

The adapter chooses and sends its instance ID. Assign a stable, unique ID through
`--via-instance`; it is never a positional index. The full registration name must
fit the existing 63-byte IPC limit. `~via:` is reserved in format 3.

A live ID reserves both side slots. Duplicate live sides, including different
attachment names with the same instance ID/side, are rejected. The companion side
must come from the same local process, verified with `SO_PEERCRED`. A pair becomes
available only after both sides register; losing either excludes the whole pair.
Ordinary legacy named-port replacement remains unchanged.

No separate cookie exchange is required: the switch generates three random ASCII
letters and includes them in every offer. The adapter saves that context and
returns the cookie unchanged. This is a framing discriminator, not authentication.
The local IPC socket's access permissions remain the trust boundary.

Capture is unavailable in this example until an implementation registers its pair;
`unavailable pass` skips it. The divert adapter provides TUNs and label restoration,
not a PCAP writer or smithproxy configuration.

## Manual namespace / VRF setup

The following commands are an example for a Linux router standing in for the
proxy. Run them yourself, adapting interface names and endpoint addresses. The
switch uses a filesystem Unix socket, reachable from a network namespace.
Start the switch first; create the namespace before starting the adapter:

```bash
sudo ip netns add via-proxy
sudo ip -n via-proxy link set lo up
sudo ip netns exec via-proxy sysctl -w net.ipv4.ip_forward=1
sudo ip netns exec via-proxy sysctl -w net.ipv4.conf.all.rp_filter=0
sudo ip netns exec via-proxy sysctl -w net.ipv4.conf.default.rp_filter=0

sudo ip netns exec via-proxy /tmp/tuntom-via-build/tuntom-divert-adapter di0 do0 \
  --switch-socket /tmp/via.sock --via-instance 'smithproxy#0' \
  --divert-in-port proxy-in0 --divert-out-port proxy-out0 --admission immediate
```

After the adapter has opened the TUNs, in another terminal (example client
`10.77.1.2`, server `10.77.2.2`):

```bash
sudo ip -n via-proxy address add 10.77.254.1/32 dev di0
sudo ip -n via-proxy address add 10.77.254.2/32 dev do0
sudo ip -n via-proxy route add 10.77.1.2/32 dev di0
sudo ip -n via-proxy route add 10.77.2.2/32 dev do0
```

For an optional VRF, create it and enslave both TUNs **before adding those routes**;
then use `route add table 4123 ...` for both routes:

```bash
sudo ip -n via-proxy link add vrf-via type vrf table 4123
sudo ip -n via-proxy link set vrf-via up
sudo ip -n via-proxy link set di0 master vrf-via
sudo ip -n via-proxy link set do0 master vrf-via
sudo ip -n via-proxy route add table 4123 unreachable default
```

Configure the real exit adapter and endpoint routing separately. Stop the adapter
before removing this namespace: `sudo ip netns delete via-proxy`.

## Ad hoc parity

Use [adhoc.rules](../examples/via/adhoc.rules), which defines the same services but
has no permanent `via` clause, with [divert.conf](../examples/via/divert.conf):

```text
format 3
match edge,[17,42] via [smithproxy,capture]
```

```bash
/tmp/tuntom-via-build/tuntom-switch \
  --socket /tmp/via.sock --control-socket /tmp/via.control \
  --rules-file examples/via/adhoc.rules --divert-file examples/via/divert.conf

/tmp/tuntom-via-build/tuntomctl /tmp/via.control divert enable
/tmp/tuntom-via-build/tuntomctl /tmp/via.control divert show
/tmp/tuntom-via-build/tuntomctl /tmp/via.control divert stop
```

Use `--admission warmup` on the adapter for graceful insertion. TCP/UDP learning
uses the existing adapter timers and capacity rules described in [DIVERT.md](DIVERT.md).
An admission BYPASS skips the **whole chain** and resumes normal forwarding.

Ad hoc matching runs before ordinary switch rules and takes precedence over a
permanent chain. Continuation then applies the original ingress rules, including
drop policies. It uses the same services, selection, unavailable policy,
envelope, and adapter as permanent rules. A normal forwarding rule is still
required for final delivery. `enable` can succeed without connected instances;
the configured unavailable policy decides packet handling.

`stop` disables new ad hoc offers immediately. Already marked packets can finish;
permanent chains remain active. This is still the crude stop: active sessions can
break, with no drain, flow migration, or admission timer reset. Match files are
read at process startup; service/routing definitions support transactional rules
`check/load/show` as before.

## Label layout and TCP handshake

```text
[ routing... | HEADER | CTX | ORIGIN | saved... ]

HEADER = cookie:24 | "VIA":24 | version:8 | envelope_length:8
CTX    = chain_id:32 | step:16 | saved_count:8 | flags:8
flags  = reserved:4 | action:3 | direction:1
```

`direction`: forward=0, reverse=1. `action`: OFFER=0, CONTINUE=1, BYPASS=2,
COMPLETE=3; other values and reserved bits drop. Version is 1. The reserved marker
is the `VIA` field in HEADER. Both counts are checked, with
`envelope_length == 3 + saved_count`. `step=65535` denotes COMPLETE.

Example cookie `hTX`, chain ID 1, origin 123, saved `[17,42]`:

```text
HEADER H = 0x6854585649410105
CTX forward step 0 OFFER    = 0x0000000100000200
CTX forward step 0 CONTINUE = 0x0000000100000202
CTX reverse step 0 CONTINUE = 0x0000000100000203
CTX forward step 1 OFFER    = 0x0000000100010200
CTX forward step 1 CONTINUE = 0x0000000100010202
CTX COMPLETE               = 0x00000001ffff0206
CTX reverse step 1 OFFER    = 0x0000000100010201
CTX reverse step 1 CONTINUE = 0x0000000100010203
CTX reverse step 0 OFFER    = 0x0000000100000201
```

Each VIA stack below has **seven labels**, regardless of the number of services:

```text
Client SYN -> switch adapter:                 [17,42]
  -> smithproxy divert adapter/client TUN:    [17,42,H,0x0000000100000200,123,17,42]

Proxy-generated SYN+ACK -> client TUN read:    [17,42,H,0x0000000100000203,123,17,42]
  -> switch adapter -> client:                [17,42]

Upstream SYN -> server TUN read:              [17,42,H,0x0000000100000202,123,17,42]
  -> capture divert adapter/client TUN:       [17,42,H,0x0000000100010200,123,17,42]
Capture server TUN read:                      [17,42,H,0x0000000100010202,123,17,42]
  -> switch adapter -> real exit adapter:     [99,42,H,0x00000001ffff0206,123,17,42]

Real server SYN+ACK -> exit adapter:          [99,42,H,0x00000001ffff0206,123,17,42]
  -> capture divert adapter/server TUN:       [99,42,H,0x0000000100010201,123,17,42]
Capture client TUN read:                      [99,42,H,0x0000000100010203,123,17,42]
  -> smithproxy divert adapter/server TUN:    [99,42,H,0x0000000100000201,123,17,42]
Proxy client TUN read:                        [99,42,H,0x0000000100000203,123,17,42]
  -> switch adapter -> origin 123 -> client:  [17,42]
```

The TUN that produces a packet determines its direction. The adapter never tries
to infer whether a proxy consumed or forwarded SYN/SYN+ACK. Client TUN reads go
backwards from the current step; server TUN reads go forwards. This also supports
a generated response from an intermediate service.

The envelope contains a chain identity and step, not an instance index. The switch
resolves that pair to the named service and selects a currently available instance.

## Bounds, reload, and observability

- `routing_count + 3 + saved_count <= 8`; overflow drops, never truncates. Two
  initial labels take seven slots; rewriting to three routing labels takes eight.
- Chain IDs are allocated monotonically per switch lifetime. Topology changes keep
  IDs; a loaded rules generation gets new IDs. `check` does not publish them.
  Old/unknown chain IDs drop immediately after reload; old flows are not drained.
  After VIA has been enabled, retired envelopes and reserved service ports also
  drop following a downgrade to format 1/2.
- IDs refer to immutable rule/service definitions, not rule positions on reload.
  Control-plane generation tables are reclaimed after their rules objects expire.
- The adapter supports unfragmented IPv4/IPv6 TCP/UDP and preserves separate label
  contexts per direction. Endpoint identities must survive traversal; NAT and
  overlapping identical tuples from different origins are unsupported.
- A remapped flow can first arrive on the server side; VIA learns its orientation
  there as well. This restores labels, not a stateful proxy's missing TCP session.
- Adapter caches remain bounded, never evict live entries for capacity pressure,
  and reject conflicting contexts. Restart/reload can therefore require fresh
  connections or adapter context expiry/restart.
- Stats expose `via_enabled=1`; routing counters reuse `divert_forwarded`,
  `divert_invalid_drops`, and `divert_overflow_drops`. Missing members use
  `target_disconnected`. Adapter stats include `via_instance`, `admission_mode`,
  flow/capacity/context errors and the existing IPC counters.
- MP creates only the links that a configured chain can traverse, with the existing
  bounded queues. It does not connect every ingress to every other ingress.

## Validation

Automated tests cover codec bounds, parser/export, both handshake paths, two-service
chains, duplicate registrations, process ownership, half-connected instances,
failover order/recovery, unavailable pass/drop, ad hoc stop, reload, and ST/MP IPC.

Run the process tests (they need permission to bind temporary Unix sockets):

```bash
ctest --test-dir /tmp/tuntom-via-build --output-on-failure \
  -R '^(via_.*|divert_.*|switch_ruleset.*|switch_ecmp.*)$'
```

For the isolated real TCP/TUN lab, build its user-namespace launcher and run:

```bash
g++ -std=c++17 -pthread -O2 -Isrc experiments/divert_lab/tuntom_userns.cpp \
  -o /tmp/tuntom-via-build/tuntom-userns
python3 experiments/divert_lab/run_integrated.py --via --switch st \
  --build /tmp/tuntom-via-build --output /tmp/via-st-lab
python3 experiments/divert_lab/run_integrated.py --via --switch mp --vrf --mtu 9000 \
  --build /tmp/tuntom-via-build --output /tmp/via-mp-vrf-lab
```

Use new output directories. The automated lab uses a Linux router, not smithproxy.
Manual lab testing and actual smithproxy/TPROXY validation remain pending.
