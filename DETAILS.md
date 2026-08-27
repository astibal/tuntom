# tuntom implementation details

## Motivation

`tuntom` started as a very small requirement: create a lightweight L3 tunnel over UDP without bringing in WireGuard, GRE-over-UDP plumbing, OpenSSL, or a larger VPN stack.

The intended model is:

```text
TUN -> UDP -> network/NAT -> UDP -> TUN
```

The implementation deliberately keeps transport and Linux networking separate.

`tuntom` only moves IP packets between a Linux TUN interface and UDP. What happens to packets after they appear on the remote TUN interface is up to the host:

- normal routing
- policy routing
- nftables
- transparent proxying
- smithproxy
- honeynet routing
- IDS/IPS
- packet capture
- custom applications

This makes the tunnel useful as a small remote L3 ingress primitive rather than a complete VPN product.

## Deployment model

The project intentionally uses a single C++ source file.

This is not especially beautiful as source-tree architecture, but it makes deployment trivial:

```text
udp_tun.cpp
    |
    | SSH stdin
    v
remote g++
    |
    v
/tmp/udp_tun-ID-server
```

`mk_tunnel.sh`:

1. compiles the client locally
2. sends `udp_tun.cpp` over SSH
3. compiles it remotely
4. stops the previous instance
5. starts the server
6. creates/configures the server TUN interface
7. starts the client
8. creates/configures the client TUN interface
9. tests the tunnel with ping

For normal remote use, the remote login defaults to `root`.

The current bootstrap mechanism is mainly a convenient deployment/testing tool. The C++ program itself does not depend on SSH.

## Addressing

Tunnel ID is intentionally the only numeric configuration parameter.

For ID `X`:

```text
client interface: utXc
server interface: utXs

client IP: 10.254.X.1
server IP: 10.254.X.2

UDP port: 40000 + X
```

The current script accepts IDs from 1 to 255.

## Protocol

Protocol v2 uses a fixed 32-byte header:

```text
offset  size  field
0       4     magic
4       2     tunnel_id
6       1     version
7       1     packet_type
8       8     sequence
16      16    auth_tag
32      ...   payload
```

Packet types are:

```text
1  HELLO
2  KEEPALIVE
3  DATA
```

The payload of a DATA packet is the IP packet read from the TUN interface.

The authenticated data consists of the header fields excluding `auth_tag`, followed by the entire payload.

## Authentication

The implementation has no OpenSSL dependency.

It contains a compact keyed construction based on the Ascon permutation directly inside `udp_tun.cpp`.

The master secret is supplied through:

```text
TUNTOM_SECRET
```

It must currently be exactly 128 bits represented by 32 hexadecimal characters.

A node-specific key is derived using the tunnel ID. This allows the same master secret to be shared across a deployment while producing a different effective authentication domain for each tunnel ID.

The authentication tag is 128 bits.

The current construction is intentionally small and self-contained. It uses the Ascon permutation, but it should not be described as a drop-in implementation of a specific standardized NIST Ascon MAC profile.

## Sequence numbers and replay protection

The client is intentionally almost stateless across restarts.

Sequence numbers are based on current system time at nanosecond resolution. During one process lifetime the generator also guarantees monotonicity:

```text
seq = max(current_time_ns, previous_seq + 1)
```

This avoids a persistent sequence database or session ID.

The receiver keeps a small runtime replay window:

- highest accepted sequence number
- 64-bit bitmap representing the previous 64 sequence values

This permits normal UDP packet reordering while rejecting duplicates and sufficiently old packets.

Replay state is not persisted across server restarts.

## NAT behavior

The client initiates UDP traffic.

The server learns the current client UDP source address and port from valid authenticated traffic. This naturally supports a client behind NAT.

Importantly, the server updates its remembered peer only after:

- protocol validation
- authentication validation
- replay validation

A random unauthenticated UDP packet therefore cannot simply redirect the server's return path.

## Protocol versions

Protocol v2 is the default.

Older protocol versions are rejected unless explicitly allowed at startup.

There is no automatic downgrade behavior.

This is intentional: compatibility with an older unauthenticated protocol must be an explicit decision.

## Main classes

The implementation is intentionally KISS.

The important classes are roughly:

```text
Protocol
ProtocolV1
ProtocolV2
TunDevice
UdpEndpoint
Tunnel
```

`Tunnel` contains the main packet loop.

It also provides a virtual processing hook so the tunnel logic can later be integrated into another C++ application without rewriting the transport path.

Conceptually:

```cpp
virtual bool process(Packet& packet, Direction direction);
```

The default implementation allows the packet to continue.

A derived class can inspect, modify, consume, or reject traffic in either direction.

This is particularly useful for integrating the tunnel directly into software such as smithproxy.

## Using the code in another program

The easiest integration path is to copy the relevant classes from `udp_tun.cpp` and derive from `Tunnel`.

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

Another program may eventually want to replace the TUN endpoint entirely and inject packets directly into its own packet-processing engine.

The current source does not introduce an abstraction for that yet. This is deliberate. The location is obvious, and the abstraction should be added only when a real integration needs it.

## Using the binary without the bootstrap script

The C++ binary supports client/server roles independently of `mk_tunnel.sh`.

The bootstrap script is responsible for:

- compilation
- SSH deployment
- TUN address assignment
- interface MTU
- process startup
- initial ping test

This separation is intentional.

A different orchestration layer can compile/install the binary once and manage it using systemd, containers, another deployment tool, or an application-specific controller.

## Routing and firewalling

`tuntom` does not install:

- nftables rules
- conntrack marks
- policy-routing rules
- NAT rules
- forwarding rules

That is a host responsibility.

Once traffic appears on `utXs`, Linux can treat it like traffic from any other L3 interface.

For more complex deployments with multiple probes, return-path selection may need conntrack marks and policy routing. That logic belongs on the core router rather than on the probe or inside the tunnel protocol itself.

## Debugging

The current build intentionally logs packet activity, including dumps such as:

```text
TUN read ...
ENCODE payload=... output=...
UDP recv ...
TUN write ...
```

For an IPv4 ICMP packet, valid payload data will usually begin with:

```text
45 ...
```

For IPv6 it will begin with a byte whose upper nibble is `6`, commonly:

```text
60 ...
```

The debug output is useful during development and can be removed or compiled conditionally later.

## Design philosophy

The main rule is KISS.

When a future abstraction looks useful, the current preference is to leave a comment at the natural extension point rather than introduce a framework before there is a concrete use for it.

The intended boundary remains simple:

```text
Linux networking
      |
     TUN
      |
   tuntom
      |
     UDP
```
