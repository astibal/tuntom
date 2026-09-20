# Bounded IPC retries

Production IPC producers retain records on local `EAGAIN`, `EWOULDBLOCK` and
`EINTR`. Each output has its own FIFO ring:

- 128 records, at most 256 KiB of live queued bytes;
- 100 ms maximum age, measured with the monotonic clock;
- a 1 ms retry gate after a temporary send failure;
- at most 128 submissions per flush;
- `POLLOUT` interest only while a queued record is eligible for retry.

Queue admission copies the complete canonical record. Slots reuse vector storage;
retained allocation can exceed live bytes but is bounded by 128 slots of at most
128 KiB each. Ordinary successful sends keep their existing fast path. New records
cannot bypass an existing queue. Capacity and expiry drops are counted; this is
short-term buffering, not reliable delivery or protection against sustained overload.

## Coverage

| Path | Queue owner |
| --- | --- |
| Tunnel, exit adapter, divert adapter → switch | `ipc::Transport` through `SwitchClient::append_frame/flush` |
| Relay → local adapter | Each client's `ipc::Transport` |
| Relay → local switch | Relay upstream record queue |
| Single-thread switch → output | Each connection's record queue, including relay ACKs |
| Multiprocess switch relay ACKs | Main-thread queue per port |
| Multiprocess switch DATA | Existing worker pending batches and bounded ingress/output queues |

The MP DATA path keeps its existing queue policy and lifetime; the new 100 ms
expiry applies to the new retry rings. Its control ACK queue is separate from
worker DATA and may interleave with DATA, as immediate ACK sends did before.
Handshake and control-response state machines already retain pending output.

The low-level `Transport::send/send_batch` API remains a synchronous submission
API: its caller owns an unsubmitted suffix (the MP worker uses this contract).
It returns `EBUSY` if mixed with outstanding append/flush work, preventing bypass.

## mmap and lifecycle

Normal batches continue to use mmap references. If submitting a reference record
fails temporarily, ownership is rolled back and the original payloads are copied
into the retry ring before any slot can be reused. Retries send canonical inline
frames, which are valid on V2 connections as well as legacy IPC. Successfully
submitted frames are never queued again. Counters advance on actual submissions,
not queue admission.

Disconnect/reset discards pending output. Relay session and directory changes
clear the affected output queues. The single-thread switch clears pending output
on rules replacement. Queues belong to connection objects, so reconnecting under
the same port name does not inherit old output.

## Statistics

Each new ring exports `packets`, `bytes`, `capacity`, `byte_limit`, `max_age_ms`,
`high_water`, `enqueued`, `sent`, `eagain`, `capacity_drops`, `expired`, `discarded`
and `error_drops`. The `eagain` counter counts temporary send failures, including
`EINTR`. Prefixes are:

- `switch_ipc_retry_` on ordinary clients;
- `relay_upstream_retry_` and `relay_channel_<id>_retry_` in a relay;
- `port_<name>_retry_` on the single-thread switch;
- `port_<name>_relay_ack_retry_` on the MP switch.

Existing producer backpressure-drop counters now count actual capacity/expiry
losses, not temporary failures that were queued successfully. Relay also exports
aggregate `relay_ipc_tx_frames`, `relay_ipc_tx_bytes`, `relay_ipc_drops` and
`relay_ipc_backpressure_drops`. Per-connection counters disappear when that
connection is removed; aggregate producer counters persist.

This change does not change AF_UNIX socket buffer sizes or system sysctls.

## Validation

`ipc_retry_queue_test` covers owned copies, FIFO, wrap, slot/byte bounds, expiry,
retry gating and permanent errors. `switch_v2_test` fills a real SEQPACKET socket
and verifies deferred mmap batch delivery and slot reclamation.
`switch_client_test` checks POLLOUT arming/disarming and cleanup.
`ipc_retry_integration_test` injects transient send failures into disposable
switch/tunnel children and checks successful retries over both ST and MP relay
paths. No VM, TUN device or installed service is required.
