# tuntom V4: AMAC sessions and optional Ascon-AEAD128

This is the implemented, pre-production V4 wire format. Both endpoints must
be upgraded together. Earlier V4 builds using static DATA keys are incompatible.
There is no V3 receive path or automatic downgrade. Explicit legacy V1/V2
receive flags bypass the session security model and should remain disabled.

## Common header

All integers are unsigned, big-endian. The UDP payload starts with 48 bytes:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | Magic `UTUN` |
| 4 | 2 | Tunnel ID |
| 6 | 1 | Version = 4 |
| 7 | 1 | Message type in bits 0–6; bit 7 = AEAD session packet |
| 8 | 8 | SEQ: upper 16 bits session hint, lower 48 bits counter |
| 16 | 8 | Message ID / handshake exchange ID |
| 24 | 4 | Fragment offset (DATA only) |
| 28 | 4 | Original packet length (DATA), outer MTU (PMTUD) |
| 32 | 16 | AMAC or Ascon-AEAD128 tag |
| 48 | variable | Payload |

AMAC is the project's existing custom Ascon-permutation MAC, **not HMAC** or
a standardized Ascon-MAC profile. Its input is `header[0:32] || payload`;
the tag itself is excluded. No extra session-ID bytes are added to DATA.

## Handshake messages

| Type | Direction | SEQ | Payload | Total UDP payload |
|---|---|---|---|---:|
| INIT (8) | C → S | 0 | nonce_C[32], suite[2], dh_length[2], DH_C[dh_length] | 84 B |
| RESPONSE (9) | S → C | 0 | init_hash[32], nonce_S[32], suite[2], dh_length[2], DH_S[dh_length] | 116 B |
| CONFIRM (10) | C → S | hint << 48 | empty | 48 B |
| CONFIRM_ACK (11) | S → C | hint << 48 | empty | 48 B |

All four messages share the client's nonzero random 64-bit exchange ID in
`message_id`; fragment offset and original length are zero. Nonces are 32
fresh random bytes from Linux `getrandom`, with errors failing closed.
Retransmissions preserve the exact original bytes, nonces and exchange ID.

Suite **0** (default) authenticates plaintext using AMAC. Suite **1** requires
`--encrypt-ascon` on both endpoints and uses standard NIST SP 800-232
Ascon-AEAD128. Both require `dh_length = 0`, exact payload lengths and matching
local configuration. Unsupported suites and nonempty DH are rejected. There
is no negotiation, timeout fallback, or forward secrecy.

INIT/RESPONSE remain plaintext AMAC packets with bit 7 clear. Their authenticated
suite fields are also bound into the session-key transcript. All subsequent
suite-1 packets, including CONFIRM/ACK and PMTUD, set bit 7 and use AEAD:

- Key: the existing fresh 128-bit directional session key derived below.
- Nonce: eight zero bytes followed by the exact eight wire SEQ bytes.
- Associated data: exact header bytes 0 through 31 (including bit 7).
- Ciphertext: payload at offset 48; full 128-bit tag at offset 32.
- The standard's internal lanes are little-endian; wire integers remain big-endian.

Directional keys prevent cross-direction nonce reuse. Each fragment/control
packet consumes a fresh counter. Counter zero is used only by the fixed
CONFIRM/ACK in its respective direction; retries reproduce the identical packet.
Suite 1 stops sending after counter `2^32 - 1` and establishes a fresh session,
well before 48-bit wrap. With UDP datagrams below 64 KiB this also bounds traffic
to less than `2^48` bytes per directional key. Mode-bit changes fail authentication;
wrong modes are rejected, and legacy receive flags cannot accompany encryption.
No plaintext is delivered before verification; failed candidate plaintext is wiped.

Reference: [NIST SP 800-232](https://doi.org/10.6028/NIST.SP.800-232).

## Key derivation and binding

Below, `AMAC(K, id, bytes)` is the existing 16-byte function `ascon::mac`.
`||` is byte concatenation. The master secret is 16 bytes.

Long-term handshake keys (ASCII labels, **without** trailing NUL):

```text
H_c2s = AMAC(master, id, "TUNTOM-V4-CLIENT-TO-SERVER")
H_s2c = AMAC(master, id, "TUNTOM-V4-SERVER-TO-CLIENT")
```

INIT is tagged with H_c2s; RESPONSE is tagged with H_s2c. Session DATA cannot
be authenticated with these keys, even if the direction matches.

Define the domain-separated expansion below. Here, labels **include one
trailing NUL byte** before the transcript:

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

The client derives its candidate on RESPONSE and can transmit immediately
after sending CONFIRM (normally one RTT after INIT). The server accepts no
candidate DATA before CONFIRM. If DATA overtakes CONFIRM, it is dropped without
consuming replay state. There is no TUN buffering before keys are available.

CONFIRM authorizes the server's new peer address. INIT/RESPONSE processing
does not update it. A repeated CONFIRM only resends the identical ACK; it
never resets counters, replay/reassembly state or the peer address. The client
keeps its configured destination. Authenticated ordinary active-session traffic
can still update the server's NAT peer. Previous-session traffic cannot.

The client retransmits INIT or CONFIRM every second, with a ten-second flight
deadline. Expiry starts a fresh INIT. The server has one pending exchange with
a fixed ten-second deadline; duplicates resend RESPONSE without extending it.
An unrelated INIT cannot evict the pending exchange or the active session.
The server remembers up to 64 accepted INIT byte strings during the process
lifetime to avoid reopening an expired exchange repeatedly. This is bounded
DoS mitigation, not unlimited handshake replay history or traffic rate limiting.

The previous active receive session is retained for three seconds after a
replacement. Starting another pending exchange discards older overlap state,
so at most two candidate session keys exist. The client restarts the handshake
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
recovers via the authenticated-traffic timeout. Suite 1 uses the earlier
32-bit transmission limit described above.

DATA fragmentation uses the unchanged 48-byte header and a distinct sequence
for every fragment. Reassembly tables belong to sessions, so identical message
IDs in overlapping sessions cannot mix. Existing per-table limits remain;
overlap may temporarily hold two bounded tables.

Wireshark exposes handshake fields, suite, DH length, session hint and counter.
It does not verify AMAC. Its best-effort reassembly includes protocol version
and session hint in the key; collisions between sessions with the same hint
cannot be resolved without cryptographic session keys.

Wireshark labels AEAD packets as encrypted and never reassembles or dissects
their ciphertext as inner IP. It does not decrypt or verify AEAD tags.
