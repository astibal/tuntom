# Socket batching and mmap reference batching

This isolated two-process benchmark follows [the single-frame experiment](../ipc_mmap/README.md).
It uses the existing v1 frame encoder/decoder, one nonblocking AF_UNIX
SOCK_SEQPACKET connection, and the draft's mmap slots and 40-byte references.
It changes no production code or running service. The older benchmark and its
measurements remain unchanged.

| Mode | Socket payload | A / B calls | Frames per socket record |
|---|---|---|---|
| `inline` | Complete v1 frame, header + payload iovecs | sendmsg / recvmsg | 1 |
| `mmap` | 40-byte slot reference | sendmsg / recvmsg | 1 |
| `inline-mmsg` | Complete v1 frames | sendmmsg / recvmmsg | 1 |
| `mmap-mmsg` | Separate 40-byte slot references | sendmmsg / recvmmsg | 1 |
| `mmap-batch` | One record with several slot references | sendmsg / recvmsg | up to batch size |

```text
mmap-mmsg, batch 4:
 A --one syscall--> [ref1] [ref2] [ref3] [ref4] --one or more syscalls--> B
                    four separately queued socket records

mmap-batch, batch 4:
 A --one syscall--> [batch: ref1 ref2 ref3 ref4] --one syscall--> B
                    one socket record, four independent mmap slots
```

The second mode reduces both syscalls and socket records. It still copies
A's private payload into shared memory and copies that memory into B's private
receive buffer. It is not zero-copy. An FD is transferred only during setup;
there are no per-frame acknowledgments, extra eventfds or shared descriptor rings.

## Running

```bash
g++ -std=c++17 -O3 -march=native -mtune=native -Wall -Wextra -Wconversion -pedantic \
  experiments/ipc_batch/bench.cpp -o /tmp/tuntom-ipc-batch-bench
python3 experiments/ipc_batch/run.py --binary /tmp/tuntom-ipc-batch-bench \
  --output /tmp/tuntom-ipc-batch-results --cpus 2,3
```

Defaults: mixed and 9000-byte payloads; 25k, 300k and saturated pps; single-frame
baselines plus each batching mode at 4, 8 and 16; three repeats of two seconds,
with 200ms warmup each. Variant order is reproducibly shuffled within each
profile/rate/repeat. The runner refuses to overwrite existing measurements.
It records raw results, medians, ranges, CPU/cache topology and source/binary
hashes. CPU IDs must be allowed and on distinct physical cores. Only the two
benchmark processes are pinned. The machine is not otherwise isolated.

Useful options: `--profiles mixed`, `--rates 300000,0`, `--variants
mmap:1,mmap-mmsg:8,mmap-batch:4,mmap-batch:8,mmap-batch:16`, `--slots 128`,
`--duration 2`, `--repeats 3`. `--sndbuf` is intended for correctness probes;
normal comparisons use the unchanged default socket buffer sizes.

The binary accepts positional arguments:

```text
bench mode profile seconds pps cpuA cpuB slots batch [sndbuf]
```

`pps=0` means saturation. Numeric payloads up to 65535 are accepted. Single
modes require `batch=1`; other modes permit 1..16. A one-slot pool exercises
ordered inline fallback. Short probes may deliberately use the same CPU for
both processes and a 4096-byte requested send buffer to force backpressure.

## Slot ownership and partial sends

Each mapped frame has its own slot and generation token, even in a batch:

```text
FREE(even) -> A copies bytes -> A publishes READY(odd)
           -> A sends reference -> B receives reference
           -> B checks token, copies bytes -> B publishes FREE(next even)
```

A release-store publishes the bytes; B uses an acquire-load before copying.
B release-frees each slot only after its last payload read. A never reuses a
READY slot. Slot addresses are calculated from the receiver's own mapping
base; no raw process pointers cross the socket.

For nonblocking sendmmsg, a positive return value is the number of records
accepted from the beginning of the array. Only that prefix transfers ownership.
The test retries the unsent suffix, keeping its payload buffers and slots
alive. It never resends an accepted reference. See
[sendmmsg(2)](https://man7.org/linux/man-pages/man2/sendmmsg.2.html).

A grouped reference record is accepted whole or returns EAGAIN. On EAGAIN the
whole unsent group remains owned by A until retry succeeds. Unlike the earlier
single-frame experiment, this test retains unsent slots during retry instead
of freeing and preparing them again. On a fatal error the test terminates its
own processes and destroys their mappings.

No free slot means inline fallback. For mmsg that is another ordinary record
in the same ordered array. For grouped references, an inline frame splits the
group: send preceding references, then inline, then following references.
Receiver validation checks exact sequence, so fallback cannot silently reorder.

## Test-only batch wire format

This is an experiment, not a finalized or negotiated addition to the draft.
All integers in the socket record are network byte order:

```text
TTX, version=1, type=17, flags=0, record_length:u16  (8 bytes)
epoch:u64, pool_id:u32, count:u32                   (16 bytes)
count * { slot_id:u32, token:u64, frame_length:u32 } (16 bytes each)
```

With eight references the record has 152 bytes, compared with eight separate
40-byte records (320 bytes). The eight complete frames stay in eight mapped
slots. B validates the record size/count and each slot, copies and processes
the frames in record order, then frees the respective slots. Production use
would require capability negotiation and complete untrusted-peer validation.
The benchmark's simplified setup/BEGIN/END protocol is not production registration.

## Measurement boundaries and limits

- Both paths retain two payload copies per hop. All modes share the same
  preparation and validation code in this new benchmark; the single-frame
  baselines are remeasured, not copied from the earlier report.
- A rate limit models arrivals on an absolute monotonic timeline. After waking,
  A submits at most the requested batch size from frames already due. It never
  waits for more frames to fill a batch. Normal OS timer coalescing can make
  several frames due together. The measured average send/receive batch sizes
  reveal this; a configured maximum of 16 does not imply actual batches of 16.
- At saturation all source frames are immediately available. Preparing a batch
  delays its first frame by the time needed to prepare the remaining frames.
- Nonblocking recvmmsg returns currently available records, up to its limit;
  it does not wait for a full receive batch. Empty/full sockets use poll, without
  spinning. See [recvmmsg(2)](https://man7.org/linux/man-pages/man2/recvmmsg.2.html).
- CPU is A+B user/system getrusage time, including pacing and backpressure,
  excluding allocation, mapping, warmup, final formatting and latency sorting.
- Latency runs from just before preparing a frame through B's copy and header
  decode, including preparation of later batch members and send retries.
  It excludes deliberate pacing sleep and lateness against the arrival schedule.
  Every 67th frame is sampled: 67 is coprime to 4/8/16, avoiding the bias of
  measuring only the last frame of every batch. This differs from the earlier
  benchmark's stride of 64. It measures one-way IPC, not tunnel RTT.
- Each frame checks its sequence, opcode, label, length and last payload byte.
  Every 1024th measured frame and every warmup frame checks the full pattern.
  Sent/received frames, socket records and mmap counts must match exactly.
- `send_calls`/`recv_calls` include unsuccessful attempts; receive also counts
  the END marker. `*_success_calls` let us calculate the actual frames per
  successful syscall. `*_records` count only data records, and `*_max_batch`
  counts frames handled per successful call. The END contribution is negligible.
- Kernel/user CPU is accounting, not a function-level profile. Fewer calls are
  measured directly, but their contribution cannot be separated from socket
  allocation, copies, synchronization, queuing and wakeups using these counters.
- Saturation results compare different achieved rates; compare the same offered
  rate to interpret latency. The 128-slot pool bounds mapped outstanding frames,
  but inline fallback means it is not a bound on all queued frames.
- One hop, two endpoints, one destination. No label lookup, encryption, TUN,
  RX/TX worker matrix, multi-destination fairness or dynamic scheduler is tested.
  Cache-hot repeated sources and the placement of cores matter. These pps
  numbers are not claims about whole-switch or encrypted-tunnel capacity.

For the short correctness suite (including forced partial sends), run:

```bash
python3 experiments/ipc_batch/probes.py --binary /tmp/tuntom-ipc-batch-bench \
  --output /tmp/tuntom-ipc-batch-probes.json --cpus 2,3
```
