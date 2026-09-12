# Production-target IPC comparison

This runner compares the new `tomtom-switch-mp` target with the existing
`tuntom-switch`. It reuses `switch_adapters/load.cpp`, which sends frames through
the real `SwitchClient` and checks opcode, label, size, destination and sequence.
No TUN, encrypted UDP or NIC work is included.

```bash
g++ -std=c++17 -pthread -O3 -march=native -mtune=native \
  src/switch_mp/main.cpp -o /tmp/tomtom-switch-mp-opt
g++ -std=c++17 -pthread -O3 -march=native -mtune=native \
  src/switch/main.cpp -o /tmp/tomtom-mp-reference
g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Isrc \
  experiments/switch_adapters/load.cpp -o /tmp/tomtom-mp-load
python3 experiments/switch_mp/bench.py \
  --switch /tmp/tomtom-switch-mp-opt --reference /tmp/tomtom-mp-reference \
  --driver /tmp/tomtom-mp-load --output /tmp/switch-mp-results.json
```

Default: eight tunnels, four adapters, equal full-duplex traffic, 9000-byte
payloads, 160k and 240k offered forwarded frames/s, three alternating paired
3-second trials. One offered frame represents one switch forward, not separate
RX and TX packets. Both switch variants have the same affinity of four physical
cores; two sender and two receiver threads use four other physical cores. MP
uses its normal topology policy, and the initial assignment is recorded before
traffic. At this topology it activates RX/TX/RXa/TXa on four workers.

Source `EAGAIN` is counted separately from switch output drops. The driver
does not retry that frame, matching its earlier benchmark behavior. Offered
loss therefore includes admission/backpressure at the sending socket, whereas
`switch_loss_percent` only considers frames accepted by the switch. A 500 ms
drain precedes reconciliation of driver and switch counters. CPU totals include
that drain; dividing by the send interval is an approximate occupied-core count.
P99 is sampled every 32nd source sequence and includes IPC queues and peer
scheduling. MP deliberately retains pending TX frames where the old switch
drops on output EAGAIN, so compare latency and loss alongside throughput.

The runner records commands, initial/final stats and source/binary SHA-256s.
Do not run compilers, sanitizers or other benchmarks concurrently when collecting
comparison data. This is a short integration benchmark, not an isolated hardware
capacity result or evidence of zero loss under arbitrary traffic.

## Extended stress suite

`stress.py` and the separate `load.cpp` cover 10, 12, 16 and 20 tunnel ports
with one to three adapters, including uneven fanout such as 20/3. These tests
use real AF_UNIX/SOCK_SEQPACKET IPC and the production `SwitchClient`; adapters
are IPC peers, not real TUN devices. Each accepted frame carries a source,
destination, sequence and timestamp. Receivers validate the label/opcode,
payload endpoints and per-source/destination ordering; correctness cases also
validate every payload byte.

```bash
g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Isrc \
  experiments/switch_mp/load.cpp -o /tmp/tomtom-mp-stress-load
python3 experiments/switch_mp/stress.py --phase correctness \
  --switch /tmp/tomtom-switch-mp-opt --driver /tmp/tomtom-mp-stress-load \
  --variant native --output /tmp/mp-stress-results
python3 experiments/switch_mp/pressure.py --switch /tmp/tomtom-switch-mp-opt \
  --output /tmp/mp-stress-results/pressure
python3 experiments/switch_mp/churn.py --switch /tmp/tomtom-switch-mp-opt \
  --output /tmp/mp-stress-results --tag churn-native --cycles 500 --workers 8
python3 experiments/switch_mp/mesh.py --switch /tmp/tomtom-switch-mp-opt \
  --output /tmp/mp-stress-results --variant native
python3 experiments/switch_mp/stress.py --phase soak \
  --switch /tmp/tomtom-switch-mp-opt --driver /tmp/tomtom-mp-stress-load \
  --variant native --output /tmp/mp-stress-results
python3 experiments/switch_mp/stress.py --phase performance \
  --switch /tmp/tomtom-switch-mp-opt --reference /tmp/tomtom-mp-reference \
  --driver /tmp/tomtom-mp-stress-load --output /tmp/mp-stress-results
```

Run from the repository root. The matrix requires eight available physical
cores. The correctness matrix has 90 cases, with 1, 2, 3, 4, 6 and 8 workers;
payloads include 64, 1500, 9000 and 65535 bytes and a 64/9000-byte mix. Additional
cases exercise upstream, downstream, duplex and hot-adapter traffic.
The mixed pattern generates two 9000-byte frames for each 64-byte frame.
`pressure.py` deliberately blocks adapter reads, fills queues and ingress
pools, replaces ports with pending output, and tests malformed/oversize input.
`churn.py` grows and shrinks a dense 24-by-24 matrix under independently verified
traffic between stable ports. Temporary peers are reconnected with new
generations; old-generation discard is counted separately from stable traffic.
`mesh.py` drives all 576 edges of a fully connected 24-port matrix, including
self-routes, for 30 seconds with full payload and FIFO validation.

The default native soak runs 15 minutes at 20 tunnels plus three adapters.
Two temporary trunks are added/removed every 30 seconds, forcing role migration
from 3 RX + 3 TX + 1 RXa + 1 TXa to 2 + 2 + 2 + 2 with eight workers. Other
variant names select a five-minute, lower-rate soak with full payload checking.
Use `--duration` and `--soak-churn-interval` for short pilots.

Sanitizer builds use `-O1 -g -fno-omit-frame-pointer` and either
`-fsanitize=thread -fno-pie -no-pie` or `-fsanitize=address,undefined` on both
the switch and driver. Pass their paths and a distinct `--variant` to keep
results separate. Sanitizer runs are correctness tests, not performance data.

`fault_checks.py` uses the test-only `faults.cpp` LD_PRELOAD library to inject
allocation and poll/ppoll/send/recv failures into its own disposable child:

```bash
g++ -std=c++17 -shared -fPIC -O2 experiments/switch_mp/faults.cpp \
  -o /tmp/tomtom-mp-faults.so -ldl
python3 experiments/switch_mp/fault_checks.py --help
python3 experiments/switch_mp/fault_checks.py --switch /tmp/tomtom-switch-mp-opt \
  --library /tmp/tomtom-mp-faults.so --output /tmp/mp-stress-results/faults
```

Do not preload this fixture into normal applications. It uses a marker file
owned by the test and does not change system settings or other processes.

The socket-phase probes also inject `epoll_wait`, `epoll_create1` and
`epoll_ctl` failures. They check bounded worker recovery, pool retry under
backpressure, preservation of the old plan after failed replacement and cleanup
of partially prepared epoll sets under a low FD limit.

## Socket-phase A/B comparison

`socket_bench.py` compares a saved MP binary before the socket changes with the
updated MP binary. An optional `--readiness` binary contains the same socket
changes with the original scheduler, separating their effect from the two-worker
tunnel/adapter pairing. Build all variants with identical compiler options.

```bash
python3 experiments/switch_mp/socket_bench.py \
  --baseline /tmp/mp-before --improved /tmp/mp-after \
  --readiness /tmp/mp-readiness --driver /tmp/tomtom-mp-stress-load \
  --output /tmp/socket-phase-results
```

The default matrix uses one active tunnel and one adapter, the same traffic
with 19 additional inactive tunnel ports, 10 tunnels/one adapter, and 20 tunnels/
three adapters. It runs 25k, 160k and unlimited offered rates, a two-jumbo/one-small
payload mix, and three serial repetitions with rotated variant order. Each
switch has the same four-core affinity and worker-pool limit. Two sender and
two receiver threads run on the other four physical cores. `--driver-threads 1`
provides a separate source-limit control. The script requires eight physical
cores and a fresh output directory; rates, sizes, cases and duration are tunable.

JSONL includes CPU, latency, exact accepted/delivered frame and byte accounting,
source backpressure, internal drops, per-thread context switches and syscall
counter deltas. `summary.json` gives medians. Latency spans source IPC, switch
and receiver scheduling; these measurements do not include TUN, UDP encryption,
NICs or mmap. The underlying harness verifies payload endpoints and ordering
for every received frame; full byte checking belongs to the correctness runs.

The performance matrix runs 540 serial four-second cases by default: three
repetitions, four tunnel counts, three adapter counts, four payload patterns
and offered rates of 160k, 240k and saturation. The 9000-byte cases also run
the old single-thread switch, alternating execution order. Both switches use
the same six physical cores; one sender and one receiver use the other two.
MP therefore sees a six-core worker budget. Driver CPU is recorded: a saturated
driver limits this end-to-end IPC test and must not be interpreted as the
switch's isolated maximum. Do not overlap these trials with other test loads.
The rate option is a requested rate; always compare it with actual offered
frames divided by the measured interval. A CPU-limited driver may not reach
its target even without source EAGAIN. Saturation waits for POLLOUT after
source EAGAIN, while paced cases use timed weighted rounds without retries;
these modes can produce different burst and backpressure patterns.

If the driver approaches its CPU limit, `--phase driver-scaling` runs a
controlled follow-up: a fixed four-core switch budget, 20 tunnels and one to
three adapters, with either one or two sender/receiver threads on separate
cores. It uses 9000-byte payloads at 240k offered frames/s and saturation, and
three repetitions (72 cases with the reference binary). The switch budget
stays constant while driver concurrency changes.

Unlike the older `bench.py`, the extended driver stays connected until an
explicit drain/report/stop handshake reconciles all accepted frames with TX
and deliberate drops. Source EAGAIN remains separate from internal loss.
Round-robin adapter destinations rotate their starting point to prevent
repeated source backpressure from systematically omitting a destination.
CPU includes the short drain and is divided by the sender's measured interval;
occupied-core values are approximate, and can slightly exceed a thread count.
The pool occupancy statistic is a live scan and can include transient RX
reservations for an empty `recv`; pressure tests reconcile persistent frames
using counters after traffic stops. Individual counters are relaxed atomic
observations, not a globally consistent snapshot.

Results are appended after every case to JSONL, with raw driver/control data,
per-thread CPU/context-switch counters, RSS/FD samples, commands, affinities
and source/binary hashes. Long soaks add per-second process telemetry and a
topology log. `--skip` resumes at a case index; use a new output directory or
variant to preserve earlier metadata and logs.

Generate a table of medians, ranges and driver/role CPU from a completed run:

```bash
python3 experiments/switch_mp/summarize.py \
  /tmp/mp-stress-results/performance-native.jsonl --output /tmp/mp-stress-results
```
