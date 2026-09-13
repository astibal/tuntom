# Ascon worker mock benchmark

Standalone experiment using the **current production `ProtocolV5` and
`Ascon-AEAD128`**, with an in-memory source and sink. It does not change the tunnel,
cryptographic algorithm, or wire format.

Measured on Ryzen 7 3700X: [results and interpretation (Czech)](RESULTS_CZ.md).

```text
workers=0: source copy -> real codec -> sink                 [one CPU]

workers=N: source copy -> N bounded SPSC request queues
                             |           |           |
                          Ascon       Ascon       Ascon    [N CPUs]
                             |           |           |
           ordered sink <- N bounded SPSC completion queues [same source CPU]
```

One coordinator prepares input, allocates TX sequences, distributes batches in
round-robin order, and consumes completions in submission order. Each worker has
private scratch storage. All threads share a read-only codec whose lifetime spans
all jobs. Packet/output buffers belong to a job until ordered completion, then
are reused. The total outstanding capacity stays at **256 packets**, independent
of worker count and batch size. `--batch 8` publishes one pointer per eight
packets; it still runs the scalar production codec once per packet, without SIMD.
The last partial batch is supported.

Every worker and the coordinator are pinned to distinct selected logical CPUs.
The runner defaults to one logical CPU per physical core. Workers **busy-poll**;
the experiment represents saturated operation with dedicated CPU capacity.
`workers=4` uses **five CPUs total**. It does not measure low-load sleeping/wakeup
overhead or power efficiency. Affinity does not isolate CPUs from other programs.

## Build and check

```bash
bash experiments/ascon_workers/build.sh
python3 -B experiments/ascon_workers/check.py
```

The checker compares worker outputs with direct execution across Ascon block
boundaries, validates full decrypted payloads and sequences, checks every TX
packet with the peer codec, and exercises repeated slot reuse, non-power-of-two
queues, one-slot pressure, partial batches, damaged authentication tags, and
delayed workers. These verification runs are separate from performance results.
Thread counts requiring unavailable physical cores are skipped and reported.

Optional instrumentation:

```bash
g++ -std=c++17 -pthread -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer experiments/ascon_workers/main.cpp \
  -o /tmp/tuntom-ascon-workers-asan
python3 -B experiments/ascon_workers/check.py --binary /tmp/tuntom-ascon-workers-asan

g++ -std=c++17 -pthread -O1 -g -fsanitize=thread \
  experiments/ascon_workers/main.cpp -o /tmp/tuntom-ascon-workers-tsan
/tmp/tuntom-ascon-workers-tsan --mode mixed --workers 2 --cpus 0,1,2 \
  --batch 8 --packets 10003 --verify --tamper --delay-us 50
```

Adjust the example CPU list to your allowed CPUs.
If LeakSanitizer reports that it cannot run under ptrace, use
`ASAN_OPTIONS=detect_leaks=0 python3 -B experiments/ascon_workers/check.py --binary /tmp/tuntom-ascon-workers-asan`.
This retains address/undefined-behavior checking but does not check leaks.

## Run

```bash
python3 -B experiments/ascon_workers/bench.py \
  --output /tmp/ascon-workers.json --seconds .5 --repeat 3
```

Defaults: TX, RX, and mixed; payloads 64/512/1400 B; direct execution and 1/2/4
workers, with worker batches of 1/8. A 16,384-packet pilot determines the packet
count for each configuration; pilots are excluded from summaries. Every process
warms up with 4,096 packets, then measures a fixed packet count including drain.
Configuration order is shuffled within each repeat using a recorded seed.
Results are saved after each run; only a finished matrix receives `complete=true`
and a Markdown report. Existing output paths are rejected. A changed source or
binary hash prevents a successful final report.

Useful variations:

```bash
# Coordinator/copy/queue overhead without cryptography (two payload copies).
python3 -B experiments/ascon_workers/bench.py --modes copy \
  --sizes 64,1400 --output /tmp/ascon-workers-copy.json

# Smaller queue for latency/throughput tradeoffs; CPUs are coordinator, workers.
python3 -B experiments/ascon_workers/bench.py --slots 32 --cpus 1,2,3,4,5 \
  --sizes 1400 --modes mixed --output /tmp/ascon-workers-small-window.json
```

The JSON records raw runs, pilots, source/binary hashes, git revision, compiler,
host/CPU metadata, CPU placement, and median summaries with throughput ranges.

## Workloads and interpretation

| Mode | Work per job |
|---|---|
| `tx` | Copy synthetic plaintext into a job; assign unique TX sequence; real V5 encode/AEAD; touch output byte |
| `rx` | Copy a prepared datagram into a job; real V5 decode/tag verification; touch plaintext byte |
| `mixed` | Alternate TX/RX jobs, sharing workers; one crypto operation per job |
| `copy` | Copy source into job, copy job into output, touch output byte; no crypto |

- **Gbit/s** counts payload bytes processed, excluding the V5 header/tag. Mixed
  throughput is aggregate across both directions, not throughput in each direction.
- **CPU cores** is process CPU time divided by elapsed time, including the
  coordinator and worker spinning. CPU ns/packet measures total CPU cost, not latency.
- **p50/p99** sample input preparation through ordered completion, including
  front-of-queue and ordering waits. One in 256 packets is sampled (257 in mixed
  mode to alternate sampled directions). This is saturated processing latency,
  not network RTT; queues are intentionally allowed to fill.
- Completion ordering uses a single global job order, including mixed TX/RX.
  This is stricter than independent per-direction orders and may lose throughput
  to head-of-line blocking. `out_of_order_observed` counts inversions as completion
  queues are polled, not the physical timing of worker completion.
- TX uses synthetic public keys and monotonically allocated sequences within
  each process. RX repeatedly authenticates a bounded, pre-generated corpus with
  repeated sequences. **No replay acceptance is modeled.** Do not reuse this input
  generator as a tunnel or a production cryptographic API.
- Source/ciphertext generation, buffer reservation, thread startup, warmup, and
  final percentile sorting occur outside the measured interval. Input copies,
  codec work, request/completion queues, ordering, lightweight checks, and drain
  are inside it. Performance runs do not re-decrypt TX outputs or compare complete
  payloads; `--verify` correctness runs do.

There is **no TUN, UDP, switch IPC, network loss, replay window, fragmentation,
reassembly, PFS handshake, key rotation, or kernel copy** in the measured path.
Buffers/corpus are reused and may be cache-resident. Payloads up to 65535 B are
accepted for codec stress, without claiming a valid network datagram of that size.
This benchmark tests whether parallel scalar Ascon can repay queue and ordering
costs; it does not predict deployed tunnel throughput directly.
