# Switch IPC: optional mmap payload transport

Status: **implementation direction approved by the user on 2026-09-12;
not implemented in production**. Byte assignments below remain draft
assignments. The existing [v1 protocol](SWITCH_PROTOCOL_V1.md) remains valid.

## Accepted implementation direction: batched mmap references

The desired implementation is optional mmap payload transport with **multiple
references in one socket record**, not just batching separate records through
sendmmsg/recvmmsg. The user selected this direction after the
[batching experiment](../experiments/ipc_batch/README.md).

- Keep the ordered AF_UNIX SOCK_SEQPACKET connection, legacy inline frames,
  and negotiated fallback. Old peers must continue working unchanged.
- Negotiate a distinct MMAP_BATCH capability in addition to MMAP_REF. Only
  peers that agree to it may exchange grouped reference records.
- Start with a **maximum of 8 frames per batch, configurable to 16**. Collect
  only frames already ready for the same destination socket. Send a partial
  batch immediately; never wait for a count threshold or batching timer.
  A single ready frame must also be sent immediately.
- Preserve per-link order and RX/TX round-robin fairness. Do not increase an
  ingress queue's quota merely to fill a batch. Keep the existing worker pool,
  routing, pointer matrix and reconfiguration barriers.
- Keep separate slots and generation tokens for every frame. The receiver
  copies each payload into its private buffer before releasing that slot.
  This first implementation still has two payload copies per hop.
- Handle grouped sends as one socket record: EAGAIN transfers none of the
  group; successful submission must never be replayed. Inline fallback must
  preserve order when it splits a group. Pending batches and mapping lifetimes
  must stay bounded through backpressure, reconnect and reconfiguration.
- Add batch-aware send/receive integration and logical-frame accounting. A
  one-frame-only API or one pending TX buffer is the compatibility baseline,
  not the complete approved target. Expose actual batch sizes and socket record
  counts separately from logical frame counts.

The experiment's type 17 record uses a 24-byte common header plus 16 bytes per
reference. This is a starting point, not a finalized production wire assignment;
capability encoding, receiver limits, validation and batch API details still
need to be completed before implementation.

Evidence: on two physical cores sharing L3, mixed-payload saturation improved
from 764 kpps / 2.60 us CPU per frame with single mmap references to 3.27 Mpps /
0.61 us with batches of 8, and 3.86 Mpps / 0.50 us with batches of 16. CPU is
the sum of both endpoints of one IPC hop. At 25 kpps batching saved about
13–15% CPU versus single references; at 300 kpps, up to 59%. Across different
L3 groups the batch-16 result was 814 versus 367 kpps. These are not whole-switch
capacity estimates; cache placement and offered load must accompany results.
The full [measurement report](/home/astib/Notebook/Tuntom_Notes/benchmark/ipc-batch-2026-09-12/REPORT_CZ.md)
records 261 runs and 243,544,122 frames without validation errors.

**Scope of the remaining draft:** the sections below preserve the original
single-reference negotiation, ownership and compatibility baseline. Their
one-record-per-frame, no-batch-framing and single-pending-buffer assumptions
are superseded by the accepted batch direction above for capable peers. They
must be extended consistently before treating this as a complete batch spec.

## Original single-reference baseline

The first extension keeps the Unix `SOCK_SEQPACKET` connection as the ordered
delivery channel. Each packet is either a complete v1 frame in that socket or
a short socket record referencing a complete v1 frame in shared memory.

```text
INLINE:    socket [v1 frame: labels + payload]
MMAP_REF:  socket [pool, slot, token, length] ---> mmap [complete v1 frame]
```

Both forms can alternate on one connection, independently in each direction.
This preserves a single ordering point and the existing poll FD. Routing,
SWITCH/EXIT meanings, worker scheduling, RX/TX pointer queues, queue quotas,
barriers and the network V5 protocol keep their current semantics.

This phase moves payload bytes out of kernel socket buffers. It still performs
one socket send/receive pair per packet per hop. A shared descriptor ring with
eventfd notifications would be a separate capability; this proposal does not
claim to remove packet syscalls or context switches.

## Compatibility and discovery

An old client starts with the exact `TTP\x01` registration. A new switch accepts
it as a legacy connection and sends only v1 frames, with no unsolicited ACK,
capability record or FD. An old client must never encounter the extension.

A new client can select legacy operation directly. To discover the extension,
it instead sends HELLO as the first record, **before sending a port ID**.
The existing old switch rejects this as an invalid pending registration and
closes that connection. It has not registered or replaced any named port.

In automatic mode, lack of extension support permits exactly one reconnect
using the old registration. EOF, an explicit unsupported-version response or
expiry of the initial HELLO probe permit this fallback. A malformed positive
response fails the attempt. The probe has a one-second maximum within the
client's existing five-second connection budget; the legacy retry uses the
remaining budget. Exhaustion returns to the existing reconnect backoff.
Legacy-only peers may be cached briefly to avoid probing on every reconnect.

After a valid WELCOME, setup can agree on inline-only operation on the same
connection. There is no speculative v1 reconnect after data publication.
Already accepted packets are never replayed during fallback or reconnect.

| Client | Switch | Result |
|---|---|---|
| v1 | v1 | Existing registration and frames |
| v1 | extension | Identical v1 behavior |
| extension, automatic | v1 | Unnamed probe rejected; fresh v1 connection |
| extension | extension, mmap unavailable | Negotiated inline-only connection |
| extension | extension, mmap available | Per-packet INLINE or MMAP_REF |

## Extension records

The common extension header is eight bytes. All socket integers are unsigned
and use network byte order. It is distinct from both TTP registration and
v1 data frames, whose first byte is 1.

```text
offset  bytes  field
0       3      magic = "TTX"
3       1      extension_version = 1
4       1      type
5       1      flags = 0
6       2      record_length, including this header
8       ...    body
```

Each extension occupies exactly one seqpacket record. Sizes below are exact;
trailing bytes, nonzero reserved fields and unexpected types in the current
state are errors. No implicit C/C++ structure layouts are serialized.
Capabilities are u32; bit 0 is MMAP_REF. Inline v1 frames are mandatory.
Unknown requested capability bits are ignored; selected bits must be a subset
of requested bits. Pool ABI 1 is defined below.

| Type | Value | Body, in order | Total bytes |
|---|---:|---|---:|
| HELLO | 1 | capabilities:u32, pool_abi:u32, requested_slots:u32, max_frame:u32 | 24 |
| WELCOME | 2 | epoch:u64, capabilities:u32, pool_abi:u32, slots:u32, max_frame:u32 | 32 |
| SETUP | 3 | epoch:u64, capabilities:u32, pool_abi:u32, slots:u32, frame_capacity:u32, slot_stride:u64, mapping_size:u64 | 48 |
| READY | 4 | epoch:u64, capabilities:u32, status:u32 | 24 |
| ACTIVE | 5 | epoch:u64, capabilities:u32, reserved:u32 | 24 |
| MMAP_REF | 16 | epoch:u64, pool_id:u32, slot_id:u32, token:u64, frame_length:u32, reserved:u32 | 40 |
| ERROR | 127 | error_code:u32, reserved:u32 | 16 |

Epoch is a fresh nonzero connection identifier assigned by the switch. It is
unrelated to label-table or scheduler versions and the port's internal
generation. Fresh connections always get fresh mappings; epoch is an additional
validation field, not permission to reuse an old mapping.

Max-frame is the complete serialized v1 frame size, including its labels.
The supported limit is 17..65607 bytes; the upper bound is
`8 + 8*8 + 65535`, matching current receive capacity. The switch chooses the
smaller peer/local limit. Oversized frames fail locally; choosing inline
transport cannot bypass the negotiated connection limit.

For mmap, use 1..128 slots per direction initially, with the switch selecting
no more than requested and no more than its memory budget permits. Current
internal queue/pool sizes do not change. Mmap frame capacity may be smaller
than the maximum inline frame; larger valid frames then go inline.
With MMAP_REF retained, SETUP keeps WELCOME's slot count and pool ABI and
selects a frame capacity in 17..max_frame. An unsupported requested pool ABI
removes MMAP_REF from WELCOME. HELLO requests below one slot or outside the
max-frame range are invalid when requesting mmap; larger slot requests may be
capped to 128. Inline-only HELLO sets pool fields to zero and still requires a
valid max-frame limit.

If MMAP_REF is not selected, pool ABI, slots, capacity, stride and mapping size
are zero in records containing them. WELCOME's max-frame remains nonzero.
READY status is 0 (setup accepted) or 1 (local mmap unavailable, capabilities=0).
ERROR codes are 1 unsupported version, 2 bad record/state, 3 capacity rejected,
4 resource failure and 5 invalid mapping/reference. ERROR is best effort,
followed by close; inability to send it must not delay cleanup.

## Establishing an extended connection

```text
client                                      switch
  |---- HELLO -------------------------------->|
  |<--- WELCOME -------------------------------|  no port bound yet
  |---- original TTP v1 registration ---------->|
  |<--- SETUP + optional SCM_RIGHTS ------------|  prepare, keep old port alive
  |---- READY -------------------------------->|
  |<--- ACTIVE --------------------------------|  publish new port
  |==== v1 frames and/or MMAP_REF ==============|
```

WELCOME selects candidate capabilities. SETUP may remove MMAP_REF if allocation
fails. READY may remove it if the client cannot map the pools. No step may add
a capability. ACTIVE reports the final intersection. If mmap is removed,
both sides dispose of provisional mappings and continue inline. A corrupt
SETUP or unexpected FD is a protocol error, not a resource fallback.

SETUP with MMAP_REF carries exactly two FDs in one SCM_RIGHTS message:

```text
FD[0], pool_id 1: client produces, switch consumes
FD[1], pool_id 2: switch produces, client consumes
```

The switch creates both pools, with identical geometry. SETUP without MMAP_REF
carries no FDs. No other record carries FDs. The client validates/maps them
before READY. No data can precede ACTIVE.

Admission checks, replacement preparation and potentially failing allocations
must finish while the previous same-ID port still works. Queue ACTIVE before
making the new connection eligible for TX, then publish the prepared port
through the existing barrier. EAGAIN on ACTIVE leaves the old port active.
No packet may overtake ACTIVE. Failed or abandoned handshakes discard their
provisional resources without replacing the existing port.
The successful ACTIVE send is the activation commit point: publication after
it must not allocate or fail during normal operation. A process crash at that
boundary is still handled as an ordinary disconnect, with no replay guarantee.

All pending states share the server's original fixed five-second registration
deadline; handshake records do not extend it. Pending connections and their
provisional mappings have bounded counts and memory. The two extra FDs per
mmap port must be included in admission's FD budget.

## Pool ABI 1 and ownership

Pools use `memfd_create(MFD_CLOEXEC | MFD_ALLOW_SEALING)`, fixed file size and
MAP_SHARED. Before handing out FDs, the switch applies
`F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL`. Write seals are not applied because
both endpoints update ownership words. This uses the documented
[memfd](https://man7.org/linux/man-pages/man2/memfd_create.2.html) and
[sealing](https://man7.org/linux/man-pages/man2/F_GET_SEALS.2const.html) APIs.

ABI 1 is a Linux, little-endian layout requiring naturally aligned, lock-free
64-bit interprocess atomic loads/stores. Use an explicitly verified platform
atomic ABI on aligned uint64_t storage, not a serialized std::atomic or a
compiler-dependent structure. Unsupported platforms negotiate inline transport.
Both endpoints map read/write, without executable permission.

Ordinary shared-memory fields are little-endian. The fixed header is:

```text
offset  bytes  field
0       8      magic = "TTMMAP01"
8       4      pool_abi = 1
12      4      pool_id = 1 or 2
16      8      epoch
24      4      slot_count
28      4      frame_capacity
32      8      slot_stride
40      8      mapping_size
48      4048   reserved = zero
4096    ...    slots
```

`slot_stride = align_up(64 + frame_capacity, 64)` and
`mapping_size = 4096 + slot_count * slot_stride`, with checked arithmetic.
The file length is exact even though the kernel maps whole pages. Slot i
starts at `4096 + i * slot_stride`:

```text
slot + 0       atomic u64 ownership token, initially 0
slot + 8..63   reserved = zero
slot + 64      up to frame_capacity bytes of a complete v1 frame
```

Validate pool geometry against SETUP and fstat once, then retain it in private
connection state. Never derive bounds from mutable shared metadata on the hot
path. Compute offsets from a validated slot ID; arbitrary pointers or arbitrary
byte ranges from the peer are not accepted.
Reject a non-regular FD, a wrong file size, missing required size/seal locks or
write seals incompatible with the required writable mapping. Perform these
checks before accessing any mapped byte. Both FDs must denote different files.

Tokens are per-slot generations:

```text
FREE(2k) -> producer writes -> READY(2k+1) -> consumer finishes -> FREE(2k+2)
```

1. The sole producer finds an even token with an acquire load, reserves that
   slot privately and writes the complete frame.
2. It publishes the odd token with a release store, then sends MMAP_REF with
   that exact token. A receiver must not inspect a slot without receiving its
   reference, even if the shared token is already READY.
3. The consumer validates the reference, acquire-loads the matching odd token,
   then obtains the frame. It releases the slot by storing the next even token
   with release semantics only after its final access to the frame.
4. A duplicate reference or wrong token is a transport error. Never wrap a
   token; retire the connection before its generation space is exhausted.

On EAGAIN/EINTR from sending MMAP_REF, no record was queued. The producer can
return its unsubmitted slot to the next even token; the caller retains its
original frame and gets the existing retry/drop result. A successful full
record send transfers ownership and is never repeated inline. Unexpected short
sends or fatal connection errors retire the connection and its mappings.

No RELEASE packets or new eventfds are needed. The receiver updates the shared
ownership word directly, so a full reverse socket cannot block buffer returns.
A slot does not become free merely because its socket reference was read or
the next packet arrived.

## Sending, ordering and backpressure

After ACTIVE, one serialized sender per connection direction chooses:

```text
supported mmap + frame fits + free slot -> MMAP_REF
otherwise                              -> original inline v1 frame
```

This is a per-packet choice. A small ACK may be inline and a large data packet
mapped. The threshold is local policy, not a wire-format change. Mmap-only
waiting for a free slot is not part of this profile: pool-full falls back to
inline, and socket EAGAIN remains the flow-control mechanism. Otherwise a
writable socket could spin waiting for a slot release with no FD notification.

Both packet forms enter the same socket ordering domain. A REF whose payload
was published first is ordered by its successful socket send, not the mmap
store. Receivers process data records in that order, and do not let an inline
packet bypass an earlier outstanding mapped packet. No packet sequence,
reorder queue, batch framing or transport-switch barrier is needed.

There is no global order between different ingress ports. Existing TX
round-robin and per-link ordering apply. If a pending send is retained after
EAGAIN, newer frames do not bypass it on another transport.

## Integration boundary and validation

The first integration can retain SwitchClient send_frame and
receive(buffer, capacity) semantics:

- send_frame builds a complete frame in a shared slot or uses today's inline
  scatter/gather send. Success means accepted by this transport, not delivered
  to the final TUN or remote peer.
  Return the logical frame length on success, including for a 40-byte REF;
  preserve -1/errno on failure. Existing callers compare against frame length.
- receive consumes one INLINE/REF. For REF, copy into the caller's existing
  private receive buffer, then release the slot and return the frame length.
  Callers continue allocating the negotiated maximum receive capacity. If a
  caller supplies less, copy at most its capacity and return the full length,
  preserving MSG_TRUNC-style handling; never decode a truncated private copy.
- Switch RX still validates/relabels its private Buffer and passes its pointer
  through the existing matrix. TX still owns one pending Buffer. Mmap output
  copies into that port's outgoing mapping. Routes never contain mmap IDs or
  references belonging to another connection.

The initial implementation replaces kernel-mediated payload copies with
user-space copies; it does not promise fewer total copies or end-to-end
zero-copy. A borrowed-frame API could defer release and avoid some copies
later, outside this integration. Ingress pools are not mapped into unrelated
destination processes.

Use bounded recvmsg for setup, inline and reference records so ancillary FDs
cannot be silently mishandled. Use MSG_CMSG_CLOEXEC; check MSG_TRUNC and
MSG_CTRUNC; close all received FDs on every rejected record. FD transfer and
receive flags follow the
[Unix socket](https://man7.org/linux/man-pages/man7/unix.7.html) and
[recvmsg](https://man7.org/linux/man-pages/man2/recvmsg.2.html) interfaces.

For REF, check negotiated state/capability, exact record length, epoch,
direction's pool ID, slot bounds, matching odd token and frame length before
accessing bytes. Frame length must fit the slot and negotiated maximum.
Validate the copied private frame using the existing v1 decoder, including
equality of inner total_length and REF frame_length. An invalid v1 frame is
dropped/counted as today and its valid slot released. An invalid reference
closes the connection without accessing or releasing an unvalidated slot.
Never route using labels that can change after validation.

The local socket permission boundary remains. Size seals prevent resize
hazards, not peer mutation of mapped content. Private-copy validation and
private geometry preserve bounds even if a local peer misbehaves; the
ownership contract defines valid publication and reuse.

## Reconnect, cleanup and observability

Socket EOF/HUP retires the whole transport. Close its FDs, release local
references and unmap only after workers stop accessing it. Reconnect creates
a fresh epoch and pools, even for the same port ID. Never recover old READY
slots as fresh packets or attach old mappings to a new connection. Outstanding
accepted frames follow existing disconnect/drop semantics; successful send
is not an application-level delivery ACK.

No live pool resize/replacement within a connection is defined. Mapping
lifetime follows the existing barrier and retired-port lifetime rules. A
mapping failure after activation is a transport failure; inline fallback is
only for an unsubmitted packet and a healthy session.

Count logical RX/TX frames and bytes exactly once, independent of transport.
REF/setup metadata do not inflate throughput. Suggested new counters:
inline_rx/tx, mmap_rx/tx, mmap_pool_full_fallbacks, mmap_setup_fallbacks,
mmap_protocol_errors, mmap_copy_bytes and mmap_mapped_bytes. Record each port's
negotiated capability for interpreting measurements.

## Acceptance checks for an implementation

- All old/new peer combinations, old-client byte-for-byte behavior, unknown
  capabilities and safe rejection of unnamed HELLO by the actual old parser.
- Two-pool FD exchange, unavailable mmap falling back inline, unexpected/extra
  FDs, ancillary truncation, invalid sizes/seals/epoch and complete FD cleanup.
- Interleaved INLINE/REF in both directions with exact sequence/payload checks,
  1/8 labels, SWITCH/EXIT, capacity boundaries and larger inline frames.
- Slot reuse, stale/duplicate tokens, delayed consumption, pool-full fallback,
  socket EAGAIN and no duplicate retry after successful send.
- Same-ID replacement during stalled setup; handshake timeouts; reconnect and
  process death with outstanding slots, retaining current barrier checks.
- Mixed old/mmap ports through both switch targets and unchanged forwarding,
  adapter cache and malformed-frame handling; no slot references across ports.
- A/B at identical offered load: INLINE versus MMAP_REF, small/jumbo/mixed
  frames, one flow and many ports; total component CPU, CPU/frame, latency,
  copies, packet syscalls, throughput and loss. No gain is assumed in advance.

The isolated [one-hop A/B experiment](../experiments/ipc_mmap/README.md) now
exercises these two payload paths, including private-buffer copies and mmap
pool-full fallback. Its simplified setup is not an implementation of this
handshake; the extension remains a draft.
