# tuntom

`tuntom` is a tiny self-contained TUN-over-UDP tunnel for Linux.

It was written by **Ales Stibal and ChatGPT** :)

The main goal is simplicity:

- one C++17 source file
- one bootstrap shell script
- no external runtime dependencies
- source is copied to the remote host through SSH, compiled there, and started
- NAT-friendly client/server model
- Linux TUN interfaces on both ends
- authenticated protocol v3 with replay protection
- internal balanced fragmentation and reassembly
- configurable inner/TUN MTU and transport MTU
- automatic UDP path-MTU discovery (PMTUD)
- optional compatibility with protocol v2 and v1
- optional TTL / IPv6 Hop-Limit compensation
- Wireshark dissector with v3 fragment reassembly

The tunnel is intentionally small and dumb.

Routing, policy routing, firewalling, transparent proxying, integration, and similar logic are left to normal Linux networking.

## Requirements

Both hosts need:

- Linux
- `g++` with C++17 support
- `iproute2`
- `/dev/net/tun`

The local machine also needs:

- `ssh`
- working SSH key authentication to the remote host

The default remote SSH user is `root`.

## Usage

Set a shared 128-bit secret:

```bash
export TUNTOM_SECRET=0123456789abcdef0123456789abcdef
```

Create tunnel 42 to host `sx2`:

```bash
sudo -E ./mk_tunnel.sh 42 sx2
```

This is equivalent to connecting to:

```text
root@sx2
```

You can also specify a user explicitly:

```bash
sudo -E ./mk_tunnel.sh 42 user@sx2
```

Tunnel ID determines the interface names, addresses, and UDP port.

The `/16` prefix used for tunnel addresses is configurable. If the environment
variable is unset, `10.254` remains the default:

```bash
export TUNTOM_PREFIX16=10.10
sudo -E ./mk_tunnel.sh 42 sx2
```

The resulting client and server addresses are `10.10.42.1` and `10.10.42.2`.
The tunnel ID therefore remains part of every address.

The same value also determines the tunnel's IPv6 addresses. Dots in the IPv4
prefix are replaced with colons and the result is placed below `fd42::`:

```text
TUNTOM_PREFIX16=10.10, tunnel 42
client IPv6: fd42::10:10:42:1
server IPv6: fd42::10:10:42:2
```

For tunnel `42`:

```text
local/client interface:   ut42c
remote/server interface:  ut42s

local/client address:     10.254.42.1
remote/server address:    10.254.42.2

UDP port:                 40042
TUN MTU:                  1500
transport MTU:            1400
```

After setup, this should work:

```bash
ping -I ut42c 10.254.42.2
```

Loopback testing is supported:

```bash
sudo -E ./mk_tunnel.sh 42 localhost
```

## MTU and fragmentation

`tuntom` separates the MTU visible on the TUN interface from the MTU used by the underlying UDP transport.

Defaults:

```text
TUNTOM_MTU=1500
TUNTOM_TRANSPORT_MTU=1400
```

The TUN interface therefore behaves like a normal 1500-byte L3 interface even when the path carrying tuntom UDP datagrams requires smaller packets.

Automatic PMTUD is enabled by default. Each endpoint starts with a conservative
500-byte outer transport MTU, sends authenticated `MTU_PROBE` messages, and uses
matching `MTU_REPLY` messages to find the largest working datagram size. The
configured transport MTU is the first probe target; discovery can continue up to
at least 1500 bytes (or higher when a larger transport MTU is configured).

A missing reply is treated as a failed probe after two seconds and the remaining
range is searched. Discovery restarts when the UDP peer changes or a data send
fails. Ordinary traffic continues with the last known-good MTU during discovery.

To use `TUNTOM_TRANSPORT_MTU` as a fixed value, run the C++ binary with:

```text
--no-pmtud
```

If an inner packet does not fit into one tuntom UDP datagram, protocol v3 fragments it internally and reassembles it on the receiving side.

Fragmentation is balanced. Instead of sending one maximum-sized fragment followed by a small tail, tuntom divides the packet into approximately equal-sized parts.

Example:

```text
1500 bytes -> 750 + 750
1401 bytes -> 701 + 700
```

This avoids exposing a smaller TUN MTU to systems behind the tunnel and also avoids a characteristic large-fragment/small-tail pattern.

The values can be overridden when using the bootstrap script:

```bash
TUNTOM_MTU=9000 \
TUNTOM_TRANSPORT_MTU=1500 \
sudo -E ./mk_tunnel.sh 42 sx2
```

In this example, the tunnel presents MTU 9000 while transparently fragmenting the traffic over a 1500-byte transport path.

## TTL / Hop-Limit compensation

By default, tuntom compensates one routing hop when writing a received packet into the TUN interface.

For IPv4 it increments TTL by one and recomputes the IPv4 header checksum.

For IPv6 it increments Hop Limit by one.

This is useful when tuntom connects two routing points and should behave like one logical routed hop rather than exposing both tuntom endpoints as separate hops.

The compensation can be disabled in the C++ binary using:

```text
--no-ttl-compensate
```

### Known limitation

TTL / Hop-Limit compensation assumes that traffic entering tuntom was forwarded by the sending host before it reached the TUN interface.

Locally generated traffic is different: it has not yet consumed a forwarding hop.

`tuntom` deliberately does not try to detect this case because doing so would require additional kernel metadata, packet marking, address classification, or protocol flags.

As a result, locally generated traffic such as a ping originating directly on a tuntom endpoint may appear with TTL / Hop Limit one higher than expected on the remote side.

This is a known limitation and an intentional KISS tradeoff. The administrator of the tuntom endpoint already knows that the tunnel exists.

## Protocol compatibility

Protocol v3 is the default transmit protocol.

Legacy receive compatibility can be enabled explicitly:

```text
--allow-v2
--allow-v1
```

There is no automatic downgrade.

Protocol v1 is unauthenticated and should only be enabled when compatibility is explicitly required.

## Wireshark

The project includes a Wireshark Lua dissector:

```text
tuntom.lua
```

It understands protocol versions v1, v2, and v3.

For v3 it displays:

- sequence number
- message ID
- fragment offset
- original packet length
- fragment length
- authentication tag
- reassembly information

The dissector also reassembles fragmented v3 DATA packets and passes the reconstructed packet to Wireshark's normal IPv4 or IPv6 dissector.

It also recognizes `MTU_PROBE` and `MTU_REPLY` packets and displays their probe
ID, outer MTU, and probe padding.

## PMTUD testing

`examples/pmtud-iptables-test.sh` can simulate a silent IPv4 UDP MTU black hole
on a selected interface:

```bash
sudo ./examples/pmtud-iptables-test.sh --size 1200 --interface eth0
# start or restart the tunnel and inspect its log/statistics
sudo ./examples/pmtud-iptables-test.sh --size 1200 --interface eth0 --remove
```

The rules affect all IPv4 UDP traffic larger than the selected size on that
interface, so remove them after testing. The script is idempotent and requires
`iptables`.

## Hooks

`mk_tunnel.sh` supports local pre/post lifecycle hooks.

By default:

```text
/etc/tuntom/tuntom-pre.sh
/etc/tuntom/tuntom-post.sh
```

The hook files only need to exist on the local/caller host.

The same hook content is executed locally and streamed over SSH to the remote host.

Hook context uses:

```text
TUNTOM_SIDE=local
TUNTOM_SIDE=remote
```

Typical hook phases are:

```text
pre/down
post/down
pre/up
post/up
```

`post/up` is especially useful for adding routes, DNAT rules, or other per-tunnel policy after tuntom networking has been created.

## Logs

The bootstrap script writes logs to:

```text
/tmp/tuntom_42c.log
/tmp/tuntom_42s.log
```

Use `--debug` on the C++ binary for packet and protocol details.

## Files

```text
udp_tun.cpp     self-contained C++17 implementation
mk_tunnel.sh    build/deploy/start helper
tuntom-net.sh   optional per-tunnel Linux routing/NAT helper
tuntom.lua      Wireshark dissector
README.md       quick usage
DETAILS.md      implementation notes
LICENSE.md      BSD 3-Clause license
```
