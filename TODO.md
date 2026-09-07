# Project TODO

## Statistics write strategy

The text statistics export is approaching 100 fields and currently rewrites an
atomic temporary file every second for every tuntom process. The file does not
grow over time, but frequent create/write/rename operations will not scale to a
large number of tunnels.

Deferred design:

- add `--stats-interval <seconds>`, probably with a 10-second default;
- keep the complete periodic dump in one file rather than splitting its schema;
- immediately publish stats on switch connect, disconnect and socket error;
- retain the immediate `SIGUSR2` snapshot;
- retain `--no-stats` with no periodic writes;
- verify whether `SIGUSR1` should restore the configured interval unchanged.

The intended result is current switch diagnostics with roughly one tenth of the
normal filesystem traffic. Revisit before deploying large tunnel populations or
adding substantially more metrics.

## Faster plaintext authentication

The current plaintext suite's custom AMAC absorbs 8 bytes per Ascon-p[8] call
and requires extra payload copies. On the current test host, its codec throughput
is roughly half that of Ascon-AEAD128 for normal packet sizes.

Deferred design: replace the data-session AMAC with an authentication-only use
of standardized Ascon-AEAD128. Keep the payload public by passing an empty
plaintext and authenticating the protocol context, metadata and plaintext
payload as associated data:

```text
AD  = domain/version || tunnel_id || metadata || plaintext payload
P/C = empty
wire = metadata || 128-bit tag || plaintext payload
```

- retain the existing 128-bit tag and wire overhead;
- add scatter/gather associated-data absorption so metadata and payload form one
  padded stream without constructing a contiguous temporary buffer;
- use a directional per-session key and a unique nonce derived from the packet
  sequence; enforce the encrypted-suite counter/rekey limits for this suite too;
- allow retransmission under the same key/nonce only by replaying identical wire
  bytes;
- initially retain the existing AMAC for INIT/RESPONSE, whose sequence is zero;
  migrating the handshake needs a separate nonce design based on its exchange ID;
- assign a new negotiated suite/protocol identity; never silently reinterpret
  the existing plaintext suite because old and new tags are incompatible;
- add independent known-answer, tampering, nonce/counter, cross-suite and
  performance tests before enabling it.

Target: plaintext authentication close to AEAD throughput by using the
Ascon-AEAD128 16-byte rate, without introducing another custom wide-rate MAC.
