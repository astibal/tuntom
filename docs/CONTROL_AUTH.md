# CONTROL authority authentication

Opt-in end-to-end authentication for CONTROL commands, including commands routed
through switches and relays. It uses only the existing X25519 and AMAC code;
there are no new libraries, downloads or vendored sources. This is a project-specific
DH/MAC protocol, **not a digital signature scheme**.

## Provisioning

Generate a dedicated authority key (files must not already exist):

```sh
tuntomctl auth-keygen authority.key authority.pub 63 7
```

The private file is created with mode 0600. Install only `authority.pub` on nodes.
The daemon that originates remote CONTROL requests holds `authority.key`:

```text
originating daemon: --allow-control-trusted --control-authority-key authority.key
executing node:     --allow-control-trusted --control-trust-key authority.pub --control-require-level 3
transit-only relay: --allow-control-trusted
```

These options work on tunnels, both switches, and exit/divert adapters. They are
repeatable. Local Unix control sockets retain their existing OS access controls.
Key loading occurs at startup; changes require a restart. No TOFU, certificates,
SSH key parsing, environment secrets or network key enrollment are involved.

Public records: `x25519 PUBLIC_HEX CAPS LEVEL`.
Private records: `x25519-secret SECRET_HEX CAPS LEVEL`.
Keys are 32 bytes represented by 64 lowercase hexadecimal digits. CAPS and LEVEL
are unsigned 64-bit decimal or `0x` hexadecimal values. Blank lines and lines
starting with `#` are ignored. A record grants LEVEL to every capability in CAPS.
Repeated records for the same key merge grants, taking the maximum level per cap.
Private-file grants are only selection hints; the receiver's public-file grants
are authoritative. `authority_id` is the full 32-byte public key, not a hash or
short fingerprint. The all-zero ID means the authority may choose its key.

The existing operation permissions occupy bits 0..5:

| Bit | Capability |
| --- | --- |
| 0 | read |
| 1 | classifier validation |
| 2 | classifier modification |
| 3 | rules validation |
| 4 | rules modification |
| 5 | divert modification |

Bits 6..63 are available for deployment policy. `--control-require-caps MASK`
adds required capabilities to the command's own capability. This is a bitfield,
not a value/mask pair. `--control-require-level N` specifies the minimum level
for **each** required capability; default 0. STATUS/FINISH for a known transaction
require its original operation capability and the same authority that opened it.
A missing-history STATUS requires read access. Capabilities cannot authorize an
operation that the target component does not implement.

`--control-require-authority PUBLIC_HEX` selects one pinned authority. In this
case challenge req_caps and req_level are emitted as zero and ignored for key
selection. The node still checks its local grants and operation policy.

Network CONTROL has three explicit modes:

| Configuration | Behavior |
| --- | --- |
| No `--allow-control*` | Entire network CONTROL stack disabled: no reception, execution, transit, outgoing requests, replies, or challenges |
| `--allow-control-trusted` | Enable CONTROL; locally execute only for pinned authorities with sufficient grants |
| `--allow-control-all` | **DEBUG / AT OWN RISK:** accept unsigned commands and bypass authority/capability restrictions |

The two allow flags are mutually exclusive. Merely loading public/private keys
never enables network CONTROL. Local Unix-socket diagnostics and local commands
remain available, subject to the existing OS socket access controls; attempts to
initiate network CONTROL through that socket fail locally when disabled.

Both the origin and every relay/target must explicitly enable network CONTROL.
A trusted-mode origin needs `--control-authority-key` to initiate transactions.
A trusted-mode receiver without pinned keys authorizes no incoming operations;
a trusted-mode relay can forward frames without possessing any authority keys.
Debug mode permits unsigned execution even if trust keys have been configured.
It can also complete a MAC exchange with any authority, without checking grants.

A sender configured with an authority key rejects unsigned results, even before
its first successful handshake; response authentication never silently downgrades.
Legacy remote DISCOVER fanout has no per-target auth handshake and is rejected
in trusted mode. Local discovery and transit require an enabled stack. Authenticated
fanout discovery is not part of this version. Debug mode retains legacy discovery.

## Exchange

```text
authority                              target node
           CONTROL(request ID, command) -->
           <-- CONTROL_CHALLENGE(request ID, N, authority_id, req_caps, req_level)
           CONTROL(same request ID, command, proof) -->
           <-- CONTROL(result, proof)
```

The first unproven command is not executed or admitted to the transaction engine.
All PUT blocks, STATUS and FINISH requests are authenticated. REPLY and CONFIRMED
are MACed in the opposite direction. A transaction remains bound to its verified
authority. Retransmissions retain request ID and content but obtain fresh MAC
sequence numbers. The existing bounded transaction history provides execution
deduplication; this is not durable exactly-once execution across process restarts.

Each emitted challenge generates a new random ephemeral secret using getrandom,
and a new N using X25519, including when responding to a retransmission. It never
retransmits cached challenge bytes. Randomness failure suppresses the challenge and does not authorize the request or terminate the daemon. Uniqueness
is probabilistic (256-bit random generation followed by X25519), not a persistent
absolute guarantee; active collisions fail closed. Several challenges for the
same request may coexist. The first valid proof binds that challenge and retires
its siblings. Stale sibling responses cannot establish a second replay window.

Challenges and auth sessions expire after two minutes. Each receiving auth context
allows at most 32 live challenges and emits at most one per 20 ms. It does not
evict a live challenge to admit another. Auth state and transaction buffers are
bounded independently. The authority caches at most 16 outgoing auth sessions.
These are bounded resources, not a guarantee against denial of service by peers.

After emitting final CONFIRMED, the node retires the request's auth sessions.
If CONFIRMED is lost, the retried FINISH obtains a fresh challenge and recovers
the cached transaction result without re-execution. The authority releases its
auth session after accepting CONFIRMED.

Every direction has a monotonically increasing nonzero u64 counter and a 64-frame
replay window. Invalid MACs do not advance the window. Expiration, restart, and
local transaction-history eviction remove the associated key/counter state.

### Automatic challenge after rekey

Every tunnel with either allow flag enabled clears direct-peer auth state and
sends a fresh challenge after each confirmed initial handshake/rekey. There is
no separate `--control-challenge-after-rekey` option. The offer is a
CONTROL_CHALLENGE with zero request ID. Debug mode sends an unrestricted offer
even without pinned keys. With CONTROL disabled, no offer is generated or
processed. The authority caches the offer and uses
it for its next direct request; the node binds it to that request on the first
valid proof. If the selected key cannot authorize that command, the node issues
a command-specific challenge. In trusted mode, loss is harmless: the normal
request-driven path works without the offer. Offers are not automatically rebroadcast or routed;
routed targets challenge on demand. CONTROL_CHALLENGE from an old transport
session is rejected even during DATA's rekey grace period.

## Wire encoding

All integer fields below use network byte order. N and public keys retain the
32-byte X25519 encoding. V5 packet type 14 is CONTROL; type 15 is CONTROL_CHALLENGE.
Both use the existing 9-byte metadata prefix and transport authentication.

Direct CONTROL retains the 32-byte transaction header. Legacy unproven requests
use version 1. Version 3 supports proof-bearing CONTROL and CONTROL_CHALLENGE.
Header bytes 30..31 contain proof length (0 or 88); the proof follows the header,
before command and body. Challenge kind is 6, state RECEIVING (1), zero offset,
total and command length, no proof, and exactly 80 body bytes:

```text
N[32] | authority_id[32] | req_caps:u64 | req_level:u64
```

Only an unsolicited direct challenge may have zero request ID.

Routed CONTROL retains version 2 and its 52-byte header. Bytes 50..51 now carry
proof length (0 or 88). Proof bytes precede the destination stack, reply stack,
command and body. Routed CONTROL_CHALLENGE uses kind 10, zero state/offset/total,
no command or proof and the same 80-byte body. Existing kinds 1..9 retain their
meaning. Older decoders reject auth extensions; there is no automatic downgrade.
The mutable routing stacks are outside the end-to-end MAC. The original request
origin ID is bound by the KDF, and the challenge secret binds proof acceptance
to the issuing node. Relays need no authority keys.

Proof layout (88 bytes):

```text
N[32] | selected_authority_public_key[32] | sequence:u64 | AMAC[16]
```

The authenticated message is the canonical version-1 direct transaction encoding
with no proof: kind, state, request ID, offset, total, command length, reserved
zero bytes, command and body. Routed endpoints wrap/unwrap this encoding.

## Cryptographic construction and limits

Let D = X25519(authority_secret, N) = X25519(node_ephemeral_secret, authority_public).
All-zero DH outputs are rejected. With AMAC's tunnel/domain ID set to zero:

```text
K = AMAC(key = D[0:16],
         "TUNTOM-CONTROL-KDF-v1\0" || D[0:32] || challenge[80]
         || selected_authority_public_key[32] || origin[16])

MAC = AMAC(key = K,
           "TUNTOM-CONTROL-MAC-v1\0" || direction:u8
           || proof[0:72] || canonical_message)
```

Direct-peer origin is sixteen zero bytes; routed origin is the initiating router's
instance ID. Direction is 0 for authority requests, 1 for target replies.
The challenge includes both N and all requested policy fields. For a concrete
authority, req fields are ignored for selection, but their raw bytes still enter
the derivation. Node ephemeral secrets are wiped after successful derivation;
session keys are wiped on destruction. Long-lived authority secrets remain only
on originating daemons. No secret material is included in diagnostics.

CONTROL-KDF-v1 is a new project-specific PRF derivation, **not HKDF or a standardized
Ascon KDF**. It assumes the existing AMAC is a suitable PRF and the first 128 DH
bits supply adequate key entropy. This construction and the protocol have not
received an independent cryptographic audit. Its target strength is at most
128 bits. Static-authority/ephemeral-node DH does not provide forward secrecy
against later compromise of the authority secret.

Authentication is of the authority to the node. The reverse MAC protects replies
within the established DH exchange; it does not independently certify the node's
identity. A malicious relay can substitute its own challenge/endpoint, drop or
misroute traffic, or fabricate route errors. Authenticating the intended target
to the authority needs separately provisioned node identity/trust. Transport
protection and endpoint routing remain relevant. No public transferable signature
or proof that a command came from the authority is produced: a receiving node
also knows its own MAC key.
