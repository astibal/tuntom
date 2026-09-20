# One-hop socket payload versus mmap payload

Two processes, A and B, compare the two payload paths from the
[mmap proposal](../../docs/SWITCH_MMAP_EXTENSION_DRAFT.md). This is an isolated
experiment, not an implementation of the protocol or a change to a live switch.

```text
inline: A header + payload --sendmsg--> socket --recvmsg--> B private buffer

mmap:   A header + payload --memcpy--> shared slot
        A --sendmsg(40-byte reference)--> socket --recvmsg--> B
        B shared slot --memcpy--> B private buffer --release--> free slot
```

Both paths therefore retain two payload copies per hop. Mmap replaces the
kernel-mediated copies with user-space copies. Every frame still causes a
socket send and receive; this benchmark does not claim to measure a zero-copy
transport or a shared descriptor ring. The inline producer uses the same v1
header encoder and two-iovec sendmsg pattern as SwitchClient. Both receivers
use recvmsg, as needed by the proposed extended transport.

## How to run

```bash
g++ -std=c++17 -O3 -march=native -mtune=native -Wall -Wextra -Wconversion -pedantic \
  experiments/ipc_mmap/bench.cpp -o /tmp/tuntom-ipc-mmap-bench
python3 experiments/ipc_mmap/run.py --binary /tmp/tuntom-ipc-mmap-bench \
  --output /tmp/tuntom-ipc-mmap-results --cpus 2,3
```

Choose two allowed logical CPU IDs belonging to different physical cores.
The runner records their topology, shared caches, kernel, cgroup and hashes.
Defaults are four payload profiles (64, 1500, 9000 and mixed), 25k frames/s and
saturation, two seconds per trial and three alternating inline/mmap pairs.
Mixed means two 9000-byte payloads followed by one 64-byte payload, about
1.2 Gbit/s at 25k frames/s. Every payload also has a 16-byte v1 label header.

The process-local CPU affinity is set only for the two test processes. They
use socketpair and memfd; no root, named sockets, TUN, network namespaces,
mount changes or service restart is required. Host sandbox policy may still
require permission to run local socket operations outside the sandbox.

## What the mmap code is doing

After fork, A creates a memfd and sizes it. A maps it and sends its FD to B
through SCM_RIGHTS. B independently maps the same object; it does not inherit
an existing mapping from A. Closing the FD after mmap does not destroy either
mapping. Their destructors call munmap after the benchmark.

There is one producer-owned pool for this one-way test. The default is 128
slots, each able to hold the existing maximum wire frame (65607 bytes). Slot
starts are 64-byte aligned, and payload begins 64 bytes after its ownership
word. Slot i is found relative to each process's own mapping base.

The 64-bit ownership token alternates even FREE and odd READY generations:

```text
FREE(0) -> A writes -> READY(1) -> B copies -> FREE(2) -> ...
```

A release-publishes the token before sending a reference. B reads bytes only
after receiving that reference and acquire-checking its token. B returns the
slot only after copying its bytes. Atomic operations use the benchmark's
verified Linux little-endian, lock-free 64-bit ABI. A failed nonblocking send
returns the unsubmitted slot. If every slot is busy, A sends inline instead;
the resulting mmap percentage is recorded so mixed results are visible.

The reference has the proposal's exact 40-byte TTX record fields. The one-time
setup and BEGIN/END markers are a smaller test-only protocol, not the proposed
HELLO/registration state machine. This test uses one new mapping/epoch per
connection and does not test production negotiation or hostile-peer handling.

## Measurement boundaries

- Each trial has a 200ms saturation warmup, including full payload validation,
  outside timing. A start marker drains warmup frames before counters reset.
  Memory allocation, prefaulting, FD transfer and mmap are outside timing.
- Both sockets are nonblocking. Empty/full sockets wait with poll. There is
  no busy-spin, extra eventfd, extra worker or acknowledgment per data frame.
- The 25k mode uses absolute CLOCK_MONOTONIC sleeps before individual frames.
  Normal OS timer coalescing can produce small bursts. Pacing overhead is
  included in A's CPU in both variants; achieved pps is recorded. Saturation
  retries EAGAIN and measures achievable throughput, not a requested rate.
- CPU is getrusage(RUSAGE_SELF) user+system time for A and B, sampled at the
  boundaries only. CPU/frame includes sender backpressure and receiver drain.
  It excludes final result formatting and sorting the latency samples.
- One-way latency is sampled every 64th frame, from just before preparing its
  first send attempt until B has its private frame and decodes its header.
  It includes send retry, socket queuing, copies and receiver scheduling, but
  excludes the producer's deliberate pacing sleep. It is not tunnel RTT.
- Every frame checks sequence, label, opcode, size and a payload byte. Every
  1024th measured frame checks the entire fixed payload pattern; warmup checks
  every byte. No gaps/reordering/loss is permitted. No compiler zero-copy
  shortcut or bypass of B's private buffer is intentional.
- Sender syscall attempts, socket bytes, mmap share, pool-full fallback, polls
  and voluntary/involuntary context switches are recorded. recv_calls includes
  empty attempts and the single end marker. These are instrumented call counts,
  not a profiler trace of all kernel work.

The host is not isolated: a live tuntom workload and desktop activity can
compete for the same cores. Alternate trial order and inspect repeat ranges.
Within one run, placement and socket buffer settings stay identical across
variants. Results describe one hop and cannot be directly subtracted from the
live four-worker switch's CPU or interpreted as encrypted tunnel capacity.

Additional correctness probes can use `mmap mixed 0.3 0 2 3 1` (slot exhaustion
and ordered inline fallback) or payload 65535 in either mode (frame boundary).
These short probes are not performance conclusions.

The follow-up [batching experiment](../ipc_batch/README.md) compares the same
single-frame paths with sendmmsg/recvmmsg and one socket record containing
several mmap references. It keeps this benchmark's source and results intact.
