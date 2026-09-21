# CONTROL plane security

This document describes the trust model of network `CONTROL` and
`CONTROL_CHALLENGE`. For deployment commands, see
[Control keys quick how-to](CONTROL_KEYS_QUICK_HOWTO.md). Wire layouts and the
exact cryptographic construction are documented in [CONTROL authentication](CONTROL_AUTH.md).

## Roles and trust anchors

```text
local operator              authority daemon             executing node
    | local UNIX socket           |                            |
    +---------------------------->| CONTROL + DH/MAC proof     |
                                  +---- [optional relays] ---->|
                                  |                            |
                            private authority key       pinned public key
                                                        + local grants
```

An authority is a holder of a dedicated X25519 private key. The executing node
pins its public key with `--control-trust-key PATH`. The full 32-byte public key
is the authority ID. There is no certificate hierarchy, TOFU, automatic key
enrollment, or SSH key import. Provisioning the public key and its grants is an
out-of-band administrative operation; their integrity is part of the trust root.

The authority daemon loads its private key with `--control-authority-key PATH`.
The CLI submits commands to that daemon through its local UNIX socket. Access to
that socket therefore delegates the ability to use the daemon's authority;
existing OS socket access controls remain essential.

A daemon may be an authority, an executing node, a relay, or several of these.
Holding an authority private key does not automatically pin it for incoming
commands. A transit relay needs no authority key to forward authenticated frames.

## Fabric and collector: current integration

This section describes the current implementation, not a fixed protocol role.
The division of responsibilities between Fabric, the collector and daemons may
change. Neither Fabric nor a collector is required by the CONTROL wire protocol.

```text
browser -> Fabric HTTP API -> collector -> local daemon UNIX socket
                                             |
                                      authority daemon
                                             |
                                       network CONTROL
                                             |
                                        target node
```

Fabric provides the operator UI and HTTP API. Its collection layer discovers
local processes, reads diagnostics and submits supported operations to their
UNIX control sockets. That layer runs inside the web process by default, or in a
separate collector reached through a local UNIX RPC socket. A separate collector
can have the OS privileges needed to access daemon sockets without running the
HTTP server as root.

Neither the Fabric web application nor the collector currently loads CONTROL
authority keys, computes DH/MAC proofs, or answers `CONTROL_CHALLENGE` itself.
For routed requests, the selected local daemon originates network CONTROL and
uses its configured authority key. The target checks that daemon's authority
against its own pins and grants. The target does not receive an independently
authenticated identity for the browser user or collector.

There are separate authorization boundaries:

| Boundary | Current access control |
| --- | --- |
| Browser to Fabric HTTP API | Fabric bearer token |
| Web to separate collector | UNIX socket permissions and peer UID checks |
| Collector to local daemon | Daemon UNIX socket permissions; local process identity checks |
| Origin daemon to network target | CONTROL authority proof and target's pinned grants |

The Fabric token, collector UID and optional Syspiper API key are not CONTROL
authority credentials. Fabric's `--allow-write` gates its supported rule writes;
it neither enables network CONTROL nor grants protocol capabilities. In separate
collector mode, those writes require `--allow-write` at both web and collector.
Current synchronous rule operations use local daemon sockets; the asynchronous
routed API exposes only reads and discovery, not writes.

Local periodic diagnostics do not need network CONTROL enabled. Routed jobs use
the daemon's routed submission path, including jobs with an empty route, and are
subject to its enablement checks. Remote DISCOVER in trusted mode uses a separate
challenge at every visited node; displaying a topology still does not establish
independent cryptographic target identity.
Collector job IDs and cached results are application bookkeeping, not authority
identities or durable protocol transaction state.

The collector is therefore a privileged local client when its socket access
permits it to invoke an authority daemon. Keeping private keys out of the Python
process does not remove this delegation: compromise of that client can expose
the operations reachable through its daemon access. Current Fabric restrictions
are application policy in addition to, not a replacement for, target grants.

## Explicit enablement

| Configuration | Network behavior |
| --- | --- |
| No `--allow-control*` | Entire network CONTROL stack disabled, including outgoing requests, forwarding, replies and challenges |
| `--allow-control-trusted` | CONTROL enabled; local execution requires a pinned authority and sufficient grants |
| `--allow-control-all` | **DEBUG / AT OWN RISK:** unsigned commands accepted; authority and capability checks bypassed |

The allow flags are mutually exclusive. Loading keys alone does not enable
CONTROL. Every origin, relay and target along a path must enable it. A trusted
receiver with no pins authorizes no incoming operations, but can serve as a relay.

Local UNIX-socket diagnostics and local commands remain available when network
CONTROL is disabled. Network requests submitted through the socket then fail
locally. Configured pins do not make `--allow-control-all` safe: that mode still
bypasses them.

## Challenge and proof

```text
authority                                      executing node
    | --- CONTROL(request ID, command) ------------> |
    | <--- CONTROL_CHALLENGE(request ID,             |
    |        N, authority_id, req_caps, req_level) --- |
    | --- CONTROL(same request ID, command, proof) -> |
    | <--- CONTROL(result, proof) ------------------- |
```

The node controls the challenge. The initial unproven request is not executed or
admitted to the transaction engine in trusted mode.

| Challenge field | Meaning |
| --- | --- |
| `N` | Node's fresh ephemeral X25519 public key, 32 bytes |
| `authority_id` | Specific authority public key, or all zeros to let the authority choose |
| `req_caps` | Required capabilities as a u64 bitfield; no separate mask |
| `req_level` | Minimum u64 level for each required capability |

For a specific authority ID, `req_caps` and `req_level` are zero and ignored for
key selection. The executing node still enforces its local grants and operation
requirements. A requested identity is not a substitute for authorization.

The authority and node derive a shared secret using the authority's static
private key and the node's ephemeral private key respectively. Existing X25519
and AMAC primitives derive a session key and authenticate the command. No new
dependency or vendored crypto is required. This is **DH plus a MAC, not a digital
signature**: the receiving node also knows the MAC key.

The derivation binds the complete challenge, selected authority public key and
request origin. MACs bind the canonical transaction message, proof fields,
sequence number and direction. PUT blocks, STATUS and FINISH are authenticated;
REPLY and CONFIRMED use the opposite direction. A transaction stays bound to the
authority that opened it. An origin configured with an authority key rejects
unsigned results without silently downgrading.

Every emitted challenge uses fresh randomness, including challenges triggered by
retransmissions. Cached challenge bytes are not retransmitted. Non-repetition is
probabilistic, not a persistent global guarantee; active collisions fail closed.
Entropy failure suppresses the challenge. The first accepted proof retires sibling
challenges for the same request.

There are no automatic challenges after handshake or rekey. A new transport
session clears direct-peer authentication state; the next request obtains its own
challenge. Zero request IDs are rejected. Old-session challenges remain rejected
during the DATA rekey grace period.

DISCOVER uses one request ID across the traversal, with a separate fresh challenge
and proof for every visited remote node. Each node checks read access before
returning a MAC-protected FOUND/ALT_PATH or forwarding unsigned DISCOVER to its
neighbors, which repeat that exchange. The origin retains multiple N-keyed contexts
for the same request ID. The initial local root uses local socket authorization.
The node preserves the original ingress and routing context while awaiting proof;
MACs bind discovery kind, origin, request, trace and result body in a separate
`TUNTOM-DISCOVERY-v1` domain. Mutable routing stacks and hop budget remain outside
that MAC. No global proof or transitive grant is forwarded to the next node.

## Authorization: capabilities and levels

Public-key records on the executing node define the grants. Each record assigns
one level to every capability bit it contains. Repeated records for the same key
merge capabilities and take the maximum level for each bit.

| Bit | Numeric value | Capability |
| --- | --- | --- |
| 0 | 1 | Read |
| 1 | 2 | Classifier validation |
| 2 | 4 | Classifier modification |
| 3 | 8 | Rules validation |
| 4 | 16 | Rules modification |
| 5 | 32 | Divert modification |

Bits 6–63 are available for deployment policy. Required bits are the operation's
capability OR `--control-require-caps BITS`. The authority must hold every required
bit at least at `--control-require-level N` (default 0). A grant cannot enable an
operation the target does not implement. Known-transaction STATUS/FINISH retain
the original operation requirement; missing-history STATUS requires read access.

`--control-require-authority PUBLIC_HEX` restricts selection to one pinned key.
Private-key file capabilities and levels only help the origin select a key;
they never grant rights on the target. Different nodes can assign different
rights to the same authority.

## Replay, lifetime and routing boundaries

Each direction has a nonzero u64 sequence counter and a 64-frame replay window.
Invalid MACs do not advance the window. Retransmitted commands keep their request
ID and content but use fresh MAC sequence numbers. Bounded transaction history
deduplicates execution; it does not guarantee durable exactly-once execution
across restarts or history eviction.

Authentication sessions and challenges expire after two minutes. Each receiving
authentication context holds at most 32 live challenges and emits at most one per
20 ms; an authority caches at most 16 outgoing sessions. These bounds limit
resource use but do not prevent denial of service by peers. Final confirmation
retires authentication state. If confirmation is lost, a fresh challenge allows
recovery from retained transaction history without re-executing the command.

Relays carry the proof without needing its key. Mutable routing stacks are outside
the end-to-end MAC; the origin ID and issuing node's challenge bind the proof's
context. Discovery requires authorization at every node that expands a branch;
trust-free relaying of explicit routes does not grant the right to enumerate that
relay. Discovery retains a two-second best-effort collection window. Per router,
128 incoming and 128 outgoing discovery contexts expire after 30 seconds; new
challenges are limited to 32 per one-second window. Loss, authorization failures
and exhausted limits can make a snapshot incomplete.

## Security limits

- The protocol authenticates an authority to a node. Reply MACs authenticate the
  established exchange, **not an independently pinned target identity**. A malicious
  relay can substitute its own challenge/endpoint, fabricate route errors, or
  drop/misroute traffic. Intended-target authentication needs separate node trust.
- CONTROL authentication adds no end-to-end payload encryption. Existing transport
  protection remains relevant, and relay confidentiality is not supplied by a MAC.
- Compromise of an authority private key permits impersonation wherever that key
  is pinned, within each node's grants. This static-authority/ephemeral-node DH
  construction does not provide forward secrecy against later authority-key
  compromise.
- There is no transferable signature or non-repudiation: a receiving node can
  generate MACs for its own exchange.
- `CONTROL-KDF-v1` is a project-specific AMAC-based PRF derivation, not HKDF or a
  standardized Ascon KDF. It assumes AMAC is a suitable PRF and the first 128 DH
  bits have adequate key entropy. Target strength is at most 128 bits; the
  construction and protocol have not received an independent cryptographic audit.
- Keys and grants are loaded at startup. Revocation requires removing the pin
  from affected nodes and restarting them; there is no online revocation service.
