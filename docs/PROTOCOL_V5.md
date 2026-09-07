# tuntom V5: compact forward-secret Ascon-AEAD128 sessions

V5 is the only supported wire protocol. Upgrade both endpoints together; V1–V4
packets are rejected and there is no automatic downgrade. The Wireshark
dissector can still inspect historical captures.

## Common header

All integers are unsigned, big-endian. No magic or tunnel ID is transmitted. The
UDP socket selects the configured tunnel; its ID remains an input to all
existing key derivations. Version 5 is transmitted only in INIT/RESPONSE.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 1 | Type in bits 0–3; bits 4–5 reserved (zero), bit 6 = fragmented DATA, bit 7 = AEAD |
| 1 | 8 | SEQ: upper 16 bits session hint, lower 48 bits counter |
| 9 | variable | Type-specific extension below |
| M | 16 | AMAC or Ascon-AEAD128 tag; M = 9 + extension size |
| M + 16 | variable | Payload |


| Type | Extension at offset 9 | Total header |
|---|---|---:|
| DATA (3), no fragment flag | none | 25 B |
| HELLO (1), KEEPALIVE (2) | none | 25 B |
| DATA (3), fragment flag set | message_id[8], offset[2], original_length[2] | 37 B |
| PING (4), PONG (5) | probe_id[8] | 33 B |
| MTU_PROBE (6), MTU_REPLY (7) | probe_id[8], outer_mtu[2] | 35 B |
| INIT (8), RESPONSE (9) | version[1] = 5, exchange_id[8] | 34 B |
| CONFIRM (10), CONFIRM_ACK (11) | exchange_id[8] | 33 B |


The MAC construction is specified in [AMAC v1](AMAC_V1.md). Its input is
`wire[0:M] || payload`; the tag is excluded. AEAD authenticates the same
metadata as associated data. All flags and extensions are authenticated.
Reserved bits, unknown types, wrong handshake versions and fragment flags on
control packets are rejected.

Unfragmented DATA derives its length from the UDP payload; it carries no message
ID or fragmentation fields. Internally its sequence supplies the nonzero ID for
the complete-packet reassembly fast path. Fragment offset and original length
must fit 16 bits (maximum inner packet 65535 bytes); redundant complete-packet
fragment extensions are rejected. Fragment IDs remain 64 bits. HELLO, KEEPALIVE,
PING, PONG, MTU_REPLY and confirmations have no payload. MTU_PROBE carries
padding.

## Handshake messages

| Type | Direction | SEQ | Payload | Total UDP payload |
|---|---|---|---|---:|
| INIT (8) | C → S | 0 | nonce_C[32], timestamp[8], suite[2], dh_length[2], DH_C[dh_length] | 78 B (110 B in suite 2) |
| RESPONSE (9) | S → C | 0 | init_hash[32], nonce_S[32], suite[2], dh_length[2], DH_S[dh_length] | 102 B (134 B in suite 2) |
| CONFIRM (10) | C → S | hint << 48 | empty | 33 B |
| CONFIRM_ACK (11) | S → C | hint << 48 | empty | 33 B |


All four messages share the client's nonzero random 64-bit exchange ID in
`message_id`; fragment offset and original length are absent. Nonces are 32
fresh random bytes from Linux `getrandom`, with errors failing closed.
Retransmissions preserve the exact original bytes, nonces and exchange ID.

Suite **2** is the default. It uses Ascon-AEAD128 with ephemeral X25519 and
[AKDF v1](AKDF_V1.md); `dh_length` must be 32, giving exact payload sizes of
76/100 bytes for INIT/RESPONSE. `--crypto-auth-only` selects suite **0**, which
authenticates plaintext using AMAC, has no forward secrecy, and requires
`dh_length = 0`. Suite **1** (Ascon-AEAD128 without PFS) remains recognized for
wire compatibility and tests but has no command-line selector. The former
`--pfs` and `--encrypt-ascon` options are rejected. All suites require exact
lengths and matching local configuration. Unknown suites, wrong DH lengths and
zero DH shared results are rejected. There is no negotiation or timeout fallback.

INIT/RESPONSE remain plaintext AMAC packets with bit 7 clear. Their
authenticated suite fields are also bound into the session-key transcript. All
subsequent suite-1/2 packets, including CONFIRM/ACK and PMTUD, set bit 7 and use
AEAD:

- Key: the existing fresh 128-bit directional session key derived below.
- Nonce: eight zero bytes followed by the exact eight wire SEQ bytes.
- Associated data: exact header bytes before the tag, `wire[0:M]` (including
  both flags).
- Ciphertext: payload at offset `M + 16`; full 128-bit tag at offset `M`.
- The standard's internal lanes are little-endian; wire integers remain
  big-endian.

Directional keys prevent cross-direction nonce reuse. Each fragment/control
packet consumes a fresh counter. Counter zero is used only by the fixed
CONFIRM/ACK in its respective direction; retries reproduce the identical packet.
Suites 1/2 stop sending after counter `2^32 - 1` and establishes a fresh
session, well before 48-bit wrap. With UDP datagrams below 64 KiB this also
bounds traffic to less than `2^48` bytes per directional key. Mode-bit changes
fail authentication; wrong modes and legacy receive flags are rejected. No
plaintext is delivered before verification; failed candidate plaintext is wiped.

Reference: [NIST SP 800-232](https://doi.org/10.6028/NIST.SP.800-232).

## Key derivation and binding

All derivation labels (including historical `V4` names) and configured tunnel-ID
inputs remain unchanged. The authenticated V5 handshake bytes, including
version, are bound into the transcript; new wire framing therefore produces
fresh session keys.

Below, `AMAC(K, id, bytes)` is the existing 16-byte function `ascon::mac`. `||`
is byte concatenation. The master secret is 16 bytes.

Long-term handshake keys (ASCII labels, **without** trailing NUL):

```text
H_c2s = AMAC(master, id, "TUNTOM-V4-CLIENT-TO-SERVER")
H_s2c = AMAC(master, id, "TUNTOM-V4-SERVER-TO-CLIENT")
```

INIT is tagged with H_c2s; RESPONSE is tagged with H_s2c. Session DATA cannot be
authenticated with these keys, even if the direction matches.

For suites 0/1, define the domain-separated expansion below. Here, labels
**include one trailing NUL byte** before the transcript:

```text
expand(label, bytes) = AMAC(master, id, ASCII(label) || 0x00 || bytes)

I = complete encoded INIT, including its AMAC
init_hash = expand("V4-INIT-BIND-0", I) || expand("V4-INIT-BIND-1", I)

R = complete encoded RESPONSE, including its AMAC
T = I || R
S_c2s = expand("V4-SESSION-C2S", T)
S_s2c = expand("V4-SESSION-S2C", T)
hint  = first two bytes of expand("V4-SESSION-HINT", T), big-endian
```

`init_hash` retains the design's field name but is a **keyed 32-byte AMAC
commitment**, not SHA-256 or an unkeyed hash. Two independently labeled AMAC
outputs do not imply a claim of 256-bit security: the master key is 128 bits.
The fixed handshake message lengths make transcript concatenation unambiguous.

Suite 2 retains the same handshake keys and `init_hash` commitment, but replaces
the session-key derivation as follows.

### Suite 2: X25519 and AKDF-v1

Each new attempt generates a fresh 32-byte private scalar with `getrandom`.
X25519 clamps the scalar internally and masks the peer public key's high bit as
required by RFC 7748. Both endpoints reject an all-zero raw DH result. X25519
comes from a pinned Monocypher 4.0.3 extraction; see
[`src/vendor/README.md`](../src/vendor/README.md) for provenance and
reproduction. No system crypto library is used.

Let `Z` be the full 32-byte X25519 result, `I` the full authenticated INIT (110
bytes), and `R` the full authenticated RESPONSE (134 bytes). `T = I || R` is
exactly 244 bytes. The tunnel ID is bound through AKDF's separate `id` input.
The transcript binds the version, suite, exchange ID, client/server positions,
nonces, timestamp, both DH public keys and handshake AMACs. Extraction and
expansion framing, regression vectors and security assumptions are specified in
[AKDF v1](AKDF_V1.md). The protocol applies that construction as follows; labels
are ASCII bytes without a trailing NUL (the expansion function appends it):

```text
PRK = Extract(master, id, Z)
S_c2s = Expand(PRK, id, "TUNTOM-AKDF-v1-C2S", T)
S_s2c = Expand(PRK, id, "TUNTOM-AKDF-v1-S2C", T)
H = Expand(PRK, id, "TUNTOM-AKDF-v1-HINT", T)
hint = (H[0] << 8) | H[1]
```

The client sends with `S_c2s` and receives with `S_s2c`; the server does the
reverse. The hint is an unsigned 16-bit value, not a key or authentication tag.
Suite 2 supplies the complete raw X25519 result as `Z`, after rejecting an
all-zero result. Suites 0/1 and handshake authentication use the separate direct
AMAC derivations above.

The raw DH secret and PRK are never sent. Server private scalars and DH/KDF
temporaries are wiped after candidate derivation, including exception paths. The
client wipes its private scalar after derivation or replacement of an INIT
attempt. A new INIT retry uses a new scalar/nonce/exchange ID; CONFIRM/ACK
retries reuse the same encoded bytes and do not require retaining the scalar.
Repeated RESPONSEs only trigger a CONFIRM retry when byte-identical to the
accepted one. Session key arrays and cached AEAD keys are wiped on session
destruction; MAC/AEAD state buffers and AKDF secret input buffers are also
wiped. This does not guarantee removal of compiler-created register copies, core
dumps or VM snapshots.

The client starts a fresh DH handshake after two minutes measured from candidate
creation, even when authenticated traffic continues. The current session remains
usable while INIT is pending. Existing confirmation timeout, counter limits and
three-second previous-session receive overlap apply. Idle restart remains 20
seconds. The server does not independently initiate handshakes. Compromise of
live session keys exposes that session, and compromise of the PSK permits future
impersonation; forward secrecy concerns past erased sessions.

CONFIRM and CONFIRM_ACK use S_c2s and S_s2c, respectively. All ordinary traffic
(DATA, HELLO, KEEPALIVE, PING/PONG, MTU_PROBE/REPLY) uses these session keys.
The existing custom MAC/expansion has regression coverage, not a security proof.

## State transitions and delivery

```text
Client                                     Server
  | -- INIT --------------------------------> | pending only
  | <---------------------------- RESPONSE -- |
  | -- CONFIRM -----------------------------> | activate session and peer
  | -- DATA (may follow immediately) -------> |
  | <------------------------- CONFIRM_ACK -- |
  | <================ DATA =================> |
```


The client derives its candidate on RESPONSE and can transmit immediately after
sending CONFIRM (normally one RTT after INIT). The server accepts no candidate
DATA before CONFIRM. If DATA overtakes CONFIRM, it is dropped without consuming
replay state. There is no TUN buffering before keys are available.

CONFIRM authorizes the server's new peer address. INIT/RESPONSE processing does
not update it. A repeated CONFIRM only resends the identical ACK; it never
resets counters, replay/reassembly state or the peer address. The client keeps
its configured destination. Authenticated ordinary active-session traffic can
still update the server's NAT peer. Previous-session traffic cannot.

INIT contains an AMAC-authenticated unsigned big-endian Unix timestamp in
seconds at payload offset 32. This layout is incompatible with the earlier
84-byte INIT; upgrade both endpoints together. `--init-window 300` sets the
**total** acceptance window in seconds (default 300, tolerance +/-150). Values
must be even, 2..86400. The receiving server enforces its own setting; settings
are not negotiated.

The client retries INIT every second with fresh nonce, timestamp and exchange
ID; responses to superseded attempts are ignored. CONFIRM retries retain their
bytes and have a five-second flight deadline. The server has one pending
exchange with a fixed five-second deadline. No INIT can evict it or the active
session. Losing a RESPONSE can therefore delay recovery until the pending
deadline.

After authentication and time validation, the server records each nonce,
including INITs ignored while pending. Repeated nonces are silently dropped,
even if their timestamp or exchange ID differs. An exact set avoids Bloom-filter
false positives. Entries remain through timestamp + half-window, including that
final second. The per-process, per-tunnel set holds up to 65536 nonces;
saturation rejects new INITs instead of evicting live entries. Expired entries
are removed on the next time-valid authenticated INIT. Acceptance time never
moves backwards within a process, so a local wall-clock rollback cannot
resurrect expired entries; a large clock correction can consequently prevent
handshakes until clocks catch up.

Authenticated INITs at 80% of the tolerance trigger a clock warning;
out-of-window INITs are rejected. Warnings include source, tunnel ID, observed
timestamp offset, tolerance and suppressed-event count, limited to one per 30
seconds per tunnel (and suppressed by --quiet). The offset is relative to the
server's nondecreasing acceptance clock, and can reflect delay/replay as well as
clock skew. Statistics include `init_timestamp_rejected` and
`init_nonce_capacity_rejected`.

This limits replay age, not packet rate or fresh captured INIT attacks. History
is not persisted: after restart a still-time-valid captured INIT can reserve the
pending slot again. There is no stateless cookie in this revision.

The previous active receive session is retained for three seconds after a
replacement. Starting another pending exchange discards older overlap state, so
at most two candidate session keys exist. The client restarts the handshake
after 20 seconds without authenticated active-session traffic. Retries and
timeouts use a monotonic clock. After server restart, old INIT may prompt a
fresh RESPONSE, but old CONFIRM/DATA cannot authenticate under the new keys.

## Sequence numbers and reassembly

The session hint is only a key-selection aid; it is not unique or an authority
to create a session. On hint collision, at most two candidate keys are tried.
Only the successfully authenticating session's replay state is updated.

Counter zero is reserved for CONFIRM/ACK. Ordinary packets in each direction
start at one, sharing one counter across all ordinary message types. Receiving
zero or one first is not required. The 64-value replay window tracks only the
lower counter bits, separately per session. Duplicate confirmations bypass
ordinary replay processing only for their idempotent ACK behavior.

Counters never wrap. Once the 48-bit counter is exhausted, transmission stops;
the client initiates a fresh handshake. If the server exhausts first, the client
recovers via the authenticated-traffic timeout. Suites 1/2 use the earlier
32-bit transmission limit described above.

Fragmented DATA uses a 37-byte header and a distinct sequence for every
fragment. Reassembly tables belong to sessions, so identical message IDs in
overlapping sessions cannot mix. Existing per-table limits remain; overlap may
temporarily hold two bounded tables.

Wireshark exposes handshake fields, suite, DH length, session hint and counter.
It does not verify AMAC. Its best-effort reassembly includes protocol version
and session hint in the key; collisions between sessions with the same hint
cannot be resolved without cryptographic session keys.

Wireshark labels AEAD packets as encrypted and never reassembles or dissects
their ciphertext as inner IP. It does not decrypt or verify AEAD tags.
