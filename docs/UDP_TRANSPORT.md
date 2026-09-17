# UDP socket buffers and local send backpressure

Each tunnel requests **2 MiB effective send and receive buffers** by default:

```sh
tuntom client 42 tc0 192.0.2.1 \
    --udp-send-buffer 2097152 --udp-receive-buffer 2097152
```

The options are byte counts in the range `0..67108864` (64 MiB). Zero leaves the
OS default unchanged. These settings affect only this tunnel's UDP socket; no
sysctl or other socket is changed. Linux doubles the values supplied to
`SO_SNDBUF`/`SO_RCVBUF` for accounting. Tuntom compensates for this, so the CLI
requests the **effective** limit shown by `getsockopt` and `ss -m`. Kernel minima
and `net.core.wmem_max` / `net.core.rmem_max` can change the result. Check:

```text
udp_send_buffer_requested=2097152
udp_receive_buffer_requested=2097152
udp_send_buffer_actual=2097152
udp_receive_buffer_actual=2097152
```

These are capacity limits, not a promise of immediate resident allocation.
They are independent of `--mtu` (inner packet limit) and `--transport-mtu`
(the initial PMTUD target). Changing buffers does not change either MTU.

## Deferred DATA/IPC datagrams

On local `EAGAIN`/`EWOULDBLOCK`, a tunnel retains the **unsent suffix** of a DATA
or relay IPC batch in a FIFO ring. `EINTR` can defer the suffix as well. Successfully
sent datagrams are never repeated. The ring owns copies of the exact encoded
bytes, including their original authentication tag, sequence number and nonce;
a retry never re-encrypts the packet. New DATA/IPC cannot overtake queued data.

Limits are intentionally fixed and small:

- 64 datagrams;
- 256 KiB of queued wire bytes;
- 100 ms maximum age, measured with the monotonic clock;
- at most 64 send attempts per writable event.

Slots reuse their vector storage; retained allocation can exceed the current
queued byte count, but is bounded by 64 datagram-sized slots. There is no
unbounded packet list. The complete unsent suffix must fit, otherwise it is
rejected and counted. Already sent fragments cannot be recalled. Large relay
records may exceed the ring's slot limit when fragmented over a small MTU.

`POLLOUT` is enabled only while there is queued data. A ready-but-`EAGAIN` result
adds a 1 ms retry gate, preventing a tight writable loop. TUN and established
switch input are paused while the queue is nonempty; UDP reception, control and
switch output continue. Relay IPC still needs registration and lease processing,
so it continues to be serviced and rejects excess data when the ring is full.
Handshake, keepalive, RTT and PMTUD traffic keep their immediate-send semantics.

Queued data is discarded on a transmit-session change (including rollback),
loss of session readiness, or an authenticated peer change. Expired packets are
never retried. Permanent send errors drop the failed datagram; `EMSGSIZE` with
PMTUD also clears the remaining queue and restarts discovery. Queueing cannot
recover packets already accepted by UDP and lost downstream, and it does not
provide reliability or congestion control for the tunnel.

`show stats` includes `udp_tx_queue_packets`, `udp_tx_queue_bytes`, limits,
`high_water`, `enqueued`, `sent`, `eagain`, `capacity_drops`, `expired`,
`discarded` and `error_drops` (all prefixed with `udp_tx_queue_`). Packet/byte
send counters advance only when the kernel accepts the datagram. Deferred local
backpressure is counted by the queue, rather than as `udp_send_errors`; terminal
send errors still increment `udp_send_errors`. Processing-time samples cover
immediately completed sends, not queue residence time.

Larger buffers and this short retry window absorb transient scheduling gaps.
Persistent overload still causes bounded drops. Monitor the next stage as well:
removing UDP drops can expose IPC/switch backpressure or receive-side limits.
