# Project TODO

## Fabric: peer access hints via encrypted ANNOUNCE (deferred 2026-09-17)

Expose optional peer address hints in Fabric for monitoring discovery, without
requiring IP addresses on tunnel interfaces or injecting management packets
into the kernel IP stack.

- Transport prerequisite: tuntom reads explicitly configured IPv4 addresses
  on the local **loopback interface**, excluding `127.0.0.0/8`. Do not select
  addresses from other interfaces, infer a management address, or query Syspiper.
- Send the address list in a dedicated `ANNOUNCE` frame after the initial
  completed handshake and each completed rekey, only within an authenticated,
  encrypted session. No announcement in plaintext mode; no response/ACK required.
- Expose the received list as `peer_access` metadata through control stats,
  with announcement age/session provenance. Replace the previous list on a new
  announcement, including an empty list; discard it when the session ends.
- Fabric displays these hints and may try its existing Syspiper poller against
  them, using the backend-configured key and bounded polling. Hint presence
  does not assert reachability, service availability, or permission to change
  network configuration. An empty list means only that no hints are provided.
- Keep the announcement generic: a future trepd consumer might use the same
  hints to try contacting a remote peer. No rendezvous URL, automatic routes,
  tunnel addressing, HTTP proxy, or Syspiper-specific tunnel protocol.
- Before implementation, define bounded/versioned payload encoding, peer
  compatibility and stale-hint handling; test encrypted-only emission/reception,
  rekey, empty-list replacement and Fabric deduplication of polling targets.

Status: TODO only. Fabric is the focus here; the tunnel protocol prerequisite
is deferred separately. IPv6 hint semantics remain to be defined.

## Adapter relay over tuntom tunnels (deferred 2026-09-15)

Extend remote adapter connectivity with a generic relay, using the existing
tuntom tunnel transport. The first use case is a remote
[divert adapter](docs/DIVERT.md); the same mechanism should support exit adapters.

```text
switch <-> tuntom (relay) === encrypted tunnel === tuntom (relay) <-> adapter
```

- Represent the remote adapter's ports at the local switch and expose the
  existing local switch IPC interface to the remote adapter.
- Carry a channel ID plus the complete switch data frame: opcode, label stack,
  and payload. Multiplex `divert-in` and `divert-out` over one tunnel.
- Add an explicit relay mode: ordinary tuntom currently transports the payload
  and recreates ingress labels at the receiving endpoint. Relay mode must
  preserve frames without classification or label rewriting. Keep existing
  command meanings, defaults, and ordinary tunnel behavior unchanged.
- Reuse encryption, session management, and fragmentation. Integrate relay
  endpoints into tuntom so separate relay processes are unnecessary.
- Keep the relay independent of divert admission and per-flow state; those
  remain in the divert adapter. Channel IDs must not consume label slots;
  retain the current eight-label limit.
- Define channel registration, reconnect behavior, bounded queues/backpressure,
  and frame/MTU limits; verify both directions and transport failure handling.

Status: design captured only; implementation is deferred.

## Documentation: virtio-net multiqueue tip (requested 2026-09-13)

When expanding deployment/performance guidance in `README_SWITCHING_MP.md`,
add a tip about virtual NIC queues, with a cross-reference near ECMP in
`README_SWITCHING.md`:

- Multiple tunnels and vCPUs can still share one saturated vhost network worker.
  Check vhost thread CPU on the host and `ethtool -l <interface>` inside the VM,
  using the NIC carrying the tunnels' outer UDP traffic. Check both the maximum
  available and currently active combined queue counts.
- For virtio-net, multiqueue must be exposed by the hypervisor and activated in
  the guest. In libvirt, the interface's `driver` element supports `queues` with
  `model type='virtio'`. Choose the queue count for the VM and workload; a guest
  setting alone cannot exceed the maximum exposed by the virtual device.
- Observed case: 6 vCPUs, 6 tunnels, 32 parallel TCP connections, inner MTU 9000.
  With one queue pair, forward was 1.25–1.32 Gbit/s and reverse 2.41 Gbit/s;
  a vhost worker reached 99.9% CPU while the adapter was around 47%.
  After the guest reported both maximum and active combined queues as 6,
  forward reached 3.14 Gbit/s and reverse 2.76 Gbit/s, without changes to the
  switch forwarding code. Forward means VM to host. These are user-reported
  individual roughly 60-second runs, not a general throughput guarantee.

References: [libvirt NIC driver options](https://libvirt.org/formatdomain.html#setting-nic-driver-specific-options),
[KVM multiqueue](https://www.linux-kvm.org/page/Multiqueue).

## Switch IPC: mmap with grouped references (approved 2026-09-12)

Implement the [accepted mmap transport direction](docs/SWITCH_MMAP_EXTENSION_DRAFT.md#accepted-implementation-direction-batched-mmap-references):
optional shared-memory payloads, with multiple references in one negotiated
SOCK_SEQPACKET record. Default maximum batch 8, configurable to 16; send only
already-ready frames for the same socket and flush partial batches immediately.
Keep legacy inline compatibility, per-slot ownership, ordering, RX/TX fairness
and the existing switch architecture. Single-frame mmap or mmsg alone is not
the complete desired implementation.

Status: implemented on 2026-09-13 as [switch IPC V2](docs/SWITCH_PROTOCOL_V2.md),
with a [Czech mechanism guide](docs/SWITCH_MMAP_CZ.md). MP and the tunnel/adapter
clients negotiate optional sealed mmap pools and single/grouped references;
legacy clients and the single-thread V1 switch remain compatible. Bounded TX
batches, pending RX references, worker migration, backpressure and reconnect
handling are integrated. Default slots carry 16 KiB and larger valid frames go
inline; memory and batch limits are configurable.

Verification includes independent wire peers, invalid mappings/references,
fault injection, 20 tunnels + 1/2/3 adapters, cached batches across worker
migration, and the real V5 tunnel/adapter path with a simulated kernel TUN.
ASan, UBSan and TSan checks pass. The actual MP comparison is in
[the V2 results](experiments/switch_mp/RESULTS_V2_2026-09-13.md), with
[a reproduction harness](experiments/switch_mp/README.md#ipc-v2-comparison).
Do not equate its whole-switch results with the older isolated mmap experiment:
actual batches depend on ready traffic and unchanged per-ingress RR quotas.

The earlier compatible socket/worker phase remains implemented and measured;
see [its reproduction](experiments/switch_mp/README.md#socket-phase-ab-comparison).

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

## tuntom_mp

Status: deferred, low priority (2026-09-13).

Explore an optional multithreaded tunnel with Ascon encrypt/decrypt workers.
The [Ascon worker mock benchmark](experiments/ascon_workers/RESULTS_CZ.md)
shows performance headroom: 1400 B mixed TX/RX increased from 4.51 to
16.29 Gbit/s with four workers plus a coordinator, using five CPUs instead
of one. These are codec/queue results; they exclude network I/O and session
management and do not establish an advantage over multiple simple tunnels.

Keep single-threaded tuntom as the baseline, scaling through multiple tunnel
instances with IPC per CPU. Multipath tunnels to the same destinations are
planned anyway, so their infrastructure also supports this scaling approach.
Before adopting tuntom_mp, compare the complete data path against multiple
single-threaded instances at the same CPU budget, including aggregate and
single-flow throughput, latency, and implementation/operational complexity.
Potentially useful, but no implementation is prioritized for now; retain Ascon.
