# Project TODO

## Switch IPC: mmap with grouped references (approved 2026-09-12)

Implement the [accepted mmap transport direction](docs/SWITCH_MMAP_EXTENSION_DRAFT.md#accepted-implementation-direction-batched-mmap-references):
optional shared-memory payloads, with multiple references in one negotiated
SOCK_SEQPACKET record. Default maximum batch 8, configurable to 16; send only
already-ready frames for the same socket and flush partial batches immediately.
Keep legacy inline compatibility, per-slot ownership, ordering, RX/TX fairness
and the existing switch architecture. Single-frame mmap or mmsg alone is not
the complete desired implementation.

Status: selected for the next phase; only the isolated mmap benchmark exists so far.
The compatible socket/worker prerequisite is implemented and measured as of
2026-09-12: per-worker readiness, wake handling, cached lookup/queue state and
small-topology pairing. Its 81-run comparison shows lower CPU at light/sparse
loads; saturated active topologies have similar throughput. Keep mmap as a
separate follow-up, as requested. See [socket-phase reproduction](experiments/switch_mp/README.md#socket-phase-ab-comparison).
Finalize the batch capability/wire format and bounded send/receive integration,
then verify the real tuntom -> switch -> adapter path, including multiple ports,
backpressure, reconnect, CPU cache placement and equal offered-load comparisons.
See the [experiment and reproduction instructions](experiments/ipc_batch/README.md).

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
