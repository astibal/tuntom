# Experimental RX/TX switch

This is a throughput draft for the design discussed on 2026-09-11. It is a
separate binary, not a deployment change or replacement for the production
switch. `mk_switch.sh`, production sources and existing tests are unchanged.

```text
RX A -> pool A -> recv -> v1 decode -> route lookup -> rewrite label/opcode
                                                  |
                             SPSC[A][destination] stores Buffer*
                                                  |
TX destination <- RR across RX queues, quota 1 <---+
      |
      +-> one send() / one SOCK_SEQPACKET record
      +-> EAGAIN: retain buffer and wait for POLLOUT
      +-> sent/drop: buffer.used.store(false, release)
```

## Ownership and scheduling

- One RX and one TX thread per registered port. An 11-port benchmark has 22
  packet workers plus the control/registration thread. `--worker-cpus 0,1,2,3`
  pins RX port i to CPU `(2*i)%4` and TX to `(2*i+1)%4` in that list. Port order
  is the first occurrence in CLI routes/ports; benchmark adapter is port 0.
- Each RX has an array of 128 buffers by default, with atomic FREE/USED state
  first in each slot. RX searches circularly; only that RX claims buffers,
  and final TX owners release them. No return queue or allocator mutex.
- Each ordered RX/TX pair has a bounded SPSC pointer ring (128 usable entries
  by default). Producer and consumer indices are separate cache lines. Data
  publication and slot reuse use acquire/release; pointer slots are ordinary
  pointers. There is no CAS in the SPSC or pool acquisition path.
- TX advances its RR cursor after each pop, skips empty queues, and retains
  the selected buffer on EAGAIN. There is no multi-frame IPC or sendmmsg.
- **Sleeping is additional synchronization.** Default `--idle sleep` uses an
  acq_rel exchange on the target's sleep flag per successful publication and
  a futex wake only when armed. TX arms, rechecks queues, then futex-waits.
  This prevents the publication/sleep lost-wakeup race. The timeout is 20 ms
  for bounded shutdown. `wake_calls` counts actual wake syscalls, not exchanges.
  `--idle spin` is an explicitly CPU-expensive diagnostic alternative.
- RX retains no empty reserved buffer while waiting for input. On pool
  exhaustion it pauses for 50 us and retries, so pressure propagates to the
  input socket. Queue-full frames are dropped and counted. A slow output can
  therefore affect other destinations fed from that same RX pool.
- `--backpressure drop` is a diagnostic mode for separating retry buffering
  from parallelism. The default retry behavior differs from the current
  production switch's immediate drop on output EAGAIN.
- Snapshot counters have one writer each and use relaxed atomic load/store
  of local totals. Snapshot reads can be temporally inconsistent under load;
  final benchmark accounting is checked only after drain/shutdown. Per-worker
  CPU is updated at idle/block points and at thread exit, not every packet.

## Which tuntom work is preserved

The draft includes the current `src/ipc/switch_protocol.hpp` and uses its
registration and frame decoder. The benchmark producers use the real
`SwitchClient::send_frame`: one scatter/gather sendmsg of header and payload
per IPC record. The one-label header is 16 bytes. All bytes arrive into a pool
buffer via recv, and only the validated header is rewritten in place.

The string+64-bit-label route key, hash combination, reused RX lookup key,
linear output-name search, exit-port membership check, label-stack preservation
and default-back opcode match `src/switch/main.cpp`. This draft does not hide
a precompiled numeric route shortcut inside the parallel variant.

In `Tunnel::deliver_received_data`, authenticated and **reassembled** packets
are passed to IPC. The IPC benchmark's 9000-byte frame therefore represents a
whole inner packet, not one encrypted UDP fragment. The driver does not run
encryption, PFS, UDP, TUN, reverse-flow learning, NAT or Linux IP forwarding.
`check.py` separately verifies compatibility through real tuntom instances,
including PFS, encrypted UDP and bidirectional fragmentation/reassembly.

Source buffer ownership and kernel IPC copies are unchanged by passing
pointers inside the switch: this is not a claim of end-to-end zero-copy.

## Intentional draft limitations

The topology is inferred from `--route`, `--exit-port` and explicit `--port`.
All expected ports must register before workers start (15-second startup
deadline). Unknown or duplicate registrations are rejected. FDs and pools are
kept alive until the workers have joined; the experiment does **not** implement
production reconnect/replacement, dynamic routes, admission/FD policies,
allocation recovery or deployment lifecycle. Do not run it as a live service.

Buffer capacity is the current switch's maximum: 65535 payload bytes plus
the largest v1 header. Smaller default pools and pointer rings are bounded;
their RAM is different from the production switch's single receive buffer.

## Build and verify

From the repository root:

```bash
bash experiments/switch_mt/build.sh
/tmp/tuntom-switch-mt-queues-test
g++ -std=c++17 -pthread -O1 -g -fsanitize=thread \
    experiments/switch_mt/queues_test.cpp -o /tmp/tuntom-switch-mt-queues-tsan
/tmp/tuntom-switch-mt-queues-tsan
python3 experiments/switch_mt/check.py
```

Queue tests cover exact capacity, wraparound, 90000 cross-thread transfers,
payload integrity, per-pair FIFO, pool reuse, wakeups and complete reclamation.
Process tests cover full payloads through 65535 B, 1/8 labels, EXIT and
default-back, malformed frames, bounded overflow, blocked-output retry,
FIFO after pressure, recovery and final pool reclamation. They also run the
existing `tests/switch_tunnel_test.py` with current tuntom sources.

## Benchmark

```bash
python3 experiments/switch_mt/bench.py \
    --output /tmp/switch-mt-results.json --duration 4 --repeat 3 \
    --case mix:mixed:200000 --case mix:mixed:240000 \
    --case mix:9000:0 --case hub:9000:0 \
    --case mix:1500:0 --case mix:64:0 \
    --variants reference,mt2,mt4
```

Eight available physical cores are required for this documented layout:
reference on core 0, draft on the first 1/2/4 cores; source shards on cores
4,5 and sink shards on 6,7. These are positions in the available physical-core
list, not necessarily OS CPU IDs. SMT siblings are not reserved or disabled.
Affinity does not isolate the experiment from other host work.

For the hub-only affinity experiment, add `--placement adapter-dedicated`
and use `--variants reference,mt4`. Adapter RX/TX get cores 0/1 exclusively
among packet workers; all other RXs share core 2 and TXs share core 3. The
control thread is still allowed across all four cores. This uses the draft's
existing per-port CPU-list option, with no different forwarding code.

- `mix`: ten tunnel ports in five pairs plus one adapter. A tunnel offers
  80% to its partner and 20% to the adapter; the adapter has twice each
  tunnel's offered rate. At paced load this is 2/3 tunnel-to-tunnel, 1/3 via
  adapter. `hub`: every tunnel targets the adapter, whose offered rate is ten
  times a tunnel's. Source/sink shards assign whole ports by weighted load.
- `mixed`: two 9000-byte messages per 64-byte message, plus 16 bytes IPC header.
- PPS>0 is total requested source rate; each source independently paces its
  share. One scheduling round sends the assigned weighted port list. Actual
  offered/accepted/delivered rates must be inspected; OS sleeps can create
  bursts, and a limited generator may fail to reach the requested rate.
- PPS=0 is offered saturation. Default `--source-saturation poll` stops
  probing a full source socket until POLLOUT. It prevents a busy producer
  from flooding the socket lock with failed sends. `flood` reproduces the
  deliberately hostile initial probe. Saturation is not a fixed offered mix:
  individual ports advance at the rate backpressure permits.
- Each accepted payload is checked for destination, label, opcode, length,
  sequence and head/tail identity. `--verify-all` also scans the entire body,
  for correctness smoke runs only. It must not be mixed into performance A/B.
- Latency is recorded from immediately before the source send, sampled on
  every 32nd sequence, and includes both kernel socket queues, scheduling,
  the switch and any draft queueing. Sources run for the requested duration;
  receivers drain for another 500 ms and then until their sockets are empty.
  PPS uses source elapsed time, so startup/backlog drain can slightly raise
  it over sustained delivery during that window. CPU includes the drain.
- Every result checks source accepted = switch RX, sink received = switch TX,
  accepted minus received = explicitly counted switch drops, zero malformed
  or reordered frames, and zero remaining pool buffers. Input EAGAIN loss is
  separately counted. Source/sink CPU and per-port counts expose load-driver
  limitations; a saturation result is a whole IPC-path result, not proof of
  the isolated switch ceiling.
- A/B order alternates by repetition. Results include commands, core mapping,
  process and worker CPU, sampled runqueue/context-switch metrics, RSS, host
  CPU/load, binary/source hashes and the current git status. Short test runs
  are exploratory, not a latency or capacity guarantee for real tunnels.

## Port-count experiment (6 / 8 / 10 total ports)

`port_count_bench.py` uses the same switch binaries and supports a variable
number of total bidirectional ports. The driver accepts optional trailing
`PORTS pairs|hub` after its legacy arguments; the original 11-port invocation
is retained. `pairs` routes every port to `port ^ 1`, with equal offered rates.
`hub` has one adapter and PORTS-1 tunnels; paced aggregate rates are 50:50 in
the two directions. The port count includes the adapter. Saturation is still
backpressure-dependent. Correctness checks verify the runtime destination,
rewritten label, opcode, sequence, lengths, payload identities and drain counts.

```bash
python3 experiments/switch_mt/port_count_bench.py \
    --output /tmp/port-count.json --ports 6,8,10 --duration 4 --repeat 3
```

The documented host has **8 physical cores / 16 logical CPUs**, not 16 physical
cores. Each draft instance has 2*PORTS packet workers plus a control thread.
The same two source and two sink threads run on the same host in every case:
source CPU12,13 and sink CPU14,15. Their CPU and sampled scheduling waits are
recorded separately. These are not measurements with one dedicated physical
core per packet worker.

- `--layouts all16` (default): workers cycle across CPU0-15. At six ports the
  packet workers use CPU0-11; at eight they use all16; at ten the first four
  logical CPUs have two workers each. At eight and ten ports, the driver
  shares logical CPUs with some switch workers. At all port counts it shares
  physical cores through SMT. Total packet+driver workers are 16/20/24.
- `--layouts reserved12`: packet workers cycle across CPU0-11 and never share a
  logical CPU with the driver. Physical SMT sharing remains. This control
  distinguishes more packet workers from also placing workers onto the four
  driver's logical CPUs. It holds the same switch CPU budget for all counts.
- `reference` uses one CPU; `mt` uses per-TX futex sleeping; `mt-spin` polls.
  Spin still waits on POLLOUT for blocked outputs and sleeps on full RX pools.
- Default cases: 64B and 9000B pair saturation; 9000B pairs and hub each at
  **160kpps total offered**, held constant as the port count changes.
- Three repeats rotate port/variant order. Metadata includes topology, commands,
  source/binary hashes and host load. JSON includes per-port delivery and
  direction breakdown for the hub. Summary values are median and full range.
- Sampled runqueue wait sums time runnable workers waited for CPU; it is not
  time blocked in futex/poll. Sampling is every100ms and misses up to the last
  interval of a terminating thread. CPU includes drain; PPS uses source time.
- Per-RX pool and per-pair ring capacities stay128, so total buffer capacity
  grows with port count. New runs do not implement thread-local route snapshots
  or change the switch's wakeup handshake.
