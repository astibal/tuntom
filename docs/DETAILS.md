# tuntom implementation details

## Motivation

`tuntom` started as a very small requirement: create a lightweight L3 tunnel
over UDP without bringing in WireGuard, GRE-over-UDP plumbing, OpenSSL, or a
larger VPN stack.

The intended model is:

```text
TUN -> UDP -> network/NAT -> UDP -> TUN
```

The implementation deliberately keeps transport and Linux networking separate.

`tuntom` only moves IP packets between a Linux TUN interface and UDP. What
happens to packets after they appear on the remote TUN interface is up to the
host:

- normal routing
- policy routing
- nftables / iptables
- transparent proxying
- smithproxy
- honeynet routing
- IDS/IPS
- packet capture
- custom applications

This makes the tunnel useful as a small remote L3 ingress primitive rather than
a complete VPN product.

## Deployment model

The implementation lives in ordinary C++ headers and `../src/main.cpp`. Both
local and remote builds compile these directly with
`-O2 -march=native -mtune=native`, targeting each host independently. For
deployment the source directory is streamed as a tar archive and extracted into
a private temporary directory on the remote host:

```text
src/ -> tar
    |
    | SSH stdin
    v
temporary src/ -> remote g++ -> remove temporary src/
    |
    v
/tmp/udp_tun-ID-server
```

`../mk_tunnel.sh`:

1. compiles the client locally from `../src/main.cpp`
2. sends `../src/` as a tar stream over SSH into a temporary directory
3. compiles it remotely and cleans up the temporary source directory
4. stops the previous instance
5. starts the server
6. creates/configures the server TUN interface
7. starts the client
8. creates/configures the client TUN interface
9. configures optional per-tunnel Linux networking
10. runs lifecycle hooks
11. tests the tunnel with ping

For normal remote use, the remote login defaults to `root`.

The current bootstrap mechanism is mainly a convenient deployment/testing tool.
The C++ program itself does not depend on SSH.

## Addressing

Tunnel ID remains the main identity parameter.

For ID `X`:

```text
local/client interface:  utXc
remote/server interface: utXs

local/client IP:  10.254.X.1
remote/server IP: 10.254.X.2

UDP port: 40000 + X
```

The current script accepts IDs from 1 to 255.

MTU is intentionally configured separately from tunnel identity.

Defaults:

```text
TUNTOM_MTU=1500
TUNTOM_TRANSPORT_MTU=1400
```

`TUNTOM_MTU` is the inner/TUN MTU presented to the surrounding Linux network.

`TUNTOM_TRANSPORT_MTU` selects the outer IP packet size used for tuntom UDP
transport calculations. With automatic PMTUD it is the initial probe target, not
a hard ceiling; the discovered active transport MTU is used for fragmentation.
With `--no-pmtud` it is the fixed transport MTU.

This separation allows tuntom to remain transparent on networks using either
standard Ethernet MTU or jumbo frames.

## MTU model

The tunnel must not expose its transport limitations through the TUN interface
if it can avoid doing so.

The inner MTU and transport MTU are therefore independent:

```text
inner/TUN MTU
    |
    | IP packet
    v
tuntom
    |
    | optional internal fragmentation
    v
UDP transport MTU
```

Example:

```text
TUN MTU:       1500
transport MTU: 1400
```

A 1500-byte inner packet is accepted normally by the TUN interface. If it cannot
fit into one authenticated tuntom UDP datagram, tuntom fragments it internally.

Likewise, a jumbo deployment can use for example:

```text
TUN MTU:       9000
transport MTU: 1500
```

The systems behind the tunnel still see MTU 9000.

## Protocol v5

Protocol v5 is the only supported wire protocol. Both endpoints must be
upgraded. The UDP socket identifies the tunnel; its configured ID still binds
key derivation. There is no magic, and version is carried only by INIT/RESPONSE.

```text
offset  size  field
0       1     type (bits 0..3), reserved (4..5), fragment (6), encryption (7)
1       8     sequence (16-bit session hint + 48-bit counter)
9       ...   type-specific extension
M       16    auth_tag (M = 9 + extension size)
M+16    ...   payload
```

Headers: DATA/HELLO/KEEPALIVE 25 B; fragmented DATA 37 B; PING/PONG/CONFIRM/ACK
33 B; PMTUD 35 B; INIT/RESPONSE 34 B. Fragment extensions hold message ID (8 B),
offset (2 B), original length (2 B). Complete DATA packets carry none of these
fields. See [wire specification](PROTOCOL_V5.md).

Packet types are:

```text
1  HELLO
2  KEEPALIVE
3  DATA
4  PING
5  PONG
6  MTU_PROBE
7  MTU_REPLY
8  INIT
9  RESPONSE
10 CONFIRM
11 CONFIRM_ACK
```

For HELLO and KEEPALIVE:

```text
message_id       = 0
fragment_offset  = 0
original_length  = 0
payload           empty
```

For DATA, each UDP datagram represents either the complete inner packet or one
tuntom fragment.

`PING` and `PONG` provide authenticated runtime RTT measurements.

For PMTUD messages, `message_id` is the probe ID and `original_length` is the
candidate/observed outer MTU. `MTU_PROBE` padding makes the outer packet reach
that size; `MTU_REPLY` has no payload.

## V5 authentication

Each v5 datagram is authenticated independently.

The authenticated data is:

```text
all header bytes before the tag
+
payload
```

The 16-byte authentication tag following the type-specific extension is not
included in its own MAC input.

This means the following are authenticated for every fragment:

- configured tunnel ID through key derivation (not transmitted)
- packet type
- transport sequence number
- message ID
- fragment offset
- original packet length
- fragment payload

A fragment cannot therefore be moved to a different offset, message, or original
packet length without invalidating authentication.

## Authentication construction

The implementation has no OpenSSL dependency.

It uses [AMAC v1](AMAC_V1.md), which specifies the complete MAC algorithm, byte
order, padding, regression vectors and implementation history.

The master secret is supplied through:

```text
TUNTOM_SECRET
```

It must currently be exactly 128 bits represented by 32 hexadecimal characters.

V5 derives two keys directly from the master secret using the existing MAC:

```text
K_c2s = MAC(master, tunnel_id, "TUNTOM-V4-CLIENT-TO-SERVER")
K_s2c = MAC(master, tunnel_id, "TUNTOM-V4-SERVER-TO-CLIENT")
```

Labels are ASCII bytes without a trailing NUL. The client sends with K_c2s and
verifies with K_s2c; the server does the reverse. The version, direction label,
and tunnel ID separate the authentication domains. An endpoint cannot accept its
own outbound datagram as inbound traffic. Legacy packets are rejected; there is
no fallback. These long-term direction keys authenticate only INIT and RESPONSE.
DATA and CONFIRM/ACK use session-specific direction keys derived from both
complete handshake messages; see the wire specification.

## Session handshake, sequence numbers and replay protection

The exact layouts, domain labels and state transitions are specified in
[PROTOCOL_V5.md](PROTOCOL_V5.md). V5 establishes fresh session keys through INIT
/ RESPONSE / CONFIRM / CONFIRM_ACK before accepting DATA. Suite 2 with payload
encryption and PFS is the default. `--crypto-auth-only` selects plaintext suite
0; suite 1 remains wire-recognized but has no command-line selector.

The 64-bit SEQ field consists of a 16-bit transcript-derived session hint and an
independent 48-bit packet counter for each direction. Counter zero is reserved
for CONFIRM/ACK; ordinary traffic starts at one. The hint selects candidate
keys; AMAC determines the actual session. A collision is supported.

Every session has its own 64-value replay window and reassembly table. Only
counter bits enter the replay window, after successful authentication. The
first ordinary packet need not have counter one: reordering is allowed. Replayed
confirmations never reset these structures. Old session receive keys remain
valid for a three-second overlap; previous-session packets cannot change the
current peer. There are at most two candidate session keys.

Restarted receivers have no active session. Replaying a still-time-valid old
INIT may elicit an authenticated response containing a new server nonce, but
cannot restore old keys or authorize old DATA. The client restarts the handshake
after 20 seconds without authenticated active-session traffic. INIT freshness
uses an authenticated Unix timestamp and the configurable total `--init-window`
(default 300 seconds, +/-150). Nonces are remembered for their full remaining
validity. No wall clock is used for V5 DATA packet sequencing. Exhausted 48-bit
counters never wrap.

Legacy v2 retains its timestamp generator semantics and runtime-only replay
protection. Legacy receive is not supported.

`message_id` remains separate from SEQ. All fragments of one original IP packet
share it, while each datagram uses a distinct counter. Message IDs still use the
monotonic timestamp generator and are scoped by session.

## Balanced fragmentation

Fragmentation is internal to tuntom.

The sender first computes the minimum number of fragments needed for the current
transport payload limit:

```text
count = ceil(packet_size / max_fragment_payload)
```

It then divides the original packet into approximately equal-sized fragments
rather than filling every fragment to the maximum and leaving a small tail.

Examples:

```text
1500 -> 750 + 750
1401 -> 701 + 700
2400 -> 1200 + 1200
2401 -> 801 + 800 + 800
```

This has several useful properties:

- no small tail fragment pattern
- more uniform UDP datagram sizes
- no artificial reduction of the visible TUN MTU
- fragmentation is independent of IPv4/IPv6 fragmentation semantics
- the original inner IP packet is not modified by fragmentation

The receiver does not depend on equal fragment sizes. Reassembly uses
authenticated offsets and original length.

## Transport payload calculation

For protocol v5:

```text
max_fragment_payload =
    transport_mtu
    - outer_ip_header
    - UDP_header
    - v4_header
```

For IPv4 transport:

```text
outer IP header = 20
UDP header      = 8
fragment header = 37
```

For IPv6 transport:

```text
outer IP header = 40
UDP header      = 8
fragment header = 37
```

Complete DATA uses 25 bytes instead of 37; PMTUD padding uses its own 35-byte
header.

The dual-stack server uses the conservative IPv6 overhead when calculating the
safe payload size.

This may waste 20 bytes when the peer is actually IPv4-mapped, but avoids
exceeding the active transport MTU.

## Automatic path-MTU discovery

Protocol v5 performs authenticated application-level PMTUD by default. It does
not depend on receiving ICMP Packet Too Big / Fragmentation Needed messages.

At startup the active outer transport MTU is conservatively set to 500 bytes.
The endpoint sends a padded `MTU_PROBE` whose complete outer IP packet has the
candidate size. A receiver accepts it only when the declared and observed sizes
match, then returns an `MTU_REPLY` carrying the same authenticated probe ID and
size.

The search works as follows:

```text
known-good start: 500 bytes
first candidate:  configured --transport-mtu (default 1400)
upper bound:      max(1500, configured --transport-mtu)
probe timeout:    2 seconds
remaining range: binary search between known-good and known-bad
```

Normal DATA traffic uses the current known-good value throughout discovery. A
local send failure or missing reply marks that candidate as bad. Discovery is
restarted when the authenticated UDP peer changes or a DATA send fails.

The UDP socket enables kernel "do not fragment" PMTU behavior where supported.
Use `--no-pmtud` to disable discovery and use `--transport-mtu` as a fixed
value. The minimum accepted fixed or discovery MTU is 500 bytes.

The bootstrap script currently leaves PMTUD enabled on both endpoints.

## Reassembly

The receiver identifies the session, authenticates and replay-checks every
fragment before it is considered for reassembly.

Reassembly is keyed by `message_id` within each session.

Each reassembly entry contains:

- expected original length
- packet buffer
- received byte ranges
- received byte count
- last update time

The current implementation deliberately uses bounded state:

```text
maximum packet size:          configured TUN MTU
maximum incomplete messages:  64
maximum reassembly memory:     4 MiB
maximum fragments per packet: 64
reassembly timeout:            3 seconds
```

Fragments with invalid metadata are rejected.

Partial overlaps are rejected.

Duplicate/overlapping byte ranges are not used to advance reassembly.

A packet is released to the TUN side only when all bytes from offset zero
through `original_length` have been received.

## TUN processing boundary

The virtual packet processing hook remains defined on complete logical inner
packets.

Conceptually:

```cpp
virtual bool process(Packet& packet, Direction direction);
```

For transmit:

```text
read complete packet from TUN
-> process(tun_to_udp)
-> fragment
-> authenticate each fragment
-> UDP
```

For receive:

```text
UDP
-> authenticate fragment
-> replay check
-> reassembly
-> process(udp_to_tun)
-> TTL/Hop-Limit compensation
-> write complete packet to TUN
```

This means users of the processing hook do not need to know that tuntom
transport fragmentation exists.

The fragmentation layer is purely an internal transport detail.

## TTL / IPv6 Hop-Limit compensation

A routed tuntom deployment normally introduces two Linux routing points:

```text
sender-side router
-> tuntom transport
-> receiver-side router
```

Without compensation, the hidden transport topology can therefore consume an
extra visible IP hop.

By default, tuntom compensates one hop immediately before writing a received
complete packet into the TUN interface.

IPv4:

```text
TTL = min(TTL + 1, 255)
recompute IPv4 header checksum
```

IPv6:

```text
Hop Limit = min(Hop Limit + 1, 255)
```

The important placement is:

```text
decode
-> authentication
-> replay validation
-> reassembly
-> packet processing
-> TTL/Hop-Limit compensation
-> write(TUN)
```

The TTL is not modified on individual fragments and is not modified merely
because a UDP packet was decoded.

The compensation exists specifically because the packet is about to be injected
into a TUN interface and routed again by the receiving kernel.

It can be disabled using:

```text
--no-ttl-compensate
```

## Known TTL limitation: locally generated traffic

The compensation assumes that the packet entering tuntom was already forwarded
by the sending Linux host before it entered the sending TUN interface.

That assumption is correct for the main routed/honeynet use case.

It is not correct for traffic generated locally on a tuntom endpoint.

Example:

```text
local ping process
-> TUN
-> tuntom
```

The packet has not consumed a forwarding hop before reaching tuntom.

The receiver nevertheless applies its normal `+1` compensation before writing
the packet to its TUN.

As a result, locally generated traffic may appear on the remote side with TTL or
Hop Limit one higher than expected.

`tuntom` intentionally does not try to detect locally generated traffic.

Reliable detection would require additional complexity such as:

- kernel metadata
- packet marks
- netfilter/eBPF coupling
- local-address classification
- extra protocol flags

This is considered a known limitation and an intentional KISS tradeoff.

The administrator of a tuntom endpoint is assumed to know that the local machine
participates in the tunnel.

## NAT behavior

The client initiates UDP traffic.

The server learns the current client UDP source address and port from valid
traffic.

For authenticated protocol versions, the peer is updated only after:

- protocol validation
- authentication validation
- replay validation

A random unauthenticated UDP packet therefore cannot simply redirect the
server's return path.

## Protocol versions

Only V5 is transmitted and accepted. Legacy `--allow-v1` / `--allow-v2` flags
are rejected.

## Main classes

The implementation remains intentionally KISS.

The important classes are roughly:

```text
Protocol
ProtocolV5
TunDevice
UdpEndpoint
Reassembler
Tunnel
```

`Tunnel` contains the main packet loop.

It also provides the virtual processing hook described above so the transport
can later be integrated into another C++ application without rewriting the
transport path.

This is particularly useful for integrating the tunnel directly into software
such as smithproxy.

## Using the code in another program

The easiest integration path is to include `../src/tunnel.hpp` and derive from
`tuntom::Tunnel`.

For example, conceptually:

```cpp
class MyTunnel : public Tunnel {
protected:
    bool process(Packet& packet, Direction direction) override {
        if (direction == Direction::udp_to_tun) {
            inspect_packet(packet);
        }

        return true;
    }

private:
    void inspect_packet(Packet& packet) {
        // Application-specific processing.
    }
};
```

Another program may eventually want to replace the TUN endpoint entirely and
inject packets directly into its own packet-processing engine.

The current source does not introduce an abstraction for that yet. This is
deliberate. The location is obvious, and the abstraction should be added only
when a real integration needs it.

## Using the binary without the bootstrap script

The C++ binary supports client/server roles independently of `../mk_tunnel.sh`.

Examples:

```text
udp_tun server 42 ut42s --mtu 1500 --transport-mtu 1400
udp_tun client 42 ut42c remote.example --mtu 1500 --transport-mtu 1400
```

Relevant options include:

```text
--mtu <n>
--transport-mtu <n>
--pmtud
--no-pmtud
--no-ttl-compensate
--ttl-compensate
--debug
--quiet
```

PMTUD state is included in the text statistics output as
`transport_mtu_configured`, `transport_mtu_active`, `pmtud_known_good`,
`pmtud_known_bad`, and the sent/successful/lost probe counters.

The binary sets the requested TUN MTU itself through `SIOCSIFMTU`.

The bootstrap script also sets the interface MTU explicitly using `ip link`.
This is redundant but intentionally harmless: the binary works standalone, while
the bootstrap script also asserts the desired Linux interface state.

## Bootstrap MTU configuration

`../mk_tunnel.sh` uses:

```text
TUNTOM_MTU
TUNTOM_TRANSPORT_MTU
```

Defaults are:

```text
TUNTOM_MTU=1500
TUNTOM_TRANSPORT_MTU=1400
```

Both values are passed to the local and remote C++ processes.

`TUNTOM_MTU` is also used when configuring both TUN interfaces.

The same values are exported to lifecycle hooks.

`../mk_tunnel.sh` also enables text statistics and writes them atomically under
its runtime directory as `IDc.stats` and `IDs.stats` (normally
`/run/tuntom/ID{c,s}.stats`). The format can currently only be `txt` and is
selected with `TUNTOM_STATS_FORMAT`.

## Testing PMTUD black holes

The repository includes `examples/pmtud-iptables-test.sh` for simulating silent
loss of oversized IPv4 UDP datagrams:

```bash
sudo ./examples/pmtud-iptables-test.sh --size 1200 --interface eth0
sudo ./examples/pmtud-iptables-test.sh --size 1200 --interface eth0 --remove
```

It installs matching INPUT and OUTPUT length-based DROP rules. Because those
rules apply to all IPv4 UDP traffic on the interface, they should be removed as
soon as the test is complete.

## Lifecycle hooks

The bootstrap supports:

```text
pre/down
post/down
pre/up
post/up
```

Hook files are stored only on the local/caller host.

Defaults:

```text
/etc/tuntom/tuntom-pre.sh
/etc/tuntom/tuntom-post.sh
```

They can be overridden using:

```text
TUNTOM_PRE_HOOK
TUNTOM_POST_HOOK
```

The same hook file content is:

- executed directly on the local side
- streamed through SSH stdin and executed using `bash -s` on the remote side

The remote machine therefore does not need a persistent copy of the hook file.

The side is exposed as:

```text
TUNTOM_SIDE=local
TUNTOM_SIDE=remote
```

Useful exported values include:

```text
TUNTOM_ID
TUNTOM_ACTION
TUNTOM_PHASE
TUNTOM_SIDE
TUNTOM_IF
TUNTOM_LOCAL_IP
TUNTOM_PEER_IP
TUNTOM_CLIENT_IP
TUNTOM_SERVER_IP
TUNTOM_UDP_PORT
TUNTOM_MTU
TUNTOM_TRANSPORT_MTU
TUNTOM_SNAT
TUNTOM_MSS_CLAMP
TUNTOM_MARK
TUNTOM_MARK_MASK
TUNTOM_TABLE
TUNTOM_CHAIN
TUNTOM_NAT_CHAIN
TUNTOM_SNAT_CHAIN
TUNTOM_MANGLE_CHAIN
TUNTOM_FORWARD_CHAIN
```

`post/up` is the natural point for custom routes, DNAT rules, forwarding policy,
and other networking that depends on tuntom-created chains/tables.

`pre/down` is the natural point for explicit cleanup of custom routes or rules
that must be removed while the TUN and routing table still exist.

Rules inserted into tuntom-owned per-tunnel chains normally do not need explicit
removal because those chains are deleted during tunnel teardown.

## Routing and firewalling

`tuntom` itself does not attempt to become a full VPN or routing policy engine.

Linux policy is left to the administrator and helper scripts.

Typical surrounding configuration may include:

- source policy routing
- interface-based policy routing
- conntrack marks
- DNAT
- SNAT/MASQUERADE
- forwarding rules
- honeynet routes
- transparent proxying

This separation is intentional.

The tunnel transports packets. Linux decides where they go.

The optional `../tuntom-net.sh` helper makes the ingress tunnel authoritative
for connection return routing. Every tracked packet received from a TUN
interface overwrites the tuntom-owned bits of its conntrack mark, regardless of
whether conntrack classifies it as `NEW`, `ESTABLISHED`, or `RELATED`. If the
same connection is observed through multiple tunnels, the most recent TUN
ingress wins.

The ingress packet keeps those bits clear in its packet mark and therefore uses
normal destination routing. Forwarded replies restore the connection mark in
`mangle/PREROUTING`; locally generated replies restore it in `mangle/OUTPUT`.
The resulting fwmark selects the tunnel-specific routing table. Packets arriving
from the TUN as `INVALID` or `UNTRACKED` are dropped because no persistent
return path can be associated with them.

## Honeynet use case

One important deployment model is:

```text
Internet
    |
remote public probe
    |
tuntom
    |
core
    |
honeynet
```

The public probe can DNAT selected public services into the honeynet without
SNAT, preserving the original Internet source address.

The core can use tunnel-specific policy routing to ensure honeynet replies
return through the same probe.

Keeping the TUN MTU at the surrounding infrastructure's normal MTU reduces an
obvious tunnel fingerprint.

TTL/Hop-Limit compensation similarly hides one otherwise artificial routing hop
for forwarded traffic.

Together these features make tuntom useful as a small transport primitive for
geographically or logically remote honeynet probes while keeping the
honeypot-side network behavior relatively normal.

## Wireshark dissector

`tuntom.lua` understands protocol v1, v2, v3, v4, and v5.

For v5 it exposes fields including:

```text
tuntom.sequence
tuntom.message_id
tuntom.fragment_offset
tuntom.original_length
tuntom.fragment_length
tuntom.fragment_end
tuntom.fragmented
tuntom.auth_tag
```

The dissector also performs its own v5 fragment reassembly.

The reassembly key includes:

- UDP endpoint addresses and ports
- message ID
- packet direction
- protocol version and session hint (v5; hint collisions cannot be resolved
  without keys)

Once all ranges are available, the Lua dissector creates a synthetic Tvb
containing the original IP packet and hands it to Wireshark's normal IPv4 or
IPv6 dissector.

It also exposes reassembly metadata such as:

```text
tuntom.reassembled
tuntom.reassembled_length
tuntom.reassembled_in
tuntom.fragment_of
```

The Lua dissector does not verify the Ascon authentication tag.

## Debugging

### Replay regression tests

Run the replay tests without root privileges or a live tunnel:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic tests/replay_test.cpp -o /tmp/tuntom-replay-test
/tmp/tuntom-replay-test
```

The tests cover sparse timestamp reordering, duplicates, window eviction,
integer boundaries, and a 9000-byte v5 packet received as 64 fragments in
reverse order. They exercise the receive path's protocol, replay, and reassembly
components, but do not establish the security of the MAC.

### MAC regression tests

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic tests/mac_test.cpp -o /tmp/tuntom-mac-test
/tmp/tuntom-mac-test
```

These cover reference permutation comparisons, project-specific MAC vectors at
block and padding boundaries, v5 round trips, wrong keys/tunnel IDs, single-bit
changes throughout each datagram, and the previous XOR forgery. The MAC vectors
are for tuntom's custom construction, not standardized Ascon-Mac test vectors.

### Logging

Default logging is informational.

Use:

```text
--debug
```

for packet dumps, protocol details, fragmentation information, and accepted
packet metadata.

Useful log messages include:

```text
TUN read ...
FRAGMENT 1/2 ...
UDP recv ...
ACCEPT v5 ...
TUN write ...
```

For an IPv4 packet, valid inner data will normally begin with a first byte whose
upper nibble is `4`, commonly:

```text
45 ...
```

For IPv6, the upper nibble is `6`, commonly:

```text
60 ...
```

## Design philosophy

The main rule is KISS.

Features are accepted when they remove significant operational complexity or
preserve useful transparency without turning tuntom into a framework.

Current examples are:

- authenticated UDP transport
- replay protection
- internal fragmentation
- configurable inner and transport MTU
- bounded reassembly
- one-hop TTL/Hop-Limit compensation
- lifecycle hooks

At the same time, tuntom deliberately avoids trying to infer every property of
Linux packet origin or replace normal Linux routing/firewall tools.

The intended boundary remains:

```text
Linux networking
      |
     TUN
      |
   tuntom
      |
     UDP
```

Transport complexity should stay inside that boundary. Routing and deployment
policy should stay outside it.

### Adaptive event polling

The normal event loop processes one packet from each ready data descriptor per
`poll()` call.  It enters overload batching only after eight consecutive polls
return within 5 microseconds and a zero-timeout readiness check confirms that
data remains queued.  Overload uses round-robin batches of 4, 8, then 16 rounds
as the immediate-poll streak reaches 8, 32, and 128.  A processing slice ends
after 150 microseconds even when its packet budget remains, so control traffic
and timers cannot be held behind a long batch.

A poll that blocks for at least 50 microseconds immediately restores the normal
one-round mode.  Otherwise, overload also expires after 32 milliseconds without
a confirmed backlog.  The stats file exposes:

- `event_poll_overload`: whether overload batching is currently active;
- `event_poll_busy_streak`: consecutive immediate polls;
- `event_poll_batch`: the current number of round-robin rounds;
- `event_poll_overload_entries`: overload transitions since process start;
- `event_poll_backlog_confirmations`: positive post-service readiness checks;
- `event_poll_slice_limit_hits`: processing slices stopped by the 150-us limit.

TUN, UDP, and switch data descriptors are nonblocking.  `EAGAIN` ends processing
for that descriptor in the current slice.

### Processing latency statistics

Tuntom samples the first and then every 1024th TUN packet / UDP datagram using
`std::chrono::steady_clock`, independently of file export. Stats snapshots add
`processing_sample_interval=1024` and these prefixes:

- `tx_processing`: elapsed microseconds from after a successful TUN read through
  the last successful UDP send, including processing, encoding and
  fragmentation. Failed or dropped transmissions do not produce a sample.
- `rx_processing`: elapsed microseconds from after a successful UDP receive
  through DATA handling, including decoding, reassembly work and TUN write if
  completed. This is **per datagram**, not per reconstructed packet. Validated
  DATA that subsequently gets dropped or awaits more fragments is included;
  control traffic and packets rejected before DATA handling are excluded.
  Selection happens before decoding, so these exclusions can reduce the
  effective sample count.
- `reassembly_span`: elapsed microseconds from first fragment entering
  reassembly to final accepted fragment, sampled every 1024th completed
  fragmented packet. Includes time between arrivals and processing; it is not
  pure CPU time or an additional duration to add to RX processing. Unfragmented
  packets are excluded.

Each prefix has `_samples`, `_window_samples`, `_avg_us`, `_max_us`, `_p95_us`
and `_p99_us`. Average and maximum cover all samples since process start;
p95/p99 are nearest-rank percentiles of the most recent 4096 samples. Zero
samples means no measurement yet. Statistics reset on restart. No per-packet log
is added.

These measurements cover application elapsed time and sending syscalls, not
input kernel queues or scheduling before the read. Descheduling inside a
measured region is included. Debug logging and the measurement itself can affect
the results. For unfragmented traffic, TX on A plus RX on B estimates one-way
application processing; sum both directions for RTT processing. For fragmented
traffic RX work is spread over multiple datagrams, so this simple sum no longer
applies. Processing statistics do not add fields to the V5 wire format, and no
stats file is required to collect processing samples or read them over the socket.


### Tunnel throughput statistics

File and control-socket snapshots expose these rates in bits per second:

```text
tun_rx_bps_5s / tun_rx_bps_1m
tun_tx_bps_5s / tun_tx_bps_1m
udp_rx_bps_5s / udp_rx_bps_1m
udp_tx_bps_5s / udp_tx_bps_1m
```

Directions match the existing byte counters: `tun_rx` reads local IP packets for
transmission through the tunnel; `tun_tx` writes received IP packets to TUN.
`udp_tx` sends to the peer and `udp_rx` receives from it. TUN counts include
inner IP headers; UDP counts include tuntom framing and control traffic but
exclude outer UDP/IP and link-layer headers. These are interface byte rates, not
confirmed application goodput; packets subsequently dropped can be included.

Counters are observed each event-loop iteration, using its existing monotonic
clock reading. Deltas are assigned to the observation's fixed five-second
bucket. A delayed observation belongs to its current bucket; historical arrival
times cannot be recovered from counters. `_5s` reports the last completed bucket
(bytes times 8 divided by 5); `_1m` averages the last twelve completed buckets.
The current partial bucket is excluded. Idle buckets count as zero. During
startup, only available completed buckets are averaged, with zero rates before
the first bucket completes. `throughput_bucket_seconds=5` and
`throughput_window_buckets` (0–12) expose the interval and available history.
All history resets on restart.

### Session, suite and rekey statistics

The text stats file includes local session gauges and lifetime event counters.
They reset on process restart. This is an additive change to stats format 1; the
wire protocol is unchanged. Counters live in `SessionProtocol`, so socket send
failures do not change their meaning: they count protocol events and prepared
messages, not successful UDP sends.

| Field | Meaning |
|---|---|
| `suite` | Configured suite: 0 = AMAC plaintext, 1 = Ascon-AEAD128, 2 = X25519 + AKDF + Ascon-AEAD128. |
| `suite_active` | Suite of the current DATA codec, or -1 if absent. On the client this includes a candidate waiting for ACK. |
| `encryption` | Configured `off` or `ascon-aead128`. |
| `pfs` | Configured PFS: 0 or 1. |
| `session_ready` | 1 when local DATA encoding is possible, including client wait-ACK; 0 otherwise (including exhausted TX counter). |
| `session_confirmed` | 1 when the current session has completed the local handshake; this does not prove current peer reachability. |
| `session_age_seconds` | Age of the current candidate/session since key derivation; -1 if absent. |
| `session_tx_counter` | Last ordinary sequence counter allocated for encoding, excluding the session hint; 0 before any DATA/control encoding. Resets on each new session, and does not assert successful sending. |
| `handshake_state` | `idle`, `wait_response` (client), `wait_confirm` (server), or `wait_ack` (client). `idle` can mean either connected or not yet started: inspect session gauges too. |
| `handshake_started` | Client INIT attempts generated, or server pending sessions created after validation. Fresh client INIT retries count as new attempts; INITs ignored by a busy server do not. |
| `handshake_completed` | Local completions: valid CONFIRM activates server, valid ACK completes client. Duplicate confirmations/ACKs do not increment this. |
| `handshake_retries` | Client fresh INIT retries, timer/duplicate-RESPONSE CONFIRM retries, or server ACK replies to duplicate active CONFIRMs. These are generated messages, not successful sends. |
| `handshake_timeouts` | Server pending candidate expiration, or client five-second flight expiration. Each is counted once. Normal one-second fresh INIT retries supersede attempts without counting a five-second timeout. |
| `handshake_suite_mismatch` | Authenticated, structurally valid INIT/RESPONSE packets with a supported suite different from local configuration. Unknown suites and invalid lengths remain protocol drops. |
| `handshake_dh_rejected` | Authenticated suite-2 exchanges that reach DH validation and yield an all-zero X25519 result. |
| `handshake_last_age_seconds` | Seconds since most recent local completion; -1 before the first. |
| `rekey_started` | Subset of handshake attempts started after at least one successful local handshake in this process. Includes retries and idle recovery, not only periodic PFS. |
| `rekey_completed` | Successful local handshakes after the first; includes idle recovery. For uninterrupted PFS traffic this directly counts completed periodic key rotations. |
| `rekey_interval_seconds` | Configured periodic client DH interval: 120 with PFS, 0 otherwise. Server shows the same suite policy but does not initiate rotations. |

Ages use the monotonic clock and are whole seconds. Client/server attempt and
retry counts need not match (loss, validation, pending-slot occupancy and ACK
loss differ at each side). Completion counts can also temporarily differ while
ACK is outstanding. Counters disclose no private keys or DH/KDF secrets.

### Disabling and controlling statistics

`--no-stats` disables only periodic file creation/replacement. Processing-latency
sampling, reassembly-span sampling and throughput bucket updates continue, even
without a stats file. The option preserves
`--stats-file`, regardless of argument order. Basic packet/byte/drop counters,
session/rekey counters and operational RTT/PMTUD control remain active.
`mk_tunnel.sh ... --no-stats` passes the option to both processes, retaining
their normal stats paths for later activation.

`tuntomctl <control-socket> show stats` formats an up-to-date in-memory snapshot
directly into the socket response. It needs no `--stats-file`, never reads or
writes a stats file, and works even if a configured export path is unusable.

The standalone binary installs SIGUSR1 (toggle automatic file export) and SIGUSR2
(one immediate file snapshot, even while disabled) handlers. They only update
`sig_atomic_t` flags; I/O and state updates happen in the normal event loop.
Both signals are blocked briefly when consuming flags to avoid losing requests.
Two delivered USR1 signals cancel each other; multiple USR2 requests before
consumption produce one snapshot. Standard signals may coalesce before delivery,
so do not use rapid repeated signals as a reliable queue. An already running
iteration/write may finish before a disable request takes effect. No output path
means enabling has no output effect. Existing files remain untouched while
disabled unless USR2 requests a snapshot. Snapshotting does not toggle automatic
file export. The snapshot includes `stats_enabled=0/1`, describing periodic file
export, not metric collection. Counters, latency samples and throughput buckets
continue updating while export is disabled. No SIGHUP handler is installed; periodic
rekey remains unchanged.

On re-enable, writing resumes immediately in the loop. Throughput and processing
windows retain their continuously collected history. Counters are never reset by these
signals. Signal handlers are installed only by `main`, so embedding `Tunnel`
does not install process-global handlers automatically.
