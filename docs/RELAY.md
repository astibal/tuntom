# Remote VIA adapters over tuntom

A relay tunnel carries switch IPC frames directly. The remote host runs tuntom
and the existing divert adapter; it does not run a switch. Both ST and MP switches
support the same configuration. Existing local VIA services and tunnel CLI
options retain their meanings.

```text
Hub host                                  Service host

switch <-> tuntom --relay-connect   <UDP>   tuntom --relay-listen
  |         port: proxy-link                         |
  |                                       one socket per adapter side
  |                                                  |
  |                                          divert adapter
  |                                            di0     do0
  |                                             \       /
  |                                             smithproxy
  |
exit adapter -> real server
```

There is one physical IPC connection between the hub switch and its relay tuntom.
Remote channels share that connection. IPC framing, opcode, payload and all eight
available labels are preserved. Multiplexing does not consume a label. The relay
adds no inner IP/UDP encapsulation and does not inspect or rewrite inner IP packets.

## Configuration and commands

Use [the complete example](../examples/relay/switch.rules). The added service field
is `relay <physical-port-ID-or-prefix*>`; omission selects existing local attachments.
Patterns apply to the remote attachment names, e.g. `proxy-in0` / `proxy-out0`.
`hash` (rendezvous flow hashing), ordered `failover`, and `unavailable drop|pass`
work as for local services. A chain can mix local services and remote services.
Ad-hoc format-3 `match ... via [...]` uses these same service definitions.

On the hub, with existing `edge` and `internet` adapters:

```sh
tuntom-switch --socket /run/tuntom/core.sock \
  --rules-file examples/relay/switch.rules

# Set TUNTOM_SECRET to the same private 32-hex-character value on both hosts.
tuntom server 231 - \
  --relay-connect /run/tuntom/core.sock \
  --relay-port-id proxy-link
```

`tomtom-switch-mp --workers 2` can replace `tuntom-switch` with the same rules.
The hub listens on UDP port 40231. The service host connects to its address:

```sh
tuntom client 231 - 192.0.2.10 \
  --relay-listen /run/tuntom/proxy-relay.sock

tuntom-divert-adapter di0 do0 \
  --switch-socket /run/tuntom/proxy-relay.sock \
  --via-instance 'smithproxy#0' \
  --divert-in-port proxy-in0 \
  --divert-out-port proxy-out0 \
  --admission immediate
```

Provision `/run/tuntom` and its permissions yourself. The listener refuses to
replace an existing socket path; remove a stale path after an unclean shutdown.
Relay mode creates no TUN. Run it as an unprivileged account with access to these
sockets. As with existing tunnel mode, invoking it as root drops to the configured
runtime account; provision a directory writable by that account. Only the divert
adapter needs permission to create its TUNs.

You can run the remote processes with `ip netns exec NAME ...`, or put `di0` and
`do0` into your own VRF. Namespace creation, addresses, forwarding, policy routing,
and smithproxy setup remain administrator responsibilities. The example assumes
smithproxy preserves the IP/port identities expected by the adapter.

A relay tunnel must have one `--relay-connect` endpoint and one `--relay-listen`
endpoint. These options are exclusive of normal switch/classifier/TUN mode.
Both peers need this implementation; older peers cannot interpret the IPC type.
Normal DATA wire encoding is unchanged. Default PFS and encryption still apply.

## Multipath: one adapter, several independent tunnels

Use [the multipath rules](../examples/relay/multipath.rules). Each path is an
ordinary V5 relay tunnel with its own process, session, epoch and hub IPC port.
The switch selects a complete client/server pair using its existing symmetric
flow hash and rendezvous selection. There is no new tunnel wire format and no
switch flow cache.

```text
Core switch                      Service host
  proxy-link0 <== V5 tunnel 232 ==> relay0.sock --+
  proxy-link1 <== V5 tunnel 233 ==> relay1.sock --+-- divert adapter
  proxy-link2 <== V5 tunnel 234 ==> relay2.sock --+     di0    do0
  proxy-link3 <== V5 tunnel 235 ==> relay3.sock --+      \      /
                                                       proxy/router
```

The matching service definition is:

```text
service smithproxy {
    client-side proxy-in*
    server-side proxy-out*
    relay proxy-link*
    stickiness hash
    unavailable drop
}
```

Start the hub switch using `examples/relay/multipath.rules`, then run four relay
processes on each host. Set the shared `TUNTOM_SECRET` as in the single-path example.
The example hub address is `192.0.2.10`; replace it with your actual core address.

```sh
# Hub: UDP ports 40232 through 40235.
for path in 0 1 2 3; do
    tuntom server "$((232 + path))" - \
        --relay-connect /run/tuntom/core.sock \
        --relay-port-id "proxy-link$path" &
done
```

```sh
# Service host: independent tunnel processes, no switch.
for path in 0 1 2 3; do
    tuntom client "$((232 + path))" - 192.0.2.10 \
        --relay-listen "/run/tuntom/relay$path.sock" &
done

tuntom-divert-adapter di0 do0 \
    --via-instance 'smithproxy#0' \
    --divert-in-port proxy-in --divert-out-port proxy-out \
    --relay-path 0=/run/tuntom/relay0.sock \
    --relay-path 1=/run/tuntom/relay1.sock \
    --relay-path 2=/run/tuntom/relay2.sock \
    --relay-path 3=/run/tuntom/relay3.sock \
    --admission immediate \
    --control-socket /run/tuntom/divert.control
```

`--relay-path ID=SOCKET` is repeatable (1–16 paths), requires `--via-instance`,
and cannot be combined with `--switch-socket`. IDs use letters, digits, `_` and
`-`; IDs and socket paths must be unique. The original single-socket command
keeps its original registration names and behavior.

For path `0`, the adapter registers:

```text
proxy-in.0~via:c:smithproxy#0#path-0
proxy-out.0~via:s:smithproxy#0#path-0
```

Names derive from the configured ID, independent of argument or registration
order. The existing 63-byte full registration-name limit still applies. Both
sides of a pair must belong to the same relay and process owner. In the switch,
each path-qualified identity is a selectable service instance. An explicit
`instances` allowlist or ordered `stickiness failover` list must therefore use
these full identities, e.g. `["smithproxy#0#path-0", "smithproxy#0#path-1"]`.

All these paths feed the **same adapter process and the same two TUNs**. Admission
and flow contexts are shared. One transport-path slot is added to each existing
flow-cache entry; no second cache lookup is needed. The slot is fixed for the
adapter process lifetime and never appears on the wire. Each accepted ingress
packet updates the return path for both directions, while their saved VIA
envelopes remain separate. A proxy-generated SYN+ACK can therefore return along
the initial SYN's path before an upstream reply exists.

On tunnel loss, the switch removes the unavailable pair after the relay lease
expires and rendezvous selection redistributes affected flows. The next packet
delivered on another path teaches the adapter that path, retaining its flow
context. Until then, TUN output still uses the cached path and may be dropped.
The adapter does not infer remote tunnel health from a still-connected local
socket. Recovery can take the normal session timeout described below; TCP can
retransmit. This is not lossless failover. Moving between paths of one adapter
does not transfer smithproxy state between independent smithproxy processes.

Tunnel encryption and UDP handling can now run on different CPUs. A single flow
still uses one selected path, and the shared adapter remains one event loop.
This change does not alter MTU, UDP socket buffers or congestion handling.

`tuntomctl /run/tuntom/divert.control show stats` exposes `relay_paths`,
`connected_pairs` (local IPC readiness), and `relay_path_<ID>_in_*` /
`relay_path_<ID>_out_*` IPC counters. The old `divert_in_connected` /
`divert_out_connected` fields describe the first configured pair in this mode.
Use each relay's own `relay_acknowledged` statistic for tunnel registration health.

## Registration, loss and resource bounds

- Each adapter owns two local IPC connections per path. The remote listener supports
  legacy TTP and V2 inline IPC; it negotiates inline transport instead of mmap.
  The hub multiplex connection uses legacy SEQPACKET records with relay framing.
- A snapshot carries channel IDs, full VIA port names and process-owner tokens.
  Owner tokens come from local `SO_PEERCRED` comparisons. The switch admits only
  names belonging to services bound to that relay. A complete side pair must have
  one owner. Duplicate live names and duplicate instance sides are rejected.
- Up to 128 active/pending remote sockets are allowed per relay endpoint. Channel
  IDs are monotonic within the process. A fresh tunnel session gets a random
  64-bit epoch and reconnects the hub's physical switch port.
- Complete channel snapshots are acknowledged and refreshed every second. Each
  endpoint stops forwarding through registrations without a valid acknowledgement
  or lease after three seconds. Missing/invalid channels drop. Ordinary service
  unavailability follows the configured `drop|pass` policy.
- Snapshot revision ordering prevents a delayed snapshot from resurrecting a
  removed channel. Old epochs cannot replace an epoch on a live hub connection.
  Previous cryptographic sessions cannot submit relay data after rekey.
- Recovery uses existing tunnel timers: after a peer restart, a client may wait
  for the 20-second idle timeout before establishing a fresh session. There is no
  reliable data retransmission or unbounded queue. Backpressure drops packets;
  TCP endpoints can retransmit them.
- The switch stores channel registrations, not per-flow state. Adapter flow
  learning, warmup and crude stop behavior remain in the adapter.
- Maximum logical relay record: 65,639 bytes, including a full switch frame with
  eight labels and a 65,535-byte payload. IPC fragments use 32-bit offset/length
  fields and at most 256 fragments, allowing the initial 500-byte PMTUD transport
  MTU. Normal IP DATA retains its 16-bit fields and 64-fragment limit.

## Wire layout

V5 packet type **12 = IPC** uses the normal session authentication/encryption.
Whole IPC has the same 9-byte metadata plus 16-byte tag as whole DATA. Fragmented
IPC has 25-byte metadata plus tag:

```text
type/flags:8 | sequence:64 | message-ID:64 | offset:32 | total:32 | tag:128 | fragment
```

The decrypted/reassembled payload is a relay record (all integers big-endian):

```text
 0  "TTR", version=1        4 bytes
 4  type                    1 byte: RESET=1, SNAPSHOT=2, ACK=3, DATA=4
 5  reserved                3 bytes, zero
 8  channel/revision        4 bytes (channel for DATA, revision for SNAPSHOT/ACK)
12  total record length     4 bytes, including the 32-byte header
16  epoch                   8 bytes
24  reserved owner          8 bytes, zero
32  payload
```

DATA contains one complete canonical switch IPC frame. ACK has no payload.
SNAPSHOT contains `count:u16` followed by
`channel:u32, owner:u64, name-length:u8, name:bytes` entries. Names are 1–63 bytes.
RESET is reserved for diagnostics; endpoints currently reset by reconnecting and
renewing the session, rather than acting on RESET records.

## Wireshark

Load the repository's `tuntom.lua` using your Wireshark Lua plugin directory, or:

```sh
tshark -X lua_script:./tuntom.lua -r capture.pcap
```

The dissector decodes IPC relay headers, snapshot registrations, switch opcodes,
all 64-bit labels, VIA cookie, chain, step, saved count, action, reverse flag,
origin ID, and the encapsulated IPv4/IPv6 packet. It reassembles fragmented IPC.
Useful display filters:

```text
tuntom.type == 12
tuntom.ipc.channel == 9
tuntom.ipc.opcode == 2
tuntom.ipc.via.chain == 7
tuntom.ipc.via.action == 1
tuntom.ipc.malformed
```

Encrypted PFS payloads remain ciphertext: this dissector does not export or derive
session keys. Plaintext, authenticated captures (`--crypto-auth-only` in a controlled
lab) expose IPC directly. Decoded relay records or ordinary switch frames can
also be supplied in a PCAP with link type USER0 (147), one record per packet.
Neither capture mode verifies the authentication tag in Wireshark.

## Validation

`relay_test` covers snapshot ownership, duplicates, revisions, epochs, and large
fragment reassembly with authentication-only and encrypted codecs.
`relay_integration_test` runs real ST/MP switches and encrypted tuntom processes:
forward/reverse TCP-shaped traffic, full-size payloads, rejection of local service
impersonation, lease expiry, and peer restart/re-registration through one hub IPC
connection. `tests/dissector_test.py` checks the Lua output offline with tshark,
including USER0 captures. Tunnel control statistics expose `relay_mode`,
`relay_local_connected`, `relay_channels` and, on the listener, the current epoch,
revision and `relay_acknowledged` state.

`relay_multipath_test` runs both switch implementations, two encrypted V5 relay
pairs and the real divert event loop with socket-backed test TUNs. It checks
flow distribution, symmetric selection, early proxy SYN+ACK, cached return paths,
forward-first and reverse-first migration after a tunnel loss, stable mapping
after hub and remote listener restarts, adapter socket-pair reconnects, and
all-paths-down drops.

For actual TCP routing through an isolated Linux router, including adapter warmup:

```sh
python3 experiments/divert_lab/run_integrated.py \
  --build /tmp/tuntom-via-build --relay --switch mp --vrf --mtu 9000 \
  --output /tmp/my-relay-lab
```

The opt-in lab manages its own disposable user/network namespaces. It does not
configure the host network. This validates routing, not a real smithproxy setup.
Add `--relay-paths 4` to exercise four independent tunnels with one shared TUN
pair and verify traffic on every path. The lab also checks warmup bypass, TCP
payload integrity, preserved source addresses/ports and router TTL changes.
