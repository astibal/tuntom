# Switch IPC V2: mmap and grouped references

V2 adds an optional payload transport to the existing label switch. It is
implemented by `tomtom-switch-mp`, `SwitchClient`, tuntom and the adapter.
`tuntom-switch` remains a V1 server; automatic clients fall back to it.
The [V1 frame and registration encoding](SWITCH_PROTOCOL_V1.md) are unchanged.
The earlier [design and benchmark rationale](SWITCH_MMAP_EXTENSION_DRAFT.md)
is historical; the byte assignments and contracts on this page are normative.
The network V5 protocol is unrelated and unchanged. For a Czech walkthrough,
see [Jak funguje mmap](SWITCH_MMAP_CZ.md).

## What mmap changes

```
V1: producer buffer -> kernel socket buffer -> receiver buffer
V2: producer buffer -> shared mmap slot     -> receiver buffer
                              ^
              socket carries an ordered reference to this slot
```

`memfd_create` creates an anonymous RAM-backed file. `SCM_RIGHTS` transfers
access to that file over the Unix socket; `mmap(MAP_SHARED)` maps the same
pages into each process. Virtual addresses need not match. Only slot numbers,
lengths and generation tokens cross the socket, never process pointers.
Mappings keep their backing files alive after local FDs are closed.
See [memfd_create](https://man7.org/linux/man-pages/man2/memfd_create.2.html)
and [Unix descriptor passing](https://man7.org/linux/man-pages/man7/unix.7.html).

This implementation still copies a complete frame twice per hop. The receiver
copies before validating/routing, so no peer-writable labels escape into the
switch's private pointer matrix. Batching saves socket operations by putting
several references in one record; it does not create end-to-end zero-copy.
There are no new data-plane eventfds or shared descriptor rings.

## Compatibility and connection state

A legacy client sends its original `TTP\x01` registration and receives only V1
frames. V2 sends no ACK, FD or extension record to legacy clients.

```
client                                 MP switch
  HELLO ------------------------------>  no named port yet
        <--------------------- WELCOME
  TTP v1 registration ---------------->  admission / provisional pools
        <---------------- SETUP + 2 FDs (or inline SETUP without FDs)
  READY ------------------------------>  prepare complete scheduler plan
        <---------------------- ACTIVE
  INLINE / MMAP_REF / MMAP_BATCH <---->  published connection
```

Automatic discovery sends an unnamed HELLO. An old switch rejects that pending
registration and closes it, without replacing any named port. EOF, a valid
unsupported-version ERROR or the one-second probe deadline permit **one** fresh
V1 connection within the original five-second client deadline. A malformed
positive response is fatal. Explicit `v1` skips probing; explicit `inline`
requires V2 but requests no mmap. There is no downgrade/replay after registration.

Each server-side pending connection has the original fixed five-second deadline;
handshake progress does not renew it. A repeated name does not replace its old
port until resources, RX pools, matrix links, lookup tables and epoll sets are
prepared and ACTIVE is successfully queued. ACTIVE EAGAIN leaves the old port
live. The plan is then published through the existing barrier without further
allocation. Data cannot overtake ACTIVE. An unpublished prepared plan is rebuilt
against current topology after EAGAIN; it cannot resurrect an intervening retired
port. Admission is checked both before pool allocation and before activation.

Pool allocation failure or the configured memory budget can remove mmap at
SETUP. Local mapping failure can remove it at READY. Corrupt records, invalid
FDs or invalid mappings are errors, not resource fallback. Both provisional
pools are released if mmap is declined. A reconnect always creates new pools;
accepted frames are never replayed onto another connection.

## Socket record encoding

Unsigned integers are big-endian. Each record has this exact eight-byte header:

```
0..2   "TTX"
3      connection protocol version = 2
4      type
5      flags = 0
6..7   total record length, including header (u16)
```

All listed lengths are exact; extra bytes and nonzero reserved fields are
invalid. These are byte encodings, not serialized C++ structures.

| Type | Value | Body after the common header | Total bytes |
|---|---:|---|---:|
| HELLO | 1 | capabilities:u32, pool_abi:u32, requested_slots:u32, max_frame:u32, max_batch:u32, reserved:u32 | 32 |
| WELCOME | 2 | epoch:u64, capabilities:u32, pool_abi:u32, slots:u32, max_frame:u32, max_batch:u32, reserved:u32 | 40 |
| SETUP | 3 | epoch:u64, capabilities:u32, pool_abi:u32, slots:u32, frame_capacity:u32, slot_stride:u64, mapping_size:u64, max_batch:u32, reserved:u32 | 56 |
| READY | 4 | epoch:u64, capabilities:u32, status:u32 | 24 |
| ACTIVE | 5 | epoch:u64, capabilities:u32, reserved:u32 | 24 |
| MMAP_REF | 16 | epoch:u64, pool_id:u32, slot_id:u32, token:u64, frame_length:u32, reserved:u32 | 40 |
| MMAP_BATCH | 17 | epoch:u64, pool_id:u32, count:u16, reserved:u16, references[count] | 24 + 16 × count |
| ERROR | 127 | error_code:u32, reserved:u32 | 16 |

Each batch reference is `slot_id:u32, frame_length:u32, token:u64`. Slot IDs
within one record must be distinct. `count` is 1..negotiated max_batch; a
one-frame batch is valid. A peer with MMAP_REF may send single REF records even
when MMAP_BATCH is selected. The implementation emits BATCH for all references
when batching is selected. This permits one decoder and uniform accounting.

Capabilities: bit 0 = MMAP_REF, bit 1 = MMAP_BATCH. BATCH requires REF. Unknown
requested bits are ignored; selected bits must be a subset of requested bits.
Unknown selected bits are errors. Inline V1 frames are always supported.

- `max_frame`: 17..65607 bytes, including V1 labels/header. WELCOME chooses the
  smaller local/peer bound. Inline fallback cannot bypass it.
- Pool ABI: 1. An unsupported requested ABI removes both mmap capabilities.
- Slots: requested count is nonzero for mmap; WELCOME caps it at peer/local
  limits and 128. SETUP retains that count if mmap is retained.
- Batch: HELLO requests 1..16, WELCOME chooses no more than either side's
  maximum. Without MMAP_BATCH the selected value is 1.
- Without mmap, ABI/slots/capacity/stride/mapping size are zero and batch is 1.
  An inline-only HELLO uses zero ABI/slots and batch 1.
- `frame_capacity`: 17..max_frame. The default slot capacity is 16384 bytes,
  capped by max_frame. Larger valid frames are sent inline;
  `--ipc-frame-capacity` changes this memory/performance tradeoff.
- `epoch`: nonzero connection serial within a server lifetime, separate from
  scheduler/table versions and port generations. It is scoped to this socket
  and its fresh mappings, not a globally unique or secret identifier.
- READY status 0 accepts the exact SETUP capability set; status 1 declines mmap
  and uses capabilities=0. ACTIVE confirms the exact final capabilities.
- ERROR code 1 means unsupported version. Other errors close the connection;
  sending ERROR is optional. The current server closes invalid handshakes.

SETUP with mmap carries exactly two FDs in one SCM_RIGHTS message:

```
FD[0], pool 1: client writes -> switch reads
FD[1], pool 2: switch writes -> client reads
```

Both files have identical geometry and distinct file identities. SETUP without
mmap carries no FDs. Every other record rejects ancillary data. `recvmsg` uses
MSG_CMSG_CLOEXEC and bounded ancillary storage; every installed FD is closed on
rejection, including truncated control messages. Local pool FDs are closed once
setup is complete; active ports retain mappings rather than extra FDs. Admission
reserves up to two provisional FDs per pending mmap connection, in addition to
its socket and the next plan's epoll reserve.

## Pool ABI 1 and ownership

ABI 1 requires little-endian Linux and naturally aligned lock-free 64-bit
atomic loads/stores. The implementation uses compiler native atomic builtins
on aligned u64 storage and checks lock freedom; it never shares `std::atomic`
object layout. The acquire/release ordering follows the
[GCC atomic builtin contract](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html).
Unsupported platforms negotiate inline operation.

Pools are fixed-size memfds with MAP_SHARED, PROT_READ|PROT_WRITE, no executable
mapping. The switch reserves backing storage using fallocate before publication.
It requires F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL; write/future-write seals are
incompatible with both endpoints updating ownership. Fixed size prevents resize
hazards, not peer modification of payloads. Socket filesystem permissions remain
the local access boundary.

All ordinary mmap fields are little-endian:

```
offset size field
0        8  "TTMMAP01"
8        4  pool ABI = 1
12       4  pool ID (1 or 2)
16       8  epoch
24       4  slot count
28       4  frame capacity
32       8  slot stride
40       8  mapping size
48    4048  reserved zero
4096   ...  slots

stride       = align_up(64 + capacity, 64)
mapping_size = 4096 + slots * stride
slot i       = 4096 + i * stride

slot + 0       atomic u64 token, initially zero
slot + 8..63   reserved zero
slot + 64      complete serialized V1 frame
```

Geometry, bounds, file size, seals, initial header and slot state are validated
before activation. Both descriptors must be read/write and their headers are checked before treating an mmap
failure as resource fallback. The hot path computes offsets exclusively from
private validated geometry; later peer changes to shared headers cannot change
bounds. Slot generations are independent:

```
FREE(2k) -> producer writes payload -> READY(2k+1)
         -> consumer copies payload -> FREE(2k+2)
```

The producer finds an even token with acquire, writes the complete frame and
publishes the odd token with release. It then sends that exact token in a socket
reference. The receiver never polls slots looking for unsolicited READY data.
It first receives and validates the reference, acquire-loads the matching odd
token, copies into a private buffer, then releases the slot with the next even
token. Only after that may the producer overwrite the slot. Tokens never wrap;
an exhausted slot generation retires the connection.

Every reference in a batch is structurally checked before any slot is accessed
or released. Each token is rechecked at consumption, including references kept
across RX round-robin visits. Wrong epochs, directions, lengths, slots, tokens,
duplicates or unnegotiated record types close the connection. An invalid *inner
V1 frame* is handled by existing frame-drop counters after its valid slot is
released; no shared payload pointer reaches the decoder or routing table.

## Ordering, batching and backpressure

INLINE, REF and BATCH share one ordered SOCK_SEQPACKET connection per direction.
The socket send establishes order, not the time of the shared-memory write.
No release packets, polling of a shared ring or new FD notifications are needed;
the consumer releases ownership directly in mmap.

Default maximum batch is 8, configurable up to 16. Both peers must permit
16 to negotiate it. A partial batch is sent at
the end of the current bounded work slice; a full batch sends immediately. No
batch timer or count-triggered wait exists. Tuntom and adapter stage into shared
slots directly while handling already available input, avoiding a third payload
copy. The switch takes at most one pointer from each ingress queue per RR sweep,
stopping at its output batch limit. It never revisits an ingress merely to fill
a batch. Therefore a single-source switch output often has batch size 1, even
when its input records contain many frames.

A full pool falls back inline, rather than waiting for a slot without any FD
notification. Oversized-for-slot but valid frames also go inline. An earlier
staged group is flushed before that inline frame. The two transport forms
cannot overtake each other.

A failed reference send (EAGAIN/EINTR) submits none of the record. All reserved
slots in that attempt are returned to their next even tokens. MP retains up to
16 private RX-buffer pointers and their source owners; EPOLLOUT retries the
unsubmitted prefix. A successful prefix is released and never replayed. Queue
ordering and pending pointers survive scheduler changes; removed sources/targets
are discarded at the existing barrier with logical-frame drop accounting.
Client producers keep their previous drop-on-backpressure policy and report
staged frames as sent only after successful socket submission.

`Transport::send_batch` returns a logical accepted prefix, not wire byte count.
`send`/`SwitchClient::send_frame` still return logical V1 frame length or -1/errno.
`append_frame`/`flush` are the bounded client slice API: outcomes report actual
sent frames/bytes and drops; callers flush before sleeping and discard on an
exception. Immediate send APIs reject mixing with an unflushed staged batch.
`receive` returns one logical frame at a time, retaining at most 16 references.
It preserves full-length/MSG_TRUNC-style behavior for a short caller buffer.
Poll loops explicitly include `receive_pending()` so cached references cannot
be stranded after the socket record has been consumed.

## Resource bounds and observation

Default server mmap budget is 256 MiB over active **and pending** connections,
including replacements. Default slots are 128 per direction, each carrying up to
16384 bytes (including frame headers). This covers ordinary 9000-byte jumbo
payloads and costs about 4 MiB per connection, or 93 MiB for 20 tunnels plus
three adapters. Larger frames remain valid and use inline transport. Configuring
65607-byte slots costs about 16.1 MiB per connection; lower `--ipc-slots` or a
larger memory budget can accommodate more such ports. These shared pools are
additional to the switch's private RX pools/matrix.

```
MP switch / mk_switch_mp.sh:
  --ipc-mode auto|v1|inline     default auto
  --ipc-batch 1..16            default 8
  --ipc-slots 1..128           default 128
  --ipc-frame-capacity 17..65607 default 16384 (complete frame bytes)
  --ipc-memory-mib 1..65535    default 256

tuntom / adapter / mk_adapter.sh:
  --switch-ipc auto|v1|inline  default auto
  --switch-ipc-batch 1..16     default 8

mk_tunnel.sh: --client-switch-ipc / --server-switch-ipc
              --client-switch-ipc-batch / --server-switch-ipc-batch
```

Existing frames/bytes/throughput counters count logical V1 frames. Per-connection
`port_N_ipc_*` (switch) and `switch_ipc_*` (clients) expose negotiated version,
mmap state, batch limit, mapping bytes and directional syscall attempts, actual
socket records, inline/mapped frames, batch records, largest batch, pool fallback
and invalid references. Port counters cover that connection's lifetime; client
counters accumulate across reconnects. `ipc_mapping_bytes` includes provisional
server pools; `ipc_memory_budget` reports its bound. Frames/record is measured
from mapped/logical frames and actual data socket records, not configured batch
size. Handshake/control records are excluded from data counters.

## Verification and measurements

The [implementation results](../experiments/switch_mp/RESULTS_V2_2026-09-13.md)
record measured throughput, CPU, latency and actual batch sizes, together with
the host conditions and correctness evidence.

`switch_v2_test` checks codecs, generations, partial batches, mixed inline/mmap,
short receive buffers, EAGAIN rollback, independent-process transfer, unexpected
FD cleanup and client negotiation/corruption. `switch_v2_integration_test` uses
an independently encoded wire peer against the real MP binary: capability
variants, memory fallback, replacement, fixed timeout, invalid references,
resource/activation fault injection and 20 tunnels with 1, 2 and 3 adapters.

The real SwitchClient/MP performance harness is
[`experiments/switch_mp/mmap_bench.py`](../experiments/switch_mp/mmap_bench.py).
It compares V1, V2 inline, single-reference mmap and batches 8/16 at equal offered
rates and saturation, reports CPU on both sides as well as switch CPU, reconciles
accepted/delivered frames and records CPU/L3 placement. It is an IPC/switch test;
network V5 crypto, UDP, a kernel TUN and a physical NIC are not included.
